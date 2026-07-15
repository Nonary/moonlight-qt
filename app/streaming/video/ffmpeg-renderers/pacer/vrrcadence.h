#pragma once

#include <stdint.h>

// Keep tear calibration bands distinct at the rates users actually sweep.
// A 600 us identity window merged the 100/105/110 FPS intervals (roughly
// 10.0/9.5/9.1 ms), so a failed probe or fixed-vsync latch at one rate was
// incorrectly reused at its neighbors. 250 us still absorbs ordinary
// timestamp quantization while leaving ten-FPS steps as separate regimes.
static constexpr uint64_t VRR_RATE_IDENTITY_TOLERANCE_US = 250;

static inline bool vrrCadenceRateIdentityMatches(uint64_t firstIntervalUs,
                                                  uint64_t secondIntervalUs)
{
    uint64_t deltaUs = firstIntervalUs > secondIntervalUs ?
        firstIntervalUs - secondIntervalUs : secondIntervalUs - firstIntervalUs;
    return deltaUs <= VRR_RATE_IDENTITY_TOLERANCE_US;
}

// A warmed cadence entering the near-ceiling taper needs buffered true VRR
// on its first eligible sample. Entry dwell without a fixed-vsync cover leaves
// the exact high-rate region this mode protects running unbuffered.
static inline bool vrrNearBufferEntryReady(
    bool cadenceWarmed,
    uint64_t measuredSourceIntervalUs,
    uint64_t minFrameIntervalUs,
    uint64_t taperEntryZoneUs,
    bool preferenceLatchAvailable,
    uint64_t bufferFloorIntervalUs,
    uint64_t bandEntryStepUs)
{
    return cadenceWarmed &&
        measuredSourceIntervalUs < minFrameIntervalUs + taperEntryZoneUs &&
        (!preferenceLatchAvailable ||
         measuredSourceIntervalUs >= bufferFloorIntervalUs + bandEntryStepUs);
}

// Stale recovery normally uses the display's guarded flip floor. The smooth
// policy spends half of the available cadence headroom instead. This scales
// continuously with distance from the ceiling: 80 FPS can recover quickly,
// while 116 FPS cannot turn a small delivery burst into a run of 120 Hz
// presents followed by a visible gap. Explicit low-latency/A-B modes retain
// the fastest panel-floor drain.
static inline uint64_t vrrCatchUpSpacingUs(
    uint64_t flipSpacingFloorUs,
    uint64_t sourceIntervalUs,
    uint64_t minFrameIntervalUs,
    bool latchedPresents,
    bool smoothRecovery)
{
    uint64_t gentleDrainUs = sourceIntervalUs * 7 / 8;
    if (gentleDrainUs < flipSpacingFloorUs) {
        gentleDrainUs = flipSpacingFloorUs;
    }
    if (latchedPresents) {
        return gentleDrainUs;
    }
    if (!smoothRecovery || minFrameIntervalUs == 0 ||
            sourceIntervalUs <= flipSpacingFloorUs) {
        return flipSpacingFloorUs;
    }
    return flipSpacingFloorUs +
        (sourceIntervalUs - flipSpacingFloorUs) / 2;
}

// A deep transient burst needs to drain before the persistence watchdog is
// forced to discard a frame. Spend three quarters of real cadence headroom
// only while at least two successors are waiting. This remains gentler than
// a panel-floor burst and automatically collapses toward normal cadence near
// the display ceiling, where little headroom exists.
static inline uint64_t vrrPressureCatchUpSpacingUs(
    uint64_t flipSpacingFloorUs,
    uint64_t sourceIntervalUs,
    uint64_t minFrameIntervalUs,
    bool latchedPresents,
    bool smoothRecovery,
    bool deepPressure)
{
    uint64_t normalSpacingUs = vrrCatchUpSpacingUs(
        flipSpacingFloorUs, sourceIntervalUs, minFrameIntervalUs,
        latchedPresents, smoothRecovery);
    if (!deepPressure || latchedPresents || !smoothRecovery ||
            sourceIntervalUs <= flipSpacingFloorUs) {
        return normalSpacingUs;
    }
    return flipSpacingFloorUs +
        (sourceIntervalUs - flipSpacingFloorUs) / 4;
}

// Keep three frames only as a transient burst envelope. Sustained depth still
// converges to the single-frame steady-state bound after the watchdog proves
// that service cannot drain it.
static inline int vrrCadenceBacklogLimit(bool persistentBacklog)
{
    return persistentBacklog ? 1 : 3;
}

// Recovery must not turn a capped stream into a higher-refresh stream.  The
// panel floor is still a physical lower bound, but whenever the measured
// source cadence leaves headroom, this returns the source interval instead of
// spending that headroom on a recovery burst.  Callers that choose this
// policy coalesce obsolete queued frames to recover latency rather than
// presenting them faster than the negotiated stream rate.
static inline uint64_t vrrCadenceCappedSpacingUs(
    uint64_t flipSpacingFloorUs,
    uint64_t sourceIntervalUs)
{
    return sourceIntervalUs > flipSpacingFloorUs ?
        sourceIntervalUs : flipSpacingFloorUs;
}

