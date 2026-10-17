#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>

namespace Vrr13 {
// Clock-rate evidence must not contain decoder-service or adaptive-buffer time.
// Packet/RTP offsets contain the common host/client clock slope plus delivery
// variation. Use each 100 ms bucket's earliest packet, then fit the existing
// 120 s clock window. A material actual-cadence change starts a new fit window;
// cached FPS layers do not qualify or suppress this observer.
class ReceiveDriftClock {
public:
    static constexpr uint64_t WindowUs = 120000000;
    static constexpr uint64_t BucketUs = 100000;
    static constexpr uint64_t FitIntervalUs = 1000000;
    static constexpr uint64_t MinimumSpanUs = 15000000;
    static constexpr int64_t MaximumPpb = 500000;

    int64_t advance(uint64_t nowUs, bool packetValid, int64_t packetOffsetUs,
                    uint64_t sourcePeriodUs, uint64_t epoch,
                    uint64_t materialRateChangePercent) {
        if (!nowUs || (m_LastUs && nowUs <= m_LastUs)) return 0;
        // An unrefreshed clock estimate cannot extrapolate indefinitely through
        // invalid input or continuously changing source regimes.
        if (m_LastFitUs && nowUs - m_LastFitUs >= WindowUs) m_FitPpb = 0;
        const uint64_t elapsed = m_LastUs ? std::min<uint64_t>(nowUs - m_LastUs, 60000000) : 0;
        m_LastUs = nowUs;
        m_Remainder += m_FitPpb * static_cast<int64_t>(elapsed);
        const int64_t drift = m_Remainder / 1000000000;
        m_Remainder -= drift * 1000000000;
        if (!packetValid || !sourcePeriodUs || sourcePeriodUs > 1000000) return drift;

        bool newBasis = !m_HaveEpoch || epoch != m_Epoch;
        if (m_PeriodUs) {
            const uint64_t difference = sourcePeriodUs > m_PeriodUs ?
                sourcePeriodUs - m_PeriodUs : m_PeriodUs - sourcePeriodUs;
            newBasis = newBasis || difference * 100 > m_PeriodUs *
                std::min<uint64_t>(100, materialRateChangePercent);
        }
        if (newBasis) {
            // A producer phase step is not oscillator drift. Keep the prior
            // qualified clock rate while acquiring the new baseline; do not
            // change the accumulated coordinate or any buffer/history state.
            m_Points.clear();
            m_BucketStartUs = 0;
            m_LastAttemptUs = 0;
            m_PeriodUs = sourcePeriodUs;
            m_Epoch = epoch;
            m_HaveEpoch = true;
        }
        if (!m_PeriodUs) m_PeriodUs = sourcePeriodUs;
        if (!m_BucketStartUs) {
            m_BucketStartUs = nowUs;
            m_BucketMinimumUs = packetOffsetUs;
        }
        else if (nowUs - m_BucketStartUs >= BucketUs) {
            // Timestamp the actual observed bucket rather than inventing
            // samples for gaps. Invalid intervals never become clean evidence.
            m_Points.push_back({m_BucketStartUs, m_BucketMinimumUs});
            m_BucketStartUs = nowUs;
            m_BucketMinimumUs = packetOffsetUs;
        }
        else m_BucketMinimumUs = std::min(m_BucketMinimumUs, packetOffsetUs);
        while (!m_Points.empty() && nowUs - m_Points.front().atUs >= WindowUs)
            m_Points.pop_front();
        if (m_LastAttemptUs && nowUs - m_LastAttemptUs < FitIntervalUs) return drift;
        m_LastAttemptUs = nowUs;
        if (m_Points.size() < 50 ||
                m_Points.back().atUs - m_Points.front().atUs < MinimumSpanUs) return drift;

        const auto first = m_Points.front();
        double mx = 0, my = 0;
        for (const auto& p : m_Points) {
            mx += double(p.atUs - first.atUs) / 1e6;
            my += double(p.offsetUs) - double(first.offsetUs);
        }
        mx /= double(m_Points.size()); my /= double(m_Points.size());
        double xx = 0, xy = 0;
        for (const auto& p : m_Points) {
            const double x = double(p.atUs - first.atUs) / 1e6 - mx;
            xx += x * x;
            xy += x * (double(p.offsetUs) - double(first.offsetUs) - my);
        }
        if (xx > 0) {
            m_FitPpb = static_cast<int64_t>(std::llround(std::clamp(
                xy / xx * 1000.0, -double(MaximumPpb), double(MaximumPpb))));
            m_LastFitUs = nowUs;
        }
        return drift;
    }
    int64_t fitPpb() const { return m_FitPpb; }
    uint64_t lastFitUs() const { return m_LastFitUs; }
private:
    struct Point { uint64_t atUs; int64_t offsetUs; };
    std::deque<Point> m_Points;
    uint64_t m_LastUs = 0, m_LastFitUs = 0, m_LastAttemptUs = 0;
    uint64_t m_BucketStartUs = 0, m_PeriodUs = 0, m_Epoch = 0;
    int64_t m_BucketMinimumUs = 0, m_FitPpb = 0, m_Remainder = 0;
    bool m_HaveEpoch = false;
};
}
