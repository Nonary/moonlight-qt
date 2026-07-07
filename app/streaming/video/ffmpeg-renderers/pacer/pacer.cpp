#include "pacer.h"
#include "vrrcadence.h"
#include "highressleep.h"
#include "streaming/streamutils.h"

#include <QDateTime>
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <QVector>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 3
static_assert(PACER_MAX_OUTSTANDING_FRAMES == MAX_QUEUED_FRAMES + 2,
              "PACER_MAX_OUTSTANDING_FRAMES and MAX_QUEUED_FRAMES must agree");

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3
#define CADENCE_SLEEP_THRESHOLD_US 1000
#define CADENCE_YIELD_THRESHOLD_US 200
#define MAX_RECORDED_FRAME_INTERVAL_US 1000000
#define FRAME_DIAGNOSTIC_DUMP_INTERVAL_US 30000000

// Frames rendered this soon after the first render are startup ramp
// (decoder warmup, stream bring-up stalls of 100-250ms), not pacing
// signal; they would otherwise pollute the session-wide interval
// max/stddev and fire a guaranteed cadence-anomaly dump on every launch.
#define STARTUP_WARMUP_PERIOD_US 1500000
#define FRAME_DIAGNOSTIC_DUMP_SAMPLES 96
#define RTP_TIMESTAMP_HZ 90000

static uint64_t frameCadenceTimestampUs(AVFrame* frame)
{
    if (frame->pts != AV_NOPTS_VALUE && frame->pts >= 0) {
        return (uint64_t)frame->pts * 1000000ULL / RTP_TIMESTAMP_HZ;
    }

    return frame->pkt_dts > 0 ? (uint64_t)frame->pkt_dts : 0;
}

// Cross-session VRR calibration cache. The cadence thread's feedback loops
// re-learn the same hardware facts on every stream: which content rates this
// display/driver stack chronically tears at in the near-ceiling band (the
// tear-rate probe pays for that answer in visible tears - and pays again
// after every content-rate wobble, stream restart, and app launch: measured
// 2026-07-04/06 as 7-10 probe tear-bursts per session because the old
// single-slot ladder forgot each rate the moment the content moved >600us
// away from it), and how much lead margin this pipeline typically needs
// (every session restarts at the fixed 4ms default and re-converges).
// Both are stable properties of the panel + refresh mode + stream rate, so
// they are remembered in the settings store, keyed by that identity.
//
// Deliberately NOT cached: anything re-measured cheaply and invisibly
// (render-time EMA, servo trim, cadence measurement). Safety properties:
// verdicts expire after two weeks so a driver/OS update gets a fresh
// judgement; a probe PASS decays the matching verdict (halves its period)
// instead of the old erase-everything reset - post-latch probes routinely
// pass for a few seconds while the raster is still aligned from the latch
// spell and then fail again, and a full reset on those blips is what kept
// the ladder pinned at 60s; and chronic verdicts only skip the probe, never
// the fallback's own expiry re-probe, so a regime change is still found
// within one latch period. Each verdict also carries a RENDER-REGIME STAMP
// (the net render estimate at its last failure): a verdict earned under a
// materially slower renderer is stale evidence - a faster pipeline may
// well flip-follow the same rate - so the pre-latch then serves only the
// base rung before re-verifying instead of the full chronic period. A
// re-fail refreshes the stamp (the failure is panel physics, not render)
// and full-period protection resumes; the discount never skips the
// pre-latch itself, so session starts stay probe-free on known-bad rates.
// MOONLIGHT_VRR_NO_CALIBRATION_CACHE=1 disables load and save.
struct VrrTearVerdict {
    uint32_t intervalUs;     // measured content interval that failed (fixed at creation)
    uint32_t periodSecs;     // latch period last applied for this rate
    uint32_t failCount;
    qint64 lastSeenSecs;
    uint32_t renderStampUs;  // net render estimate when this rate last failed (0 = unknown/legacy)
    bool latchedThisSession; // one probe-free pre-latch per stream (not persisted)
};

class VrrCalibrationStore
{
public:
    static constexpr uint32_t kMatchToleranceUs = 600;   // same rate-identity window as the old single-slot ladder
    static constexpr uint32_t kBasePeriodSecs = 60;
    static constexpr uint32_t kMaxPeriodSecs = 480;
    static constexpr uint32_t kChronicPeriodSecs = 240;  // pre-latch without a probe at or above this

    VrrCalibrationStore() :
        m_Enabled(false),
        m_Dirty(false),
        m_SeedMarginUs(0),
        m_MarginStampUs(0),
        m_MarginSumUs(0),
        m_MarginSamples(0),
        m_LastFlushUs(0)
    {
    }

    void load(const QString& key)
    {
        m_Key = key;
        m_Enabled = !m_Key.isEmpty() &&
            qEnvironmentVariableIntValue("MOONLIGHT_VRR_NO_CALIBRATION_CACHE") == 0;
        if (!m_Enabled) {
            return;
        }

        QSettings settings;
        settings.beginGroup(QStringLiteral("VrrCalibration"));
        settings.beginGroup(m_Key);

        qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
        qint64 savedSecs = settings.value(QStringLiteral("ts")).toLongLong();
        if (savedSecs > 0 && nowSecs - savedSecs <= kTtlSecs) {
            int marginUs = settings.value(QStringLiteral("leadmarginus")).toInt();
            if (marginUs > 0) {
                m_SeedMarginUs = (uint64_t)marginUs;
            }
            int stampUs =
                settings.value(QStringLiteral("marginrenderstampus")).toInt();
            if (stampUs > 0) {
                m_MarginStampUs = (uint32_t)stampUs;
            }
        }

        if (savedSecs > 0 && nowSecs - savedSecs <= kTtlSecs) {
            const QStringList bands =
                settings.value(QStringLiteral("bandleadmargins")).toString()
                    .split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString& serialized : bands) {
                const QStringList fields = serialized.split(QLatin1Char(':'));
                if (fields.count() != 2) {
                    continue;
                }
                int bandFps = fields[0].toInt();
                uint32_t marginUs = fields[1].toUInt();
                if (bandFps >= 10 && bandFps <= 240 && marginUs > 0) {
                    m_BandSeeds.insert(bandFps, marginUs);
                }
            }
        }

        const QStringList verdicts =
            settings.value(QStringLiteral("tearverdicts")).toString()
                .split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& serialized : verdicts) {
            const QStringList fields = serialized.split(QLatin1Char(':'));
            // 4 fields = pre-render-stamp format; the missing stamp loads
            // as 0 (regime unknown), which the pre-latch treats as "may
            // have improved" and re-verifies on the base rung once.
            if (fields.count() != 4 && fields.count() != 5) {
                continue;
            }
            VrrTearVerdict verdict = {};
            verdict.intervalUs = fields[0].toUInt();
            verdict.periodSecs = qMin(fields[1].toUInt(), kMaxPeriodSecs);
            verdict.failCount = fields[2].toUInt();
            verdict.lastSeenSecs = fields[3].toLongLong();
            verdict.renderStampUs = fields.count() == 5 ? fields[4].toUInt() : 0;
            verdict.latchedThisSession = false;
            if (verdict.intervalUs == 0 || verdict.periodSecs < kBasePeriodSecs ||
                    nowSecs - verdict.lastSeenSecs > kTtlSecs ||
                    m_Verdicts.count() >= kVerdictCap) {
                continue;
            }
            m_Verdicts.append(verdict);
        }

        if (m_SeedMarginUs != 0 || !m_Verdicts.isEmpty()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "VRR calibration cache: restored %d tear verdict(s), lead margin %.2f ms [%s]",
                        (int)m_Verdicts.count(),
                        m_SeedMarginUs / 1000.0,
                        qUtf8Printable(m_Key));
        }
    }

    // The global seed is consumed at stream start, where the render-time
    // EMA is still its init value rather than a measurement - the regime
    // stamp can't be judged against that, so the seed is served as-is. It
    // still protects a heavy pipeline from the first frame, the adaptive
    // loop corrects it within its ~12s window, and the persist-side
    // regime turnover below cleans the stored value itself after one
    // session on a faster renderer.
    uint64_t seedLeadMarginUs() const
    {
        return m_SeedMarginUs;
    }

    // Lead-margin history is kept PER CONTENT-RATE BAND (nearest 10fps).
    // Content rate is variable by design under VRR - a game swings from a
    // 10fps loading screen or paused desktop to 110fps gameplay - and the
    // regimes have genuinely different render behavior (idle GPU clocks
    // make low-rate renders slow and spiky, saturated clocks make
    // high-rate renders fast and tight). One global average lets a stretch
    // of desktop idling poison the seed that 110fps gameplay starts from.
    //
    // Band seeds are served only under the render regime that earned them
    // (same stamp rule as the tear verdicts): a margin history from a
    // materially slower renderer is insurance against overshoots that
    // renderer produced, and re-imposing it on every band transition
    // re-adds its 4-6.5ms of standing lead mid-session. When the regime
    // improved, serve no seed and let the live overshoot window rule - a
    // wrong call self-corrects in one frame, since any overshoot past the
    // margin raises it immediately.
    uint64_t seedLeadMarginForBandUs(int bandFps, uint64_t liveRenderUs) const
    {
        if (renderRegimeImprovedUs(m_MarginStampUs, liveRenderUs)) {
            return 0;
        }
        return m_BandSeeds.value(bandFps, 0);
    }

    void noteLeadMargin(uint64_t marginUs, int bandFps)
    {
        m_MarginSumUs += marginUs;
        m_MarginSamples++;
        if (bandFps > 0) {
            BandAccum& accum = m_BandAccums[bandFps];
            accum.sumUs += marginUs;
            accum.samples++;
        }
    }

    VrrTearVerdict* findVerdict(uint64_t intervalUs)
    {
        int i = findIndex(intervalUs);
        return i >= 0 ? &m_Verdicts[i] : nullptr;
    }

    // True when the live renderer is materially faster than the stamped
    // regime: at least 1.5ms AND 30% quicker, so ordinary estimate wobble
    // or a small clock difference never discounts real evidence. A zero
    // stamp (legacy format, or a store that has never persisted margins)
    // counts as improved once - the next failure or persist stamps it
    // with current numbers.
    static bool renderRegimeImprovedUs(uint32_t stampUs, uint64_t liveRenderUs)
    {
        return stampUs == 0 ||
            (liveRenderUs + 1500 <= stampUs &&
             liveRenderUs * 10 <= (uint64_t)stampUs * 7);
    }

    static bool renderRegimeImproved(const VrrTearVerdict& verdict,
                                     uint64_t liveRenderUs)
    {
        return renderRegimeImprovedUs(verdict.renderStampUs, liveRenderUs);
    }

    // Records a failed tear-rate probe and returns the latch period to apply:
    // the next rung of this rate's own ladder (base for a first offense),
    // resuming from wherever earlier streams and sessions left it. The
    // render stamp refreshes on every fail - a rate that still tears under
    // the current renderer is current evidence, and the ladder position is
    // kept (the failure is panel/driver physics, so a faster render does
    // not restart it).
    uint32_t recordTearFail(uint64_t intervalUs, uint64_t nowUs,
                            uint64_t renderEstUs)
    {
        qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
        uint32_t periodSecs = kBasePeriodSecs;
        int i = findIndex(intervalUs);
        if (i >= 0) {
            periodSecs = qMin(m_Verdicts[i].periodSecs * 2, kMaxPeriodSecs);
            m_Verdicts[i].periodSecs = periodSecs;
            m_Verdicts[i].failCount++;
            m_Verdicts[i].lastSeenSecs = nowSecs;
            m_Verdicts[i].renderStampUs = (uint32_t)renderEstUs;
            m_Verdicts[i].latchedThisSession = true;
        }
        else {
            if (m_Verdicts.count() >= kVerdictCap) {
                int oldest = 0;
                for (int j = 1; j < m_Verdicts.count(); j++) {
                    if (m_Verdicts[j].lastSeenSecs < m_Verdicts[oldest].lastSeenSecs) {
                        oldest = j;
                    }
                }
                m_Verdicts.removeAt(oldest);
            }
            VrrTearVerdict verdict = {};
            verdict.intervalUs = (uint32_t)intervalUs;
            verdict.periodSecs = periodSecs;
            verdict.failCount = 1;
            verdict.lastSeenSecs = nowSecs;
            verdict.renderStampUs = (uint32_t)renderEstUs;
            verdict.latchedThisSession = true;
            m_Verdicts.append(verdict);
        }

        m_Dirty = true;
        // Persist fail verdicts promptly: the process has a history of not
        // reaching a clean teardown, and losing the ladder means paying the
        // probe's tears all over again next session. The write lands while
        // presents are vsync-latched anyway, where a millisecond of settings
        // I/O is invisible.
        flushIfDue(nowUs, 5000000ULL, renderEstUs);
        return periodSecs;
    }

    void recordTearPass(uint64_t intervalUs)
    {
        int i = findIndex(intervalUs);
        if (i < 0) {
            return;
        }
        if (m_Verdicts[i].periodSecs <= kBasePeriodSecs) {
            m_Verdicts.removeAt(i);
        }
        else {
            m_Verdicts[i].periodSecs /= 2;
        }
        m_Dirty = true;
    }

    void saveOnExit(uint64_t renderEstUs)
    {
        if (m_Enabled && (m_Dirty || m_MarginSamples >= kMinMarginSamples)) {
            persist(renderEstUs);
        }
    }

