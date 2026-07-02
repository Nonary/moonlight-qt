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
    }

    uint64_t nextTargetUs(uint64_t nowUs, uint64_t sourceTimeUs)
    {
        // Track the content cadence as a smoothed average rather than using
        // each raw timestamp delta. A game vsynced on the host quantizes its
        // frame times to whole refresh slots (~87fps on a 120Hz host arrives
        // as alternating 8.3ms/16.7ms deltas); pacing presents by the raw
        // deltas reproduces that alternation 1:1 on the VRR panel, which
        // reads as judder during camera pans. Pacing by the average converts
        // it into a near-even cadence instead. The EMA (1/8 weight) still
        // follows genuine content rate changes within a handful of frames.
        if (m_LastSourceTimeUs != 0 && sourceTimeUs > m_LastSourceTimeUs) {
            uint64_t sourceDeltaUs = sourceTimeUs - m_LastSourceTimeUs;
            if (sourceDeltaUs >= m_NominalFrameIntervalUs / 2 &&
                    sourceDeltaUs <= m_NominalFrameIntervalUs * 4) {
                m_SmoothedIntervalUs =
                    (m_SmoothedIntervalUs * 7 + sourceDeltaUs) / 8;
            }
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
    uint64_t m_NominalFrameIntervalUs;
    uint64_t m_MinFrameIntervalUs;
    uint64_t m_SmoothedIntervalUs;
    uint64_t m_LastSourceTimeUs;
    uint64_t m_LastTargetTimeUs;
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
