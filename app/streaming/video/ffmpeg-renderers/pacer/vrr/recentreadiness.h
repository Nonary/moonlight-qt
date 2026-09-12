#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace Vrr13 {
// Live readiness protection, independent of the five-minute calibration prior.
// Time-bounded 100 ms buckets include successful observations. The default
// retains the historical three-second p99 policy for exact replay.
// Source timing is removed before observation; smoothing advance is an explicit
// additional deadline cost rather than an error in the FIFO source model.
class RecentReadiness {
public:
    explicit RecentReadiness(uint64_t windowUs = 3000000, uint64_t targetPerMillion = 990000)
        : m_WindowUs(std::max<uint64_t>(100000, std::min<uint64_t>(120000000, windowUs))),
          m_TargetPerMillion(std::max<uint64_t>(1, std::min<uint64_t>(1000000, targetPerMillion))),
          m_Buckets(m_WindowUs / BucketUs)
    {}

    void observe(uint64_t raw, uint64_t advance, uint64_t applied, uint64_t at) {
        if (!at || (m_Last && at < m_Last)) return;
        expire(at);
        if (!m_First || (m_Last && at - m_Last >= m_WindowUs)) m_First = at;
        const uint64_t required = std::min<uint64_t>(100000,
            std::min<uint64_t>(100000, raw) + std::min<uint64_t>(100000, advance));
        auto& bucket = m_Buckets[(at / BucketUs) % m_Buckets.size()];
        bucket.tick = at / BucketUs;
        ++bucket.counts[(required + BinUs - 1) / BinUs];
        ++m_Weights[(required + BinUs - 1) / BinUs];
        ++m_Count;
        m_Last = at;
        if (required > applied && required - applied >= 1000) {
            // A new miss renews protection; reading an old tail never does.
            if (!m_LastMiss || at - m_LastMiss >= HoldUs) m_Boost = 0;
            m_Boost = std::max(m_Boost, required);
            m_LastMiss = at;
        }
    }

    uint64_t demand(uint64_t at) {
        expire(at);
        uint64_t accumulated = 0, quantile = 0;
        if (m_Count) {
            const auto rank = m_Count - m_Count * (1000000 - m_TargetPerMillion) / 1000000;
            for (std::size_t i = 0; i < m_Weights.size(); ++i) {
                accumulated += m_Weights[i];
                if (accumulated >= rank) { quantile = i * BinUs; break; }
            }
        }
        return std::max(quantile,
            m_LastMiss && at >= m_LastMiss && at - m_LastMiss < HoldUs ? m_Boost : 0);
    }

    bool canRelease(uint64_t at) const {
        // Silence or an excluded overload episode is not clean evidence.
        return m_Count >= 32 && m_First && at >= m_First && at - m_First >= HoldUs &&
            at >= m_Last && at - m_Last <= 250000 &&
            (!m_LastMiss || (at >= m_LastMiss && at - m_LastMiss >= HoldUs));
    }
private:
    static constexpr uint64_t BucketUs = 100000,
        HoldUs = 2000000, BinUs = 250;
    struct Bucket {
        uint64_t tick = 0;
        std::array<uint32_t, 401> counts{};
    };
    void expire(uint64_t at) {
        const auto tick = at / BucketUs;
        for (auto& bucket : m_Buckets) {
            if (tick >= bucket.tick && tick - bucket.tick >= m_Buckets.size()) {
                for (std::size_t i = 0; i < m_Weights.size(); ++i) {
                    m_Weights[i] -= bucket.counts[i];
                    m_Count -= bucket.counts[i];
                }
                bucket = {};
                bucket.tick = tick;
            }
        }
    }
    uint64_t m_WindowUs, m_TargetPerMillion;
    std::vector<Bucket> m_Buckets;
    std::array<uint64_t, 401> m_Weights{};
    uint64_t m_Count = 0, m_First = 0, m_Last = 0, m_LastMiss = 0, m_Boost = 0;
};
}
