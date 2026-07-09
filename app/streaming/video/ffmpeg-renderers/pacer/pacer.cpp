#include "pacer.h"
#include "vrrcadence.h"
#include "vrrqueueage.h"
#include "highressleep.h"
#include "streaming/streamutils.h"

#include <QVector>

#include <array>

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

// Per-stream VRR calibration. The cadence thread still remembers failed
// near-ceiling tear probes during a stream, so a rate that has already proven
// bad can be vsync-latched for a while instead of tearing continuously. That
// state deliberately dies with the stream: persisted calibration proved too
// easy to poison across renderer, resolution, power, and workload changes,
// while the live adaptive loops re-converge quickly from their defaults.
struct VrrTearVerdict {
    uint32_t intervalUs;     // measured content interval that failed (fixed at creation)
    uint32_t periodSecs;     // latch period last applied for this rate
    uint32_t failCount;
    uint64_t lastSeenUs;
    uint32_t renderStampUs;  // net render estimate when this rate last failed (0 = unknown)
    bool latchedThisSession; // one probe-free pre-latch per stream (not persisted)
};

class VrrCalibrationStore
{
public:
    static constexpr uint32_t kMatchToleranceUs = 600;   // same rate-identity window as the old single-slot ladder
    static constexpr uint32_t kBasePeriodSecs = 60;
    static constexpr uint32_t kMaxPeriodSecs = 480;
    static constexpr uint32_t kChronicPeriodSecs = 240;  // pre-latch without a probe at or above this

    VrrCalibrationStore() = default;

    VrrTearVerdict* findVerdict(uint64_t intervalUs)
    {
        int i = findIndex(intervalUs);
        return i >= 0 ? &m_Verdicts[i] : nullptr;
    }

    // True when the live renderer is materially faster than the regime that
    // earned a failed tear verdict. A zero stamp means no failure has stamped
    // this verdict yet, so any pre-latch should use only the base rung before
    // re-verifying.
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
    // the next rung of this rate's own per-stream ladder (base for a first
    // offense). The render stamp refreshes on every fail: a rate that still
    // tears under the current renderer is current evidence, so a faster
    // render does not restart its ladder during the same stream.
    uint32_t recordTearFail(uint64_t intervalUs, uint64_t nowUs,
                            uint64_t renderEstUs)
    {
        uint32_t periodSecs = kBasePeriodSecs;
        int i = findIndex(intervalUs);
        if (i >= 0) {
            periodSecs = qMin(m_Verdicts[i].periodSecs * 2, kMaxPeriodSecs);
            m_Verdicts[i].periodSecs = periodSecs;
            m_Verdicts[i].failCount++;
            m_Verdicts[i].lastSeenUs = nowUs;
            m_Verdicts[i].renderStampUs = (uint32_t)renderEstUs;
            m_Verdicts[i].latchedThisSession = true;
        }
        else {
            if (m_Verdicts.count() >= kVerdictCap) {
                int oldest = 0;
                for (int j = 1; j < m_Verdicts.count(); j++) {
                    if (m_Verdicts[j].lastSeenUs < m_Verdicts[oldest].lastSeenUs) {
                        oldest = j;
                    }
                }
                m_Verdicts.removeAt(oldest);
            }
            VrrTearVerdict verdict = {};
            verdict.intervalUs = (uint32_t)intervalUs;
            verdict.periodSecs = periodSecs;
            verdict.failCount = 1;
            verdict.lastSeenUs = nowUs;
            verdict.renderStampUs = (uint32_t)renderEstUs;
            verdict.latchedThisSession = true;
            m_Verdicts.append(verdict);
        }
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
    }

private:
    static constexpr int kVerdictCap = 12;

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

    QVector<VrrTearVerdict> m_Verdicts;
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

    // Per-stream tear-rate ladder state. Owned by this thread alone.
    VrrCalibrationStore calibration;

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

    // Scope learned timing evidence to a content-rate regime. A material rate
    // jump invalidates both queue-age spread and render-overshoot history.
    int timingBandFps = 0;
    int timingBandCandidate = 0;
    uint64_t timingBandCandidateSinceUs = 0;
    uint64_t leadMarginUs =
        classicPresent ? 0 : (fixedMarginEnv > 1 ? (uint64_t)fixedMarginEnv : 4000);

    // Rolling ~500ms of pacing queue depth, mirroring the hysteresis the
    // handleVsync/renderFrame paths already use. Network jitter routinely
    // delivers frames in a gap-then-pair pattern; dropping the older frame of
    // every pair (the old zero-tolerance policy here) turned each burst into
    // a visible skip - measured at 3-8% of all frames on Wi-Fi.
    QQueue<int> queueDepthHistory;
    int queueDepthHistoryCap =
        VrrQueueAgeController::windowSampleCount(0, me->m_MaxVideoFps);

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

