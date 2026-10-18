// Recorded input / feedback tests of the real production controller.
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QFileInfo>
#include <QDir>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <algorithm>
#include <deque>
struct Sample { uint64_t at,frame,buffer,target,missed,eligible; bool presented,releaseQualified; uint64_t decoder,submission,planned,error,lateness; };
static int failures=0;
static void check(bool ok,const char* message) {if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",message);}}
static std::vector<Sample> run(const QJsonObject& fixture,bool validate) {
 VrrTimingParameters p;const auto params=fixture["params"].toObject();
#define LOAD(type,key,member,defaultValue) if(params.contains(#key))p.member=static_cast<type>(params[#key].toDouble());
 VRR_TIMING_PARAMETER_FIELDS(LOAD)
#undef LOAD
 if(validate)p.playoutInputStableAdaptation=1;
 VrrSessionConfig s;s.displayRefreshHz=fixture["display_hz"].toInt();s.streamRateHz=fixture["stream_hz"].toInt();s.allowAdditionalQueuedFrame=fixture["additional_queue"].toInt();
 s.latencyFix=p.latencyFixEnabled && !p.latencyFixAllRates;s.latencyMode=p.latencyFixAllRates?(p.latencyFixDelayPeriodPerMille?1:2):0;
 s.timingOptions={int(p.playoutDelayMaximumPeriodPerMille),int(p.playoutOnTimeTargetPerMillion/100),int(p.playoutReadinessWindowUs/1000000),int(p.playoutIntervalToleranceUs)};
 if(!validate) {
  auto production=s;production.experimentalInputPercentilePpm=1;
  p.playoutInputStableAdaptation=vrrTimingParametersForSession(production).playoutInputStableAdaptation;
 }
 const auto revision=p.playoutInputStableAdaptation;
 VrrTimingController c(s,fixture["can_latch"].toInt(),p);
 std::vector<int64_t> profile;for(const auto v:fixture["initial_profile"].toArray())profile.push_back(int64_t(v.toDouble()));
 check(c.loadPlayoutHistory(profile),"starting calibration must load before the first frame");
 QHash<QString,int> columns;int col=0;for(const auto v:fixture["columns"].toArray())columns[v.toString()]=col++;
 std::vector<Sample> samples;uint64_t mismatches=0;std::vector<int64_t> values(columns.size(),0);
 for(const auto row:fixture["rows"].toArray()) {
  const auto a=row.toArray();for(int j=0;j<a.size();++j)values[j]+=int64_t(a[j].toDouble());
  const auto n=[&](const char* key)->uint64_t {
   if(!columns.contains(key)){std::fprintf(stderr,"missing fixture column %s\n",key);std::exit(2);}
   return uint64_t(values[columns.value(key)]);};
  const auto no=n("frame"),at=n("decision_us");
  PacedFrame frame(nullptr,int(no),uint32_t(n("rtp_timestamp")),n("rtp_valid"),n("decoder_output_us"));
  frame.noteGpuReadyUs(n("decode_complete_us"));frame.setLostPackets(uint32_t(n("lost_packets")));frame.noteDecodeSyncWaitUs(n("decode_sync_wait_us"));
  frame.setDeliveryTimeline(n("frame_receive_us"),n("frame_reassembled_us"),n("decode_submit_us"));frame.setDecodeHoldUs(n("decode_hold_us"));
  if(n("external_rebase_applied"))c.rebase();
  const auto d=c.schedule(frame,at);
  if(n("trajectory_resync_dropped"))c.noteTrajectoryResyncDrop();
  if(n("trajectory_resync_deferred"))c.noteTrajectoryDeferred();
  if(n("input_buffer_miss_observation_us")&&(n("input_network_buffer_miss")||n("input_decode_buffer_miss")))c.noteInputBufferMiss(n("input_buffer_miss_observation_us"),n("input_network_buffer_miss"),n("input_decode_buffer_miss"));
  const auto disposition=n("disposition_code");const bool stale=disposition==2,afterWait=stale&&n("render_wait_entry_us");
  const bool prep=n("prepare_start_us")||n("prepare_end_us")||n("prepare_us");
  if(prep) {
   c.notePreparationDuration(n("prepare_us"),n("prepare_timing_valid")?n("prepare_acquire_us"):0,n("prepare_end_us"),0);
   c.noteGpuReadyWait(n("gpu_ready_wait_us"),n("gpu_ready_timing_valid")&&n("gpu_ready_wait_result_valid")&&n("gpu_ready_wait_result")==0,n("gpu_ready_time_us"));
  }
  if(disposition<=1) {
   if(n("spacing_guard_feedback_us"))c.noteSpacingDeficit(n("spacing_guard_feedback_us"));
   c.noteSchedulerDelays(n("render_wait_overshoot_us"),n("target_scheduler_delay_us"),n("target_scheduler_delay_valid"));
  }
  const auto compare=[&](){
   const auto st=c.intervalStats().inputPercentile;
   const bool same=st.currentUs==n("input_percentile_current_us")&&st.targetUs==n("input_percentile_target_us")&&st.eligible==n("input_percentile_eligible")&&st.missed==n("input_percentile_missed")&&st.samples==n("input_percentile_samples");
   if(validate&&!same&&mismatches++<3)std::fprintf(stderr,"state drift frame %llu buffer=%llu/%llu target=%llu/%llu eligible=%llu/%llu missed=%llu/%llu samples=%llu/%llu\n",(unsigned long long)no,(unsigned long long)st.currentUs,(unsigned long long)n("input_percentile_current_us"),(unsigned long long)st.targetUs,(unsigned long long)n("input_percentile_target_us"),(unsigned long long)st.eligible,(unsigned long long)n("input_percentile_eligible"),(unsigned long long)st.missed,(unsigned long long)n("input_percentile_missed"),(unsigned long long)st.samples,(unsigned long long)n("input_percentile_samples"));
   samples.push_back({at,no,st.currentUs,st.targetUs,st.missed,st.eligible,bool(n("presented")),st.recentReleaseSamples>=8 && st.recentReleaseMisses==0 && st.validPairCoverageUs>=500000 && st.vrrHeadroomUs && st.currentUs>st.minimumUs && st.targetUs<=st.currentUs,n("decoder_output_us"),n("submission_boundary_us"),d.targetUs,c.intervalStats().update.intervalErrorUs,c.intervalStats().update.latenessUs});
  };
  if(stale)compare();
  if(afterWait){if(c.latencyFixActive()||p.playoutMetronomeEnabled)c.noteSubmission(false,false,0);else c.rebase();}
  else {
   if(n("native_flip_protection_latched"))c.noteNativeFlipProtection(n("native_flip_protection_reference_us"));
   c.noteSubmission(n("presented"),n("cancelled"),n("scheduling_boundary_us")?n("scheduling_boundary_us"):n("submission_boundary_us"),n("submission_boundary_us"));
  }
  if(!stale)compare();
  // This Linux recording has fixed presentation mode and no controller-owned
  // display floor. Display reports are not substituted for input evidence.
 }
 if(validate){std::fprintf(stderr,"recorded revision %llu: %zu decisions, %llu state mismatches\n",(unsigned long long)revision,samples.size(),(unsigned long long)mismatches);check(mismatches==0,"full pre-roll and each chunk boundary must reproduce recorded buffer state");}
 return samples;
}
int main(int argc,char** argv) {
 QCoreApplication app(argc,argv);
 const QString path=argc>1?QString::fromLocal8Bit(argv[1]):QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("fixtures/buffer-drain-20261017.json.qz");
 QFile f(path);if(!f.open(QIODevice::ReadOnly)){std::fprintf(stderr,"cannot read fixture: %s\n",qPrintable(f.errorString()));return 2;}
 QJsonParseError error;const auto doc=QJsonDocument::fromJson(qUncompress(f.readAll()),&error);if(error.error!=QJsonParseError::NoError)return 2;
 const auto fixture=doc.object();
 if(fixture["version"].toInt()!=1 || fixture["row_encoding"].toString()!="column-deltas-v1")return 2;
 const auto old=run(fixture,true);if(failures)return 1;
 const auto candidate=run(fixture,false);
 const auto segment=fixture["segments"].toArray()[0].toObject();const uint64_t first=uint64_t(segment["start_frame"].toDouble()),last=uint64_t(segment["end_frame"].toDouble());
 auto begin=std::find_if(candidate.begin(),candidate.end(),[&](const Sample& a){return a.frame>=first;});
 auto end=std::find_if(begin,candidate.end(),[&](const Sample& a){return a.frame>last;});
 check(begin!=candidate.end()&&end-begin>1000,"reported plateau needs full history and a substantial scored segment");
 uint64_t lastChange=begin->at,level=begin->buffer,longest=0,low=level;
 for(auto i=begin;i!=end;++i) {
  if(i->buffer<level || !i->releaseQualified){longest=std::max(longest,i->at-lastChange);lastChange=i->at;}
  level=i->buffer;
  low=std::min(low,i->buffer);
 }
 longest=std::max(longest,(end-1)->at-lastChange);
 std::fprintf(stderr,"reported segment: start=%llu minimum=%llu final=%llu maximum wait for lower buffer=%llu us, misses=%llu\n",(unsigned long long)begin->buffer,(unsigned long long)low,(unsigned long long)(end-1)->buffer,(unsigned long long)longest,(unsigned long long)((end-1)->missed-begin->missed));
 check(low+100<=begin->buffer,"107s of qualified clean feedback must permit a bounded downward trial below the stale plateau");
 check(longest<=10000000,"no 10s local window may pin obsolete protection after qualified clean feedback");

 // Short diagnostics do not redefine the selected 120-second probability.
 for(const auto value:fixture["segments"].toArray()) {
  const auto part=value.toObject();const auto name=part["name"].toString();
  const uint64_t from=uint64_t(part["start_frame"].toDouble()),to=uint64_t(part["end_frame"].toDouble());
  auto a=std::find_if(candidate.begin(),candidate.end(),[&](const Sample& x){return x.frame>=from;});
  auto b=std::find_if(a,candidate.end(),[&](const Sample& x){return x.frame>to;});
  check(a!=candidate.end() && b-a>=100,"each named event chunk must retain substantial input");
  if(a==candidate.end() || b==a)continue;
  uint64_t reversals=0,anchor=a->buffer;int direction=0;
  for(auto i=a;i!=b;++i)if(i->buffer>anchor+100 || i->buffer+100<anchor) {
   const int next=i->buffer>anchor?1:-1;if(direction && direction!=next)++reversals;
   direction=next;anchor=i->buffer;
  }
  std::fprintf(stderr,"chunk %s: frames=%llu..%llu buffer=%llu..%llu reversals(>=100us)=%llu\n",qPrintable(name),(unsigned long long)a->frame,(unsigned long long)(b-1)->frame,(unsigned long long)a->buffer,(unsigned long long)(b-1)->buffer,(unsigned long long)reversals);
  for(const uint64_t seconds:{1,2,5,10}) {
   uint64_t worstMisses=0,worstCluster=0,worstSeverity=0,worstPlanned=0,worstRecorded=0,minPresents=UINT64_MAX;
   auto left=a;uint64_t count=0,presents=0;
   for(auto i=a;i!=b;++i) {
    const auto previous=i==candidate.begin()?i->missed:(i-1)->missed;
    count+=i->missed-previous;presents+=i->presented;
    while(left<i && i->at-left->at>=seconds*1000000) {
     const auto prev=left==candidate.begin()?left->missed:(left-1)->missed;
     count-=left->missed-prev;presents-=left->presented;++left;
    }
    if(i->at-a->at<seconds*1000000)continue;
    worstMisses=std::max(worstMisses,count);minPresents=std::min(minPresents,presents);
    uint64_t cluster=0;
    for(auto j=left;j<=i;++j) {
     const auto prev=j==candidate.begin()?j->missed:(j-1)->missed;
     if(j->missed>prev){++cluster;worstSeverity=std::max(worstSeverity,std::min(j->error,j->lateness));}else cluster=0;
     worstCluster=std::max(worstCluster,cluster);
     worstPlanned=std::max(worstPlanned,j->planned>j->decoder?j->planned-j->decoder:0);
     if(j->presented)worstRecorded=std::max(worstRecorded,j->submission>j->decoder?j->submission-j->decoder:0);
    }
   }
   if(minPresents!=UINT64_MAX)std::fprintf(stderr,"  %llus worst: misses=%llu cluster=%llu attributed_severity=%lluus planned_target_latency_max=%lluus recorded_CPU_submission_latency_max=%lluus recorded_presentations_min=%llu\n",(unsigned long long)seconds,(unsigned long long)worstMisses,(unsigned long long)worstCluster,(unsigned long long)worstSeverity,(unsigned long long)worstPlanned,(unsigned long long)worstRecorded,(unsigned long long)minPresents);
  }
 }
 uint64_t previousSubmission=0,maxGap=0;
 for(const auto& x:old)if(x.presented && x.submission) {
  if(previousSubmission)maxGap=std::max(maxGap,x.submission-previousSubmission);
  previousSubmission=x.submission;
 }
 std::fprintf(stderr,"recorded presentation progress: maximum CPU submission gap=%llu us (recorded feedback; not candidate worker output)\n",(unsigned long long)maxGap);
 // A raw long submission interval is not a defect: the source may be idle.
 // Require a nearly full one-second window, eight distinct fresh decoder
 // outputs, and no presentation before flagging observed loss of progress.
 std::deque<const Sample*> progressWindow;
 uint64_t starvedWindows=0;
 for(const auto& x:old) {
  progressWindow.push_back(&x);
  while(!progressWindow.empty() && x.at-progressWindow.front()->at>=1000000)progressWindow.pop_front();
  if(progressWindow.empty() || x.at-progressWindow.front()->at<900000)continue;
  uint64_t fresh=0,presentations=0,lastFrame=0,lastDecoder=0;
  for(const auto* y:progressWindow) {
   presentations+=y->presented;
   if(y->decoder && y->at>=y->decoder && y->at-y->decoder<1000000 && y->frame>lastFrame && y->decoder>lastDecoder) {
    ++fresh;lastFrame=y->frame;lastDecoder=y->decoder;
   }
  }
  starvedWindows+=presentations==0 && fresh>=8;
 }
 std::fprintf(stderr,"recorded 1s windows with fresh decoded frames but no presentation=%llu\n",(unsigned long long)starvedWindows);
 if(argc>2 && QString::fromLocal8Bit(argv[2])=="--require-recorded-progress")
  check(starvedWindows==0,"recorded presentation progress fails locally while fresh decoded frames continue; buffer adaptation does not repair the separate startup starvation");
 std::fprintf(stderr,"trace-chunk regressions: %s (%d failures)\n",failures?"FAIL":"PASS",failures);return failures?1:0;
}