// Keep the source-rate cap confined to the recovery/backlog transition. A
// stale cadence sample must not throttle ordinary fixed-refresh pacing after
// the queue has recovered.
static inline uint64_t vrrCadenceRecoverySpacingUs(
    bool recoveryActive,
    uint64_t flipSpacingFloorUs,
    uint64_t sourceIntervalUs)
{
    return recoveryActive ?
        vrrCadenceCappedSpacingUs(flipSpacingFloorUs, sourceIntervalUs) :
        flipSpacingFloorUs;
}

// Source-cadence clamping and frame coalescing are last resorts for a queue
// that is physically unable to drain. Any positive service headroom is owned
// by the continuous catch-up controller, whose recovery rate already scales
// with distance from the panel ceiling.
static inline bool vrrStrictCadenceRecoveryRequired(
    bool strictRecoveryEnabled,
    bool ceilingConstrained,
    uint64_t sourceIntervalUs,
    uint64_t flipSpacingFloorUs,
    bool staleSchedule,
    int queuedBehindCount)
{
    return strictRecoveryEnabled && ceilingConstrained &&
        sourceIntervalUs <= flipSpacingFloorUs &&
        (staleSchedule || queuedBehindCount >= 2);
}

// A persistent-depth watchdog must allow at least enough samples for one
// queued source frame to drain at the recovery path's actual service
// headroom. Without this, 116 FPS on a 120 Hz panel reaches the old 500 ms
// watchdog before its deliberately gentle smooth-recovery cadence can finish.
static inline int vrrBacklogHistorySampleCap(
    int baseSampleCap,
    uint64_t sourceIntervalUs,
    uint64_t flipSpacingFloorUs,
    uint64_t minFrameIntervalUs,
    bool latchedPresents,
    bool smoothRecovery)
{
    if (baseSampleCap < 1) {
        return baseSampleCap;
    }

    // The watchdog must use the same spacing as the selected recovery path.
    // Smooth recovery deliberately spends only half of the physical service
    // headroom. Counting drain frames from the raw flip floor therefore
    // declared a 116-on-120 queue persistent after ~0.6 s even though the
    // selected 8.551 ms recovery cadence needs ~1.08 s to retire one queued
    // 8.620 ms source frame. That converted a smooth, physically progressing
    // recovery into an avoidable coalescing drop.
    uint64_t recoverySpacingUs = vrrCatchUpSpacingUs(
        flipSpacingFloorUs, sourceIntervalUs, minFrameIntervalUs,
        latchedPresents, smoothRecovery);
    if (sourceIntervalUs <= recoverySpacingUs) {
        return baseSampleCap;
    }

    uint64_t headroomUs = sourceIntervalUs - recoverySpacingUs;
    uint64_t drainFrames =
        (sourceIntervalUs + headroomUs - 1) / headroomUs;
    uint64_t desiredCap = drainFrames + 8;
    uint64_t maximumCap = (uint64_t)baseSampleCap * 4;
    if (desiredCap < (uint64_t)baseSampleCap) {
        desiredCap = baseSampleCap;
    }
    if (desiredCap > maximumCap) {
        desiredCap = maximumCap;
    }
    return (int)desiredCap;
}

// Phase build is derived from a finite target deficit. Consume that deficit
// explicitly so a cadence/sample-window change cannot apply the old per-frame
// step for more frames and overshoot the controller's one-frame target cap.
static inline uint64_t vrrConsumePhaseDelayBudget(
    uint64_t requestedStepUs,
    uint64_t* remainingBudgetUs)
{
    if (remainingBudgetUs == nullptr || *remainingBudgetUs == 0) {
        return 0;
    }
    uint64_t appliedUs = requestedStepUs < *remainingBudgetUs ?
        requestedStepUs : *remainingBudgetUs;
    *remainingBudgetUs -= appliedUs;
    return appliedUs;
}

// Select the target for a stale recovery present. Fast recovery retains the
// historical accelerate-only behavior: it may pull a future target earlier,
// but never delays one. A gentle spacing policy is different—the spacing is
// the smoothness bound itself, so an already-scheduled floor-rate target must
// also move later to honor it. Near the ceiling that remains roughly
// 0.6-1.1ms; at low FPS it is capped to half of the available panel headroom,
// the deliberate midpoint between instant queue drain and cadence stability.
static inline uint64_t vrrCatchUpTargetUs(
    uint64_t lastFlipUs,
    uint64_t currentTargetUs,
    uint64_t nowUs,
    uint64_t catchUpSpacingUs,
    bool enforceMinimumSpacing)
{
    uint64_t selectedTargetUs = lastFlipUs + catchUpSpacingUs;
    if (selectedTargetUs < nowUs) {
        selectedTargetUs = nowUs;
    }

    if (selectedTargetUs < currentTargetUs || enforceMinimumSpacing) {
        return selectedTargetUs;
    }
    return currentTargetUs;
}