private:
    static constexpr qint64 kTtlSecs = 14LL * 24 * 60 * 60;
    static constexpr int kVerdictCap = 12;
    // ~10s of frames minimum before a session's margin average is considered
    // representative enough to overwrite the stored seed.
    static constexpr uint64_t kMinMarginSamples = 1000;

    int findIndex(uint64_t intervalUs) const
    {
        for (int i = 0; i < m_Verdicts.count(); i++) {
            uint64_t deltaUs = intervalUs > m_Verdicts[i].intervalUs ?
                intervalUs - m_Verdicts[i].intervalUs :
                m_Verdicts[i].intervalUs - intervalUs;
            if (deltaUs <= kMatchToleranceUs) {
                return i;
            }
        }
        return -1;
    }

    void flushIfDue(uint64_t nowUs, uint64_t minGapUs, uint64_t renderEstUs)
    {
        if (!m_Enabled || !m_Dirty) {
            return;
        }
        if (m_LastFlushUs != 0 && nowUs - m_LastFlushUs < minGapUs) {
            return;
        }
        persist(renderEstUs);
        m_LastFlushUs = nowUs;
    }

    void persist(uint64_t renderEstUs)
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("VrrCalibration"));
        settings.beginGroup(m_Key);
        settings.setValue(QStringLiteral("ts"),
                          QDateTime::currentSecsSinceEpoch());

        bool sessionMargins = m_MarginSamples >= kMinMarginSamples;
        uint64_t marginUs = sessionMargins ?
            m_MarginSumUs / m_MarginSamples : m_SeedMarginUs;
        if (marginUs != 0) {
            settings.setValue(QStringLiteral("leadmarginus"), (int)marginUs);
        }

        // Regime turnover for the margin store. Blending a fast session
        // 50/50 into seeds earned under a materially slower renderer drags
        // the old regime along for several more sessions (a 5.0ms seed
        // averaged with a 1.8ms session still stores 3.4ms), and bands the
        // session never visited would re-impose it wholesale on their
        // first visit - so on regime improvement the session's own
        // averages replace the store and unvisited bands are dropped. Only
        // a session with real margin data can turn the stamp over; a short
        // session (verdict-only flush) keeps the stored regime untouched.
        if (sessionMargins) {
            if (renderRegimeImprovedUs(m_MarginStampUs, renderEstUs)) {
                m_BandSeeds.clear();
            }
            m_MarginStampUs = (uint32_t)renderEstUs;
        }
        if (m_MarginStampUs != 0) {
            settings.setValue(QStringLiteral("marginrenderstampus"),
                              (int)m_MarginStampUs);
        }

        // Fold this session's per-band averages into the stored seeds. A
        // band must have ~5s of dwell time at its own rate to qualify
        // (sample counts alone would let high-rate bands qualify in a
        // second while 10fps bands never do), and existing seeds blend
        // 50/50 rather than being replaced so one unusual session doesn't
        // erase a band's history. Bands untouched this session carry over
        // (except across a regime turnover, above).
        for (auto it = m_BandAccums.constBegin(); it != m_BandAccums.constEnd(); ++it) {
            if (it.value().samples < (quint64)it.key() * 5) {
                continue;
            }
            uint32_t avgUs = (uint32_t)(it.value().sumUs / it.value().samples);
            uint32_t oldSeedUs = m_BandSeeds.value(it.key(), 0);
            m_BandSeeds.insert(it.key(),
                               oldSeedUs != 0 ? (oldSeedUs + avgUs) / 2 : avgUs);
        }
        QStringList bandsOut;
        for (auto it = m_BandSeeds.constBegin(); it != m_BandSeeds.constEnd(); ++it) {
            bandsOut.append(QString(QStringLiteral("%1:%2"))
                            .arg(it.key()).arg(it.value()));
        }
        settings.setValue(QStringLiteral("bandleadmargins"),
                          bandsOut.join(QLatin1Char(';')));

        QStringList serialized;
        for (const VrrTearVerdict& v : m_Verdicts) {
            serialized.append(QString(QStringLiteral("%1:%2:%3:%4:%5"))
                              .arg(v.intervalUs).arg(v.periodSecs)
                              .arg(v.failCount).arg(v.lastSeenSecs)
                              .arg(v.renderStampUs));
        }
        settings.setValue(QStringLiteral("tearverdicts"),
                          serialized.join(QLatin1Char(';')));
        m_Dirty = false;
    }

    struct BandAccum {
        quint64 sumUs;
        quint64 samples;
    };

    QString m_Key;
    bool m_Enabled;
    bool m_Dirty;
    QVector<VrrTearVerdict> m_Verdicts;
    QMap<int, uint32_t> m_BandSeeds;
    QMap<int, BandAccum> m_BandAccums;
    uint64_t m_SeedMarginUs;
    uint32_t m_MarginStampUs;
    uint64_t m_MarginSumUs;
    uint64_t m_MarginSamples;
    uint64_t m_LastFlushUs;
};

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_DeferredFreeFrame(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_VrrTearingPreferred(false),
    m_VrrCushionUs(4500),
    m_VideoStats(videoStats),
    m_RendererAttributes(0),
    m_LastRenderTimeUs(0),
    m_FirstRenderTimeUs(0),
    m_EstimatedRenderTimeUs(1000),
    m_LastNetRenderTimeUs(0),
    m_LastFrameDiagnosticDumpUs(0),
    m_FrameDiagnosticRingIndex(0),
    m_FrameDiagnosticRingCount(0),
    m_PresentationMode(IFFmpegRenderer::PresentationMode::Auto)
{

}

Pacer::~Pacer()
{
    // The worker threads check m_Stopping under m_FrameQueueLock in their
    // condition-wait loops, so it must be set (and the conditions signalled)
    // while holding that lock. Otherwise a thread that has just tested
    // m_Stopping but not yet blocked would miss this wakeup and wait forever,
    // hanging SDL_WaitThread below.
    m_FrameQueueLock.lock();
    m_Stopping = true;
    m_PacingQueueNotEmpty.wakeAll();
    m_RenderQueueNotEmpty.wakeAll();
    m_VsyncSignalled.wakeAll();
    m_FrameQueueLock.unlock();

    // Stop the V-sync/cadence thread
    if (m_VsyncThread != nullptr) {
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else if (m_PresentationMode == IFFmpegRenderer::PresentationMode::VrrCadence &&
             m_VsyncThread != nullptr) {
        // VRR cadence renders directly on m_VsyncThread, so cleanup happened there.
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        RenderQueueEntry entry = m_RenderQueue.dequeue();
        av_frame_free(&entry.frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
    av_frame_free(&m_DeferredFreeFrame);
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        RenderQueueEntry entry = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        if (entry.targetPresentUs == 0 || waitUntil(entry.targetPresentUs)) {
            renderFrame(entry.frame);
        }
        else {
            av_frame_free(&entry.frame);
        }
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            me->m_FrameQueueLock.lock();
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
            me->m_FrameQueueLock.unlock();
        }
        else {
            // Let the VSync source wait in the context of our thread
            me->m_VsyncSource->waitForVsync();
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::cadenceThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    VrrCadenceClock cadenceClock(me->m_MaxVideoFps, me->m_DisplayFps);

    // Cross-session memory for the tear-rate ladder and the adaptive lead
    // margin (see VrrCalibrationStore above). Owned by this thread alone.
    VrrCalibrationStore calibration;
    calibration.load(me->m_VrrCalibrationKey);

    // On renderers whose presents all latch at a vblank (plvk FIFO), the
    // display enforces no flip spacing of its own under VRR flip-following -
    // the panel flips the moment a present arrives - so any catch-up tier
    // that spaces flips at the hardware floor scans out at max refresh and
    // shows up on the display's refresh readout as spikes far above the
    // content rate (observed 2026-07-05: 100fps content bouncing the OSD to
    // 118Hz). Those tiers must self-limit relative to the measured cadence
    // here; on scan-position renderers (D3D11) the alignment machinery
    // already spaces them.
    const bool latchedPresents = me->m_VsyncRenderer->arePresentsVsyncLatched();

    // The renderer holds each present back to the pacer's target itself, so
    // leading the render start by a little extra only costs idle wait, while
    // arriving late costs a missed blanking gap - bias the lead accordingly.
    // The margin must cover the render time's spread above its EMA, not just
    // scheduling slop: measured render times swing 3-9ms at 1440p/4K, and a
    // 500us margin let ~30% of frames overshoot their hold and present
    // unaligned (phase noise + mid-scan tears). In classic present mode the
    // renderer presents as soon as it's aligned, so no lead is applied there.
    //
    // The margin is ADAPTIVE: the fixed 4ms that validated on real hardware
    // pays worst-case insurance on every frame, and on a steady scene it is
    // simply 2-3ms of added display latency. Track how far each render's
    // net time overshoots the EMA it was scheduled with, and size the margin
    // to the worst overshoot seen in the recent window plus slack.
    // Asymmetric on purpose - a single overshoot beyond the current margin
    // raises it IMMEDIATELY (the next frame is already protected), while
    // recovery glides down slowly, so one stutter buys seconds of widened
    // protection but genuinely steady scenes still converge to ~1.5-2.5ms.
    //
    // The window/glide/ceiling are sized to how tears actually happened on
    // real hitchy content (measured 2026-07-03): a render that overshoots
    // the margin presents late, lands mid-scan, and the aligner rightly
    // gives up (blank out of reach) - one visible tear per overshoot. With
    // a 3s window and 50us/frame glide the margin cycled 5.0 -> 2.2 -> 5.0
    // endlessly, and every glide-down re-exposed the next spike: 2-4% torn
    // presents in every gameplay window, while the user-validated fixed-4ms
    // era tore far less. Games hitch on a 5-15s rhythm (loads, shader
    // comp, combat bursts), so the margin must REMEMBER a spike across
    // that rhythm: ~12s of overshoot history, a ~10us/frame glide
    // (~0.9ms/s), and a 6.5ms ceiling that actually covers the measured
    // 5-6.5ms overshoot tail (7ms+ spikes stay uncovered by choice - rare
    // enough to eat, and chasing them costs standing latency). Latency
    // only rises while the content itself demonstrates it is misbehaving.
    // MOONLIGHT_VRR_FIXED_MARGIN=1 restores the fixed 4ms (or =<us> for a
    // custom fixed value) for A/B comparison.
    const bool classicPresent =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_CLASSIC_PRESENT") != 0;
    const int fixedMarginEnv =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_FIXED_MARGIN");
    const bool adaptiveMargin = !classicPresent && fixedMarginEnv == 0;
    const uint64_t marginFloorUs = 1000;
    const uint64_t marginCeilUs = 6500;
    const uint64_t marginSlackUs = 750;
    const uint64_t marginGlideUs = 10;
    const int overshootWindowCap = me->m_MaxVideoFps > 0 ?
        qBound(60, me->m_MaxVideoFps * 12, 1536) : 720;
    int32_t overshootRing[1536];
    int overshootHead = 0;
    int overshootCount = 0;
    uint64_t lastMarginLogUs = 0;
    uint64_t lastLoggedMarginUs = 0;

    // Content-rate band scoping for the margin (nearest 10fps, with a
    // short dwell so boundary wobble doesn't thrash it). On a REGIME jump
    // (>=2 bands, e.g. 10fps desktop -> 110fps gameplay) the overshoot
    // window is stale evidence from different GPU-clock behavior: drop it
    // and restart from the new band's own cached history instead of
    // gliding down from the old regime's margin over seconds. Adjacent
    // band drift (109 <-> 111fps) keeps the window - same regime.
    int marginBandFps = 0;
    int marginBandCandidate = 0;
    uint64_t marginBandCandidateSinceUs = 0;
    uint64_t lastBandSeedLogUs = 0;
    uint64_t leadMarginUs =
        classicPresent ? 0 : (fixedMarginEnv > 1 ? (uint64_t)fixedMarginEnv : 4000);
    if (adaptiveMargin && calibration.seedLeadMarginUs() != 0) {
        // Start where this configuration typically lives instead of the
        // one-size default: a heavy (4K) pipeline gets its full protection
        // from the first frame rather than after the first stutter, and a
        // light one starts without the default's extra standing latency.
        // Either way the adaptive loop takes over within its ~12s window.
        leadMarginUs = qBound(marginFloorUs, calibration.seedLeadMarginUs(),
                              marginCeilUs);
    }

    // Rolling ~500ms of pacing queue depth, mirroring the hysteresis the
    // handleVsync/renderFrame paths already use. Network jitter routinely
    // delivers frames in a gap-then-pair pattern; dropping the older frame of
    // every pair (the old zero-tolerance policy here) turned each burst into
    // a visible skip - measured at 3-8% of all frames on Wi-Fi.
    QQueue<int> queueDepthHistory;
    const int queueDepthHistoryCap =
        me->m_MaxVideoFps > 0 ? qMax(me->m_MaxVideoFps / 2, 1) : 60;

    // Near-ceiling taper state for the alignment budget (see below). Starts
    // in full-alignment and drops to the taper the moment the measured
    // cadence closes on the panel's max flip spacing; returning to full
    // requires sustained evidence of real headroom, because the measured
    // EMA rides host-vsync quantization spikes (one 16.7ms delta in a
    // ~9.1ms cadence lifts it ~0.9ms for several frames) and a single hard
    // threshold flip-flops the budget between ~0.6ms and 3ms+, recreating
    // the exact flip-phase wobble the taper exists to kill.
    bool alignTapered = false;
    int alignFullDwell = 0;

    // Standing-latency servo state (see the trim block below).
    // MOONLIGHT_VRR_NO_TRIM=1 disables it for A/B comparison.
    //
    // The cushion is the latency-vs-tearing sweet-spot dial. The pre-servo
    // builds carried a standing 8-9ms queue that measured near-zero tears:
    // every frame sat ~8ms in the pipeline, so no arrival wobble or render
    // spike could make a present late. Trimming to a 2.5ms cushion
    // reclaimed that latency but converted the protection into exposure -
    // the user judged the result as tearing "a lot" against the old build
    // on the same content. The default sits in between; the lead margin
    // covers render-time spikes while this cushion covers arrival jitter,
    // and the two add. The value comes from the "VRR pacing buffer"
    // setting in the UI (default 4500), with MOONLIGHT_VRR_CUSHION_US
    // overriding it for A/B (2500 = old aggressive trim, 6000 = maximum;
    // clamped because at 100fps content a cushion much past 6ms parks
    // pipeline age over the 1.25-interval stale threshold and the schedule
    // thrashes in rush catch-ups).
    const bool latencyTrimEnabled =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_NO_TRIM") == 0;
    const int cushionEnv =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_CUSHION_US");
    const int cushionCfgUs = cushionEnv > 0 ? cushionEnv :
        (me->m_VrrCushionUs > 0 ? me->m_VrrCushionUs : 4500);
    const uint64_t queueCushionUs =
        qBound((uint64_t)1500, (uint64_t)cushionCfgUs, (uint64_t)6000);
    const int slackWindowCap =
        me->m_MaxVideoFps > 0 ? qMax(me->m_MaxVideoFps / 2, 16) : 30;
    uint64_t slackWindowMinUs = UINT64_MAX;
    int slackWindowSamples = 0;
    uint64_t trimStepUs = 0;
    uint64_t padStepUs = 0;
    uint64_t lastTrimLogUs = 0;

    // Measured cushion need. The cushion dial and the in-band 5/8-interval
    // floor are static insurance budgets; the quantity they insure against
    // is measurable: the spread of pipeline ages (max - min of the servo's
    // own queue-delay estimate) across a window in which the schedule held
    // still is exactly the arrival turbulence the buffer absorbed. Track
    // the worst spread over the last ~24 CLEAN half-second windows (windows
    // where the trim/pad servo moved the phase, a snap fired, or a
    // veto-grade event hit are self-distorted and excluded), and let the
    // effective cushion THIN below the static budgets toward that
    // measurement plus a guard - never fatten above them, so a chaotic
    // link behaves exactly like the static design and a calm one stops
    // paying insurance it provably doesn't need. Entirely
    // measurement-derived, so it sizes itself per device and per link.
    // MOONLIGHT_VRR_STATIC_CUSHION=1 disables the thinning for A/B.
    const bool staticCushion =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_STATIC_CUSHION") != 0;
    uint64_t slackWindowMaxUs = 0;
    bool slackWindowTainted = false;
    uint32_t jitterSpreadRingUs[24];
    int jitterSpreadHead = 0;
    int jitterSpreadCount = 0;
    int taintedWindowStreak = 0;
    uint64_t jitterNeedUs = UINT64_MAX;  // UINT64_MAX until enough clean windows
    uint64_t lastCushionLogUs = 0;

    // Scale-free pacing geometry. Every zone and guard below that used to
    // be a fixed microsecond offset is expressed as a fraction (per-mille)
    // of the panel's scanout period, anchored to the value it validated at
    // on the 8.33ms/120Hz reference panel - a fixed 1350us entry zone
    // means "16% of a scanout" there but would mean 32% on a 240Hz panel
    // and 8% at 60Hz, silently moving every behavioral boundary. Fractions
    // keep the zones meaning the same thing on any refresh rate.
    // Deliberately NOT scaled: human-latency quantities (the cushion dial
    // bounds, lead-margin caps, snap sizes, jitter guards) - perceived
    // delay is absolute time regardless of panel speed - and content-
    // domain constants (the ~22fps cadence-adoption bound). Env overrides
    // stay absolute microseconds: they are user-facing measurement knobs.
    const uint64_t scanoutPeriodUs =
        me->m_DisplayFps > 0 ? 1000000ULL / me->m_DisplayFps : 8333;
    auto scanoutFracUs = [scanoutPeriodUs](uint64_t perMille) {
        return scanoutPeriodUs * perMille / 1000;
    };
    const uint64_t flipGuardDefaultUs = scanoutFracUs(18);      // 150us on the reference panel
    const uint64_t ritualSlackDefaultUs = scanoutFracUs(96);    // 800us
    const uint64_t taperEntryZoneUs = scanoutFracUs(162);       // 1350us
    const uint64_t taperExitZoneUs = scanoutFracUs(192);        // 1600us
    const uint64_t bandSlowReleaseZoneUs = scanoutFracUs(300);  // 2500us
    const uint64_t bandEntryStepUs = scanoutFracUs(12);         // 100us
    const uint64_t cushionClampZoneUs = scanoutFracUs(312);     // 2600us
    const uint64_t cadenceSlackGuardUs = scanoutFracUs(24);     // 200us
    const uint64_t alignSpinFloorUs = scanoutFracUs(360);       // 3000us
    const uint64_t alignWideExtraUs = scanoutFracUs(240);       // 2000us
    const uint64_t rushBudgetCapUs = scanoutFracUs(300);        // 2500us
    const uint64_t scheduleGuardUs = scanoutFracUs(60);         // 500us

    // Post-stall recovery tuning (the flip-spacing floor, staleSchedule
    // catch-up, rush-budget floor and cadence-cold latch below).
    // MOONLIGHT_VRR_CLASSIC_RECOVERY=1 restores the old recovery behavior
    // (nominal-spacing catch-up, zero-budget rush presents, no cadence-cold
    // latch) for A/B.
    //
    // The out-of-band free-run spacing floor used to sit 750us above the
    // nominal max-refresh spacing - a "tear-free flip ceiling" measured
    // during free-run collapses, where a free-running raster tears at ANY
    // spacing, so the number was an artifact of the broken regime it was
    // measured in. The near-ceiling band below reached that verdict on
    // 2026-07-04 and has run at a nominal+150us guard since,
    // field-validated in the tightest part of the range; out of band the
    // floor only binds during catch-up bursts anyway (steady content there
    // has >=1.35ms of cadence slack by definition), where the smaller
    // guard just drains post-stall backlogs a shade faster.
    // MOONLIGHT_VRR_FLIP_SLACK_US=750 restores the old floor if post-stall
    // tearing reappears out of band.
    const bool classicRecovery =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_CLASSIC_RECOVERY") != 0;
    const int flipSlackEnv =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_FLIP_SLACK_US");
    const uint64_t flipCeilingSlackUs = flipSlackEnv > 0 ?
        (uint64_t)flipSlackEnv : flipGuardDefaultUs;
    const uint64_t rushAlignFloorUs = scanoutFracUs(72);  // 600us on the reference panel

    // Near-ceiling buffered VRR. Content cadence between the taper
    // threshold (~103fps) and just under the panel's nominal max refresh
    // used to be handed to the vsync latch wholesale, and latched presents
    // run a FIXED raster: content quantized to 120Hz slots repeats a frame
    // on a periodic beat (every ~6 frames at 105fps, every ~29 at 116) -
    // judder during pans. The panel itself can follow this entire range
    // exactly like it follows a local game capped a few fps below max
    // refresh (the classic VRR configuration): in flip-following the panel
    // waits in vblank for the next flip, so the only real per-flip floor
    // is the ~8.3ms scanout itself. What actually failed in the old
    // free-run measurements here (36-57% tears, continuous drops) was
    // presents inheriting arrival/render jitter with no standing buffer:
    // any flip that went out tighter than the previous scanout tore, and
    // bursts of them knocked the driver out of flip-following entirely.
    // NOTE: the 750us "flip ceiling" slack used by the free-run floors
    // elsewhere in this thread was measured during exactly those
    // collapses - a free-running raster tears at ANY spacing - so the band
    // treats it as a free-run artifact, not a flip-following limit, and
    // runs on the nominal scanout spacing plus a small guard instead.
    // So instead of latching the band, decouple flip spacing from the
    // jitter:
    //  - hold a deliberate ~one-content-interval standing queue (the
    //    re-timing buffer: jitter lands on a frame that is already queued,
    //    not on the flip instant),
    //  - keep the schedule on the smoothed cadence with the spacing floor
    //    at nominal-scanout-plus-guard, and
    //  - move the rush/stale/drain machinery behind the buffer so routine
    //    wobble can never fire the tight catch-up flips that broke
    //    flip-following in the first place.
    // Costs ~5ms of standing latency versus the free-run cushion, only
    // while content is inside the band. MOONLIGHT_VRR_NO_NEARBUFFER=1
    // restores the old latch-everything behavior; MOONLIGHT_VRR_BUFFER_GUARD_US
    // moves the band's fast edge (default 150us over nominal max-refresh
    // spacing, admitting content to ~117.9fps on 120Hz; raise it if
    // in-band tearing appears - 1050 recreates the old ~110fps ceiling).
    const bool nearBufferEnabled = !classicRecovery &&
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_NO_NEARBUFFER") == 0;
    const bool fixedNearBufferTarget =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_FIXED_NEARBUFFER") != 0;
    const int bufferGuardEnv =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_BUFFER_GUARD_US");
    const uint64_t bufferGuardUs = bufferGuardEnv > 0 ?
        (uint64_t)bufferGuardEnv : flipGuardDefaultUs;
    bool nearBuffered = false;
    bool prevNearBuffered = false;
    int nearBufferDwell = 0;
    int bandRearmDwell = 0;
    uint64_t lastWideReanchorUs = 0;
    uint64_t lastBufferLogUs = 0;
    // Slack gate for the re-lock ritual (see the ritual block in the loop).
    // MOONLIGHT_VRR_RITUAL_MIN_SLACK_US overrides the 800us default; =1
    // effectively arms rituals at any cadence slack, for the flip-follow
    // ceiling retest described at the gate.
    const int ritualSlackEnv =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_RITUAL_MIN_SLACK_US");
    const uint64_t ritualMinSlackUs = ritualSlackEnv > 0 ?
        (uint64_t)ritualSlackEnv : ritualSlackDefaultUs;
    int relockBurstRemaining = 0;
    uint64_t lastRelockBurstUs = 0;
    uint64_t lastRelockLogUs = 0;
    bool bandSnapPending = false;
    bool overfillDropPending = false;
    uint64_t lastSnapLogUs = 0;

    // The slice of the measured render tail the margin is not allowed to
    // cover (windowed worst overshoot past the margin ceiling). Updated in
    // the adaptive-margin block below; the in-band buffer target adds it,
    // so the queue picks up exactly the render risk the margin hands off.
    uint64_t renderTailBeyondMarginUs = 0;

    // Post-band-release grace (see the staleSchedule block). Sized to one
    // servo window so the trim machinery gets a full measurement cycle to
    // start draining before the stale/drain tiers may fire again.
    int bandStaleGraceFrames = 0;

    // Tear-rate feedback: the self-calibrating replacement for hardcoded
    // per-panel rate limits. Whether a given display/driver stack can
    // flip-follow near-ceiling free-run presents is not knowable a priori
    // (this panel proved unable above ~110fps; others may differ), so
    // measure it: count mid-scan presents across a rolling in-band probe
    // window, and when the observed tear rate proves chronic, hand this
    // content to the vsync latch for a while. Expiry re-probes, so a
    // regime change (different content rate, driver re-engaging) is found
    // within one probe window (~2-3s of tearing per minute worst-case).
    // Only meaningful when the latch exists to fall back to;
    // tearing-preferred users chose cadence-over-tears explicitly.
    // The per-rate ladder (latch period doubling per repeat offense) lives
    // in the calibration store so it survives content-rate wobble, stream
    // restarts, and sessions - the old single-slot version restarted every
    // rate at 60s whenever content moved >600us away and back.
    uint32_t bandTearWindowPresents = 0;
    uint32_t bandTearWindowTears = 0;
    uint64_t bandTearFallbackUntilUs = 0;

    while (!me->m_Stopping) {
        me->m_FrameQueueLock.lock();

        while (!me->m_Stopping && me->m_PacingQueue.isEmpty()) {
            me->m_PacingQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            me->m_FrameQueueLock.unlock();
            break;
        }

        while (queueDepthHistory.count() >= queueDepthHistoryCap) {
            queueDepthHistory.dequeue();
        }
        queueDepthHistory.enqueue(me->m_PacingQueue.count());

        // Absorb transient bursts instead of dropping them: keep up to a one
        // frame backlog (drained via the catch-up target below) and only fall
        // back to keep-newest when the backlog has persisted a full history
        // window - that means presents genuinely can't keep up with delivery
        // and holding more frames would just be permanent added latency.
        //
        // In the near-ceiling buffered band the standing queue IS the design
        // (one frame deliberately in flight as the jitter re-timing buffer),
        // so the bar shifts up one: tolerate a routine depth of 2 and only
        // collapse a persistent 3+. (nearBuffered is last iteration's state
        // here - a one-frame lag on band transitions is harmless.)
        int steadyDepth = nearBuffered ? 2 : 1;
        int frameDropTarget = steadyDepth + 1;
        if (queueDepthHistory.count() == queueDepthHistoryCap) {
            bool persistentBacklog = true;
            for (int depth : std::as_const(queueDepthHistory)) {
                if (depth <= steadyDepth) {
                    persistentBacklog = false;
                    break;
                }
            }
            if (persistentBacklog) {
                frameDropTarget = steadyDepth;
                queueDepthHistory.clear();
            }
        }

        while (me->m_PacingQueue.count() > frameDropTarget) {
            AVFrame* staleFrame = me->m_PacingQueue.dequeue();
            me->m_FrameQueueLock.unlock();
            // The clock must see dropped frames' timestamps too or sustained
            // drops inflate the measured cadence - see observeSourceTime().
            cadenceClock.observeSourceTime(frameCadenceTimestampUs(staleFrame));
            me->m_VideoStats->pacerDroppedFrames++;
            me->maybeLogFrameDiagnostics("vrr cadence queue drop", 0);
            av_frame_free(&staleFrame);
            me->m_FrameQueueLock.lock();
        }

        AVFrame* frame = me->m_PacingQueue.dequeue();
        int queuedBehindCount = me->m_PacingQueue.count();
        bool backlogged = queuedBehindCount > 0;
        me->m_FrameQueueLock.unlock();

        uint64_t nowUs = LiGetMicroseconds();
        uint64_t targetUs = cadenceClock.nextTargetUs(nowUs,
                                                      frameCadenceTimestampUs(frame));
        bool clockPhaseReset = cadenceClock.consumePhaseReset();

        // The clock's smoothed measurement of the actual content cadence -
        // the stream's nominal FPS is only an upper bound (a game hovering
        // at 90fps on a 120fps stream delivers frames every ~11.1ms).
        uint64_t measuredSourceIntervalUs = cadenceClock.smoothedIntervalUs();

        if (adaptiveMargin && cadenceClock.warmedUp() &&
                measuredSourceIntervalUs != 0) {
            int fpsNow = (int)(1000000ULL / measuredSourceIntervalUs);
            int band = qBound(10, ((fpsNow + 5) / 10) * 10, 240);
            if (band == marginBandFps) {
                marginBandCandidate = 0;
            }
            else if (band != marginBandCandidate) {
                marginBandCandidate = band;
                marginBandCandidateSinceUs = nowUs;
            }
            else if (nowUs - marginBandCandidateSinceUs > 700000ULL) {
                int prevBand = marginBandFps;
                marginBandFps = band;
                marginBandCandidate = 0;

                uint64_t bandSeedUs =
                    calibration.seedLeadMarginForBandUs(band,
                                                        me->m_EstimatedRenderTimeUs);
                if (prevBand == 0) {
                    // First warm band of the stream: the margin holds no
                    // stale regime yet, so only RAISE it toward the band's
                    // history (dropping below live early-session evidence
                    // would shed real protection).
                    if (bandSeedUs != 0) {
                        leadMarginUs = qMax(leadMarginUs,
                                            qBound(marginFloorUs, bandSeedUs,
                                                   marginCeilUs));
                    }
                }
                else if (qAbs(band - prevBand) >= 20) {
                    // Regime jump: the overshoot window measured a
                    // different GPU-clock regime. Restart the evidence
                    // window and stand the margin on this band's own
                    // history (or keep the current value when the band is
                    // unknown - the glide takes over from there). A wrong
                    // seed self-corrects in one frame: any overshoot past
                    // it raises the margin immediately.
                    overshootHead = 0;
                    overshootCount = 0;
                    if (bandSeedUs != 0) {
                        uint64_t seededUs = qBound(marginFloorUs, bandSeedUs,
                                                   marginCeilUs);
                        if (seededUs != leadMarginUs &&
                                nowUs - lastBandSeedLogUs > 10000000ULL) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "VRR lead margin: %.2f ms from %dfps band history (was %.2f ms from %dfps regime)",
                                        seededUs / 1000.0, band,
                                        leadMarginUs / 1000.0, prevBand);
                            lastBandSeedLogUs = nowUs;
                        }
                        leadMarginUs = seededUs;
                    }
                }
            }
        }

        uint64_t minFrameIntervalUs = me->m_DisplayFps > 0 ? (1000000ULL / me->m_DisplayFps) : 0;

        // Taper-zone hysteresis: drop to the taper immediately once the
        // cadence closes to within ~1.35ms of the panel's max flip spacing
        // (content above ~103fps on 120Hz); rearm full alignment only after
        // the EMA has shown ~100fps-or-less worth of headroom for a
        // sustained run. The dwell is a leaky counter rather than a
        // consecutive requirement so boundary wobble (content hovering
        // right at ~100fps) makes steady progress instead of resetting.
        //
        // The entry margin deliberately sits ~600us BELOW the panel's
        // tear-free flip ceiling (~750us above max-refresh spacing), not at
        // it: the band between this threshold and the ceiling is the worst
        // measured operating zone for true VRR, not a usable one. At
        // 105-109fps content on the 120Hz panel there is near-zero cadence
        // slack, so alignment waits eat the pipeline's absorb margin -
        // measured as 36-57% tears with 1.4-4% continuous drops - while the
        // same content vsync-latched measures 0.0% tears and 0.00% drops.
        // Do not widen this margin toward the ceiling without hardware
        // measurements showing that zone has become viable. (Those numbers
        // condemn UNBUFFERED free-run alignment only: the near-ceiling
        // buffered band below is the sanctioned way to operate in this
        // zone - it re-times presents behind a standing buffer instead of
        // aligning jittery ones, which is a different regime.)
        if (measuredSourceIntervalUs < minFrameIntervalUs + taperEntryZoneUs) {
            alignTapered = true;
            alignFullDwell = 0;
        }
        else if (measuredSourceIntervalUs >= minFrameIntervalUs + taperExitZoneUs) {
            if (alignFullDwell < 24) {
                alignFullDwell++;
            }
            else {
                alignTapered = false;
            }
        }
        else if (alignFullDwell > 0) {
            alignFullDwell--;
        }

        bool latchAvailable = !me->m_VrrTearingPreferred &&
            qEnvironmentVariableIntValue("MOONLIGHT_VRR_NO_LATCH") == 0;

        // Cadence-cold grace: the clock's sample window restarts on any real
        // stall (stream bring-up, loading screens, entering a game - all the
        // places content cadence genuinely ramps), and until it refills
        // (~0.5s of monotonic timestamps) the measured interval is a warmup
        // EMA that free-run alignment can't be trusted to schedule against.
        // The measured early-session tear storms (11-31% tear windows, 41%
        // of one session's total tears inside the first ~35s) all sat in
        // these cold spans. Latch tear-free until the window is warm:
        // content is chaotic at those moments anyway, so the vsync
        // quantization is imperceptible and the cost lasts under a second
        // per stall.
        bool cadenceColdLatch = latchAvailable && !classicRecovery &&
            !cadenceClock.warmedUp();

        // Band membership for near-ceiling buffered VRR, with hysteresis on
        // the fast edge only (the slow edge inherits the taper's own
        // hysteresis via alignTapered). bufferFloorIntervalUs is both the
        // exit threshold and the in-band flip spacing floor: cadence below
        // it means content is essentially at or above the panel's max
        // refresh, where no pacing can keep service at the arrival rate -
        // that stays the latch's territory. Entry needs the cadence a step
        // inside the band for a sustained dwell (the windowed mean wobbles
        // tens of us; without the step, edge content would flap across the
        // boundary). Exit past the fast edge is immediate: content over
        // the max refresh backlogs by physics, and the fallback (latch, or
        // taper free-run for tearing-preferred users) is always safer than
        // pretending to pace it.
        // Deliberately NOT gated on latchAvailable: tearing-preferred /
        // NO_LATCH users have no latch to fall back to, so without the band
        // their near-ceiling content runs the raw unbuffered free-run that
        // collapses here (measured 2026-07-04: 108fps content, 0 latched,
        // budgets pinned at the 3ms floor, 30% jitter drops) - they need
        // the buffer MORE, not less.
        //
        // The fast edge likewise only exists when the latch is available
        // to hand content to (latched 116-on-120 measured 0.19-0.26ms
        // interval stddev - steadier than anything free-run can do at zero
        // cadence slack). Without the latch, everything past the edge is
        // rush-at-zero-budget free-run - measured 2026-07-04 as 94-97%
        // torn presents at ~118fps content - and band pacing with the
        // guard floor beats that outright, so the band keeps ALL tapered
        // content. This also kills the edge-flapping that unstable content
        // (measured swinging 105-118fps within seconds) causes: each flap
        // wiped the standing buffer before it could do its job.
        // Warm-up matters for ENTRY only. A mid-session content stall
        // restarts the clock's sample window, and releasing the band on
        // that (measured 2026-07-04 on hitchy content stalling every few
        // seconds: "released at 110-115fps" lines that were warmup resets,
        // not rate changes) tears down every in-band protection at exactly
        // the moment content is chaotic. The frozen smoothed interval is
        // still the best cadence estimate available, and the flip floor
        // holds regardless. (For latch users, the cadence-cold latch
        // outranks the band below, which is the safer stint there.)
        // The band is bounded by the measured interval on BOTH sides, not
        // just by alignTapered: the taper's exit dwell holds it engaged for
        // ~24 frames after content slows, and a 24fps cutscene that arrives
        // during that window must not enter the band (measured 2026-07-04:
        // "engaged at 23.8 fps" - the in-band cushion tracks the interval,
        // so slow content targeted a 42ms standing buffer, pure latency).
        // Entry needs the near-ceiling zone proper; the hold releases
        // instantly once content is clearly slower than ~92fps, where full
        // free-run has abundant slack and needs no buffer.
        uint64_t bufferFloorIntervalUs = minFrameIntervalUs + bufferGuardUs;
        bool pastFastEdge = latchAvailable &&
            measuredSourceIntervalUs < bufferFloorIntervalUs;
        bool clearlySlowerThanBand =
            measuredSourceIntervalUs >= minFrameIntervalUs + bandSlowReleaseZoneUs;
        bool tearRateFallback = latchAvailable &&
            nowUs < bandTearFallbackUntilUs;
        if (!nearBufferEnabled || pastFastEdge ||
                clearlySlowerThanBand || tearRateFallback) {
            nearBuffered = false;
            nearBufferDwell = 0;
            bandRearmDwell = 0;
        }
        else if (!alignTapered) {
            // The taper re-arming (content showing sustained headroom below
            // ~min+1600) is a DWELLED band release, not an instant one.
            // Content oscillating across the boundary (measured 2026-07-06:
            // 89-108fps swings, band released at 94.8/100.5fps and
            // re-engaged at 104-108 every 5-20s) paid a full buffer
            // teardown and snap rebuild per crossing - a standing-latency
            // sawtooth. The leaky counter (~0.35s at these rates,
            // content-relative so it scales with any panel/rate) rides out
            // boundary wobble; a genuine slowdown still releases instantly
            // via clearlySlowerThanBand above, so slow content never pays
            // the in-band cushion for long.
            if (!nearBuffered) {
                nearBufferDwell = 0;
                bandRearmDwell = 0;
            }
            else if (bandRearmDwell < 36) {
                bandRearmDwell++;
            }
            else {
                nearBuffered = false;
                nearBufferDwell = 0;
                bandRearmDwell = 0;
            }
        }
        else if (!nearBuffered) {
            bandRearmDwell = 0;
            if (cadenceClock.warmedUp() &&
                    measuredSourceIntervalUs < minFrameIntervalUs + taperEntryZoneUs &&
                    (!latchAvailable ||
                     measuredSourceIntervalUs >= bufferFloorIntervalUs + bandEntryStepUs)) {
                if (nearBufferDwell < 12) {
                    nearBufferDwell++;
                }
                else {
                    nearBuffered = true;
                }
            }
            else if (nearBufferDwell > 0) {
                nearBufferDwell--;
            }
        }
        else if (bandRearmDwell > 0) {
            bandRearmDwell--;
        }

        // The clock snapping onto "now" after a stall wipes the standing
        // buffer's phase offset; re-establish it in one step below rather
        // than re-learning it over seconds of padding.
        if (nearBuffered && clockPhaseReset) {
            bandSnapPending = true;
        }

        if (nearBuffered != prevNearBuffered) {
            if (nearBuffered) {
                // Fresh from a latch spell or free-run chaos the panel is
                // certainly on a fixed raster; allow the re-lock ritual to
                // arm immediately, regardless of when the last one ran, and
                // build the standing buffer in one step.
                lastRelockBurstUs = 0;
                bandSnapPending = true;
            }
            if (nowUs - lastBufferLogUs > 5000000ULL) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "VRR near-ceiling buffer: %s at %.1f fps measured (interval %.2f ms, band floor %.2f ms)",
                            nearBuffered ? "engaged" : "released",
                            measuredSourceIntervalUs > 0 ?
                                1000000.0 / measuredSourceIntervalUs : 0.0,
                            measuredSourceIntervalUs / 1000.0,
                            bufferFloorIntervalUs / 1000.0);
                lastBufferLogUs = nowUs;
            }
            prevNearBuffered = nearBuffered;
        }

        // Cached tear verdict: when earlier streams proved this content rate
        // chronically tears in-band on this display (ladder at or past the
        // chronic rung), latch immediately instead of paying the probe's
        // visible tear burst to rediscover it. One shot per rate per stream,
        // and only ever skips the probe - the fallback expiry still re-probes,
        // so an improved regime (driver update, different stack behavior) is
        // found within one latch period and the pass decay unwinds the
        // verdict from there.
        if (nearBuffered && latchAvailable && nowUs >= bandTearFallbackUntilUs) {
            VrrTearVerdict* verdict =
                calibration.findVerdict(measuredSourceIntervalUs);
            if (verdict != nullptr && !verdict->latchedThisSession &&
                    verdict->periodSecs >= VrrCalibrationStore::kChronicPeriodSecs) {
                // A chronic verdict earned under a slower renderer gets
                // only the base rung before its expiry re-probe: the fast
                // pipeline may flip-follow a rate the slow one provably
                // couldn't (render eating the alignment slack was a
                // dominant failure mode), and blind-latching 4-8 minutes
                // on stale evidence is the "takes forever to notice the
                // hardware got better" trap. The pre-latch itself always
                // happens - session starts stay probe-free - and if the
                // re-probe still fails, the stamp refreshes and the full
                // chronic period resumes, at the one-time cost of a single
                // early-aborted probe burst (~0.2s).
                bool regimeImproved = VrrCalibrationStore::renderRegimeImproved(
                    *verdict, me->m_EstimatedRenderTimeUs);
                uint32_t latchSecs = regimeImproved ?
                    VrrCalibrationStore::kBasePeriodSecs : verdict->periodSecs;
                verdict->latchedThisSession = true;
                bandTearFallbackUntilUs = nowUs +
                    (uint64_t)latchSecs * 1000000ULL;
                if (regimeImproved) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR tear-rate cache: %.1f fps measured tore chronically before (%u failures), but render is now %.2f ms vs %.2f ms then; vsync-latching only %us before re-verifying",
                                measuredSourceIntervalUs > 0 ?
                                    1000000.0 / measuredSourceIntervalUs : 0.0,
                                verdict->failCount,
                                me->m_EstimatedRenderTimeUs / 1000.0,
                                verdict->renderStampUs / 1000.0,
                                latchSecs);
                }
                else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR tear-rate cache: %.1f fps measured tore chronically before (%u failures); vsync-latching for %us without a probe",
                                measuredSourceIntervalUs > 0 ?
                                    1000000.0 / measuredSourceIntervalUs : 0.0,
                                verdict->failCount,
                                latchSecs);
                }
            }
        }

        bool vsyncLatchPresent = ((alignTapered && !nearBuffered) ||
                                  cadenceColdLatch) &&
            latchAvailable;

        // Every free-run spacing floor below adds a small guard over the
        // nominal max-refresh spacing so no flip is asked to go out tighter
        // than the panel can scan. Latched presents keep the nominal floor
        // - the vblank enforces its own spacing, and holding taper-zone
        // renders (105-115fps content) to a wider floor would starve
        // service below the arrival rate. Tearing-preferred users keep the
        // nominal floor too: they traded tears for latency.
        uint64_t flipSpacingFloorUs = minFrameIntervalUs;
        if (!classicRecovery && !vsyncLatchPresent) {
            if (nearBuffered) {
                // In-band the floor is the band's own guard over nominal
                // scanout spacing, NOT the 750us free-run ceiling slack:
                // 116fps content's 8.62ms interval sits BELOW that old
                // ceiling, and flooring flips at it would service slower
                // than arrival - permanent backlog and drops. The guard
                // keeps every flip clear of the previous scanout;
                // flip-following is what makes that sufficient (see the
                // band comment above). Applied to tearing-preferred users
                // too: it costs no latency (it only forbids flips tighter
                // than the panel can scan), and in-band the raster lock is
                // the whole game.
                flipSpacingFloorUs += bufferGuardUs;
            }
            else if (!me->m_VrrTearingPreferred) {
                flipSpacingFloorUs += flipCeilingSlackUs;
            }
        }

        // Belt-and-suspenders on top of the clock's own max-refresh floor: the
        // clock only knows the last INTENDED target, not whether the actual
        // flip overran it (common right after a stall/catch-up recovery,
        // where render work is often heavier than usual). Clamp against the
        // last ACTUAL flip instant so a late-running render can't leave the
        // next target behind reality.
        //
        // The flip instant must be the renderer's Present() call time, NOT
        // renderFrame()'s return time: the latter runs later by the scanline
        // alignment wait plus Present overhead, and flooring the next target
        // on it paces presents slower than frames arrive. That backlog is
        // dropped as "vrr cadence queue drop" (measured 16%+ of the stream),
        // and the over-spaced targets land mid-scan, lengthening the next
        // alignment wait - a self-reinforcing tearing/dropping regime.
        uint64_t lastFlipUs = me->m_VsyncRenderer->getLastPresentUs();
        if (lastFlipUs == 0) {
            // Renderer doesn't report its present instant; fall back to
            // renderFrame() completion (the old, slightly-late behavior).
            lastFlipUs = me->m_LastRenderTimeUs;
        }
        if (lastFlipUs != 0 && targetUs < lastFlipUs + flipSpacingFloorUs) {
            targetUs = lastFlipUs + flipSpacingFloorUs;
        }

        // Effective cushion: the latency servo's set-point (see the servo
        // below) and the yardstick the stale/overfill thresholds are sized
        // from, computed up here because the stale detection needs it.
        // In-band it is the buffer target, sized from what actually
        // threatens the flip instead of a blanket interval. The original
        // fixed one-content-interval target dates from the shared-device
        // render era (net render 8-13ms, stddev ~7ms), when the lead
        // margin's ceiling could not cover the render tail and the
        // standing buffer had to double as render-spike insurance. The
        // render path now runs ~3.5-4ms (2026-07-06 phase-split
        // telemetry), and the margin already starts every render early
        // by the measured worst overshoot - an overshoot within the
        // margin never consumes queue at all - so a full interval
        // double-pays that protection as standing latency. The target is
        //  - the cushion dial (the arrival-jitter budget, the same
        //    meaning it has out of band),
        //  - floored at 5/8 interval so the band always holds real
        //    deliberate depth (the flip-decoupling it exists for) even
        //    on a minimal dial,
        //  - plus the render tail the margin provably cannot cover
        //    right now: it grows the frame after a ceiling-busting
        //    overshoot and decays with the ~12s overshoot window, so
        //    the buffer re-fattens exactly when render misbehaves
        //    (battery power states, shader-comp storms) and thins back
        //    to the dial when it proves steady.
        // Clamped as before so a measurement excursion (stall adopted
        // as cadence while the band hysteresis is still releasing) can
        // never target a multi-frame cushion.
        // MOONLIGHT_VRR_FIXED_NEARBUFFER=1 restores the one-interval
        // target for A/B; fixed-margin/classic builds keep it too,
        // since without the adaptive margin there is no tail
        // measurement to hand off.
        // The measured need (worst clean-window spread + guard, see the
        // jitter ring above) may only ever THIN the cushion below the
        // static budgets - same one-way philosophy as the render-tail cap.
        uint64_t effCushionUs = queueCushionUs;
        if (jitterNeedUs != UINT64_MAX) {
            effCushionUs = qBound((uint64_t)1500, jitterNeedUs,
                                  queueCushionUs);
        }
        if (nearBuffered) {
            uint64_t bandTargetUs;
            if (fixedNearBufferTarget || !adaptiveMargin) {
                bandTargetUs = measuredSourceIntervalUs + scheduleGuardUs;
            }
            else {
                // The render tail may only ever THIN the buffer below the
                // old fixed one-interval design, never fatten it past that.
                // At near-ceiling saturation the "render overshoots" the
                // margin window sees are present-queue backpressure, not
                // GPU work (wall render time == the flip retire interval;
                // measured 2026-07-06 at 112-115fps as 17ms overshoots
                // that pinned this target at its clamp), and feeding
                // backpressure back as a bigger standing buffer deepens
                // the very queue causing it - a 20-26ms measured spiral
                // against the validated ~9ms design.
                uint64_t staticBaseUs = qMax(queueCushionUs,
                                             measuredSourceIntervalUs * 5 / 8);
                uint64_t baseUs = jitterNeedUs != UINT64_MAX ?
                    qBound((uint64_t)1500, jitterNeedUs, staticBaseUs) :
                    staticBaseUs;
                bandTargetUs = qMin(baseUs + renderTailBeyondMarginUs,
                                    measuredSourceIntervalUs + scheduleGuardUs);
            }
            effCushionUs = qMax(effCushionUs,
                                qMin(bandTargetUs,
                                     minFrameIntervalUs + cushionClampZoneUs));
        }

        // Detect a schedule that has drifted late relative to frame delivery:
        // a frame should spend roughly one content interval in the pipeline
        // (decode completion -> pacing queue -> present), so one that is
        // already older than that plus slack means every subsequent frame
        // will queue behind us as pure added latency (measured as ~14ms avg
        // frame queue delay against an 8.3ms frame time).
        uint64_t staleAgeUs =
            measuredSourceIntervalUs + measuredSourceIntervalUs / 4;
        bool bandDrainGrace = false;
        if (nearBuffered) {
            // The deliberate standing buffer holds frames longer by design,
            // so the stale trigger must sit beyond the buffer plus jitter -
            // otherwise routine wobble fires ceiling-spaced rush catch-ups,
            // the exact tight-flip generator the buffer exists to kill. It
            // is sized from the ACTUAL buffer target: the old flat
            // one-interval add dated from the fixed one-interval buffer,
            // and against the thinner render-aware target it let post-hitch
            // queues park a full interval above design (~19.4ms trigger vs
            // a ~6ms set-point at 116fps) before anything rushed them out,
            // leaving the 250us/frame trim to grind the excess off over
            // seconds.
            staleAgeUs = qMax(staleAgeUs,
                              effCushionUs + measuredSourceIntervalUs);
            bandStaleGraceFrames = slackWindowCap;
        }
        else if (bandStaleGraceFrames > 0) {
            // The band just released with its deliberate standing buffer
            // still queued. Without a grace period the very first
            // out-of-band frame reads over the 1.25-interval stale
            // threshold and fires a max-rate rush burst - on content
            // hovering at the band edge (measured 2026-07-05: 96-105fps
            // wobble flapping engage/release every few seconds) that made
            // every release a refresh-rate spike on the panel. Keep the
            // in-band stale and drain thresholds briefly so the latency
            // trim servo drains the buffer at its gentle per-frame rate
            // instead.
            staleAgeUs = qMax(staleAgeUs,
                              effCushionUs + measuredSourceIntervalUs);
            bandStaleGraceFrames--;
            bandDrainGrace = true;
        }
        bool staleSchedule = measuredSourceIntervalUs != 0 && frame->pkt_dts > 0 &&
            nowUs > (uint64_t)frame->pkt_dts + staleAgeUs;

        // Two-tier catch-up, rebasing the clock onto any instant actually
        // used so the schedule converges back to arrival phase instead of
        // staying permanently late.
        //
        // Genuinely stale (>1.25 content intervals of pipeline age - a real
        // stall): rush at the panel's max refresh and skip blank alignment;
        // a possible tear beats compounding lateness into dropped frames.
        //
        // Merely backlogged (a transient extra queued frame, routine with
        // network jitter): drain gently at ~12% tighter than the measured
        // content cadence with alignment still on. Draining at full panel
        // rate here would compress an 11.7ms content cadence to 8.3ms for
        // every absorbed burst - cadence distortion that reads as judder
        // during camera pans, far more visible than the latency it saves
        // (measured: ~45% of frames rushed at 85fps content on Wi-Fi).
        bool rushPresent = false;
        if (lastFlipUs != 0) {
            if (staleSchedule) {
                // Catch up at the free-run flip spacing floor (guarded
                // nominal spacing, see above): stale bursts emitted at raw
                // nominal spacing were the dominant steady-state tear
                // source on hitchy content - measured ~2.5% of presents
                // (~2 tears/sec at 88fps) clustered around game hitches.
                // The extra guard per catch-up frame is immaterial against
                // the >1.25-interval lateness that triggered the rush.
                uint64_t rushSpacingUs = flipSpacingFloorUs;
                if (latchedPresents) {
                    // A latched present under VRR flip-following scans out
                    // the instant it arrives - the display enforces no
                    // spacing - so a floor-spaced rush burst slams the
                    // panel to max refresh (100fps content reading 118Hz
                    // on the OSD). Cap the rush at the drain tier's
                    // ~12%-tighter-than-content spacing; the few extra
                    // frames of convergence are invisible next to the
                    // >1.25-interval lateness that triggered the rush.
                    rushSpacingUs = qMax(rushSpacingUs,
                                         measuredSourceIntervalUs * 7 / 8);
                }
                uint64_t catchUpUs = qMax(lastFlipUs + rushSpacingUs,
                                          LiGetMicroseconds());
                if (catchUpUs < targetUs) {
                    targetUs = catchUpUs;
                    cadenceClock.rebaseTarget(targetUs);
                }
                rushPresent = true;
            }
            else if ((nearBuffered || bandDrainGrace) ?
                         queuedBehindCount >= 2 : backlogged) {
                // In-band, one queued frame is the buffer operating as
                // designed, not backlog - draining it would just re-expose
                // the flip to jitter. Only a genuine 2+ pile-up drains.
                uint64_t drainIntervalUs = qMax(flipSpacingFloorUs,
                                                measuredSourceIntervalUs * 7 / 8);
                uint64_t drainUs = qMax(lastFlipUs + drainIntervalUs,
                                        LiGetMicroseconds());
                if (drainUs < targetUs) {
                    targetUs = drainUs;
                    cadenceClock.rebaseTarget(targetUs);
                }
            }
        }

        // Wait for render start, not present time - renderFrame() below still
        // has to do the CPU submission and wait out the GPU before the flip
        // can actually happen at targetUs. Waiting all the way until targetUs
        // here would make every flip late by that much, which is exactly what
        // leaves tears near the frame edges instead of landing presents on
        // the panel's refresh boundary.
        uint64_t schedEstUs = me->m_EstimatedRenderTimeUs;
        uint64_t renderLeadUs = schedEstUs + leadMarginUs;

        // Standing-latency servo. The schedule's phase only ever moves LATER
        // on its own: a stall snaps the clock's target onto the processing
        // instant of an already-late frame, the stale/drain rebases above
        // anchor onto instants that are late by construction, and any
        // residual cadence-rate error compounds open-loop - while
        // early-arriving frames just wait, so nothing pulls phase back.
        // Measured: after startup stalls a 60fps stream carried a permanent
        // 15-24ms frame queue delay (equilibrium just under the
        // 1.25-interval stale threshold) while pacing 100% in-blank. Track
        // the MINIMUM estimated queue delay across a half-second window -
        // the frame that arrived closest to its slot bounds how phase-late
        // the whole schedule is - and counter it with a per-frame trim rate
        // that spreads that excess over the next window, holding the floor
        // at the queueCushionUs shock-absorber. The rate tops out at 250us/frame
        // (~1.5-2.5% cadence compression, far below the 12% drain tier
        // already judged imperceptible), and one late frame in the window
        // vetoes the trim, so links whose jitter genuinely needs the queue
        // as a shock absorber keep it (smoothness over latency).
        // In-band, the servo's set-point moves up to the deliberate
        // one-interval buffer and gains a symmetric build direction: the
        // schedule only ever drifts later on its own, so without an active
        // pad the buffer would only exist after a lucky stall. The pad
        // stretches the cadence <=1% until the window-min age reaches the
        // set-point; the trim shrinks it back identically once content
        // leaves the band. A deep backlog or stale frame vetoes both.
        uint64_t effDeadbandUs = effCushionUs + 1000;
        bool servoVeto = staleSchedule ||
            ((nearBuffered || bandDrainGrace) ?
                 queuedBehindCount >= 2 : backlogged);

        // One-step buffer snap on band entry and after stall resets. The
        // gradual pad below moves <=100us/frame (~1.5s to build the full
        // cushion) - correct for drift, but on hitchy content (host
        // processing spikes of 400-550ms every few seconds, measured
        // 2026-07-04) the buffer got wiped faster than it could rebuild
        // and in-band pacing effectively never had its jitter protection.
        // A single deliberate phase step is imperceptible (it is latency,
        // not motion) and buys immediate protection.
        if (bandSnapPending) {
            if (nearBuffered && !staleSchedule && frame->pkt_dts > 0) {
                uint64_t renderStartEstUs = targetUs > renderLeadUs ?
                    targetUs - renderLeadUs : 0;
                uint64_t ageUs = renderStartEstUs > (uint64_t)frame->pkt_dts ?
                    renderStartEstUs - (uint64_t)frame->pkt_dts : 0;
                if (ageUs + 1000 < effCushionUs) {
                    uint64_t snapUs = qMin(effCushionUs - ageUs,
                                           (uint64_t)12000);
                    targetUs += snapUs;
                    cadenceClock.rebaseTarget(targetUs);
                    slackWindowTainted = true;
                    if (nowUs - lastSnapLogUs > 10000000ULL) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "VRR buffer snap: +%.2f ms to rebuild the standing buffer (age %.2f ms, cushion %.2f ms)",
                                    snapUs / 1000.0,
                                    ageUs / 1000.0,
                                    effCushionUs / 1000.0);
                        lastSnapLogUs = nowUs;
                    }
                }
            }
            bandSnapPending = false;
        }

        if (latencyTrimEnabled && frame->pkt_dts > 0) {
            uint64_t renderStartEstUs = targetUs > renderLeadUs ?
                targetUs - renderLeadUs : 0;
            uint64_t frameQueueEstUs = renderStartEstUs > (uint64_t)frame->pkt_dts ?
                renderStartEstUs - (uint64_t)frame->pkt_dts : 0;
            slackWindowMinUs = qMin(slackWindowMinUs, frameQueueEstUs);
            slackWindowMaxUs = qMax(slackWindowMaxUs, frameQueueEstUs);
            if (servoVeto) {
                slackWindowTainted = true;
            }

            if (++slackWindowSamples >= slackWindowCap) {
                uint64_t newStepUs = 0;
                uint64_t newPadUs = 0;
                if (!servoVeto && slackWindowMinUs > effDeadbandUs) {
                    newStepUs = qBound((uint64_t)20,
                                       (slackWindowMinUs - effCushionUs) / (uint64_t)slackWindowCap,
                                       (uint64_t)250);
                }
                else if (nearBuffered && !servoVeto &&
                         slackWindowMinUs != UINT64_MAX &&
                         slackWindowMinUs + 1000 < effCushionUs) {
                    newPadUs = qBound((uint64_t)20,
                                      (effCushionUs - slackWindowMinUs) / (uint64_t)slackWindowCap,
                                      (uint64_t)100);
                }
                else if (nearBuffered && !staleSchedule &&
                         slackWindowMinUs != UINT64_MAX &&
                         slackWindowMinUs > effCushionUs +
                             measuredSourceIntervalUs / 2) {
                    // Standing overfill: at saturation (arrival rate ~=
                    // service rate, routine at 116-on-120) the trim servo
                    // is depth-vetoed and the gentle drain's ~140us/frame
                    // never catches up, so the queue parks a whole extra
                    // frame above the cushion - measured 2026-07-04 as a
                    // standing 17-20ms queue against a 9ms design, sitting
                    // right at the stale threshold and thrashing it. Shed
                    // exactly one frame (a single ~8.6ms content skip, at
                    // most twice a second) instead of letting random
                    // queue-drop bursts and stale rushes do it messily.
                    // Half an interval of persistent WINDOW-MIN excess
                    // (tightened from 3/4 alongside the render-aware
                    // target) is unambiguous: the pad/trim servo holds the
                    // min at the cushion when healthy, a parked frame
                    // shows up as a full interval, and jitter alone never
                    // lifts the half-second minimum that far.
                    overfillDropPending = true;
                }
                if (newStepUs != 0 && trimStepUs == 0 &&
                        nowUs - lastTrimLogUs > 30000000ULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR cadence trim: min queue %.2f ms last window, trimming %u us/frame",
                                slackWindowMinUs / 1000.0,
                                (unsigned int)newStepUs);
                    lastTrimLogUs = nowUs;
                }
                if (newPadUs != 0 && padStepUs == 0 &&
                        nowUs - lastTrimLogUs > 30000000ULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR cadence pad: min queue %.2f ms last window, building buffer %u us/frame toward %.2f ms",
                                slackWindowMinUs / 1000.0,
                                (unsigned int)newPadUs,
                                effCushionUs / 1000.0);
                    lastTrimLogUs = nowUs;
                }
                // A window qualifies for the jitter measurement only if the
                // schedule held still throughout: no trim/pad motion, no
                // snap, no veto-grade event. What remains in max-min is the
                // routine arrival turbulence the cushion exists to absorb
                // (stalls and backlogs are the rush machinery's job, priced
                // separately).
                // Sustained chaos safety valve: a measurement frozen from a
                // calmer era must not keep the cushion thin while the link
                // is demonstrably turbulent. If no window has qualified for
                // ~10s straight, forget the measurement and stand the
                // static budgets back up until clean evidence returns.
                if (slackWindowTainted || trimStepUs != 0 || padStepUs != 0) {
                    if (++taintedWindowStreak >= 20) {
                        jitterSpreadCount = 0;
                        jitterNeedUs = UINT64_MAX;
                    }
                }
                else {
                    taintedWindowStreak = 0;
                }
                if (!staticCushion && !slackWindowTainted &&
                        trimStepUs == 0 && padStepUs == 0 &&
                        slackWindowMinUs != UINT64_MAX) {
                    uint64_t spreadUs = slackWindowMaxUs - slackWindowMinUs;
                    jitterSpreadRingUs[jitterSpreadHead] =
                        (uint32_t)qMin(spreadUs, (uint64_t)UINT32_MAX);
                    jitterSpreadHead = (jitterSpreadHead + 1) % 24;
                    if (jitterSpreadCount < 24) {
                        jitterSpreadCount++;
                    }
                    // ~4s of clean evidence before thinning begins; until
                    // then the static budgets stand.
                    if (jitterSpreadCount >= 8) {
                        uint64_t worstUs = 0;
                        for (int i = 0; i < jitterSpreadCount; i++) {
                            worstUs = qMax(worstUs,
                                           (uint64_t)jitterSpreadRingUs[i]);
                        }
                        uint64_t newNeedUs = worstUs + 750;
                        uint64_t deltaUs = jitterNeedUs == UINT64_MAX ? newNeedUs :
                            (newNeedUs > jitterNeedUs ? newNeedUs - jitterNeedUs :
                                                        jitterNeedUs - newNeedUs);
                        if (deltaUs > 1000 &&
                                nowUs - lastCushionLogUs > 30000000ULL) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "VRR cushion: measured arrival spread %.2f ms over last %d clean windows (need %.2f ms, dial %.2f ms)",
                                        worstUs / 1000.0,
                                        jitterSpreadCount,
                                        newNeedUs / 1000.0,
                                        queueCushionUs / 1000.0);
                            lastCushionLogUs = nowUs;
                        }
                        jitterNeedUs = newNeedUs;
                    }
                }
                trimStepUs = newStepUs;
                padStepUs = newPadUs;
                slackWindowMinUs = UINT64_MAX;
                slackWindowMaxUs = 0;
                slackWindowTainted = false;
                slackWindowSamples = 0;
            }
        }

        if (trimStepUs > 0) {
            if (servoVeto) {
                // These paths rebase the schedule phase themselves; trimming
                // on top of a rebase would over-correct. Re-measure instead.
                trimStepUs = 0;
            }
            else {
                uint64_t floorUs = nowUs;
                if (lastFlipUs != 0 && floorUs < lastFlipUs + flipSpacingFloorUs) {
                    floorUs = lastFlipUs + flipSpacingFloorUs;
                }
                uint64_t trimmedUs = targetUs > trimStepUs ?
                    targetUs - trimStepUs : floorUs;
                if (trimmedUs < floorUs) {
                    trimmedUs = floorUs;
                }
                if (trimmedUs < targetUs) {
                    targetUs = trimmedUs;
                    cadenceClock.rebaseTarget(targetUs);
                }
            }
        }
        else if (padStepUs > 0) {
            if (servoVeto || !nearBuffered) {
                padStepUs = 0;
            }
            else {
                targetUs += padStepUs;
                cadenceClock.rebaseTarget(targetUs);
            }
        }

        if (overfillDropPending) {
            AVFrame* overfillFrame = nullptr;
            me->m_FrameQueueLock.lock();
            if (nearBuffered && !me->m_PacingQueue.isEmpty()) {
                overfillFrame = me->m_PacingQueue.dequeue();
            }
            me->m_FrameQueueLock.unlock();
            if (overfillFrame != nullptr) {
                // Dropped frames must still feed the cadence clock - see
                // observeSourceTime().
                cadenceClock.observeSourceTime(
                    frameCadenceTimestampUs(overfillFrame));
                me->m_VideoStats->pacerDroppedFrames++;
                av_frame_free(&overfillFrame);
                me->maybeLogFrameDiagnostics("vrr buffer overfill drop", 0);
            }
            overfillDropPending = false;
        }

        uint64_t targetRenderStartUs = targetUs > renderLeadUs ?
            targetUs - renderLeadUs : 0;

        // How long the renderer may hold this present past its target while
        // waiting for the panel's blanking gap. A tearing-allowed present
        // that goes out mid-scan is a guaranteed visible tear, and a burst
        // of torn flips knocks the driver out of VRR flip-following into a
        // fixed-cadence raster; presents then land at random scan phase, a
        // short fixed wait catches the blank only ~40% of the time, and the
        // resulting tears keep VRR disengaged - measured as minutes-long
        // ~60% mid-scan phases at 78fps content the panel could follow
        // trivially. Sizing the wait from the measured cadence makes
        // in-blank flips the norm whenever content leaves headroom: rushed
        // drains stay aligned (prevention) and, once free-running, a wide
        // wait re-anchors flips to the raster's blank until the driver
        // re-engages (cure).
        //
        // threadSlackUs is the hard wall: waiting longer than the content
        // interval minus the next frame's render needs would push the next
        // flip past its own target and snowball into queue drops.
        uint64_t renderReserveUs = me->m_EstimatedRenderTimeUs + 1200;
        uint64_t threadSlackUs = measuredSourceIntervalUs > renderReserveUs ?
            measuredSourceIntervalUs - renderReserveUs : 0;
        uint64_t cadenceSlackUs =
            (minFrameIntervalUs != 0 &&
             measuredSourceIntervalUs > minFrameIntervalUs + cadenceSlackGuardUs) ?
                measuredSourceIntervalUs - minFrameIntervalUs - cadenceSlackGuardUs : 0;

        // Re-lock ritual state. The driver re-enters VRR flip-following
        // only after a SUSTAINED streak of aligned flips - the 2026-07-03/04
        // forensics showed single wide re-anchors produce tear/catch
        // alternation (one flip caught by chase, the next floor-budget flip
        // tears again, streak never forms). So while the renderer cannot
        // prove lock, pay for a short burst of consecutive
        // full-scanout-budget presents; the standing buffer and the raised
        // in-band stale threshold absorb the cost (~4ms average wait per
        // present, bounded at 8 presents per 2s = <2% of wall time) as
        // temporary queue instead of drops. The burst ends early the moment
        // lock is demonstrated, and the whole mechanism only runs in-band -
        // out-of-band content has the cadence slack to re-anchor organically.
        //
        // Gated on real cadence slack (default >=800us, content up to
        // ~107fps): above that the pipeline was saturated - every present
        // ran late-past-target, the renderer's out-of-reach fast-give-up
        // fired before the ritual budget was ever spent, and the burst was
        // pure churn (measured 2026-07-04: rituals arming every 2s for
        // minutes at 114-116fps content, avg waits 0.1-0.3ms against 10ms
        // budgets, zero locks established).
        // NOTE: that verdict was earned in the shared-device render era,
        // when net render (8.3-8.6ms) itself consumed the whole interval -
        // the saturation WAS the render time. The 2026-07-06 render split
        // cut net render to ~3.5-4ms, which invalidates the evidence
        // behind the gate but not necessarily its conclusion (the panel
        // may still refuse to flip-follow up there). The env override
        // exists to re-ask the panel at 111fps+ in a controlled session,
        // with the align stats and ritual logs as the verdict.
        if (nearBuffered) {
            if (relockBurstRemaining > 0 &&
                    !me->m_VsyncRenderer->isVrrRasterLockUncertain()) {
                relockBurstRemaining = 0;
            }
            else if (relockBurstRemaining == 0 &&
                     cadenceSlackUs >= ritualMinSlackUs &&
                     me->m_VsyncRenderer->isVrrRasterLockUncertain() &&
                     nowUs - lastRelockBurstUs > 2000000ULL) {
                relockBurstRemaining = 8;
                lastRelockBurstUs = nowUs;
                if (nowUs - lastRelockLogUs > 10000000ULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR re-lock ritual: bursting max-budget aligned presents (raster lock uncertain)");
                    lastRelockLogUs = nowUs;
                }
            }
        }
        else {
            relockBurstRemaining = 0;
        }

        uint64_t alignBudgetUs;
        if ((alignTapered || cadenceColdLatch) && !nearBuffered) {
            // Content near or above this panel's true tear-free flip
            // ceiling (empirically ~110fps on the 120Hz Ally X panel, i.e.
            // ~750us above the nominal max-refresh spacing) has no tear-free
            // operating point for tearing-allowed presents - they can only
            // choose where tears land - and every phase-anchoring scheme
            // tried here traded tears against flip-law chatter (the queue
            // equilibrium straddles the stale threshold once alignment waits
            // stop inflating service time, and the pacer flip-flops between
            // rush and snap present timing in 2-8Hz bursts). Classic
            // vblank-latched presentation is simply better up here: ask the
            // renderer to present WITHOUT the tearing flag so every flip
            // latches tear-free at the display's next vblank, while cadence
            // pacing still spaces the presents at the content rate. This is
            // the fixed-vsync feel the user validated as clearly smoother at
            // 116-on-120; the moment measured content falls back below the
            // ceiling (hysteresis above), tearing presents and true VRR
            // pacing resume. The low-latency VRR settings checkbox (and
            // MOONLIGHT_VRR_NO_LATCH=1 for A/B) opts into tear-and-snap
            // instead: immediate flips shave a few ms of display latency at
            // the cost of visible tearing.
            alignBudgetUs = vsyncLatchPresent ? 0 : alignSpinFloorUs;
        }
        else if (relockBurstRemaining > 0) {
            // Ritual presents take the full-scanout budget even when the
            // rush/stale machinery wants them out fast: a blind rush flip
            // mid-ritual resets the renderer's alignment streak and wastes
            // every wait the ritual already paid for. The rush targeting
            // (floor-spaced catch-up rebase) still applies - only the
            // wait-for-blank budget is protected here.
            relockBurstRemaining--;
            alignBudgetUs = minFrameIntervalUs + alignWideExtraUs;
        }
        else if (rushPresent) {
            // A catch-up present may only spend the cadence's real per-frame
            // slack (content interval over the panel's max flip spacing), so
            // with headroom the drain runs a shade slower but stays aligned
            // and a jitter storm of rush presents can no longer break the
            // panel's VRR lock.
            //
            // Floored rather than allowed to starve to zero: a heavy stream
            // (render time near the content interval) zeroes threadSlackUs,
            // and a zero-budget rush present goes out blind. With catch-up
            // spacing held at the flip ceiling the blank is due almost
            // immediately, so a sub-ms wait usually converts a guaranteed
            // tear into an aligned flip; 600us is ~5% of an interval and
            // cannot compound lateness into drops the way an unbounded wait
            // would. (Content below the taper threshold has cadenceSlackUs
            // >=~1.1ms, so there the floor only overrides render-bound
            // starvation; in the near-ceiling buffered band the slack
            // itself shrinks toward zero at max refresh and the floor is
            // load-bearing - the standing buffer absorbs its cost.)
            alignBudgetUs = qMin(qMin(cadenceSlackUs, threadSlackUs),
                                 rushBudgetCapUs);
            if (!classicRecovery &&
                    (nearBuffered || !me->m_VrrTearingPreferred) &&
                    alignBudgetUs < rushAlignFloorUs) {
                // In-band the floor applies even for tearing-preferred
                // users: one blind rush flip can cost the raster lock, and
                // re-earning it costs far more latency than 600us.
                alignBudgetUs = rushAlignFloorUs;
            }
        }
        else if (nearBuffered) {
            // Buffered band, lock held (or ritual on cooldown): while the
            // panel is flip-following, the blank arrives ON our schedule
            // and the wait measures ~0, so the budget only needs to cover
            // measurement noise - and it must never exceed the cadence's
            // true per-frame slack, or alignment waits eat the buffer's
            // absorb margin (the old free-run collapse in exactly this
            // band). Re-entry from a lost lock is the re-lock ritual's
            // job, armed above.
            alignBudgetUs = qMax(qMin(cadenceSlackUs, rushBudgetCapUs),
                                 rushAlignFloorUs);
        }
        else {
            // Real headroom: floor at the fixed 3ms spin that reached
            // almost-tear-free on real hardware, and with an idle pipeline
            // allow up to one full scanout cycle plus slack - the blanking
            // gap recurs within one cycle even on a free-running raster, so
            // that width guarantees re-capturing the panel's flip lock.
            // While a backlog exists, cap at the 3ms floor: the wide waits
            // are only for re-anchoring a free-running raster, and the
            // render-time estimate this thread-slack math leans on is
            // clamped to one frame interval, so during a genuine overload
            // (render time past the interval) the slack is overestimated
            // and wide waits would deepen the drop cascade.
            uint64_t maxAlignUs = backlogged ?
                alignSpinFloorUs : qMax(alignSpinFloorUs,
                                        minFrameIntervalUs + alignWideExtraUs);
            alignBudgetUs = qBound(alignSpinFloorUs, threadSlackUs, maxAlignUs);

            // Forensics-driven re-anchor: while the renderer cannot prove
            // the panel is back in VRR flip-following, every floor-spaced
            // present lands at the free-running raster's whim - and at
            // heavy render loads threadSlackUs starves this budget to its
            // 3ms floor, which cannot reach a blank up to a full scanout
            // away. Measured (2026-07-03 forensics) as the DOMINANT
            // residual tear population: first as chains of late~0 /
            // gap~9.1ms give-ups after each trigger tear, then - once a
            // one-shot re-anchor broke the chains - as an alternating
            // tear/catch pattern whenever the raster ran fixed-cadence.
            // Keep the full-scanout budget until the lock is demonstrated;
            // it costs nothing while the panel is actually following.
            // While backlogged it is RATIONED rather than suppressed: the
            // old all-or-nothing gate let a jitter-burst backlog pin every
            // present at the 3ms floor for its whole duration, and with the
            // raster free-running that was measured (2026-07-04, 90-94fps
            // content) as 20-30% tear windows - chains of floor-spaced
            // flips crawling the tear line for hundreds of frames. One wide
            // wait per second breaks the chain at a bounded cost (~8ms of
            // extra queue, drained in a few frames of cadence surplus),
            // where waiting wide on EVERY backlogged present really would
            // deepen the overload.
            if (!classicRecovery &&
                    me->m_VsyncRenderer->isVrrRasterLockUncertain()) {
                if (!backlogged) {
                    alignBudgetUs = maxAlignUs;
                }
                else if (nowUs - lastWideReanchorUs > 1000000ULL) {
                    alignBudgetUs = qMax(alignSpinFloorUs,
                                         minFrameIntervalUs + alignWideExtraUs);
                    lastWideReanchorUs = nowUs;
                }
            }
        }

        // The frame is committed to presentation from here on - hand it to
        // the renderer before sleeping so GPU-heavy renderers can overlap
        // their rendering with the wait (the flip itself is still held to
        // targetUs by the presenter).
        me->m_VsyncRenderer->prepareFrameForPresent(frame);

        me->waitUntil(targetRenderStartUs);

        if (me->m_Stopping) {
            av_frame_free(&frame);
            break;
        }

        me->m_VsyncRenderer->setPresentTargetUs(targetUs, rushPresent, alignBudgetUs,
                                                vsyncLatchPresent, nearBuffered);
        me->m_VsyncRenderer->waitToRender();
        me->renderFrame(frame);

        // In-band tear-rate probe (see the feedback state above). Counted
        // only for non-latched in-band presents, and any regime change
        // resets the window so a rate is never judged across a boundary.
        // 15% cleanly separates the measured populations: a working band
        // runs 0.2-2.6% torn, a non-following raster 40-95%.
        //
        // The probe itself is the only tearing this regime shows the user,
        // so it is kept as short and rare as the statistics allow:
        //  - Early abort: 16 tears is already conclusive against a <3%
        //    working band, so the probe fails the moment it accumulates
        //    them (>=24 presents so a single burst can't decide alone) -
        //    ~0.2s at the measured 70-90% failure rates instead of the
        //    full 256-present window.
        //  - Exponential backoff: consecutive failures at the same cadence
        //    double the latch period (60s up to 8min), so steady-state
        //    probe cost falls to ~0.1% of wall time. A passing probe or a
        //    genuine content-rate change (>600us from the failing
        //    interval) resets the period.
        uint32_t midScanTears = me->m_VsyncRenderer->popMidScanTearCount();
        if (nearBuffered && !vsyncLatchPresent) {
            bandTearWindowPresents++;
            bandTearWindowTears += midScanTears;

            bool probeFailed = false;
            if (bandTearWindowPresents >= 24 && bandTearWindowTears >= 16) {
                probeFailed = true;
            }
            else if (bandTearWindowPresents >= 256) {
                probeFailed =
                    bandTearWindowTears * 100 >= bandTearWindowPresents * 15;
                if (!probeFailed) {
                    // The band demonstrably works at this rate right now;
                    // decay any stored verdict for it one rung.
                    calibration.recordTearPass(measuredSourceIntervalUs);
                    bandTearWindowPresents = 0;
                    bandTearWindowTears = 0;
                }
            }

            if (probeFailed) {
                if (latchAvailable) {
                    // Each content rate climbs its own ladder in the
                    // calibration store, resuming from wherever earlier
                    // streams left it.
                    uint32_t fallbackSecs = calibration.recordTearFail(
                        measuredSourceIntervalUs, nowUs,
                        me->m_EstimatedRenderTimeUs);
                    bandTearFallbackUntilUs = nowUs +
                        (uint64_t)fallbackSecs * 1000000ULL;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR tear-rate fallback: %u of %u in-band presents tore mid-scan at %.1f fps measured; vsync-latching this content for %us",
                                bandTearWindowTears,
                                bandTearWindowPresents,
                                measuredSourceIntervalUs > 0 ?
                                    1000000.0 / measuredSourceIntervalUs : 0.0,
                                fallbackSecs);
                }
                bandTearWindowPresents = 0;
                bandTearWindowTears = 0;
            }
        }
        else if (bandTearWindowPresents != 0) {
            bandTearWindowPresents = 0;
            bandTearWindowTears = 0;
        }

        if (adaptiveMargin) {
            int64_t overshootUs64 =
                (int64_t)me->m_LastNetRenderTimeUs - (int64_t)schedEstUs;
            if (overshootUs64 > INT32_MAX) {
                overshootUs64 = INT32_MAX;
            }
            else if (overshootUs64 < INT32_MIN) {
                overshootUs64 = INT32_MIN;
            }
            overshootRing[overshootHead] = (int32_t)overshootUs64;
            overshootHead = (overshootHead + 1) % overshootWindowCap;
            if (overshootCount < overshootWindowCap) {
                overshootCount++;
            }

            int32_t windowMaxUs = INT32_MIN;
            for (int i = 0; i < overshootCount; i++) {
                windowMaxUs = qMax(windowMaxUs, overshootRing[i]);
            }

            renderTailBeyondMarginUs = 0;
            if (windowMaxUs > 0 &&
                    (uint64_t)windowMaxUs + marginSlackUs > marginCeilUs) {
                renderTailBeyondMarginUs =
                    (uint64_t)windowMaxUs + marginSlackUs - marginCeilUs;
            }

            uint64_t targetMarginUs = windowMaxUs > 0 ?
                qBound(marginFloorUs,
                       (uint64_t)windowMaxUs + marginSlackUs,
                       marginCeilUs) :
                marginFloorUs;
            if (targetMarginUs > leadMarginUs) {
                // A render just ran longer than the margin planned for -
                // protect the very next frame rather than averaging in the
                // spike over time. Stutter costs more than latency here.
                leadMarginUs = targetMarginUs;
            }
            else if (leadMarginUs > targetMarginUs) {
                leadMarginUs -= qMin(leadMarginUs - targetMarginUs,
                                     marginGlideUs);
            }

            // Feed the margin into the calibration store under the current
            // content-rate band - each band's average becomes that band's
            // starting point in later sessions.
            calibration.noteLeadMargin(leadMarginUs, marginBandFps);

            uint64_t marginDeltaUs = leadMarginUs > lastLoggedMarginUs ?
                leadMarginUs - lastLoggedMarginUs :
                lastLoggedMarginUs - leadMarginUs;
            if (marginDeltaUs > 750) {
                uint64_t logNowUs = LiGetMicroseconds();
                if (logNowUs - lastMarginLogUs > 30000000ULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR lead margin: %.2f ms (worst render overshoot %.2f ms in window)",
                                leadMarginUs / 1000.0,
                                windowMaxUs > 0 ? windowMaxUs / 1000.0 : 0.0);
                    lastMarginLogUs = logNowUs;
                    lastLoggedMarginUs = leadMarginUs;
                }
            }
        }
    }

    calibration.saveOnExit(me->m_EstimatedRenderTimeUs);

    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        // Acquire the frame queue lock to protect the queue and
        // the not empty condition
        me->m_FrameQueueLock.lock();

        // Wait for a frame to be ready to render
        while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
            me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            // Exit this thread
            me->m_FrameQueueLock.unlock();
            break;
        }

        RenderQueueEntry entry = me->m_RenderQueue.dequeue();
        me->m_FrameQueueLock.unlock();

        if (entry.targetPresentUs != 0) {
            uint64_t targetRenderStartUs =
                entry.targetPresentUs > me->m_EstimatedRenderTimeUs ?
                    entry.targetPresentUs - me->m_EstimatedRenderTimeUs :
                    entry.targetPresentUs;

            if (!me->waitUntil(targetRenderStartUs)) {
                av_frame_free(&entry.frame);
                break;
            }
        }

        me->renderFrame(entry.frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame* frame, uint64_t targetPresentUs)
{
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue({ frame, targetPresentUs });

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : std::as_const(m_PacingQueueHistory)) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        maybeLogFrameDiagnostics("pacing queue drop", 0);
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS) - TIMER_SLACK_MS)) {
            // Wait timed out - unlock and bail
            m_FrameQueueLock.unlock();
            return;
        }

        if (m_Stopping) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, bool enableVrrTearing, int vrrCushionUs)
{
    m_MaxVideoFps = maxVideoFps;
    // A tearing preference only means something when the renderer can
    // actually present with tearing. On renderers whose presents all latch
    // at a vblank anyway (plvk's FIFO VrrCadence path), honoring it would
    // strip the pacer's vsync-latch fallback and flip-spacing slack - all
    // of tearing-preferred pacing's cost with none of its latency benefit
    // (observed 2026-07-05: 100fps content flipping at the panel ceiling
    // on every catch-up because vrrtearing=true removed the +750us floor).
    m_VrrTearingPreferred = enableVrrTearing &&
        !m_VsyncRenderer->arePresentsVsyncLatched();
    if (vrrCushionUs > 0) {
        m_VrrCushionUs = vrrCushionUs;
    }
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();
    m_PresentationMode = m_VsyncRenderer->getPresentationMode();

    if (m_PresentationMode == IFFmpegRenderer::PresentationMode::Auto) {
        m_PresentationMode = enablePacing ?
            IFFmpegRenderer::PresentationMode::FixedVsync :
            IFFmpegRenderer::PresentationMode::Immediate;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Presentation mode: %s",
                IFFmpegRenderer::getPresentationModeName(m_PresentationMode));

    if (enablePacing && m_PresentationMode == IFFmpegRenderer::PresentationMode::FixedVsync) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            m_VsyncSource = new DxVsyncSource(this);
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else if (m_PresentationMode != IFFmpegRenderer::PresentationMode::VrrCadence) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }
    else if (m_PresentationMode == IFFmpegRenderer::PresentationMode::VrrCadence) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR cadence pacing: direct render target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        // Identity for the cross-session calibration cache: the tear-rate
        // verdicts and typical lead margin are properties of the panel, its
        // refresh mode, and the stream's rate cap. Stream resolution also
        // influences the lead margin, but it isn't known here - the margin
        // seed is only a starting point the adaptive loop corrects within
        // seconds, so the coarser key is acceptable.
        int displayIndex = SDL_GetWindowDisplayIndex(window);
        const char* displayName = displayIndex >= 0 ?
            SDL_GetDisplayName(displayIndex) : nullptr;
        m_VrrCalibrationKey = QString(QStringLiteral("%1|%2Hz|%3fps"))
            .arg(displayName != nullptr ?
                 QString::fromUtf8(displayName) : QStringLiteral("display"))
            .arg(m_DisplayFps)
            .arg(m_MaxVideoFps);
        m_VrrCalibrationKey.replace(QLatin1Char('/'), QLatin1Char('_'));
        m_VrrCalibrationKey.replace(QLatin1Char('\\'), QLatin1Char('_'));

        m_VsyncThread = SDL_CreateThread(Pacer::cadenceThread, "PacerCadence", this);
    }

    if (m_VsyncRenderer->isRenderThreadSupported() &&
            m_PresentationMode != IFFmpegRenderer::PresentationMode::VrrCadence) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    uint64_t beforeRender = LiGetMicroseconds();
    m_VideoStats->totalPacerTimeUs += (beforeRender - (uint64_t)frame->pkt_dts);

    // Render it
    m_VsyncRenderer->renderFrame(frame);
    uint64_t afterRender = LiGetMicroseconds();

    m_VideoStats->totalRenderTimeUs += (afterRender - beforeRender);
    m_VideoStats->renderedFrames++;
    recordFrameInterval(beforeRender, afterRender, beforeRender - (uint64_t)frame->pkt_dts);

    uint64_t renderTimeUs = afterRender - beforeRender;

    // Don't let the renderer's phase-alignment wait (idling for the panel's
    // blanking gap before the present) count as render work. Feeding it back
    // into the estimate would start each render earlier, which just lengthens
    // the alignment wait it's measuring - drifting latency up instead of
    // converging on the true render lead time.
    uint64_t alignmentWaitUs = m_VsyncRenderer->popPresentAlignmentWaitUs();
    renderTimeUs -= qMin(renderTimeUs, alignmentWaitUs);

    // Unclamped net render time for the cadence thread's adaptive lead
    // margin - the interval clamp below is right for the EMA (a genuine
    // overload shouldn't drag the schedule a whole frame early) but would
    // hide exactly the spikes the margin must cover.
    m_LastNetRenderTimeUs = renderTimeUs;

    uint64_t maxEstimateUs = m_MaxVideoFps != 0 ? 1000000ULL / m_MaxVideoFps : 16666ULL;
    renderTimeUs = qMin(renderTimeUs, maxEstimateUs);
    m_EstimatedRenderTimeUs = (m_EstimatedRenderTimeUs * 7 + renderTimeUs) / 8;

    // Wait until after next frame to free this one to ensure the GPU
    // doesn't stall or read garbage if the backing buffer gets returned
    // to the pool and the decoder tries to write a new frame into it
    std::swap(frame, m_DeferredFreeFrame);
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    m_FrameQueueLock.lock();

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : std::as_const(m_RenderQueueHistory)) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        RenderQueueEntry entry = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        maybeLogFrameDiagnostics("render queue drop", 0);
        av_frame_free(&entry.frame);
        m_FrameQueueLock.lock();
    }

    m_FrameQueueLock.unlock();
}