    // Closed-loop queue-age control. The UI selects how much measured timing
    // variation the policy may insure (2.5/4.5/6ms); it is not a fixed wait.
    // Near the refresh ceiling the controller also owns a bounded phase and
    // acquisition reserve. Initial acquisition is driven by actual age;
    // steady-state changes require window-level evidence so isolated source
    // gaps cannot trigger a rebuffer cycle. The controller never learns stalls
    // or recovery motion as normal. MOONLIGHT_VRR_NO_TRIM=1 disables all
    // queue-age feedback for A/B.
    const bool queueAgeServoEnabled =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_NO_TRIM") == 0;
    const int queueAgeEnv =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_CUSHION_US");
    const int queueAgeCfgUs = queueAgeEnv > 0 ? queueAgeEnv :
        (me->m_VrrCushionUs > 0 ? me->m_VrrCushionUs : 4500);
    const uint64_t configuredPolicyCapUs =
        qBound((uint64_t)1500, (uint64_t)queueAgeCfgUs, (uint64_t)6000);
    // MOONLIGHT_VRR_STATIC_CUSHION=1 keeps the conservative policy target.
    const bool forceStaticQueueTarget =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_STATIC_CUSHION") != 0;
    VrrQueueAgeController queueAgeController(configuredPolicyCapUs,
                                               forceStaticQueueTarget);
    int queueAgeWindowSamples = 0;
    bool queueAgeWindowTainted = false;
    uint64_t phaseAdvanceUs = 0;
    uint64_t phaseDelayUs = 0;
    uint64_t fastRecoveryRemainingUs = 0;
    uint64_t previousQueueAgeMedianUs = 0;
    bool queueAcquisitionPending = false;
    uint32_t lastObservedPacerDropCount =
        me->m_VideoStats->pacerDroppedFrames;
    int postDropRecoverySuppressFrames = 0;
    std::array<uint32_t, 120> queueAgeWindowUs = {};
    uint64_t lastQueueServoLogUs = 0;
    uint64_t lastQueueTargetLogUs = 0;
    uint64_t lastLoggedQueueTargetUs = 0;
    uint64_t lastQueueAcquisitionLogUs = 0;
    auto resetQueueAgeWindow = [&]() {
        queueAgeWindowSamples = 0;
        queueAgeWindowTainted = false;
        phaseAdvanceUs = 0;
        phaseDelayUs = 0;
        fastRecoveryRemainingUs = 0;
        previousQueueAgeMedianUs = 0;
    };

    // Scale-free pacing geometry. Every zone and guard below that used to
    // be a fixed microsecond offset is expressed as a fraction (per-mille)
    // of the panel's scanout period, anchored to the value it validated at
    // on the 8.33ms/120Hz reference panel - a fixed 1350us entry zone
    // means "16% of a scanout" there but would mean 32% on a 240Hz panel
    // and 8% at 60Hz, silently moving every behavioral boundary. Fractions
    // keep the zones meaning the same thing on any refresh rate.
    // Deliberately NOT scaled: human-latency quantities (the policy reserve
    // bounds, lead-margin caps, acquisition limits, jitter guards) - perceived
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
    const uint64_t queueAgeClampZoneUs = scanoutFracUs(312);    // 2600us
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
    // So instead of latching the band, decouple flip spacing from arrival
    // timing with a time-based queue-age target, keep the smoothed schedule
    // above nominal scanout plus a guard, and reserve catch-up for genuine
    // excess. Queue depth remains an overflow signal, not the latency target.
    // MOONLIGHT_VRR_NO_NEARBUFFER=1
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
    int relockFailureCount = 0;
    uint64_t lastRelockBurstUs = 0;
    uint64_t lastRelockLogUs = 0;
    bool overfillDropPending = false;
    uint64_t overfillDropDeadlineUs = 0;

    // Post-band-release grace (see the staleSchedule block). Sized to one
    // servo window so the trim machinery gets a full measurement cycle to
    // start draining before the stale/drain tiers may fire again.
    int bandStaleGraceFrames = 0;
    uint64_t lastNearQueueAgeTargetUs = 0;
    uint64_t lastNearBufferReleaseUs = 0;

    // Tear-rate feedback: the self-calibrating replacement for hardcoded
    // per-panel rate limits. Whether a given display/driver stack can
    // flip-follow near-ceiling free-run presents is not knowable a priori
    // (this panel proved unable above ~110fps; others may differ), so
    // measure it: count mid-scan presents across a rolling in-band probe
    // window, and when the observed tear rate proves chronic, hand this
    // content to the vsync latch for a while. Expiry re-probes, so a
    // regime change (different content rate, driver re-engaging) is found
    // within one probe window (at most 64 frames, about 0.5-0.7s here).
    // Only meaningful when the renderer has a non-tearing present path.
    // Standard policy uses the calibration ladder; low-latency tearing uses
    // a short fallback only after the probe measures a sustained storm.
    // The per-rate ladder (latch period doubling per repeat offense) lives
    // for this stream, so content-rate wobble cannot reset every returning
    // rate to a fresh 60s fallback as the old single-slot version did.
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