// Fixed-vsync fallback is useful at the panel's physical ceiling, but a long
// latch at a rate with real VRR headroom turns an isolated overlay/raster
// disturbance into floor-specific judder. Keep the first safety rung for
// rates with at least roughly one scanout of source headroom; only the
// ceiling-adjacent regime may climb the long per-rate ladder.
static inline uint32_t vrrHeadroomFallbackPeriodSecs(
    uint64_t sourceIntervalUs,
    uint64_t minFrameIntervalUs,
    uint64_t headroomThresholdUs,
    uint32_t basePeriodSecs,
    uint32_t requestedPeriodSecs)
{
    return sourceIntervalUs >= minFrameIntervalUs + headroomThresholdUs &&
            requestedPeriodSecs > basePeriodSecs ?
        basePeriodSecs : requestedPeriodSecs;
}

// Maximum time an alignment wait may consume without making the selected
// flip-spacing floor plus alignment service slower than the source cadence.
// The cadence guard retains the existing nominal-scanout safety margin; the
// service guard also covers a larger runtime/env-selected spacing floor.
static inline uint64_t vrrCadenceAlignmentSlackUs(
    uint64_t sourceIntervalUs,
    uint64_t minFrameIntervalUs,
    uint64_t cadenceGuardUs,
    uint64_t flipSpacingFloorUs,
    uint64_t serviceGuardUs)
{
    uint64_t cadenceFloorUs = minFrameIntervalUs + cadenceGuardUs;
    uint64_t serviceFloorUs = flipSpacingFloorUs + serviceGuardUs;
    uint64_t budgetFloorUs = cadenceFloorUs > serviceFloorUs ?
        cadenceFloorUs : serviceFloorUs;
    return sourceIntervalUs > budgetFloorUs ?
        sourceIntervalUs - budgetFloorUs : 0;
}

// A long-lived tear-rate verdict must describe normal in-band operation, not
// the bounded transition used to acquire queue phase and raster lock. The
// explicit per-present flag covers the final fast-recovery step, where the
// remaining counter reaches zero before the present is measured.
static inline bool vrrTearProbeTransitionSettled(
    bool recoveryAlignmentPresent,
    bool queueAcquisitionActive,
    uint64_t fastRecoveryRemainingUs,
    bool fastQueueRecoveryPresent)
{
    return !recoveryAlignmentPresent && !queueAcquisitionActive &&
        fastRecoveryRemainingUs == 0 && !fastQueueRecoveryPresent;
}

static inline bool vrrQueueAcquisitionTransitionActive(
    bool queueAgeServoEnabled,
    bool frameHasQueueTimestamp,
    bool queueAcquisitionPending)
{
    return queueAgeServoEnabled && frameHasQueueTimestamp &&
        queueAcquisitionPending;
}

static inline bool vrrQueueAcquisitionOverlapsRecovery(
    bool nearBuffered,
    bool recoveryAlignmentPresent,
    uint64_t fastRecoveryRemainingUs,
    bool fastQueueRecoveryPresent)
{
    return nearBuffered && recoveryAlignmentPresent &&
        (fastRecoveryRemainingUs != 0 || fastQueueRecoveryPresent);
}

class VrrCadenceClock
{
public:
    explicit VrrCadenceClock(int nominalFps = 0, int maxRefreshFps = 0)
    {
        reset(nominalFps, maxRefreshFps);
    }

    void reset(int nominalFps, int maxRefreshFps = 0)
    {
        m_NominalFrameIntervalUs = 1000000ULL / (nominalFps > 0 ? nominalFps : 1);
        // The display can't physically refresh faster than this, no matter how
        // precise our present timing is - a tearing-allowed present tighter than
        // this is guaranteed to tear mid-scan since the panel is still mid-way
        // through the previous refresh.
        m_MinFrameIntervalUs = maxRefreshFps > 0 ? (1000000ULL / maxRefreshFps) : 0;
        m_SmoothedIntervalUs = m_NominalFrameIntervalUs;
        m_LastSourceTimeUs = 0;
        m_LastTargetTimeUs = 0;
        m_PhaseReset = false;

        // ~Half a second of source timestamps for the windowed cadence mean.
        int cap = nominalFps > 0 ? nominalFps / 2 + 1 : 31;
        if (cap < 17) {
            cap = 17;
        }
        if (cap > MAX_SOURCE_TIMES) {
            cap = MAX_SOURCE_TIMES;
        }
        m_SourceTimeCap = cap;
        m_SourceTimeHead = 0;
        m_SourceTimeCount = 0;
        m_TimestamplessFrames = 0;
        m_PendingStallDeltaUs = 0;
        resetRatePhaseTracker();
        resetFasterCadenceCandidate();
        resetSlowerCadenceCandidate();
        m_FasterCadenceAdopted = false;
        m_FasterCadenceTrusted = false;
        m_SlowerCadenceAdopted = false;
        m_SlowerCadenceTrusted = false;
    }