bool Pacer::waitUntil(uint64_t targetUs)
{
    return waitForVrrCadenceTargetUs(targetUs,
                                     []() { return LiGetMicroseconds(); },
                                     [](uint64_t sleepUntilUs) { HighResSleep::sleepUntilUs(sleepUntilUs); },
                                     []() { SDL_Delay(0); },
                                     [this]() { return m_Stopping; });
}

void Pacer::recordFrameInterval(uint64_t beforeRenderUs, uint64_t afterRenderUs, uint64_t queueDelayUs)
{
    uint32_t intervalUs = 0;

    if (m_FirstRenderTimeUs == 0) {
        m_FirstRenderTimeUs = afterRenderUs;
    }
    bool startupWarmup = afterRenderUs < m_FirstRenderTimeUs + STARTUP_WARMUP_PERIOD_US;

    if (m_LastRenderTimeUs != 0) {
        uint64_t intervalUs64 = afterRenderUs - m_LastRenderTimeUs;

        if (intervalUs64 <= MAX_RECORDED_FRAME_INTERVAL_US) {
            intervalUs = (uint32_t)intervalUs64;

            if (!startupWarmup) {
                m_VideoStats->totalFrameIntervalUs += intervalUs;
                m_VideoStats->totalSquaredFrameIntervalUs += (uint64_t)intervalUs * intervalUs;
                m_VideoStats->frameIntervalSamples++;

                if (m_VideoStats->minFrameIntervalUs == 0) {
                    m_VideoStats->minFrameIntervalUs = intervalUs;
                }
                else {
                    m_VideoStats->minFrameIntervalUs = qMin(m_VideoStats->minFrameIntervalUs, intervalUs);
                }

                m_VideoStats->maxFrameIntervalUs = qMax(m_VideoStats->maxFrameIntervalUs, intervalUs);
            }
        }
    }

    m_LastRenderTimeUs = afterRenderUs;

    FrameDiagnosticSample& sample = m_FrameDiagnosticRing[m_FrameDiagnosticRingIndex];
    sample.intervalUs = intervalUs;
    sample.queueDelayUs = queueDelayUs <= UINT32_MAX ? (uint32_t)queueDelayUs : UINT32_MAX;
    sample.renderUs = (uint32_t)(afterRenderUs - beforeRenderUs);

    m_FrameDiagnosticRingIndex = (m_FrameDiagnosticRingIndex + 1) % PACER_FRAME_DIAGNOSTIC_RING_SIZE;
    m_FrameDiagnosticRingCount = qMin<uint32_t>(m_FrameDiagnosticRingCount + 1, PACER_FRAME_DIAGNOSTIC_RING_SIZE);

    if (intervalUs != 0 && !startupWarmup) {
        uint32_t expectedIntervalUs = m_MaxVideoFps != 0 ? 1000000 / m_MaxVideoFps : 0;
        uint32_t longIntervalThresholdUs = qMax(expectedIntervalUs * 5 / 2, expectedIntervalUs + 20000);
        uint32_t shortIntervalThresholdUs = expectedIntervalUs / 3;

        if (expectedIntervalUs != 0 && intervalUs >= longIntervalThresholdUs) {
            maybeLogFrameDiagnostics("long render interval", intervalUs);
        }
        else if (shortIntervalThresholdUs != 0 && intervalUs <= shortIntervalThresholdUs) {
            maybeLogFrameDiagnostics("short render interval", intervalUs);
        }
    }
}

