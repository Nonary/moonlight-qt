// Counterfactual admission/service simulation using the actual production worker.
// No window, network, decoder or graphics device is created.
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.h"
#include "vrrtestfakes.h"
#include "vrrreplayconfig.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/profilecodec.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

static std::atomic_uint64_t virtualNow { 1000000 };
extern "C" uint64_t LiGetMicroseconds(void) { return virtualNow.load(); }

static QString resolvedPath(const QString& path) {
    const QFileInfo info(path);const auto canonical=info.canonicalFilePath();
    if(!canonical.isEmpty())return canonical;
    const auto parent=QFileInfo(info.absolutePath()).canonicalFilePath();
    return QDir::cleanPath(QDir(parent.isEmpty()?info.absolutePath():parent).filePath(info.fileName()));
}
static bool samePath(const QString& a,const QString& b) {
#ifdef Q_OS_WIN
    return resolvedPath(a).compare(resolvedPath(b),Qt::CaseInsensitive)==0;
#else
    return resolvedPath(a)==resolvedPath(b);
#endif
}
static bool existsOrLink(const QString& path) {
    const QFileInfo info(path);return info.exists()||info.isSymLink();
}
static bool writeNewJson(const QString& path,const QJsonObject& value) {
    if(existsOrLink(path)){std::fprintf(stderr,"Refusing to replace existing JSON output\n");return false;}
    QSaveFile out(path);const auto bytes=QJsonDocument(value).toJson();
    if(!out.open(QIODevice::WriteOnly)||out.write(bytes)!=bytes.size()||!out.commit()) {
        std::fprintf(stderr,"JSON output write/commit failed: %s\n",qPrintable(out.errorString()));return false;
    }
    return true;
}

class VrrWorkerReplayHarness {
public:
    struct Input {
        uint64_t sequence=0, arrival=0, output=0, ready=0, receive=0, reassembled=0, decodeSubmit=0;
        uint64_t prepare=0, present=0, loss=0;
        int frame=0; uint32_t rtp=0; bool rtpValid=true, cancelled=false, completion=false;
        bool assumedPrepare=false, assumedPresent=false;
    };
    struct Presenter : IVrrFramePresenter {
        VrrWorkerReplayHarness* h=nullptr; Input* active=nullptr; uint64_t serial=0;
        VrrFallbackReason checkSupport() const override { return VrrFallbackReason::NoFallback; }
        bool canLatchAdaptivePresent() const override { return true; }
        VrrDecodeReadiness observeDecodeReadiness(AVFrame* f,uint64_t) override {
            auto& i=h->inputs[size_t(f->pts)]; const auto start=LiGetMicroseconds();
            if (i.completion) h->advance(std::max(start,i.ready));
            return {LiGetMicroseconds()-start,i.completion,!i.completion};
        }
        VrrPrepareResult prepareFrame(AVFrame* f,uint64_t) override {
            active=&h->inputs[size_t(f->pts)];
            h->executedUnknownPreparation+=active->assumedPrepare;
            h->advance(LiGetMicroseconds()+active->prepare);
            VrrPrepareResult r;r.prepared=true;r.sourceFrameReusable=true;return r;
        }
        VrrPresentFeedback presentAdaptive(const VrrPresentRequest&) override {
            h->executedUnknownPresent+=active->assumedPresent;
            VrrPresentFeedback f;f.presented=!active->cancelled;f.cancelled=active->cancelled;
            f.nativeBackendValid=true;f.nativeBackend=VrrNativePresentationBackend::Vulkan;
            f.nativePresentResultValid=true;f.nativePresentResult=f.presented?0:-1;
            f.submissionTimeValid=true;f.submissionTimeUs=LiGetMicroseconds();
            f.submissionIdValid=f.presented;++serial;f.submissionId=f.presented?serial:0;
            // Unknown display timing stays unknown. Never echo captured
            // display events onto a counterfactual enqueue timeline.
            h->advance(LiGetMicroseconds()+active->present);return f;
        }
    } presenter;
    std::vector<Input> inputs;
    std::unique_ptr<TrackedFrameLifetime[]> lifetime;
    bool traceCompleted=false;
    std::unique_ptr<VrrPacingWorker> worker;
    PacerTelemetry telemetry;
    size_t next=0, unknownService=0, unknownPresentService=0, observedPreparedAhead=0;
    size_t executedUnknownPreparation=0, executedUnknownPresent=0;
    VrrSessionConfig config;
    VrrTimingParameters parameters;
    QByteArray inputHash;
    QByteArray initialProfile;
    bool permitMissing=false;