    // Feed a source timestamp into the cadence measurement without scheduling
    // anything. Every frame's timestamp must pass through here - INCLUDING
    // frames the pacer drops without presenting. A dropped frame's interval
    // otherwise vanishes from the window while its time span remains, so the
    // measured cadence reads drop-inflated, and near the panel ceiling that
    // error is self-sealing: 116fps content dropped at 10-15% measured as
    // ~103fps, which sat just above the vsync-latch threshold, which kept the
    // pacer free-running at the flip-ceiling spacing floor (~110fps), which
    // caused the very drops corrupting the measurement.
    void observeSourceTime(uint64_t sourceTimeUs)
    {
        if (sourceTimeUs == 0) {
            // No usable timestamp on this frame. Track a run length so
            // warmedUp() can report a stream that never carries timestamps
            // as warm instead of perpetually cold - there is nothing to
            // measure, and pacing already free-runs off the nominal
            // interval in that case.
            if (m_TimestamplessFrames < 1000) {
                m_TimestamplessFrames++;
            }
            resetRatePhaseTracker();
            resetFasterCadenceCandidate();
            resetSlowerCadenceCandidate();
            return;
        }

        m_TimestamplessFrames = 0;

        if (m_LastSourceTimeUs != 0 && sourceTimeUs > m_LastSourceTimeUs) {
            uint64_t sourceDeltaUs = sourceTimeUs - m_LastSourceTimeUs;

            // A stall is a delta far outside the cadence actually being
            // measured, not the stream's nominal rate. Nominal is only an
            // upper bound on content fps: a threshold pinned to 4x nominal
            // sits a mere 3.5% above a 30fps game's real deltas on a 116fps
            // stream, so ordinary content jitter restarted the window every
            // few frames, the clock never reported warm, and the pacer
            // parked in the cadence-cold vsync latch on content VRR handles
            // trivially.
            uint64_t stallThresholdUs =
                (m_SmoothedIntervalUs > m_NominalFrameIntervalUs ?
                     m_SmoothedIntervalUs : m_NominalFrameIntervalUs) * 4;

            // The half-second mean deliberately rejects individual short
            // deltas, but after low-FPS gameplay/cutscenes its fixed sample
            // capacity can span nearly two seconds. A real upward rate step
            // then services the old slow schedule long enough to overflow the
            // queue (measured as a 16% drop burst on a 30 -> 85 FPS return).
            // Adopt only a large, sustained acceleration: six consecutive
            // usable source intervals at least 1.5x faster than the cadence
            // that started the candidate. Holding the initial baseline keeps
            // the moving long-window mean from moving the goalpost. Ordinary
            // host-vsync quantization (for example 8.3/16.7 ms around 90 FPS)
            // breaks the streak and remains averaged by the normal window.
            bool adoptedCadenceStepThisSample = false;

            // The half-second mean is intentionally stable once a cadence is
            // established, but that stability used to make moderate gameplay
            // upshifts (for example 95 -> 106 FPS) take long enough to build
            // two or more queued frames. Recovery then had to coalesce them,
            // producing the skip and refresh spike the cadence buffer is
            // meant to prevent.
            //
            // Track cumulative phase drift against the established cadence.
            // Host-vsync quantization is self-cancelling: its run of short
            // slots stays less than one source interval ahead before the long
            // slot brings phase back to zero. A real rate change keeps moving
            // in one direction and crosses one interval. Adopt that candidate
            // immediately, before it can create more than one frame of queue
            // pressure. This adds no standing delay; it only shortens the
            // estimator's response when the source rate genuinely changes.
            uint64_t phaseTrackedIntervalUs =
                observeModerateCadencePhase(sourceDeltaUs);
            if (phaseTrackedIntervalUs != 0) {
                bool faster = phaseTrackedIntervalUs < m_SmoothedIntervalUs;
                m_SmoothedIntervalUs = qMax(
                    phaseTrackedIntervalUs, m_NominalFrameIntervalUs);
                m_SourceTimeCount = 0;
                m_PendingStallDeltaUs = 0;
                m_FasterCadenceAdopted = faster;
                m_FasterCadenceTrusted = faster;
                m_SlowerCadenceAdopted = !faster;
                m_SlowerCadenceTrusted = !faster;
                adoptedCadenceStepThisSample = true;
                resetFasterCadenceCandidate();
                resetSlowerCadenceCandidate();
            }

            uint64_t fasterReferenceUs = m_FasterCadenceCandidateCount != 0 ?
                m_FasterCadenceBaselineUs : m_SmoothedIntervalUs;
            bool fasterCadenceCandidate =
                !adoptedCadenceStepThisSample &&
                m_SmoothedIntervalUs > m_NominalFrameIntervalUs &&
                sourceDeltaUs >= m_NominalFrameIntervalUs / 2 &&
                sourceDeltaUs * 3 <= fasterReferenceUs * 2;
            if (fasterCadenceCandidate) {
                if (m_FasterCadenceCandidateCount == 0) {
                    m_FasterCadenceBaselineUs = m_SmoothedIntervalUs;
                    m_FasterCadenceTotalUs = 0;
                }
                m_FasterCadenceTotalUs += sourceDeltaUs;
                m_FasterCadenceCandidateCount++;
                if (m_FasterCadenceCandidateCount >=
                        FASTER_CADENCE_CONFIRM_INTERVALS) {
                    uint64_t adoptedIntervalUs = m_FasterCadenceTotalUs /
                        (uint64_t)m_FasterCadenceCandidateCount;
                    // The negotiated stream FPS is an upper bound. A run of
                    // quantized/bursty source timestamps may be shorter, but
                    // must not invent a cadence faster than the host can send.
                    m_SmoothedIntervalUs = adoptedIntervalUs >
                        m_NominalFrameIntervalUs ? adoptedIntervalUs :
                        m_NominalFrameIntervalUs;
                    m_SourceTimeCount = 0;
                    m_PendingStallDeltaUs = 0;
                    m_FasterCadenceAdopted = true;
                    m_FasterCadenceTrusted = true;
                    adoptedCadenceStepThisSample = true;
                    resetRatePhaseTracker();
                    resetFasterCadenceCandidate();
                }
            }
            else {
                resetFasterCadenceCandidate();
            }

            // Downshifts need the same bounded handoff as upshifts. Without
            // it, the half-second source window keeps the old near-ceiling
            // cadence alive through a 60-70 FPS step, and the pacer alternates
            // between the stale high-rate target and the new arrival times.
            // Require six consecutive intervals at least 25% slower than the
            // cadence that began the candidate. This is long enough to reject
            // an isolated hitch, while adopting a real 60/70 FPS step in
            // roughly one tenth of a second. The upper bound leaves genuine
            // stalls to the existing stall/recovery path.
            if (!adoptedCadenceStepThisSample) {
                uint64_t slowerReferenceUs =
                    m_SlowerCadenceCandidateCount != 0 ?
                        m_SlowerCadenceBaselineUs : m_SmoothedIntervalUs;
                bool slowerCadenceCandidate =
                    sourceDeltaUs * 4 >= slowerReferenceUs * 5 &&
                    sourceDeltaUs <= slowerReferenceUs * 2 &&
                    sourceDeltaUs <= stallThresholdUs;
                if (slowerCadenceCandidate) {
                    if (m_SlowerCadenceCandidateCount == 0) {
                        m_SlowerCadenceBaselineUs = m_SmoothedIntervalUs;
                        m_SlowerCadenceTotalUs = 0;
                    }
                    m_SlowerCadenceTotalUs += sourceDeltaUs;
                    m_SlowerCadenceCandidateCount++;
                    if (m_SlowerCadenceCandidateCount >=
                            SLOWER_CADENCE_CONFIRM_INTERVALS) {
                        uint64_t adoptedIntervalUs = m_SlowerCadenceTotalUs /
                            (uint64_t)m_SlowerCadenceCandidateCount;
                        m_SmoothedIntervalUs = adoptedIntervalUs >
                            m_NominalFrameIntervalUs ? adoptedIntervalUs :
                            m_NominalFrameIntervalUs;
                        m_SourceTimeCount = 0;
                        m_PendingStallDeltaUs = 0;
                        m_SlowerCadenceAdopted = true;
                        m_SlowerCadenceTrusted = true;
                        adoptedCadenceStepThisSample = true;
                        resetRatePhaseTracker();
                        resetSlowerCadenceCandidate();
                    }
                }
                else {
                    resetSlowerCadenceCandidate();
                }
            }
            else {
                resetSlowerCadenceCandidate();
            }

            if (sourceDeltaUs > stallThresholdUs) {
                // A stall supersedes any faster-cadence rebase that may have
                // been detected in an earlier timestamp from the same
                // dropped-frame batch.
                m_FasterCadenceAdopted = false;
                m_FasterCadenceTrusted = false;
                m_SlowerCadenceAdopted = false;
                m_SlowerCadenceTrusted = false;
                if (m_PendingStallDeltaUs != 0 &&
                        sourceDeltaUs <= MAX_ADOPTABLE_INTERVAL_US &&
                        m_PendingStallDeltaUs <= MAX_ADOPTABLE_INTERVAL_US &&
                        sourceDeltaUs < m_PendingStallDeltaUs * 2 &&
                        m_PendingStallDeltaUs < sourceDeltaUs * 2) {
                    // Two consecutive over-threshold deltas of similar
                    // magnitude are a cadence, not a stall - content running
                    // slower than a quarter of the measured rate (a 24fps
                    // cutscene right after high-fps gameplay). Adopt it, or
                    // every subsequent delta re-restarts the window against
                    // a smoothed interval that can never learn the new rate.
                    //
                    // Bounded at ~18fps: real gameplay/cutscene cadences at
                    // 20-30fps (33-50ms) are supported, while HOST HITCHES on
                    // a struggling game arrive as similar consecutive 60-80ms gaps every
                    // few seconds (measured 2026-07-06) and were adopted as
                    // a fake 13-16fps "cadence" - the schedule then paced
                    // 60-80ms against ~9ms arrivals until the window
                    // re-warmed, a stale-rush tear chain measured at 30-50%
                    // mid-scan for the duration. Slower than the bound is
                    // always treated as a stall; genuinely sub-18fps content
                    // (loading screens) just presents on arrival via the
                    // stall snap, which is the right behavior for it anyway.
                    m_SmoothedIntervalUs =
                        (m_PendingStallDeltaUs + sourceDeltaUs) / 2;
                    m_PendingStallDeltaUs = 0;
                    resetRatePhaseTracker();
                }
                else {
                    // Genuine stall: restart the window so the gap doesn't
                    // pollute the mean for the next half second.
                    m_SourceTimeCount = 0;
                    m_PendingStallDeltaUs = sourceDeltaUs;
                    resetRatePhaseTracker();
                }
            }
            else {
                m_PendingStallDeltaUs = 0;
            }

            m_SourceTimesUs[m_SourceTimeHead] = sourceTimeUs;
            m_SourceTimeHead = (m_SourceTimeHead + 1) % m_SourceTimeCap;
            if (m_SourceTimeCount < m_SourceTimeCap) {
                m_SourceTimeCount++;
            }

            int intervals = m_SourceTimeCount - 1;
            if (!adoptedCadenceStepThisSample && intervals >= 16) {
                uint64_t oldestUs = m_SourceTimesUs[
                    (m_SourceTimeHead + m_SourceTimeCap - m_SourceTimeCount) % m_SourceTimeCap];
                m_SmoothedIntervalUs = (sourceTimeUs - oldestUs) / (uint64_t)intervals;
            }
            else if (!adoptedCadenceStepThisSample &&
                     sourceDeltaUs >= m_NominalFrameIntervalUs / 2 &&
                     sourceDeltaUs <= stallThresholdUs) {
                // Warmup fallback until the window fills: the old EMA. Its
                // bias is immaterial over a handful of frames.
                m_SmoothedIntervalUs =
                    (m_SmoothedIntervalUs * 7 + sourceDeltaUs) / 8;
            }

            // Source timestamp quantization may momentarily imply a cadence
            // above the negotiated stream FPS. Keep that upper bound as a
            // class invariant for both the normal window and warmup EMA, not
            // only at the fast-adoption instant.
            if (m_SmoothedIntervalUs < m_NominalFrameIntervalUs) {
                m_SmoothedIntervalUs = m_NominalFrameIntervalUs;
            }
        }
        else if (m_LastSourceTimeUs != 0 && sourceTimeUs <= m_LastSourceTimeUs) {
            // Non-monotonic timestamps (stream restart): restart the window.
            m_SourceTimeCount = 0;
            m_PendingStallDeltaUs = 0;
            resetRatePhaseTracker();
            resetFasterCadenceCandidate();
            resetSlowerCadenceCandidate();
            m_FasterCadenceAdopted = false;
            m_FasterCadenceTrusted = false;
            m_SlowerCadenceAdopted = false;
            m_SlowerCadenceTrusted = false;
        }

        m_LastSourceTimeUs = sourceTimeUs;
    }

