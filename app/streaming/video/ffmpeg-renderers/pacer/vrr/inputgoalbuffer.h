#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Vrr13 {
// Smallest constant input buffer that meets the client-timing goal over a
// span of adjacent input pairs. A frame is shown at slot + max(B, lateness),
// and each interval loses min(1, (|error| - tolerance) / intended) of one
// source interval. Lateness is input (network and decode) readiness relative
// to the unbuffered slot.
//
// IntervalBuffer's reported score weights the same fraction by the interval's
// presented duration, so a freeze counts for its whole length. Sizing must
// not: every millisecond of buffer shortens a 30 ms freeze by a millisecond
// and removes none of it, and that gradient took Smooth from 1 ms to as much
// as 10.75 ms after each stall beyond its 17 ms cap, for the same seven
// hitches. An interval here can lose at most itself, so one that is wholly
// lost with or without the buffer asks for nothing.
//
// Loss that remains even at the cap cannot be bought back by any permitted
// buffer either. The goal applies to what the buffer can still absorb: choose
// the smallest buffer whose loss is within the allowance of the best the cap
// can do. Sustained jitter beyond the cap still reaches the cap, because every
// step below it loses more.
//
// A loss rate needs several late frames. Over a window that has only begun to
// fill, the allowance is a few milliseconds and one late frame exceeded it: a
// single 6 ms frame 8 s into a session took Smooth to 4.25 ms where the same
// frame at 150 s moved nothing. While fewer than RatePairs intervals lose at a
// candidate buffer, the allowance is the caller's budget, a part of the whole
// window. Sustained lateness loses on many intervals and is held to its rate
// from the first second.
class InputGoalSolver {
public:
    // Four late frames, each a stretched and a compressed interval.
    static constexpr size_t RatePairs = 8;
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
    // Source-interval time lost at a constant buffer.
    double lostUs(uint64_t bufferUs, uint64_t toleranceUs) const {
        const int64_t buffer = int64_t(std::min<uint64_t>(bufferUs, kLimitUs));
        double lost = 0;
        for (const auto& p : m_Pairs) lost += double(lostUs(p, buffer, toleranceUs));
        return lost;
    }
    // The same loss as a fraction of the scored time.
    double loss(uint64_t bufferUs, uint64_t toleranceUs) const {
        return m_IntendedUs ? std::min(1.0, lostUs(bufferUs, toleranceUs) / double(m_IntendedUs)) : 0;
    }
    // bufferUs meets probabilityPpm. requiredUs is the smallest buffer that
    // meets goalProbabilityPpm, a goal no stricter than that; it is bufferUs
    // when no separate goal is given.
    struct Solution { uint64_t bufferUs = 0; bool goalReachable = true; uint64_t requiredUs = 0; };
    // budgetUs is the scored time the allowance covers while too few intervals
    // lose to establish a rate. Zero, or less than the evidence, uses the
    // evidence as it stands.
    Solution solve(uint64_t probabilityPpm, uint64_t toleranceUs,
                   uint64_t minimumUs, uint64_t maximumUs, uint64_t stepUs,
                   uint64_t budgetUs = 0, uint64_t goalProbabilityPpm = 0) const {
        const double fraction = double(1000000 - std::min<uint64_t>(probabilityPpm, 1000000)) / 1e6;
        const double rateAllowed = fraction * double(m_IntendedUs);
        const double budgetAllowed = fraction * double(std::max(budgetUs, m_IntendedUs));
        const double goalScale = goalProbabilityPpm && fraction > 0 ? std::max(1.0,
            double(1000000 - std::min<uint64_t>(goalProbabilityPpm, 1000000)) / 1e6 / fraction) : 1.0;
        bool haveRequired = false;
        uint64_t requiredUs = 0;
        stepUs = std::max<uint64_t>(stepUs, 1);
        const int64_t cap = int64_t(std::min<uint64_t>(maximumUs, kLimitUs));
        m_AtCap.resize(m_Pairs.size());
        double unabsorbable = 0;
        for (size_t i = 0; i < m_Pairs.size(); ++i) {
            m_AtCap[i] = lostUs(m_Pairs[i], cap, toleranceUs);
            unabsorbable += double(m_AtCap[i]);
        }
        const bool reachable = unabsorbable <= rateAllowed;
        for (uint64_t b = minimumUs;; b = std::min(maximumUs, b + stepUs)) {
            if (b >= maximumUs) return {b, reachable, haveRequired ? requiredUs : b};
            const int64_t buffer = int64_t(std::min<uint64_t>(b, kLimitUs));
            // Loss the cap would remove, and the intervals it comes from.
            double absorbable = 0;
            size_t pairs = 0;
            for (size_t i = 0; i < m_Pairs.size(); ++i) {
                const uint64_t here = lostUs(m_Pairs[i], buffer, toleranceUs);
                if (here > m_AtCap[i]) { absorbable += double(here - m_AtCap[i]); ++pairs; }
            }
            const double allowed = pairs >= RatePairs ? rateAllowed : budgetAllowed;
            if (!haveRequired && absorbable <= allowed * goalScale) { requiredUs = b; haveRequired = true; }
            if (absorbable <= allowed) return {b, reachable, requiredUs};
        }
    }
private:
    static constexpr int64_t kLimitUs = 10000000;
    struct Pair { int64_t previous, current; uint64_t intendedUs; };
    static uint64_t lostUs(const Pair& p, int64_t buffer, uint64_t toleranceUs) {
        const uint64_t error = uint64_t(std::llabs(std::max(buffer, p.current) - std::max(buffer, p.previous)));
        return error > toleranceUs ? std::min(error - toleranceUs, p.intendedUs) : 0;
    }
    std::vector<Pair> m_Pairs;
    mutable std::vector<uint64_t> m_AtCap; // solve() scratch
    uint64_t m_IntendedUs = 0;
};
}