        // The overfill detector decides after rendering the preceding frame.
        // Service its request here, before advancing the next presentation
        // target, but only with an immediately available replacement. Feeding
        // the skipped timestamp to the cadence clock preserves source-rate
        // measurement while not advancing the target removes approximately
        // one source interval of parked phase latency.
        if (overfillDropPending &&
                LiGetMicroseconds() <= overfillDropDeadlineUs &&
                me->m_PacingQueue.count() >= 2) {
            AVFrame* overfillFrame = me->m_PacingQueue.dequeue();
            me->m_FrameQueueLock.unlock();
            cadenceClock.observeSourceTime(
                frameCadenceTimestampUs(overfillFrame));
            me->m_VideoStats->pacerDroppedFrames++;
            av_frame_free(&overfillFrame);
            me->maybeLogFrameDiagnostics("vrr buffer overfill drop", 0);
            overfillDropPending = false;
            overfillDropDeadlineUs = 0;
            // A coalescing drop is a one-interval phase discontinuity. Start
            // a fresh distribution and cancel the correction that diagnosed
            // it rather than trimming the same excess twice.
            resetQueueAgeWindow();
            continue;
        }
        // Without an immediate replacement, discard the old overfill request.
        // Keep the next frame instead of turning a transient depth change into
        // an additional visible skip.
        overfillDropPending = false;
        overfillDropDeadlineUs = 0;

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
        // Queue age, not band membership, determines whether a successor is
        // useful. Tolerate one transient successor in every mode, but never
        // let near-ceiling pacing turn it into a permanent extra frame.
        constexpr int steadyDepth = 1;
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