    uint64_t nextTargetUs(uint64_t nowUs, uint64_t sourceTimeUs)
    {
        // Track the content cadence as an average rather than using each raw
        // timestamp delta. A game vsynced on the host quantizes its frame
        // times to whole refresh slots (~87fps on a 120Hz host arrives as
        // alternating 8.3ms/16.7ms deltas); pacing presents by the raw
        // deltas reproduces that alternation 1:1 on the VRR panel, which
        // reads as judder during camera pans. Pacing by the average converts
        // it into a near-even cadence instead.
        //
        // The average is the mean delta over a ~half-second window of
        // timestamps, not a per-delta EMA. The old EMA's outlier band was
        // asymmetric - it rejected deltas under half nominal but accepted up
        // to 4x - and delivery is gap-then-burst shaped, so each gap was
        // averaged in while the tiny burst delta cancelling it was rejected.
        // That overestimates the interval by a fraction of a percent, and an
        // open-loop rate error compounds: the schedule walks a few ms later
        // every second (measured as the latency trimmer in the pacer
        // re-arming 51 times in 3 minutes chasing regenerating lateness).
        // The windowed mean pairs every gap with its burst. A genuine stall,
        // a timestamp going backwards on stream restart, or one of the
        // guarded sustained rate-step detectors above resets the window. It is also
        // naturally immune to the single-spike EMA rides that the pacer's
        // taper hysteresis was added to absorb.
        observeSourceTime(sourceTimeUs);

        uint64_t targetUs = nowUs;
        bool cadenceStepAdopted = m_FasterCadenceAdopted ||
            m_SlowerCadenceAdopted;
        m_FasterCadenceAdopted = false;
        m_SlowerCadenceAdopted = false;

        if (cadenceStepAdopted) {
            // Drop the old slow schedule immediately. The pacer still clamps
            // this target against the last actual flip and the panel's scanout
            // floor, so rebasing removes backlog without over-driving the
            // display.
            m_PhaseReset = true;
        }
        else if (m_LastTargetTimeUs != 0) {
            uint64_t frameIntervalUs = m_SmoothedIntervalUs;

            targetUs = m_LastTargetTimeUs + frameIntervalUs;

            if (targetUs + frameIntervalUs < nowUs) {
                // A stall longer than a frame interval: snap the schedule
                // onto the present. This wipes whatever standing phase
                // offset the pacer had built on top of the schedule, so
                // report it. The pacer can reacquire from the first usable
                // actual-age sample with bounded phase motion instead of
                // treating the old offset as still valid.
                targetUs = nowUs;
                m_PhaseReset = true;
            }

            // Applies to both the normal path above and the catch-up reset just
            // above it - neither considers the display's max refresh rate on its
            // own, so clamp the result here regardless of which path produced it.
            if (targetUs < m_LastTargetTimeUs + m_MinFrameIntervalUs) {
                targetUs = m_LastTargetTimeUs + m_MinFrameIntervalUs;
            }
        }

        m_LastTargetTimeUs = targetUs;

        return targetUs;
    }

