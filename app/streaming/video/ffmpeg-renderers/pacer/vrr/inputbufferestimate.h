#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace Vrr13 {
// Fixed-buffer input-delivery model, independent of the buffer used when the
// observations were recorded. It does not resimulate worker admission, GPU
// contention, compositor scheduling or physical scanout.
class InputBufferEstimate {
public:
    // Calibrate modeled miss allowance against actual selected-window outcomes.
    // A deficit tightens the allowance continuously, never to zero; meeting
    // the goal restores exactly the user's allowance. Products fit uint64_t
    // for the bounded 262144-frame history and supported probabilities.
    static uint64_t calibratedProbability(uint64_t probability, uint64_t samples, uint64_t misses) {
        probability = std::clamp<uint64_t>(probability, 1, 999999);
        const uint64_t tail = 1000000 - probability;
        if (!samples || misses * 1000000 <= samples * tail) return probability;
        const uint64_t correctedTail = tail * tail * samples / (misses * 1000000);
        return 1000000 - std::max<uint64_t>(1, correctedTail);
    }
    void clear() { m_Thresholds.clear(); m_Quantile = minimum(); m_CoverageUs = 0; }
    template<class Samples>
    void evaluate(const Samples& samples, uint64_t probabilityPpm, uint64_t toleranceUs) {
        clear();
        std::vector<int64_t> values;
        values.reserve(samples.size());
        bool previousValid = false;
        uint64_t previousSlot = 0, previousEpoch = 0;
        int64_t carry = 0;
        for (const auto& sample : samples) {
            if (sample.seed) { previousValid = false; continue; }
            const bool adjacent = previousValid && sample.epoch == previousEpoch &&
                sample.slot > previousSlot && sample.slot - previousSlot < 1000000;
            if (adjacent) {
                // At constant buffer B, lateness is max(0, carry-B). A
                // readiness-caused interval stretch >T is possible exactly
                // when the new requirement exceeds the previous carry by T.
                // The buffer needed to absorb that event is requirement-T.
                // Actual same-frame input hitches are stronger evidence
                // than this simplified recovery model. Never let modeled
                // carry hide protection a proven hitch actually needed.
                const bool event = sample.missed || (sample.required > carry &&
                    uint64_t(sample.required) - uint64_t(carry) > toleranceUs);
                const int64_t threshold = event ? sample.required : minimum();
                m_Thresholds.push_back({sample.at, threshold});
                values.push_back(threshold);
                const uint64_t interval = sample.slot - previousSlot;
                m_CoverageUs += interval;
                // The caller supplies bounded recovery room, not a promise
                // that a whole physical refresh gap is free correction.
                carry = std::max(sample.required,
                    subtract(carry, int64_t(std::min(sample.recoveryUs, interval))));
            } else carry = sample.required;
            previousValid = true;
            previousSlot = sample.slot;
            previousEpoch = sample.epoch;
        }
        if (values.empty()) return;
        probabilityPpm = std::clamp<uint64_t>(probabilityPpm, 1, 999999);
        const size_t rank = size_t((probabilityPpm * values.size() + 999999) / 1000000);
        std::nth_element(values.begin(), values.begin() + rank - 1, values.end());
        m_Quantile = values[rank - 1];
    }
    size_t samples() const { return m_Thresholds.size(); }
    uint64_t coverageUs() const { return m_CoverageUs; }
    uint64_t requiredUs(int64_t clockOffsetUs, int64_t budgetUs, uint64_t toleranceUs) const {
        return project(m_Quantile, clockOffsetUs, budgetUs, toleranceUs);
    }
    uint64_t missesAt(uint64_t candidateUs, uint64_t atUs, uint64_t windowUs,
                      int64_t clockOffsetUs, int64_t budgetUs, uint64_t toleranceUs) const {
        uint64_t misses = 0;
        for (auto i = m_Thresholds.rbegin(); i != m_Thresholds.rend(); ++i) {
            if (atUs >= i->at && atUs - i->at >= windowUs) break;
            if (i->at <= atUs)
                misses += project(i->required, clockOffsetUs, budgetUs, toleranceUs) > candidateUs;
        }
        return misses;
    }
private:
    static constexpr int64_t minimum() { return std::numeric_limits<int64_t>::min(); }
    static int64_t subtract(int64_t value, int64_t offset) {
        constexpr auto lo = std::numeric_limits<int64_t>::min();
        constexpr auto hi = std::numeric_limits<int64_t>::max();
        if (offset > 0 && value < lo + offset) return lo;
        if (offset < 0 && value > hi + offset) return hi;
        return value - offset;
    }
    static uint64_t project(int64_t raw, int64_t offset, int64_t budget, uint64_t tolerance) {
        if (raw == minimum()) return 0;
        const int64_t relative = subtract(subtract(raw, offset), budget);
        const uint64_t required = relative > 0 ? uint64_t(relative) : 0;
        return required - std::min(required, tolerance);
    }
    struct Threshold { uint64_t at; int64_t required; };
    std::vector<Threshold> m_Thresholds;
    int64_t m_Quantile = minimum();
    uint64_t m_CoverageUs = 0;
};
}
