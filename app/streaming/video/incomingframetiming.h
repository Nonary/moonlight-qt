#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>

#include "../../settings/vrrtimingoptions.h"
#include "ffmpeg-renderers/pacer/vrr/intervalscore.h"

// Spacing of adjacent host frames on the 90 kHz RTP clock.
class IncomingFrameTiming
{
public:
    explicit IncomingFrameTiming(VrrTimingOptions options = VrrTimingOptions::preset(1))
        : m_HistoryBuckets(uint64_t(options.historySeconds) * 1000000 / Vrr13::IntervalScoreBucketUs),
          m_ToleranceUs(options.toleranceUs)
    {
    }

    void observe(uint32_t frameNumber, uint32_t rtpTimestamp)
    {
        if (!m_PreviousFrame) {
            m_PreviousFrame = frameNumber;
            m_PreviousTimestamp = rtpTimestamp;
            m_SourceTicks = rtpTimestamp;
            return;
        }
        const uint32_t frames = frameNumber - *m_PreviousFrame;
        if (frames == 0 || frames >= 0x80000000U) return;

        const uint32_t ticks = rtpTimestamp - m_PreviousTimestamp;
        m_PreviousFrame = frameNumber;
        m_PreviousTimestamp = rtpTimestamp;
        if (ticks >= 0x80000000U) {
            // The source clock restarted. Its old history has no age in this epoch.
            m_History.clear();
            m_Count = 0;
            m_SourceTicks = rtpTimestamp;
            return;
        }
        m_SourceTicks += ticks;
        if (frames != 1 || !ticks || ticks >= 90000) {
            commitTail();
            m_Count = 0;
        }
        else {
            if (m_Count == m_Intervals.size()) {
                std::move(m_Intervals.begin() + 1, m_Intervals.end(), m_Intervals.begin());
                --m_Count;
            }
            m_Intervals[m_Count++] = {ticks, currentBucket()};
            if (m_Count >= 3) commit(m_Count - 3);
        }
        while (!m_History.empty() && expired(m_History.front().tick)) m_History.pop_front();
    }

    std::optional<double> score() const
    {
        Totals total;
        for (const auto& bucket : m_History) total.add(bucket.total);
        // Include the latest frames using the preceding cadence until the
        // following two intervals can confirm a rate change.
        for (size_t i = tailStart(); i < m_Count; ++i) {
            if (!expired(m_Intervals[i].bucket)) total.add(evaluate(i));
        }
        if (!total.durationUs) return std::nullopt;
        return 100.0 * (1.0 - std::clamp(total.lossUs / total.durationUs, 0.0, 1.0));
    }

private:
    struct Interval { uint32_t ticks; uint64_t bucket; };
    struct Totals {
        uint64_t durationUs = 0;
        double lossUs = 0;
        void add(Totals other) { durationUs += other.durationUs; lossUs += other.lossUs; }
    };
    struct Bucket { uint64_t tick; Totals total; };

    uint64_t currentBucket() const { return m_SourceTicks / (Vrr13::IntervalScoreBucketUs * 9 / 100); }
    bool expired(uint64_t tick) const { return currentBucket() - tick >= m_HistoryBuckets; }
    size_t tailStart() const { return m_Count > 2 ? m_Count - 2 : 0; }

    Totals evaluate(size_t i) const
    {
        // Two adjacent periods average out a long/short alternation. Estimate
        // cadence independently on either side, excluding the interval under
        // test. At a rate transition, accept the range between those fits.
        const bool before = i >= 2, after = i + 2 < m_Count;
        if (!before && !after) return {};
        const auto periodUs = [this](size_t first) {
            return ((m_Intervals[first].ticks + m_Intervals[first + 1].ticks) * 100ULL + 9) / 18;
        };
        const uint64_t left = periodUs(before ? i - 2 : i + 1);
        const uint64_t right = after ? periodUs(i + 1) : left;
        const uint64_t actual = (m_Intervals[i].ticks * 100ULL + 4) / 9;
        const uint64_t intended = std::clamp(actual, std::min(left, right), std::max(left, right));
        const uint64_t error = std::max(actual, intended) - std::min(actual, intended);
        return {actual, actual * Vrr13::intervalLossFraction(error, intended, m_ToleranceUs)};
    }

    void commit(size_t i)
    {
        const auto weight = evaluate(i);
        if (!weight.durationUs) return;
        const auto tick = m_Intervals[i].bucket;
        if (m_History.empty() || m_History.back().tick != tick) m_History.push_back({tick, {}});
        m_History.back().total.add(weight);
    }

    void commitTail()
    {
        for (size_t i = tailStart(); i < m_Count; ++i) commit(i);
    }

    uint64_t m_HistoryBuckets;
    uint64_t m_ToleranceUs;
    std::optional<uint32_t> m_PreviousFrame;
    uint32_t m_PreviousTimestamp = 0;
    uint64_t m_SourceTicks = 0;
    std::array<Interval, 5> m_Intervals{};
    size_t m_Count = 0;
    std::deque<Bucket> m_History;
};