    void rebaseTarget(uint64_t targetUs)
    {
        // The pacer presented earlier than our schedule (latency catch-up).
        // Build subsequent targets from the instant actually used, otherwise
        // the schedule stays permanently late relative to frame delivery and
        // the catch-up never converges.
        m_LastTargetTimeUs = targetUs;
    }

    uint64_t smoothedIntervalUs() const
    {
        return m_SmoothedIntervalUs;
    }

    bool consumePhaseReset()
    {
        bool reset = m_PhaseReset;
        m_PhaseReset = false;
        return reset;
    }

    bool warmedUp() const
    {
        // Warm = enough monotonic samples spanning ~0.5s of content, or a
        // cadence accepted by one of the guarded six-interval rate-step
        // paths.
        // Goes false on reset() and whenever the window restarts for a
        // genuine stall or non-monotonic timestamp - exactly the moments the
        // smoothed interval is least trustworthy (stream bring-up, loading
        // screens, entering a game). The count path covers content
        // near the nominal rate; the span path covers slower content, whose
        // samples cover half a second long before the nominal-rate cap fills
        // (a 30fps game would otherwise stay "cold" - and vsync-latched -
        // for 2s after every window restart on a 116fps stream). Streams
        // that never carry usable timestamps report warm: nothing to
        // measure.
        if (m_FasterCadenceTrusted || m_SlowerCadenceTrusted ||
                m_TimestamplessFrames >= 32) {
            return true;
        }
        if (m_SourceTimeCount >= m_SourceTimeCap) {
            return true;
        }
        if (m_SourceTimeCount >= 17) {
            uint64_t newestUs = m_SourceTimesUs[
                (m_SourceTimeHead + m_SourceTimeCap - 1) % m_SourceTimeCap];
            uint64_t oldestUs = m_SourceTimesUs[
                (m_SourceTimeHead + m_SourceTimeCap - m_SourceTimeCount) % m_SourceTimeCap];
            return newestUs - oldestUs >= 500000;
        }
        return false;
    }

private:
    static const int MAX_SOURCE_TIMES = 128;
    static const int FASTER_CADENCE_CONFIRM_INTERVALS = 6;
    static const int SLOWER_CADENCE_CONFIRM_INTERVALS = 6;
    static const int MODERATE_CADENCE_MIN_INTERVALS = 6;
    // Slowest delta pair adoptable as a real content cadence (~18fps). This
    // includes the requested 20 FPS lower bound while keeping the measured
    // 60-80ms host-hitch population on the stall path.
    static const uint64_t MAX_ADOPTABLE_INTERVAL_US = 55000;

