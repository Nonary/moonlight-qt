#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace Vrr13 {
// A rolling thirty-second outcome window, with 100 ms bucket resolution.
// Counts are snapshots, never cumulative counters to be summed across windows.
class ReadinessWindow {
public:
    struct Snapshot {
        uint64_t atUs = 0;
        uint64_t samples = 0;
        uint64_t misses = 0;
        uint64_t over1ms = 0;
        uint64_t over2ms = 0;
        uint64_t dropped = 0;
    };

    void record(uint64_t atUs, uint64_t latenessUs, bool dropped)
    {
        if (!atUs) return;
        // Producers on different threads can reach the lock in reverse order.
        m_LastUs = std::max(m_LastUs, atUs);
        const uint64_t tick = m_LastUs / BucketUs;
        auto& bucket = m_Buckets[tick % m_Buckets.size()];
        if (bucket.tick != tick) bucket = Bucket{tick, {}};
        ++bucket.counts.samples;
        bucket.counts.misses += dropped || latenessUs != 0;
        // A dropped frame is a miss, but has no measured lateness magnitude.
        bucket.counts.over1ms += !dropped && latenessUs > 1000;
        bucket.counts.over2ms += !dropped && latenessUs > 2000;
        bucket.counts.dropped += dropped;
    }

    Snapshot snapshot(uint64_t nowUs = 0) const
    {
        Snapshot result;
        if (!m_LastUs) return result;
        result.atUs = std::max(nowUs, m_LastUs);
        const auto tick = result.atUs / BucketUs;
        for (const auto& bucket : m_Buckets) {
            if (tick >= bucket.tick && tick - bucket.tick < m_Buckets.size()) {
                result.samples += bucket.counts.samples;
                result.misses += bucket.counts.misses;
                result.over1ms += bucket.counts.over1ms;
                result.over2ms += bucket.counts.over2ms;
                result.dropped += bucket.counts.dropped;
            }
        }
        return result;
    }

private:
    static constexpr uint64_t BucketUs = 100000;
    struct Bucket { uint64_t tick = 0; Snapshot counts; };
    std::array<Bucket, 300> m_Buckets{};
    uint64_t m_LastUs = 0;
};
}
