#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Vrr13 {
// Smallest constant input buffer that meets the client-timing goal over a
// span of adjacent input pairs, scored like IntervalBuffer's severity score:
// a frame is shown at slot + max(B, lateness), and each interval loses
// min(1, (|error| - tolerance) / intended) of its duration. Lateness is input
// (network and decode) readiness relative to the unbuffered slot.
//
// Loss that remains even at the cap cannot be bought back by any permitted
// buffer, such as a 30 ms delivery stall under a 14 ms cap. It must not spend
// buffer, so the goal applies above that floor: choose the smallest buffer
// within the allowed loss of the best the cap can do. Sustained jitter beyond
// the cap still reaches the cap, because every step below it loses more.
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
        const double unabsorbable = loss(maximumUs, toleranceUs);
        const bool reachable = unabsorbable <= allowed;
        for (uint64_t b = minimumUs;; b = std::min(maximumUs, b + stepUs)) {
            if (b >= maximumUs || loss(b, toleranceUs) <= allowed + unabsorbable) return {b, reachable};
        }
    }
private:
    static constexpr int64_t kLimitUs = 10000000;
    struct Pair { int64_t previous, current; uint64_t intendedUs; };
    std::vector<Pair> m_Pairs;
    uint64_t m_IntendedUs = 0;
};
}
