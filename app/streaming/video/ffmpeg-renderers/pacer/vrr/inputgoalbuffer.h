#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace Vrr13 {
// Smallest constant input buffer that meets the client-timing goal over a
// span of adjacent input pairs, scored like IntervalBuffer's severity score:
// a frame is shown at slot + max(B, lateness), and each interval loses
// min(1, (|error| - tolerance) / intended) of its duration. Lateness is input
// (network and decode) readiness relative to the unbuffered slot.
//
// When no buffer meets the goal, choose the smallest buffer with the best
// achievable score.
class InputGoalSolver {
public:
    void clear() { m_Pairs.clear(); m_IntendedUs = 0; }
    void add(int64_t previousLatenessUs, int64_t latenessUs, uint64_t intendedUs, uint64_t toleranceUs) {
        if (!intendedUs) return;
        m_IntendedUs += intendedUs;
        // A pair whose lateness differs by no more than the tolerance is clean
        // at every buffer size, so only the remaining pairs are retained.
        const int64_t a = std::clamp(previousLatenessUs, -kLimitUs, kLimitUs);
        const int64_t b = std::clamp(latenessUs, -kLimitUs, kLimitUs);
        if (uint64_t(std::llabs(b - a)) > toleranceUs) m_Pairs.push_back({a, b, intendedUs});
    }
    size_t jitterPairs() const { return m_Pairs.size(); }
    uint64_t intendedUs() const { return m_IntendedUs; }
    // Fraction of scored time lost at a constant buffer.
    double loss(uint64_t bufferUs, uint64_t toleranceUs) const {
        if (!m_IntendedUs) return 0;
        const int64_t buffer = int64_t(std::min<uint64_t>(bufferUs, kLimitUs));
        double lost = 0;
        for (const auto& p : m_Pairs) {
            const int64_t shift = std::max(buffer, p.current) - std::max(buffer, p.previous);
            const uint64_t error = uint64_t(std::llabs(shift));
            if (error <= toleranceUs) continue;
            const double actual = std::max(0.0, double(int64_t(p.intendedUs) + shift));
            lost += actual * std::min(1.0, double(error - toleranceUs) / double(p.intendedUs));
        }
        return std::min(1.0, lost / double(m_IntendedUs));
    }
    struct Solution { uint64_t bufferUs = 0; bool goalReachable = true; };
    Solution solve(uint64_t probabilityPpm, uint64_t toleranceUs,
                   uint64_t minimumUs, uint64_t maximumUs, uint64_t stepUs) const {
        const double allowed = double(1000000 - std::min<uint64_t>(probabilityPpm, 1000000)) / 1e6;
        stepUs = std::max<uint64_t>(stepUs, 1);
        Solution best{minimumUs, false};
        double bestValue = -std::numeric_limits<double>::infinity();
        for (uint64_t b = minimumUs;; b = std::min(maximumUs, b + stepUs)) {
            const double lost = loss(b, toleranceUs);
            if (lost <= allowed) return {b, true};
            const double value = -100.0 * lost;
            if (value > bestValue) { bestValue = value; best.bufferUs = b; }
            if (b >= maximumUs) break;
        }
        return best;
    }
private:
    static constexpr int64_t kLimitUs = 10000000;
    struct Pair { int64_t previous, current; uint64_t intendedUs; };
    std::vector<Pair> m_Pairs;
    uint64_t m_IntendedUs = 0;
};
}
