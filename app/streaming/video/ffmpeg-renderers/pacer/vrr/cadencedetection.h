#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Vrr13 {

// Source-referenced evidence, not a perceptual score or a buffer request.
// Keep separate instances for CPU submissions and identity-matched OS display
// events. A fixed latency cancels; real source cadence changes remain in the
// reference. Never learn a deadband from the disturbances being measured.
class CadenceDetection {
public:
    static constexpr uint64_t BucketUs = 100000;
    static constexpr size_t Buckets = 20;
    static constexpr uint64_t RecurrenceUs = 250000;
    struct Frame {
        uint64_t id = 0, sourceUs = 0, actualUs = 0, observedUs = 0, uncertaintyUs = 0;
        bool valid = false;
    };
    struct Stats {
        uint64_t intervals = 0, deviations = 0, uncertain = 0;
        uint64_t stretches = 0, recurring = 0, reversals = 0;
        uint64_t jerkPairs = 0, coalesced = 0;
        uint64_t sourceDurationUs = 0, worstErrorUs = 0, worstJerkUs = 0;
        uint64_t lastObservedUs = 0, toleranceUs = 0;
        double errorTotalUs = 0;
        double normalizedErrorPercent() const {
            return sourceDurationUs ? 100.0 * errorTotalUs / sourceDurationUs : 0.0;
        }
    };

    void observe(const Frame& f, uint64_t toleranceUs)
    {
        if (m_HaveClock && (f.observedUs < m_LastObservedUs || toleranceUs != m_ToleranceUs)) reset();
        m_HaveClock = true;
        m_LastObservedUs = f.observedUs;
        m_ToleranceUs = toleranceUs;
        if (!f.valid || !f.actualUs || f.observedUs < f.actualUs) { breakSequence(); return; }
        if (m_HavePrevious && f.id <= m_Previous.id) return;
        const auto previous = m_Previous;
        const bool adjacent = m_HavePrevious && f.id - previous.id == 1 &&
            f.sourceUs > previous.sourceUs && f.sourceUs - previous.sourceUs < 1000000 &&
            f.actualUs >= previous.actualUs && f.actualUs - previous.actualUs < 1000000;
        if (!adjacent) breakSequence();
        m_Previous = f;
        m_HavePrevious = true;
        if (!adjacent) return;

        const uint64_t source = f.sourceUs - previous.sourceUs;
        const uint64_t actual = f.actualUs - previous.actualUs;
        const int64_t residual = int64_t(actual) - int64_t(source);
        const uint64_t error = uint64_t(std::abs(residual));
        const uint64_t uncertainty = add(f.uncertaintyUs, previous.uncertaintyUs);
        const uint64_t lower = error - std::min(error, uncertainty);
        const bool deviation = lower > toleranceUs;
        const bool uncertain = !deviation && add(error, uncertainty) > toleranceUs;
        const auto tick = f.observedUs / BucketUs;
        auto& b = m_Buckets[tick % Buckets];
        if (b.tick != tick) b = Bucket{tick};
        ++b.stats.intervals;
        m_LastScoredUs = f.observedUs;
        b.stats.deviations += deviation;
        b.stats.uncertain += uncertain;
        b.stats.coalesced += actual == 0;
        b.stats.sourceDurationUs += source;
        b.stats.errorTotalUs += lower;
        b.stats.worstErrorUs = std::max(b.stats.worstErrorUs, lower);
        if (deviation && residual > 0) {
            ++b.stats.stretches;
            // Count a delayed frame and its catch-up as one stretch event.
            // An isolated impulse cannot by itself establish recurrence.
            if (m_LastStretchUs && f.actualUs >= m_LastStretchUs &&
                f.actualUs - m_LastStretchUs <= RecurrenceUs) ++b.stats.recurring;
            m_LastStretchUs = f.actualUs;
        }
        if (m_HaveResidual) {
            ++b.stats.jerkPairs;
            const uint64_t jerk = uint64_t(std::abs(residual - m_Residual));
            // The shared middle timestamp has coefficient two:
            // u[i] + 2*u[i-1] + u[i-2], not just the two endpoints.
            const uint64_t jerkUncertainty = add(uncertainty, m_ResidualUncertainty);
            const uint64_t certainJerk = jerk - std::min(jerk, jerkUncertainty);
            b.stats.worstJerkUs = std::max(b.stats.worstJerkUs, certainJerk);
            b.stats.reversals += deviation && m_PreviousDeviation &&
                ((residual < 0) != (m_Residual < 0));
        }
        m_Residual = residual;
        m_ResidualUncertainty = uncertainty;
        m_PreviousDeviation = deviation;
        m_HaveResidual = true;
    }

    Stats stats() const
    {
        Stats s;
        s.lastObservedUs = m_LastScoredUs;
        s.toleranceUs = m_ToleranceUs;
        const auto tick = m_LastObservedUs / BucketUs;
        for (const auto& b : m_Buckets) {
            if (b.tick > tick || tick - b.tick >= Buckets) continue;
            s.intervals += b.stats.intervals;
            s.deviations += b.stats.deviations;
            s.uncertain += b.stats.uncertain;
            s.stretches += b.stats.stretches;
            s.recurring += b.stats.recurring;
            s.reversals += b.stats.reversals;
            s.jerkPairs += b.stats.jerkPairs;
            s.coalesced += b.stats.coalesced;
            s.sourceDurationUs += b.stats.sourceDurationUs;
            s.errorTotalUs += b.stats.errorTotalUs;
            s.worstErrorUs = std::max(s.worstErrorUs, b.stats.worstErrorUs);
            s.worstJerkUs = std::max(s.worstJerkUs, b.stats.worstJerkUs);
        }
        return s;
    }
    void breakSequence() {
        m_HavePrevious = m_HaveResidual = m_PreviousDeviation = false;
        m_LastStretchUs = 0;
    }
    void reset() { *this = CadenceDetection{}; }
private:
    static uint64_t add(uint64_t a, uint64_t b) {
        return a + std::min(b, std::numeric_limits<uint64_t>::max() - a);
    }
    struct Bucket { uint64_t tick = 0; Stats stats; };
    std::array<Bucket, Buckets> m_Buckets{};
    Frame m_Previous;
    int64_t m_Residual = 0;
    uint64_t m_LastObservedUs = 0, m_LastScoredUs = 0, m_ToleranceUs = 0;
    uint64_t m_LastStretchUs = 0, m_ResidualUncertainty = 0;
    bool m_HaveClock = false, m_HavePrevious = false, m_HaveResidual = false, m_PreviousDeviation = false;
};
}