    bool load(const QString& path,QString& error) {
        QFile f(path);if(!f.open(QIODevice::ReadOnly)){error=f.errorString();return false;}
        const auto bytes=f.readAll();inputHash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
        const auto lines=bytes.split('\n');if(lines.empty()){error="empty CSV";return false;}
        const auto header=lines[0].trimmed().split(',');QMap<QByteArray,int> columns;
        uint64_t expectedRows=0;
        if(header.contains("trace_schema")) {
            const auto marker=bytes.indexOf("#vrr_trace_footer,");
            if(marker<1 || bytes[marker-1]!='\n' || bytes.indexOf("#vrr_trace_footer,",marker+1)>=0) {
                error="capture lacks one complete clean-close footer";return false;
            }
            const auto tail=bytes.mid(marker).trimmed();
            if(tail.contains('\n')) {error="data after trace footer";return false;}
            QMap<QByteArray,QByteArray> footer;
            for(const auto& part:tail.split(',')) {
                const auto eq=part.indexOf('=');if(eq>0)footer[part.left(eq)]=part.mid(eq+1);
            }
            if(footer.value("clean_shutdown")!="1" || footer.value("rows_dropped")!="0" ||
               footer.value("write_failed")!="0" || footer.value("size_capped")!="0") {
                error="capture incomplete/dropped/capped/write-failed";return false;
            }
            if(QCryptographicHash::hash(bytes.left(marker),QCryptographicHash::Sha256).toHex()!=footer.value("decoded_sha256")) {
                error="capture decoded checksum mismatch";return false;
            }
            bool ok=false;expectedRows=footer.value("rows_enqueued").toULongLong(&ok);
            if(!ok || !expectedRows) {error="invalid footer row count";return false;}
        }
        for(int k=0;k<header.size();++k) {
            if(header[k].isEmpty()||columns.contains(header[k])){error="empty/duplicate input column";return false;}
            columns[header[k]]=k;
        }
        for(const auto& key:{"arrival_sequence","frame","rtp_timestamp","rtp_valid","pacer_arrival_us","decoder_output_us"})
            if(!columns.contains(key)){error=QString("missing input column: ")+key;return false;}
        QSet<QByteArray> parameterKeys;
#define VRR_PARAMETER_KEY(type,name,member,defaultValue) parameterKeys.insert("param_" #name);
        VRR_TIMING_PARAMETER_FIELDS(VRR_PARAMETER_KEY)
#undef VRR_PARAMETER_KEY
        QList<QByteArray> sessionKeys {"display_refresh_hz","stream_rate_hz","session_latency_mode",
            "additional_queued_frame","can_latch_present","playout_initial_profile","trace_schema","trace_model_kind"};
        for(const auto& key:header)if(key.startsWith("param_")) {
            if(!parameterKeys.contains(key)){error="unsupported controller parameter: "+QString::fromLatin1(key);return false;}
            sessionKeys.append(key);
        }
        bool session=false;QJsonObject snapshot;QMap<QByteArray,QByteArray> sessionValues;
        for(int line=1;line<lines.size();++line){
            if(lines[line].trimmed().isEmpty()||lines[line].startsWith('#'))continue;
            const auto v=lines[line].trimmed().split(',');if(v.size()!=header.size()){error="malformed CSV";return false;}
            QMap<QByteArray,uint64_t> parsed;
            const auto number=[&](const char* key,uint64_t maximum=std::numeric_limits<uint64_t>::max()) {
                const int n=columns.value(key,-1);if(n<0)return true;
                const auto& token=v[n];uint64_t result=0;
                const char* begin=token.constData();const char* end=begin+token.size();
                const auto converted=std::from_chars(begin,end,result);
                if(token.isEmpty()||(token.size()>1&&token[0]=='0')||converted.ec!=std::errc{}||
                   converted.ptr!=end||result>maximum) {
                    error=QString("invalid numeric input %1 at line %2").arg(QString::fromLatin1(key)).arg(line+1);return false;
                }
                parsed[key]=result;return true;
            };
            for(const auto* key:{"arrival_sequence","pacer_arrival_us","decoder_output_us","decode_complete_us",
                    "frame_receive_us","frame_reassembled_us","decode_submit_us","prepare_us","present_call_us",
                    "prepare_start_us","prepare_end_us","present_start_us","present_end_us"})
                if(!number(key))return false;
            for(const auto* key:{"frame","display_refresh_hz","stream_rate_hz","session_latency_mode"})
                if(!number(key,std::numeric_limits<int>::max()))return false;
            for(const auto* key:{"rtp_timestamp","lost_packets","trace_schema","trace_model_kind"})
                if(!number(key,std::numeric_limits<uint32_t>::max()))return false;
            for(const auto* key:{"rtp_valid","cancelled","decode_readiness_observed","model_decode_observed",
                    "prepared_ahead","can_latch_present","additional_queued_frame"})
                if(!number(key,1))return false;
#define VRR_PARSE_PARAMETER(type,name,member,defaultValue) \
            if(!number("param_" #name,std::min<uint64_t>(9007199254740991ULL,std::numeric_limits<type>::max())))return false;
            VRR_TIMING_PARAMETER_FIELDS(VRR_PARSE_PARAMETER)
#undef VRR_PARSE_PARAMETER
            for(const auto& key:sessionKeys)if(columns.contains(key)) {
                const auto& field=v[columns[key]];
                if(session&&sessionValues.value(key)!=field){error="session metadata changes within input: "+QString::fromLatin1(key);return false;}
                if(!session)sessionValues[key]=field;
            }
            const auto value=[&](const char* key)->uint64_t {return parsed.value(key,0);};
            Input i;i.sequence=value("arrival_sequence");i.frame=int(value("frame"));i.rtp=uint32_t(value("rtp_timestamp"));i.rtpValid=value("rtp_valid");
            i.arrival=value("pacer_arrival_us");i.output=value("decoder_output_us");i.ready=value("decode_complete_us");
            i.receive=value("frame_receive_us");i.reassembled=value("frame_reassembled_us");i.decodeSubmit=value("decode_submit_us");
            i.prepare=value("prepare_us");i.present=value("present_call_us");i.cancelled=value("cancelled");i.loss=value("lost_packets");
            i.completion=value("decode_readiness_observed")||value("model_decode_observed");
            observedPreparedAhead+=value("prepared_ahead")!=0;
            const auto costKnown=[&](const char* duration,const char* start,const char* end) {
                if(!columns.contains(duration))return false;
                if(!columns.contains("trace_schema"))return true; // Explicitly declared synthetic cost, including zero.
                return value(duration)>0 || (value(start)>0 && value(end)>=value(start) &&
                    value(end)-value(start)==value(duration));
            };
            for(const auto& cost:{std::array<const char*,3>{"prepare_us","prepare_start_us","prepare_end_us"},
                                 std::array<const char*,3>{"present_call_us","present_start_us","present_end_us"}}) {
                if(columns.contains("trace_schema")&&value(cost[1])>0&&value(cost[2])>=value(cost[1])&&
                   columns.contains(cost[0])&&value(cost[2])-value(cost[1])!=value(cost[0])) {
                    error="inconsistent measured service duration: "+QString::fromLatin1(cost[0]);return false;
                }
            }
            if(!costKnown("prepare_us","prepare_start_us","prepare_end_us")) {
                ++unknownService;i.prepare=400;i.assumedPrepare=true;
            }
            if(!costKnown("present_call_us","present_start_us","present_end_us")) {
                ++unknownPresentService;i.present=20;i.assumedPresent=true;
            }
            if(!i.sequence||!i.arrival||!i.output||i.output>i.arrival){error="invalid immutable arrival/output metadata";return false;}
            inputs.push_back(i);
            if(!session){
                if(value("param_native_synchronized_presentation")||value("param_native_scheduled_presentation")){error="only Deck non-native-sync/non-scheduled path supported";return false;}
                if(columns.contains("can_latch_present")&&!value("can_latch_present")){error="Deck latch-capable path required";return false;}
                config.displayRefreshHz=int(value("display_refresh_hz"));config.streamRateHz=int(value("stream_rate_hz"));
                config.latencyMode=int(value("session_latency_mode"));config.smoothFrameTiming=true;
                config.allowAdditionalQueuedFrame=value("additional_queued_frame");
                config.experimentalInputPercentilePpm=value("param_playout_input_percentile_target_per_million");
                config.experimentalInputPercentileWindowUs=value("param_playout_input_percentile_window_us");
                if(columns.contains("playout_initial_profile"))initialProfile=v[columns["playout_initial_profile"]];
#define VRR_READ_REPLAY_PARAMETER(type,name,member,defaultValue) \
                if(columns.contains("param_" #name)) snapshot[#name]=double(value("param_" #name)); \
                else if(columns.contains("trace_schema")) snapshot[#name]=vrrLegacyControllerParameterDefaultText(QStringLiteral(#name)).toDouble();
                VRR_TIMING_PARAMETER_FIELDS(VRR_READ_REPLAY_PARAMETER)
#undef VRR_READ_REPLAY_PARAMETER
                session=true;
            }
        }
        if(expectedRows && expectedRows!=inputs.size()) {error="footer row count mismatch";return false;}
        if(inputs.empty()||config.displayRefreshHz<=0||config.streamRateHz<=0){error="missing session/input";return false;}
        if(observedPreparedAhead){error="prepared-ahead renderer lifecycle is not modeled";return false;}
        if((unknownService||unknownPresentService)&&!permitMissing){error="unmeasured service costs: explicit --allow-missing-costs required (400us prepare/20us present assumptions)";return false;}
        std::sort(inputs.begin(),inputs.end(),[](const Input&a,const Input&b){return a.arrival<b.arrival||(a.arrival==b.arrival&&a.sequence<b.sequence);});
        std::vector<uint64_t> sequences;for(auto&i:inputs)sequences.push_back(i.sequence);
        std::sort(sequences.begin(),sequences.end());
        for(size_t k=0;k<sequences.size();++k)if(sequences[k]!=k+1){error="incomplete/duplicate arrival sequence";return false;}
        // Captured omissions use the frozen reader's historical mapping above;
        // declared synthetic omissions use the explicitly compiled schema defaults.
        // Neither path silently imports today's live-session resolver.
        parameters=VrrTimingParameters{};
        return applyVrrReplayControllerSnapshot(snapshot,parameters,error);
    }
    void advance(uint64_t until) {
        until=std::max(until,LiGetMicroseconds());
        while(next<inputs.size()&&inputs[next].arrival<=until){
            const size_t k=next++;const auto&i=inputs[k];virtualNow.store(std::max(LiGetMicroseconds(),i.arrival));
            auto f=makeTrackedPacedFrame(i.frame,i.rtp,i.output,lifetime[k],i.rtpValid);
            f.frame()->pts=int64_t(k);f.setDeliveryTimeline(i.receive,i.reassembled,i.decodeSubmit);f.setLostPackets(uint32_t(i.loss));
            f.setDecoderOutputComplete(i.completion&&i.ready<=i.output);
            worker->submit(std::move(f));
        }
        virtualNow.store(std::max(until,LiGetMicroseconds()));
    }
    static void before(void* context,bool reserved) {
        auto& h=*static_cast<VrrWorkerReplayHarness*>(context);auto& w=*h.worker;
        while(!w.replayStartReady.load())SDL_Delay(1);
        // Offline-only disk throttling never advances virtual time. Production
        // tracing remains nonblocking and drops/accounting remain unchanged.
        while(w.m_TraceRowsEnqueued.load()>w.replayTraceConsumed.load()+2048) SDL_Delay(1);
        if(reserved||w.m_FrameQueueDepth.load())return;
        if(h.next<h.inputs.size())h.advance(std::max(LiGetMicroseconds(),h.inputs[h.next].arrival));
        else w.m_Stopping.store(true);
    }
    bool run(QString trace,bool audit,QString& error) {
        presenter.h=this;lifetime=std::make_unique<TrackedFrameLifetime[]>(inputs.size());
        virtualNow.store(inputs.front().arrival);
        qputenv("MOONLIGHT_VRR_TRACE",QFile::encodeName(trace));qputenv("MOONLIGHT_VRR_BUFFER_AUDIT",audit?"1":"0");
        config.calibrationKey.clear();config.calibrationPath.clear();
        worker=std::make_unique<VrrPacingWorker>(&presenter,config,&telemetry);
        worker->m_TimingController=std::make_unique<VrrTimingController>(worker->m_Config,true,parameters);
        // start() serializes the worker seed independently of the controller.
        // Preserve the captured seed while keeping calibration I/O disabled.
        worker->m_StartDelaySeedUs=parameters.playoutDelayStartSeedUs;
        if(!initialProfile.isEmpty()) {
            std::vector<int64_t> words;
            if(!decodeVrrPlayoutProfile(initialProfile,words)||!worker->m_TimingController->loadPlayoutHistory(words)) {
                error="captured startup history incompatible with selected controller";return false;
            }
        }
        worker->m_QueueCapacity=worker->m_TimingController->queuedFrameCapacity();
        worker->replayBeforeDequeue=before;worker->replayContext=this;
        worker->replayTraceResult=&traceCompleted;
        VrrTargetWaiterHooks hooks;hooks.nowUs=[] {return LiGetMicroseconds();};
        hooks.sleepForUs=[this](uint64_t n){advance(LiGetMicroseconds()+n);};
        hooks.yield=[this]{advance(LiGetMicroseconds()+10);};
        worker->m_TargetWaiter=std::make_unique<VrrTargetWaiter>(hooks);
        if(worker->m_NativeSynchronizedPresentation||worker->m_NativeScheduledPresentation){error="Deck native-sync contract mismatch";return false;}
        if(!worker->start()){error="worker start failed";return false;}
        SDL_WaitThread(worker->m_WorkerThread,nullptr);worker->m_WorkerThread=nullptr;
        worker.reset();
        if(!traceCompleted){error="required trace was not completely written/closed with exact row accounting";return false;}
        for(size_t k=0;k<inputs.size();++k)if(lifetime[k].releases!=1){error="frame ownership/release mismatch";return false;}
        return true;
    }
};

int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);QCommandLineParser p;p.addHelpOption();
    p.setApplicationDescription("Production-worker counterfactual simulation. NOT frozen-input exact replay or physical display proof.");
    p.addPositionalArgument("input.csv","Complete expanded trace or declared synthetic arrival/service CSV.");
    p.addOption({"trace","New trace output (CSV or .vrrtrace).","path"});
    p.addOption({"output","Fidelity/parameters JSON output.","path"});
    p.addOption({"lifecycle","captured,0,1,2; no other policy changes.","mode","captured"});
    p.addOption({"audit","Opt-in scalar snapshots,0 or1.","enabled","1"});
    p.addOption({"allow-missing-costs","Acknowledge400us preparation/20us Present assumptions for unmeasured service costs; measured/declared zero remains zero."});
    p.addOption({"audit-benchmark","Measure scalar snapshot batch cost only; requires --output, no input/GPU/client."});
    p.process(app);
    if(p.isSet("audit-benchmark")) {
        if(!p.isSet("output"))p.showHelp(2);
        VrrSessionConfig c;c.displayRefreshHz=120;c.streamRateHz=116;
        VrrTimingController controller(c,true,vrrTimingParametersForSession(c,false,false));
        std::array<double,128> samples;volatile uint64_t checksum=0;
        for(auto& sample:samples) {
            const auto begin=std::chrono::steady_clock::now();
            for(unsigned n=0;n<1024;++n) {const auto s=controller.bufferAudit(1000000+n,6000);checksum=checksum+s.appliedUs+s.flags;}
            sample=double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count())/1024;
        }
        std::sort(samples.begin(),samples.end());double sum=0;for(auto x:samples)sum+=x;
        QJsonObject j{{"scope","128 batch means of1024 scalar snapshots; empty scalar stats; not per-frame/live trace overhead"},
            {"unit","ns per snapshot, averaged within each batch"},{"n",128},{"min",samples[0]},{"mean",sum/128},
            {"p50",samples[63]},{"p95",samples[121]},{"p99",samples[126]},{"max",samples[127]},
            {"snapshot_bytes",int(sizeof(VrrBufferAuditSnapshot))},{"ring_snapshot_payload_bytes",qint64(2*sizeof(VrrBufferAuditSnapshot)*8192)},
            {"checksum",qint64(checksum)}};
        return writeNewJson(p.value("output"),j)?0:1;
    }
    if(p.positionalArguments().size()!=1||!p.isSet("trace")||!p.isSet("output"))p.showHelp(2);
    const auto source=p.positionalArguments()[0],trace=p.value("trace"),output=p.value("output");
    if(samePath(source,trace)||samePath(source,output)||samePath(trace,output)||
       existsOrLink(trace)||existsOrLink(output)) {
        std::fprintf(stderr,"Input, new trace and new JSON output must be distinct; outputs must not already exist\n");return 2;
    }
    QTemporaryDir privateSettings;qputenv("XDG_CONFIG_HOME",QFile::encodeName(privateSettings.path()));
    qputenv("XDG_CACHE_HOME",QFile::encodeName(privateSettings.path()));qputenv("XDG_DATA_HOME",QFile::encodeName(privateSettings.path()));
    if(SDL_Init(SDL_INIT_TIMER)!=0)return 2;
    VrrWorkerReplayHarness h;h.permitMissing=p.isSet("allow-missing-costs");QString error;
    if(!h.load(p.positionalArguments()[0],error)){std::fprintf(stderr,"%s\n",qPrintable(error));return 2;}
    if(p.value("lifecycle")!="captured"){
        bool ok=false;const uint64_t m=p.value("lifecycle").toULongLong(&ok);if(!ok||m>2)return 2;h.parameters.playoutInputLayerLifecycle=m;
    }
    if(!validateVrrTimingParameters(h.parameters,error)){std::fprintf(stderr,"%s\n",qPrintable(error));return 2;}
    const auto start=std::chrono::steady_clock::now();
    if(!h.run(p.value("trace"),p.value("audit")=="1",error)){std::fprintf(stderr,"%s\n",qPrintable(error));return 1;}
    QJsonObject result{{"model","production-worker-closed-loop-v1"},{"frozen_input_exact_replay",false},{"physical_display_proof",false},
        {"all_arrivals_readmitted",true},{"production_queue_drop_schedule_feedback",true},{"native_sync",false},{"native_scheduled",false},
        {"display_observation","unavailable; no captured display events transplanted"},{"source_input_sha256",QString::fromLatin1(h.inputHash)},
        {"inputs",qint64(h.inputs.size())},{"unmeasured_preparation_cost_frames",qint64(h.unknownService)},
        {"unmeasured_present_cost_frames",qint64(h.unknownPresentService)},
        {"executed_assumed_preparation_calls",qint64(h.executedUnknownPreparation)},
        {"executed_assumed_present_calls",qint64(h.executedUnknownPresent)},
        {"assumed_cost_population","unmeasured_*_frames counts input assignments, including subsequently dropped inputs; executed_* counts modeled calls"},
        {"service_scope","recorded per-frame CPU costs/completion bounds, external cancellations reused; no GPU/resource contention or decoder backpressure prediction"},
        {"runtime_ms",double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count())/1000},
        {"controller",vrrTimingParametersToJson(h.parameters)}};
    const bool written=writeNewJson(p.value("output"),result);
    SDL_Quit();return written?0:1;
}