    uint64_t observeModerateCadencePhase(uint64_t sourceDeltaUs)
    {
        const uint64_t baselineUs = m_SmoothedIntervalUs;
        if (baselineUs == 0) {
            resetRatePhaseTracker();
            return 0;
        }

        // Large steps and isolated hitches belong to the existing guarded
        // six-sample detectors. Keeping this tracker inside a 2/3x..1.5x
        // band prevents one long frame from masquerading as a new cadence.
        if (sourceDeltaUs * 3 < baselineUs * 2 ||
                sourceDeltaUs * 2 > baselineUs * 3) {
            resetRatePhaseTracker();
            return 0;
        }

        int64_t deltaErrorUs = (int64_t)sourceDeltaUs -
            (int64_t)baselineUs;
        uint64_t zeroToleranceUs = qMax((uint64_t)50,
                                         baselineUs / 100);
        if ((uint64_t)qAbs(deltaErrorUs) <= zeroToleranceUs) {
            // The source returned to the established cadence. Discard any
            // phase excursion left by a one-off long or short frame.
            resetRatePhaseTracker();
            return 0;
        }

        int64_t nextErrorUs = m_RatePhaseErrorUs + deltaErrorUs;
        bool crossedZero =
            (m_RatePhaseErrorUs < 0 && nextErrorUs >= 0) ||
            (m_RatePhaseErrorUs > 0 && nextErrorUs <= 0);
        if (m_RatePhaseCandidateCount == 0 || crossedZero) {
            m_RatePhaseErrorUs = deltaErrorUs;
            m_RatePhaseCandidateTotalUs = sourceDeltaUs;
            m_RatePhaseCandidateCount = 1;
        }
        else {
            m_RatePhaseErrorUs = nextErrorUs;
            m_RatePhaseCandidateTotalUs += sourceDeltaUs;
            m_RatePhaseCandidateCount++;
        }

        if ((uint64_t)qAbs(m_RatePhaseErrorUs) < baselineUs ||
                m_RatePhaseCandidateCount < MODERATE_CADENCE_MIN_INTERVALS) {
            return 0;
        }

        uint64_t candidateUs = m_RatePhaseCandidateTotalUs /
            (uint64_t)m_RatePhaseCandidateCount;
        uint64_t differenceUs = candidateUs > baselineUs ?
            candidateUs - baselineUs : baselineUs - candidateUs;
        resetRatePhaseTracker();

        // One interval of accumulated drift normally implies a much larger
        // difference than this. The explicit floor keeps rounding noise from
        // causing a phase reset on unusually long candidate windows.
        if (differenceUs * 32 < baselineUs) {
            return 0;
        }
        return qMax(candidateUs, m_NominalFrameIntervalUs);
    }

