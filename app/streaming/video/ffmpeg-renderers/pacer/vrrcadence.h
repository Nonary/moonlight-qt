#pragma once

#include <stdint.h>

// Stale recovery normally uses the display's guarded flip floor. In the
// smooth policy, high-rate content needs a gentler drain: compressing a
// 90-96 FPS cadence all the way to a 120 Hz floor creates a visible speed-up
// and a slowly walking tear line while raster lock is uncertain. Retain the
// aggressive floor for explicit low-latency/A-B modes and low-FPS upshifts.
// The short 90-to-75 FPS blend avoids a target jump when measured cadence
// straddles the policy boundary. Latched presentation keeps its existing
// gentle drain at every rate.
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
            gentleDrainUs == flipSpacingFloorUs) {
        return flipSpacingFloorUs;
    }

    uint64_t fullGentleLimitUs = (minFrameIntervalUs * 4 + 2) / 3;
    if (sourceIntervalUs <= fullGentleLimitUs) {
        return gentleDrainUs;
    }

    uint64_t fullFloorLimitUs = (minFrameIntervalUs * 8 + 4) / 5;
    if (sourceIntervalUs >= fullFloorLimitUs) {
        return flipSpacingFloorUs;
    }

    uint64_t blendWidthUs = fullFloorLimitUs - fullGentleLimitUs;
    uint64_t gentleExtraUs = gentleDrainUs - flipSpacingFloorUs;
    uint64_t blendRemainingUs = fullFloorLimitUs - sourceIntervalUs;
    return flipSpacingFloorUs +
        gentleExtraUs * blendRemainingUs / blendWidthUs;
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
        resetFasterCadenceCandidate();
        m_FasterCadenceAdopted = false;
        m_FasterCadenceTrusted = false;
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
            resetFasterCadenceCandidate();
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
            bool adoptedFasterCadenceThisSample = false;
            uint64_t fasterReferenceUs = m_FasterCadenceCandidateCount != 0 ?
                m_FasterCadenceBaselineUs : m_SmoothedIntervalUs;
            bool fasterCadenceCandidate =
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
                    adoptedFasterCadenceThisSample = true;
                    resetFasterCadenceCandidate();
                }
            }
            else {
                resetFasterCadenceCandidate();
            }

            if (sourceDeltaUs > stallThresholdUs) {
                // A stall supersedes any faster-cadence rebase that may have
                // been detected in an earlier timestamp from the same
                // dropped-frame batch.
                m_FasterCadenceAdopted = false;
                m_FasterCadenceTrusted = false;
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
                    // Bounded at ~22fps: real cutscene/menu cadences live at
                    // 24-30fps (33-42ms), while HOST HITCHES on a struggling
                    // game arrive as similar consecutive 60-80ms gaps every
                    // few seconds (measured 2026-07-06) and were adopted as
                    // a fake 13-16fps "cadence" - the schedule then paced
                    // 60-80ms against ~9ms arrivals until the window
                    // re-warmed, a stale-rush tear chain measured at 30-50%
                    // mid-scan for the duration. Slower than the bound is
                    // always treated as a stall; genuinely sub-22fps content
                    // (loading screens) just presents on arrival via the
                    // stall snap, which is the right behavior for it anyway.
                    m_SmoothedIntervalUs =
                        (m_PendingStallDeltaUs + sourceDeltaUs) / 2;
                    m_PendingStallDeltaUs = 0;
                }
                else {
                    // Genuine stall: restart the window so the gap doesn't
                    // pollute the mean for the next half second.
                    m_SourceTimeCount = 0;
                    m_PendingStallDeltaUs = sourceDeltaUs;
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
            if (!adoptedFasterCadenceThisSample && intervals >= 16) {
                uint64_t oldestUs = m_SourceTimesUs[
                    (m_SourceTimeHead + m_SourceTimeCap - m_SourceTimeCount) % m_SourceTimeCap];
                m_SmoothedIntervalUs = (sourceTimeUs - oldestUs) / (uint64_t)intervals;
            }
            else if (!adoptedFasterCadenceThisSample &&
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
            resetFasterCadenceCandidate();
            m_FasterCadenceAdopted = false;
            m_FasterCadenceTrusted = false;
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
        // a timestamp going backwards on stream restart, or the guarded
        // sustained rate-upshift detector above resets the window. It is also
        // naturally immune to the single-spike EMA rides that the pacer's
        // taper hysteresis was added to absorb.
        observeSourceTime(sourceTimeUs);

        uint64_t targetUs = nowUs;
        bool fasterCadenceAdopted = m_FasterCadenceAdopted;
        m_FasterCadenceAdopted = false;

        if (fasterCadenceAdopted) {
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
        // cadence accepted by the guarded six-interval fast-upshift path.
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
        if (m_FasterCadenceTrusted || m_TimestamplessFrames >= 32) {
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
    // Slowest delta pair adoptable as a real content cadence (~22fps); see
    // the adoption branch in observeSourceTime().
    static const uint64_t MAX_ADOPTABLE_INTERVAL_US = 45000;

    void resetFasterCadenceCandidate()
    {
        m_FasterCadenceBaselineUs = 0;
        m_FasterCadenceTotalUs = 0;
        m_FasterCadenceCandidateCount = 0;
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
    uint64_t m_FasterCadenceBaselineUs;
    uint64_t m_FasterCadenceTotalUs;
    int m_FasterCadenceCandidateCount;
    bool m_FasterCadenceAdopted;
    bool m_FasterCadenceTrusted;
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