void Pacer::maybeLogFrameDiagnostics(const char* reason, uint32_t intervalUs)
{
    uint64_t now = LiGetMicroseconds();

    if (m_LastFrameDiagnosticDumpUs != 0 &&
            now <= m_LastFrameDiagnosticDumpUs + FRAME_DIAGNOSTIC_DUMP_INTERVAL_US) {
        return;
    }

    logFrameDiagnostics(reason, intervalUs);
    m_LastFrameDiagnosticDumpUs = now;
}

void Pacer::logFrameDiagnostics(const char* reason, uint32_t triggerIntervalUs)
{
    if (m_FrameDiagnosticRingCount == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame cadence anomaly: %s, no frame-level samples available yet",
                    reason);
        return;
    }

    char samples[1536];
    int offset = 0;
    uint32_t count = qMin<uint32_t>(m_FrameDiagnosticRingCount, FRAME_DIAGNOSTIC_DUMP_SAMPLES);
    uint32_t start = (m_FrameDiagnosticRingIndex + PACER_FRAME_DIAGNOSTIC_RING_SIZE - count) %
            PACER_FRAME_DIAGNOSTIC_RING_SIZE;

    samples[0] = 0;
    for (uint32_t i = 0; i < count; i++) {
        const FrameDiagnosticSample& sample =
                m_FrameDiagnosticRing[(start + i) % PACER_FRAME_DIAGNOSTIC_RING_SIZE];
        int ret = snprintf(&samples[offset],
                           sizeof(samples) - offset,
                           "%s%.2f/%.2f/%.2f",
                           i != 0 ? "," : "",
                           sample.intervalUs / 1000.0,
                           sample.queueDelayUs / 1000.0,
                           sample.renderUs / 1000.0);
        if (ret < 0 || ret >= (int)sizeof(samples) - offset) {
            break;
        }

        offset += ret;
    }

    if (triggerIntervalUs != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame cadence anomaly: %s at %.2f ms. Recent frames interval/queue/render ms: %s",
                    reason,
                    triggerIntervalUs / 1000.0,
                    samples);
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame cadence anomaly: %s. Recent frames interval/queue/render ms: %s",
                    reason,
                    samples);
    }
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        av_frame_free(&frame);
    }
}

void Pacer::dropFrameForEnqueue(QQueue<RenderQueueEntry>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        RenderQueueEntry entry = queue.dequeue();
        av_frame_free(&entry.frame);
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr ||
            m_PresentationMode == IFFmpegRenderer::PresentationMode::VrrCadence) {
        dropFrameForEnqueue(m_PacingQueue);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}
