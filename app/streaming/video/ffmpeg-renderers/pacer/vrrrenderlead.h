#pragma once

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdint>

// Keeps enough render lead to cover the recurring tail without turning a
// single hitch into seconds of standing presentation latency. A genuine miss
// still attacks immediately; ordinary recovery follows a short clean-sample
// percentile and therefore forgets isolated spikes quickly.
class VrrRenderLeadController
{
public:
    struct Update {
        uint64_t marginUs;
        uint64_t tailOvershootUs;
        bool lateSample;
    };

    explicit VrrRenderLeadController(uint64_t initialMarginUs = 4000) :
        m_MarginUs(qBound(kMarginFloorUs, initialMarginUs, kMarginCeilingUs)),
        m_Head(0),
        m_Count(0)
    {
    }

    void resetSamples()
    {
        m_Head = 0;
        m_Count = 0;
    }

    Update observe(int64_t overshootUs, uint64_t sourceIntervalUs,
                   bool clean)
    {
        if (!clean) {
            return { m_MarginUs, tailOvershootUs(sourceIntervalUs), false };
        }

        int32_t boundedOvershootUs = (int32_t)qBound(
            (int64_t)INT32_MIN, overshootUs, (int64_t)INT32_MAX);
        m_Samples[m_Head] = boundedOvershootUs;
        m_Head = (m_Head + 1) % kMaximumSamples;
        if (m_Count < kMaximumSamples) {
            m_Count++;
        }

        uint64_t tailUs = tailOvershootUs(sourceIntervalUs);
        uint64_t targetUs = qBound(
            kMarginFloorUs, tailUs + kTailSlackUs, kMarginCeilingUs);
        bool lateSample = overshootUs > (int64_t)m_MarginUs;

        // A miss means the current guard was disproven. Protect the next
        // frame immediately, while allowing this isolated sample to age out
        // of the robust tail instead of pinning the guard for many seconds.
        if (lateSample && overshootUs > 0) {
            uint64_t missTargetUs = qBound(
                kMarginFloorUs,
                (uint64_t)overshootUs + kTailSlackUs,
                kMarginCeilingUs);
            targetUs = qMax(targetUs, missTargetUs);
        }

        if (targetUs > m_MarginUs) {
            uint64_t attackStepUs = lateSample ? targetUs - m_MarginUs :
                qBound((uint64_t)100,
                       kAttackPerSecondUs * normalizedIntervalUs(sourceIntervalUs) /
                           1000000ULL,
                       (uint64_t)500);
            m_MarginUs += qMin(targetUs - m_MarginUs, attackStepUs);
        }
        else if (targetUs < m_MarginUs) {
            uint64_t releaseStepUs = qMax(
                (uint64_t)1,
                kReleasePerSecondUs * normalizedIntervalUs(sourceIntervalUs) /
                    1000000ULL);
            m_MarginUs -= qMin(m_MarginUs - targetUs, releaseStepUs);
        }

        return { m_MarginUs, tailUs, lateSample };
    }

    uint64_t marginUs() const
    {
        return m_MarginUs;
    }

private:
    static constexpr int kMaximumSamples = 480;
    static constexpr uint64_t kWindowUs = 2000000;
    static constexpr uint64_t kDefaultIntervalUs = 10000;
    static constexpr uint64_t kMarginFloorUs = 1000;
    static constexpr uint64_t kMarginCeilingUs = 6500;
    static constexpr uint64_t kTailSlackUs = 500;
    static constexpr uint64_t kAttackPerSecondUs = 25000;
    static constexpr uint64_t kReleasePerSecondUs = 2500;
    static constexpr int kTailPercentile = 98;

    static uint64_t normalizedIntervalUs(uint64_t sourceIntervalUs)
    {
        return sourceIntervalUs != 0 ? sourceIntervalUs : kDefaultIntervalUs;
    }

    int activeSampleCount(uint64_t sourceIntervalUs) const
    {
        if (m_Count == 0) {
            return 0;
        }
        uint64_t intervalUs = normalizedIntervalUs(sourceIntervalUs);
        uint64_t requested = kWindowUs / intervalUs;
        int windowSamples = qBound(
            16, (int)qMin(requested, (uint64_t)kMaximumSamples),
            kMaximumSamples);
        return qMin(windowSamples, m_Count);
    }

    uint64_t tailOvershootUs(uint64_t sourceIntervalUs) const
    {
        int activeCount = activeSampleCount(sourceIntervalUs);
        if (activeCount <= 0) {
            return 0;
        }

        std::array<int32_t, kMaximumSamples> ordered = {};
        int oldest = (m_Head - activeCount + kMaximumSamples) %
            kMaximumSamples;
        for (int i = 0; i < activeCount; i++) {
            ordered[i] = m_Samples[(oldest + i) % kMaximumSamples];
        }
        int percentileIndex = (activeCount - 1) * kTailPercentile / 100;
        std::nth_element(ordered.begin(), ordered.begin() + percentileIndex,
                         ordered.begin() + activeCount);
        return ordered[percentileIndex] > 0 ?
            (uint64_t)ordered[percentileIndex] : 0;
    }

    std::array<int32_t, kMaximumSamples> m_Samples = {};
    uint64_t m_MarginUs;
    int m_Head;
    int m_Count;
};