    void resetRatePhaseTracker()
    {
        m_RatePhaseErrorUs = 0;
        m_RatePhaseCandidateTotalUs = 0;
        m_RatePhaseCandidateCount = 0;
    }

    void resetFasterCadenceCandidate()
    {
        m_FasterCadenceBaselineUs = 0;
        m_FasterCadenceTotalUs = 0;
        m_FasterCadenceCandidateCount = 0;
    }

    void resetSlowerCadenceCandidate()
    {
        m_SlowerCadenceBaselineUs = 0;
        m_SlowerCadenceTotalUs = 0;
        m_SlowerCadenceCandidateCount = 0;
    }

    uint64_t m_NominalFrameIntervalUs;
    uint64_t m_MinFrameIntervalUs;
    uint64_t m_SmoothedIntervalUs;
    uint64_t m_LastSourceTimeUs;
    uint64_t m_LastTargetTimeUs;
    uint64_t m_SourceTimesUs[MAX_SOURCE_TIMES];
    int m_SourceTimeCap;
    int m_SourceTimeHead;
    int m_SourceTimeCount;
    int m_TimestamplessFrames;
    uint64_t m_PendingStallDeltaUs;
    int64_t m_RatePhaseErrorUs;
    uint64_t m_RatePhaseCandidateTotalUs;
    int m_RatePhaseCandidateCount;
    uint64_t m_FasterCadenceBaselineUs;
    uint64_t m_FasterCadenceTotalUs;
    int m_FasterCadenceCandidateCount;
    bool m_FasterCadenceAdopted;
    bool m_FasterCadenceTrusted;
    uint64_t m_SlowerCadenceBaselineUs;
    uint64_t m_SlowerCadenceTotalUs;
    int m_SlowerCadenceCandidateCount;
    bool m_SlowerCadenceAdopted;
    bool m_SlowerCadenceTrusted;
    bool m_PhaseReset;
};

template<typename NowFn, typename SleepUntilFn, typename YieldFn, typename StopFn>
static bool waitForVrrCadenceTargetUs(uint64_t targetUs,
                                      NowFn nowFn,
                                      SleepUntilFn sleepUntilFn,
                                      YieldFn yieldFn,
                                      StopFn stopFn)
{
    while (!stopFn()) {
        uint64_t nowUs = nowFn();
        if (nowUs >= targetUs) {
            return true;
        }

        uint64_t remainingUs = targetUs - nowUs;
        if (remainingUs > 2000) {
            sleepUntilFn(targetUs - 500);
        }
        else {
            yieldFn();
        }
    }

    return false;
}