        bool cadenceQueueDropped = false;
        while (me->m_PacingQueue.count() > frameDropTarget) {
            AVFrame* staleFrame = me->m_PacingQueue.dequeue();
            me->m_FrameQueueLock.unlock();
            // The clock must see dropped frames' timestamps too or sustained
            // drops inflate the measured cadence - see observeSourceTime().
            cadenceClock.observeSourceTime(frameCadenceTimestampUs(staleFrame));
            me->m_VideoStats->pacerDroppedFrames++;
            me->maybeLogFrameDiagnostics("vrr cadence queue drop", 0);
            av_frame_free(&staleFrame);
            cadenceQueueDropped = true;
            me->m_FrameQueueLock.lock();
        }
        if (cadenceQueueDropped) {
            resetQueueAgeWindow();
            overfillDropPending = false;
            overfillDropDeadlineUs = 0;
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
        queueDepthHistoryCap = VrrQueueAgeController::windowSampleCount(
            measuredSourceIntervalUs, me->m_MaxVideoFps);

        if (cadenceClock.warmedUp() && measuredSourceIntervalUs != 0) {
            int fpsNow = (int)(1000000ULL / measuredSourceIntervalUs);
            int band = qBound(10, ((fpsNow + 5) / 10) * 10, 240);
            if (band == timingBandFps) {
                timingBandCandidate = 0;
            }
            else if (band != timingBandCandidate) {
                timingBandCandidate = band;
                timingBandCandidateSinceUs = nowUs;
            }
            else if (nowUs - timingBandCandidateSinceUs > 700000ULL) {
                int prevBand = timingBandFps;
                timingBandFps = band;
                timingBandCandidate = 0;

                if (prevBand != 0 && qAbs(band - prevBand) >= 20) {
                    resetQueueAgeWindow();
                    queueAgeController.resetLearning();
                    relockFailureCount = 0;
                    lastRelockBurstUs = 0;
                    if (nearBuffered) {
                        queueAgeController.enterNearCeiling(
                            measuredSourceIntervalUs);
                        queueAcquisitionPending = true;
                        lastNearBufferReleaseUs = 0;
                    }
                    else {
                        queueAgeController.leaveNearCeiling();
                        queueAcquisitionPending = false;
                        lastNearBufferReleaseUs = 0;
                    }
                    if (adaptiveMargin) {
                        overshootHead = 0;
                        overshootCount = 0;
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

        bool latchSupported =
            qEnvironmentVariableIntValue("MOONLIGHT_VRR_NO_LATCH") == 0;
        bool preferenceLatchAvailable = !me->m_VrrTearingPreferred &&
            latchSupported;

        // Cadence-cold grace: the clock's sample window restarts on any real
        // stall (stream bring-up, loading screens, entering a game - all the
        // places content cadence genuinely ramps), and until it refills
        // (~0.5s of monotonic timestamps) the measured interval is a warmup
        // EMA that free-run alignment can't be trusted to schedule against.
        // The measured early-session tear storms (11-31% tear windows, 41%
        // of one session's total tears inside the first ~35s) all sat in
        // these cold spans. Latch tear-free until the window is warm:
        // content is chaotic at those moments anyway, so the vsync
        // quantization is preferable to an unstable tear burst and the cost
        // lasts under a second per stall.
        bool cadenceColdLatch = preferenceLatchAvailable && !classicRecovery &&
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
        // Deliberately NOT gated on the user's latch preference: tearing-preferred /
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
        // "engaged at 23.8 fps" - the old interval-sized target became a
        // 42ms standing delay, pure latency).
        // Entry needs the near-ceiling zone proper; the hold releases
        // instantly once content is clearly slower than ~92fps, where full
        // free-run has abundant slack and needs no buffer.
        uint64_t bufferFloorIntervalUs = minFrameIntervalUs + bufferGuardUs;
        bool pastFastEdge = preferenceLatchAvailable &&
            measuredSourceIntervalUs < bufferFloorIntervalUs;
        bool clearlySlowerThanBand =
            measuredSourceIntervalUs >= minFrameIntervalUs + bandSlowReleaseZoneUs;
        bool tearRateFallback = latchSupported &&
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
            // re-engaged at 104-108 every 5-20s) repeatedly discarded the
            // controller's evidence and rebuilt protection, creating a
            // standing-latency sawtooth. The leaky counter (~0.35s at these
            // rates,
            // content-relative so it scales with any panel/rate) rides out
            // boundary wobble; a genuine slowdown still releases instantly
            // via clearlySlowerThanBand above, so slow content never pays
            // the in-band queue target for long.
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
                    (!preferenceLatchAvailable ||
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

        // A source/render stall invalidates the current statistics and phase
        // correction. Do not reacquire a standing reserve here: a gap longer
        // than the reserve cannot be hidden, and rebuilding after every gap
        // creates a drop/rebuffer cycle on genuinely variable content.
        if (clockPhaseReset) {
            resetQueueAgeWindow();
            if (nearBuffered) {
                queueAcquisitionPending = false;
            }
            else {
                queueAgeController.leaveNearCeiling();
                queueAcquisitionPending = false;
                lastNearBufferReleaseUs = 0;
            }
        }

        if (nearBuffered != prevNearBuffered) {
            resetQueueAgeWindow();
            if (nearBuffered) {
                bool resumeRecentState = lastNearBufferReleaseUs != 0 &&
                    nowUs - lastNearBufferReleaseUs <= 5000000ULL;
                if (!resumeRecentState) {
                    // A genuinely fresh regime gets an immediate lock attempt.
                    // Brief band-edge wobble retains failed-attempt backoff.
                    relockFailureCount = 0;
                    lastRelockBurstUs = 0;
                    queueAgeController.enterNearCeiling(
                        measuredSourceIntervalUs);
                }
                queueAcquisitionPending = !resumeRecentState;
                lastNearBufferReleaseUs = 0;
            }
            else {
                // Band-edge rate wobble is not a new timing regime. Retain
                // near-ceiling recovery/safety progress for five seconds so
                // a quick re-entry resumes convergence. Learned spread can
                // still update out of band; a long absence, phase reset, or
                // material rate change forces reacquisition above.
                lastNearBufferReleaseUs = clockPhaseReset ? 0 : nowUs;
                phaseDelayUs = 0;
                queueAcquisitionPending = false;
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

        // Cached tear verdict: when earlier probes in this stream proved this rate
        // chronically tears in-band on this display (ladder at or past the
        // chronic rung), latch immediately instead of paying the probe's
        // visible tear burst to rediscover it. One shot per rate per stream,
        // and only ever skips the probe - the fallback expiry still re-probes,
        // so an improved regime (driver update, different stack behavior) is
        // found within one latch period and the pass decay unwinds the
        // verdict from there.
        if (nearBuffered && preferenceLatchAvailable &&
                nowUs >= bandTearFallbackUntilUs) {
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
            (preferenceLatchAvailable || tearRateFallback);

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

        // One explicit total queue-age target feeds the servo and every
        // stale/overfill threshold. Render-call overshoot is owned by the
        // lead-margin controller; adding it here delayed render start without
        // increasing render lead and double-counted present backpressure.
        VrrQueueAgeController::Target queueTarget = queueAgeController.target(
            nearBuffered, fixedNearBufferTarget, measuredSourceIntervalUs,
            minFrameIntervalUs, scheduleGuardUs, queueAgeClampZoneUs);
        uint64_t targetQueueAgeUs = queueTarget.queueAgeUs;
        if (nearBuffered) {
            lastNearQueueAgeTargetUs = targetQueueAgeUs;
        }
        int queueAgeWindowSampleCap = VrrQueueAgeController::windowSampleCount(
            measuredSourceIntervalUs, me->m_MaxVideoFps);

        uint64_t queueTargetLogDeltaUs =
            lastLoggedQueueTargetUs > targetQueueAgeUs ?
                lastLoggedQueueTargetUs - targetQueueAgeUs :
                targetQueueAgeUs - lastLoggedQueueTargetUs;
        bool queueTargetRose = targetQueueAgeUs > lastLoggedQueueTargetUs;
        if (lastLoggedQueueTargetUs == 0 ||
                (queueTargetLogDeltaUs >= 500 &&
                 (queueTargetRose || nowUs - lastQueueTargetLogUs > 5000000ULL))) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "VRR queue target: %.2f ms (%s policy %.2f ms, phase %.2f ms, safety %.2f ms)",
                        targetQueueAgeUs / 1000.0,
                        queueTarget.learned ? "learned" : "conservative",
                        queueTarget.policyReserveUs / 1000.0,
                        queueTarget.phaseReserveUs / 1000.0,
                        queueTarget.safetyReserveUs / 1000.0);
            lastQueueTargetLogUs = nowUs;
            lastLoggedQueueTargetUs = targetQueueAgeUs;
        }

        // Detect decode-to-render age that is too old to be deliberate phase
        // reserve. Near ceiling, leave one source interval above the current
        // target so routine timing variation does not trigger catch-up.
        uint64_t staleAgeUs =
            measuredSourceIntervalUs + measuredSourceIntervalUs / 4;
        bool bandDrainGrace = false;
        if (nearBuffered) {
            // Keep recovery one source interval beyond the live target.
            // Anything younger is timing variation the queue is meant to
            // absorb; anything older is excess that should converge.
            staleAgeUs = qMax(staleAgeUs,
                              targetQueueAgeUs + measuredSourceIntervalUs);
            bandStaleGraceFrames = queueAgeWindowSampleCap;
        }
        else if (bandStaleGraceFrames > 0) {
            // The band just released with its deliberate queue-age reserve
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
                              qMax(targetQueueAgeUs,
                                   lastNearQueueAgeTargetUs) +
                                  measuredSourceIntervalUs);
            bandStaleGraceFrames--;
            bandDrainGrace = true;
            if (bandStaleGraceFrames == 0) {
                lastNearQueueAgeTargetUs = 0;
            }
        }
        bool staleSchedule = measuredSourceIntervalUs != 0 && frame->pkt_dts > 0 &&
            nowUs > (uint64_t)frame->pkt_dts + staleAgeUs;

        // Two-tier catch-up, rebasing the clock onto any instant actually
        // used so the schedule converges back to arrival phase instead of
        // staying permanently late.
        //
        // Genuinely stale (the 1.25-interval baseline out of band, or target
        // plus one interval near ceiling): rush and skip blank alignment; a
        // possible tear beats compounding lateness into dropped frames.
        //
        // Merely backlogged (a transient extra queued frame, routine with
        // network jitter): drain gently at ~12% tighter than the measured
        // content cadence with alignment still on. Draining at full panel
        // rate here would compress an 11.7ms content cadence to 8.3ms for
        // every absorbed burst - cadence distortion that reads as judder
        // during camera pans, far more visible than the latency it saves
        // (measured: ~45% of frames rushed at 85fps content on Wi-Fi).
        bool rushPresent = false;
        bool scheduleRecoveryRebased = false;
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
                    scheduleRecoveryRebased = true;
                }
                rushPresent = true;
            }
            else if ((nearBuffered || bandDrainGrace) ?
                         queuedBehindCount >= 2 : backlogged) {
                // Queue age controls latency. Queue depth is only a pressure
                // guard here: tolerate one transient successor while service
                // is saturated, but drain a persistent 2+ successor pile-up.
                uint64_t drainIntervalUs = qMax(flipSpacingFloorUs,
                                                measuredSourceIntervalUs * 7 / 8);
                uint64_t drainUs = qMax(lastFlipUs + drainIntervalUs,
                                        LiGetMicroseconds());
                if (drainUs < targetUs) {
                    targetUs = drainUs;
                    cadenceClock.rebaseTarget(targetUs);
                    scheduleRecoveryRebased = true;
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

        // Phase is closed on actual queue age. Robust window percentiles below
        // distinguish standing excess from isolated low/high samples; bounded
        // per-frame motion corrects it without turning ordinary variation into
        // catch-up judder. Stale or deeply backlogged frames taint learning.
        bool servoVeto = staleSchedule ||
            ((nearBuffered || bandDrainGrace) ?
                 queuedBehindCount >= 2 : backlogged);

        if (phaseAdvanceUs > 0) {
            if (VrrQueueAgeController::shouldDiscardPhaseAdvance(
                    staleSchedule, scheduleRecoveryRebased)) {
                // A catch-up/drain rebase moves phase discontinuously. Discard
                // the old trim and re-measure. Ordinary backlog no longer
                // vetoes trimming unless its drain actually moved the target.
                phaseAdvanceUs = 0;
            }
            else {
                uint64_t floorUs = nowUs;
                if (lastFlipUs != 0 && floorUs < lastFlipUs + flipSpacingFloorUs) {
                    floorUs = lastFlipUs + flipSpacingFloorUs;
                }
                uint64_t trimmedUs = targetUs > phaseAdvanceUs ?
                    targetUs - phaseAdvanceUs : floorUs;
                if (trimmedUs < floorUs) {
                    trimmedUs = floorUs;
                }
                if (trimmedUs < targetUs) {
                    targetUs = trimmedUs;
                    cadenceClock.rebaseTarget(targetUs);
                }
            }
        }
        else if (fastRecoveryRemainingUs > 0) {
            if (servoVeto || !nearBuffered) {
                fastRecoveryRemainingUs = 0;
            }
            else {
                uint64_t fastDelayStepUs = qMin(
                    fastRecoveryRemainingUs,
                    qBound((uint64_t)100,
                           measuredSourceIntervalUs / 32,
                           (uint64_t)300));
                targetUs += fastDelayStepUs;
                cadenceClock.rebaseTarget(targetUs);
                fastRecoveryRemainingUs -= fastDelayStepUs;
                queueAgeWindowTainted = true;
            }
        }
        else if (phaseDelayUs > 0) {
            if (servoVeto || !nearBuffered) {
                phaseDelayUs = 0;
            }
            else {
                targetUs += phaseDelayUs;
                cadenceClock.rebaseTarget(targetUs);
            }
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
        // full-scanout-budget presents. Failed bursts back off from 2s to 4s
        // and then 8s; repeating a known-failing wide wait every 2s consumed
        // render headroom without improving the measured tear rate. The burst
        // ends early and resets backoff the moment lock is demonstrated.
        //
        // Gated on real cadence slack (default >=800us, content up to
        // ~107fps): above that the pipeline was saturated - every present
        // ran late-past-target, the renderer's out-of-reach fast-give-up
        // fired before the ritual budget was ever spent, and the burst was
        // pure churn (measured 2026-07-04: rituals arming every 2s for
        // minutes at 114-116fps content, avg waits 0.1-0.3ms against 10ms
        // budgets, zero locks established).
        // Renderer changes invalidated the old saturation measurements, but
        // not the possibility that a panel refuses to flip-follow this close
        // to its ceiling. The env override keeps that gate testable.
        if (nearBuffered) {
            if (relockBurstRemaining > 0 &&
                    !me->m_VsyncRenderer->isVrrRasterLockUncertain()) {
                relockBurstRemaining = 0;
                relockFailureCount = 0;
            }
            else if (relockBurstRemaining == 0 &&
                     cadenceSlackUs >= ritualMinSlackUs &&
                     me->m_VsyncRenderer->isVrrRasterLockUncertain() &&
                     nowUs - lastRelockBurstUs >
                         (2000000ULL << qMin(relockFailureCount, 2))) {
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

        bool recoveryAlignmentPresent = cadenceColdLatch;
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
            // pacing resume. The low-latency VRR setting keeps immediate
            // recovery here, but its measured tear probe can still latch
            // briefly if this becomes a sustained storm.
            // MOONLIGHT_VRR_NO_LATCH=1 is the strict tear-and-snap A/B path.
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
            if (relockBurstRemaining == 0 &&
                    me->m_VsyncRenderer->isVrrRasterLockUncertain()) {
                relockFailureCount = qMin(relockFailureCount + 1, 2);
            }
            recoveryAlignmentPresent = true;
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

            // The floor above guarantees at least alignSpinFloorUs (3ms)
            // regardless of cadenceSlackUs, but content moderately below the
            // panel's max refresh - inside a high-refresh VRR window, not yet
            // tapered or near-ceiling-buffered - can have real VRR headroom
            // well under that: 90fps on a 120Hz panel has ~2.8ms of cadence
            // slack, 100fps has ~1.7ms. Spending the 3ms floor there waits
            // past the panel's actual extendable-blanking window on every
            // present, so service quietly runs slower than arrival - the
            // frame COUNT stays correctly bounded (dropFrameForEnqueue still
            // caps it), but each present's target keeps inheriting the
            // previous one's lateness (see the flip-spacing floor's
            // lastFlipUs clamp above), so the standing schedule/queue DELAY
            // creeps upward indefinitely instead of settling - the trim
            // servo further up can only claw back queue-side slack, not an
            // align wait that is structurally too wide for what this content
            // rate can afford. Bound the steady-state budget by the same
            // cadenceSlackUs the rush and near-buffered branches already
            // respect; the raster-lock-uncertain override just below
            // deliberately spends wider than this to re-anchor a free-running
            // raster and is left alone.
            if (cadenceSlackUs != 0 && cadenceSlackUs < alignBudgetUs) {
                alignBudgetUs = cadenceSlackUs;
            }

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
                    recoveryAlignmentPresent = true;
                }
                else if (nowUs - lastWideReanchorUs > 1000000ULL) {
                    alignBudgetUs = qMax(alignSpinFloorUs,
                                         minFrameIntervalUs + alignWideExtraUs);
                    lastWideReanchorUs = nowUs;
                    recoveryAlignmentPresent = true;
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
        const bool frameHasQueueTimestamp = frame->pkt_dts > 0;
        uint64_t frameQueueAgeUs = me->renderFrame(frame);
        uint32_t midScanTears = me->m_VsyncRenderer->popMidScanTearCount();

        if (queueAgeServoEnabled && frameHasQueueTimestamp) {
            // Close the servo on actual decode-completion to render-start
            // age - the same quantity reported as frame queue delay in
            // the performance overlay. The old planned-start estimate could
            // sit a full synchronous render interval in the past at
            // near-ceiling saturation, so it held a cushion on top of that
            // unavoidable occupancy and parked one surplus frame.
            uint32_t currentPacerDropCount =
                me->m_VideoStats->pacerDroppedFrames;
            if (currentPacerDropCount != lastObservedPacerDropCount) {
                // A coalesced frame advances queue age by roughly one source
                // interval. The following near-zero age is induced catch-up,
                // not starvation; restart statistics and briefly suppress
                // protection so a drop cannot immediately rebuild its buffer.
                resetQueueAgeWindow();
                postDropRecoverySuppressFrames = 4;
                lastObservedPacerDropCount = currentPacerDropCount;
            }
            bool postDropRecoverySuppressed =
                postDropRecoverySuppressFrames > 0;
            if (postDropRecoverySuppressFrames > 0) {
                postDropRecoverySuppressFrames--;
            }

            SDL_assert(queueAgeWindowSamples <
                       (int)queueAgeWindowUs.size());
            queueAgeWindowUs[queueAgeWindowSamples] =
                (uint32_t)qMin(frameQueueAgeUs, (uint64_t)UINT32_MAX);
            if (servoVeto || recoveryAlignmentPresent ||
                    postDropRecoverySuppressed) {
                queueAgeWindowTainted = true;
            }

            // Only a fresh band/rate acquisition may request fast bounded
            // recovery. A single near-empty steady-state sample commonly
            // follows a source gap or long render; padding after the fact
            // cannot hide that gap and instead starts a rebuffer cycle.
            bool acquisitionSample = queueAcquisitionPending && nearBuffered &&
                !staleSchedule && !servoVeto &&
                !postDropRecoverySuppressed;
            bool acquisitionShortfall = acquisitionSample &&
                frameQueueAgeUs + 1000 < targetQueueAgeUs;
            if (acquisitionSample) {
                queueAcquisitionPending = false;
            }
            if (nearBuffered && !staleSchedule && !servoVeto &&
                    acquisitionShortfall) {
                phaseAdvanceUs = 0;
                VrrQueueAgeController::Target recoveryTarget =
                    queueAgeController.target(
                        nearBuffered, fixedNearBufferTarget,
                        measuredSourceIntervalUs, minFrameIntervalUs,
                        scheduleGuardUs, queueAgeClampZoneUs);
                uint64_t recoveryDeficitUs =
                    recoveryTarget.queueAgeUs > frameQueueAgeUs ?
                        recoveryTarget.queueAgeUs - frameQueueAgeUs : 0;
                phaseDelayUs = 0;
                fastRecoveryRemainingUs = recoveryDeficitUs;
                queueAgeWindowTainted = true;
                uint64_t queueFeedbackNowUs = LiGetMicroseconds();
                if (acquisitionShortfall && recoveryDeficitUs != 0 &&
                        queueFeedbackNowUs - lastQueueAcquisitionLogUs >
                            10000000ULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR queue acquisition: measured age %.2f ms, recovering %.2f ms toward %.2f ms",
                                frameQueueAgeUs / 1000.0,
                                recoveryDeficitUs / 1000.0,
                                recoveryTarget.queueAgeUs / 1000.0);
                    lastQueueAcquisitionLogUs = queueFeedbackNowUs;
                }
            }
            else if (frameQueueAgeUs + 1500 >= targetQueueAgeUs) {
                fastRecoveryRemainingUs = 0;
            }

            if (++queueAgeWindowSamples >= queueAgeWindowSampleCap) {
                VrrQueueAgeController::WindowStatistics queueStats =
                    VrrQueueAgeController::summarizeWindow(
                        queueAgeWindowUs.data(), queueAgeWindowSamples);
                bool learningWindowClean = !queueAgeWindowTainted &&
                    phaseAdvanceUs == 0 && phaseDelayUs == 0 &&
                    fastRecoveryRemainingUs == 0;
                uint64_t oldMeasuredReserveUs =
                    queueAgeController.measuredReserveUs();
                VrrQueueAgeController::Target oldQueueTarget =
                    queueAgeController.target(
                        nearBuffered, fixedNearBufferTarget,
                        measuredSourceIntervalUs, minFrameIntervalUs,
                        scheduleGuardUs, queueAgeClampZoneUs);
                queueAgeController.observeWindow(
                    queueStats.p10Us, queueStats.p90Us,
                    learningWindowClean, nearBuffered);
                VrrQueueAgeController::Target newQueueTarget =
                    queueAgeController.target(
                        nearBuffered, fixedNearBufferTarget,
                        measuredSourceIntervalUs, minFrameIntervalUs,
                        scheduleGuardUs, queueAgeClampZoneUs);

                // Typical age controls excess latency even when an isolated
                // backlog/stale sample tainted learning. Using the absolute
                // minimum here let one low outlier pin most frames 4-10ms
                // above target in the field. A coalescing drop is only armed
                // after one full trim window failed to improve by even 0.5ms.
                bool overfillEligible = phaseAdvanceUs != 0 &&
                    previousQueueAgeMedianUs != 0 &&
                    queueStats.medianUs + 500 >= previousQueueAgeMedianUs;
                VrrQueueAgeController::PhaseDecisionInput phaseInput = {};
                phaseInput.stats = queueStats;
                phaseInput.targetAgeUs = newQueueTarget.queueAgeUs;
                phaseInput.previousTargetAgeUs = oldQueueTarget.queueAgeUs;
                phaseInput.sourceIntervalUs = measuredSourceIntervalUs;
                phaseInput.sampleCount = queueAgeWindowSamples;
                phaseInput.nearCeiling = nearBuffered;
                phaseInput.windowTainted = queueAgeWindowTainted;
                phaseInput.phaseAdvanceActive = phaseAdvanceUs != 0;
                phaseInput.phaseDelayActive = phaseDelayUs != 0;
                phaseInput.fastRecoveryActive =
                    fastRecoveryRemainingUs != 0;
                phaseInput.staleSchedule = staleSchedule;
                phaseInput.overfillEligible = overfillEligible;
                VrrQueueAgeController::PhaseDecision phaseDecision =
                    VrrQueueAgeController::decidePhase(phaseInput);
                uint64_t newPhaseAdvanceUs = phaseDecision.advanceUs;
                uint64_t newPhaseDelayUs = phaseDecision.delayUs;

                if (phaseDecision.requestOverfillDrop) {
                    // If at least 80% of a window carries half an interval of
                    // excess after a stalled trim, request one coalescing drop
                    // when a replacement is immediately available.
                    overfillDropPending = true;
                    overfillDropDeadlineUs = LiGetMicroseconds() +
                        qMax(measuredSourceIntervalUs * 2, (uint64_t)25000);
                }
                uint64_t queueNowUs = LiGetMicroseconds();
                if (newPhaseAdvanceUs != 0 && phaseAdvanceUs == 0 &&
                        queueNowUs - lastQueueServoLogUs > 30000000ULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR queue trim: median age %.2f ms (p20 %.2f), advancing %u us/frame toward %.2f ms",
                                queueStats.medianUs / 1000.0,
                                queueStats.p20Us / 1000.0,
                                (unsigned int)newPhaseAdvanceUs,
                                newQueueTarget.queueAgeUs / 1000.0);
                    lastQueueServoLogUs = queueNowUs;
                }
                if (newPhaseDelayUs != 0 && phaseDelayUs == 0 &&
                        queueNowUs - lastQueueServoLogUs > 30000000ULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR queue build: median age %.2f ms (p20 %.2f), delaying %u us/frame toward %.2f ms",
                                queueStats.medianUs / 1000.0,
                                queueStats.p20Us / 1000.0,
                                (unsigned int)newPhaseDelayUs,
                                newQueueTarget.queueAgeUs / 1000.0);
                    lastQueueServoLogUs = queueNowUs;
                }
                uint64_t newMeasuredReserveUs =
                    queueAgeController.measuredReserveUs();
                if (learningWindowClean &&
                        newMeasuredReserveUs != oldMeasuredReserveUs &&
                        queueNowUs - lastQueueTargetLogUs > 5000000ULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR queue learning: p10-p90 %.2f-%.2f ms, measured reserve %.2f ms",
                                queueStats.p10Us / 1000.0,
                                queueStats.p90Us / 1000.0,
                                newMeasuredReserveUs / 1000.0);
                    lastQueueTargetLogUs = queueNowUs;
                }
                phaseAdvanceUs = newPhaseAdvanceUs;
                phaseDelayUs = newPhaseDelayUs;
                previousQueueAgeMedianUs = queueStats.medianUs;
                queueAgeWindowTainted = false;
                queueAgeWindowSamples = 0;
            }
        }

        // In-band tear-rate probe (see the feedback state above). Counted
        // only for non-latched in-band presents, and any regime change
        // resets the window so a rate is never judged across a boundary.
        // 15% cleanly separates the measured populations: a working band
        // runs 0.2-2.6% torn, a non-following raster 40-95%.
        //
        // The probe itself is visible. Use staged rate tests so a broken
        // raster lock exits after a short burst without eventually treating
        // isolated healthy tears as a cumulative failure. The final 64-frame
        // verdict catches less extreme failures in about half a second.
        //  - Standard policy: consecutive failures at the same cadence
        //    double the latch period (60s up to 8min), so steady-state probe
        //    cost falls to ~0.1% of wall time. A passing probe or a genuine
        //    rate change resets the period.
        //  - Low-latency policy: latch for eight seconds, then retry. This
        //    bounds added latency while preventing a storm from persisting.
        if (nearBuffered && !vsyncLatchPresent) {
            bandTearWindowPresents++;
            bandTearWindowTears += midScanTears;

            bool probeFailed = false;
            if (bandTearWindowPresents >= 12 &&
                    bandTearWindowPresents < 24 &&
                    bandTearWindowTears * 100 >=
                        bandTearWindowPresents * 30) {
                probeFailed = true;
            }
            else if (bandTearWindowPresents >= 24 &&
                     bandTearWindowPresents < 64 &&
                     bandTearWindowTears * 100 >=
                        bandTearWindowPresents * 20) {
                probeFailed = true;
            }
            else if (bandTearWindowPresents >= 64) {
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
                if (preferenceLatchAvailable) {
                    // Each content rate climbs its own per-stream ladder,
                    // resuming after temporary content-rate changes.
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
                else if (latchSupported && me->m_VrrTearingPreferred) {
                    // "Allow tearing" remains immediate in healthy true VRR,
                    // but it must not permit a measured tear storm to persist.
                    // Eight seconds is long enough to make the bad interval
                    // visually quiet without turning this preference into a
                    // permanent fixed-vsync mode; expiry automatically probes
                    // true VRR again. No queue reserve is added.
                    const uint32_t fallbackSecs = 8;
                    bandTearFallbackUntilUs = nowUs +
                        (uint64_t)fallbackSecs * 1000000ULL;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "VRR low-latency tear rescue: %u of %u in-band presents tore visibly at %.1f fps measured; temporarily vsync-latching for %us",
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

uint64_t Pacer::renderFrame(AVFrame* frame)
{
    // Record decode-completion-to-render-start age for the pacing metric.
    uint64_t beforeRender = LiGetMicroseconds();
    uint64_t frameQueueAgeUs = frame->pkt_dts > 0 &&
        beforeRender > (uint64_t)frame->pkt_dts ?
            beforeRender - (uint64_t)frame->pkt_dts : 0;
    m_VideoStats->totalPacerTimeUs += frameQueueAgeUs;

    // Render it
    m_VsyncRenderer->renderFrame(frame);
    uint64_t afterRender = LiGetMicroseconds();

    m_VideoStats->totalRenderTimeUs += (afterRender - beforeRender);
    m_VideoStats->renderedFrames++;
    recordFrameInterval(beforeRender, afterRender, frameQueueAgeUs);

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
    return frameQueueAgeUs;
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
