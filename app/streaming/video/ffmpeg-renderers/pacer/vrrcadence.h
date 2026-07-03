#pragma once

#include <stdint.h>

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
        // The windowed mean pairs every gap with its burst; only a genuine
        // stall (>4x nominal, or a timestamp going backwards on a stream
        // restart) resets the window. It is also naturally immune to the
        // single-spike EMA rides that the pacer's taper hysteresis was added
        // to absorb.
        if (m_LastSourceTimeUs != 0 && sourceTimeUs > m_LastSourceTimeUs) {
            uint64_t sourceDeltaUs = sourceTimeUs - m_LastSourceTimeUs;
            if (sourceDeltaUs > m_NominalFrameIntervalUs * 4) {
                // Genuine stall: restart the window so the gap doesn't
                // pollute the mean for the next half second.
                m_SourceTimeCount = 0;
            }

            m_SourceTimesUs[m_SourceTimeHead] = sourceTimeUs;
            m_SourceTimeHead = (m_SourceTimeHead + 1) % m_SourceTimeCap;
            if (m_SourceTimeCount < m_SourceTimeCap) {
                m_SourceTimeCount++;
            }

            int intervals = m_SourceTimeCount - 1;
            if (intervals >= 16) {
                uint64_t oldestUs = m_SourceTimesUs[
                    (m_SourceTimeHead + m_SourceTimeCap - m_SourceTimeCount) % m_SourceTimeCap];
                m_SmoothedIntervalUs = (sourceTimeUs - oldestUs) / (uint64_t)intervals;
            }
            else if (sourceDeltaUs >= m_NominalFrameIntervalUs / 2 &&
                     sourceDeltaUs <= m_NominalFrameIntervalUs * 4) {
                // Warmup fallback until the window fills: the old EMA. Its
                // bias is immaterial over a handful of frames.
                m_SmoothedIntervalUs =
                    (m_SmoothedIntervalUs * 7 + sourceDeltaUs) / 8;
            }
        }
        else if (m_LastSourceTimeUs != 0 && sourceTimeUs != 0 &&
                 sourceTimeUs <= m_LastSourceTimeUs) {
            // Non-monotonic timestamps (stream restart): restart the window.
            m_SourceTimeCount = 0;
        }

        uint64_t targetUs = nowUs;

        if (m_LastTargetTimeUs != 0) {
            uint64_t frameIntervalUs = m_SmoothedIntervalUs;

            targetUs = m_LastTargetTimeUs + frameIntervalUs;

            if (targetUs + frameIntervalUs < nowUs) {
                targetUs = nowUs;
            }

            // Applies to both the normal path above and the catch-up reset just
            // above it - neither considers the display's max refresh rate on its
            // own, so clamp the result here regardless of which path produced it.
            if (targetUs < m_LastTargetTimeUs + m_MinFrameIntervalUs) {
                targetUs = m_LastTargetTimeUs + m_MinFrameIntervalUs;
            }
        }

        m_LastTargetTimeUs = targetUs;
        if (sourceTimeUs != 0) {
            m_LastSourceTimeUs = sourceTimeUs;
        }

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

private:
    static const int MAX_SOURCE_TIMES = 128;

    uint64_t m_NominalFrameIntervalUs;
    uint64_t m_MinFrameIntervalUs;
    uint64_t m_SmoothedIntervalUs;
    uint64_t m_LastSourceTimeUs;
    uint64_t m_LastTargetTimeUs;
    uint64_t m_SourceTimesUs[MAX_SOURCE_TIMES];
    int m_SourceTimeCap;
    int m_SourceTimeHead;
    int m_SourceTimeCount;
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
