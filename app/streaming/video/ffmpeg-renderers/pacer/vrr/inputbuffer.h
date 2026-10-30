#pragma once
#include "inputgoalbuffer.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>

namespace Vrr13 {
// Learns an input buffer from source-relative readiness and the selected CPU
// interval-quality goal. requiredUs is the causal offset from the input slot;
// slotUs establishes adjacency. The caller excludes packet loss and local
// backlog that cannot be corrected by buffering. Coverage reports actual
// input-caused hitches separately from the solver's interval-quality estimate.
// Retained samples use a source-coordinate clock offset so mapping movement
// does not turn into artificial input jitter.
class InputGoalBuffer {
public:
    // Compatibility only: raw stage misses no longer arm a hard hold.
    static constexpr uint64_t BufferMissHoldUs = 0;
    static constexpr uint64_t ReleaseFeedbackWindowUs = 1000000;
    static constexpr uint64_t MinimumWindowUs = 10000000;
    static constexpr uint64_t MaximumWindowUs = 300000000;
    static constexpr size_t MaximumSamples = 262144;
    struct Stats {
        uint64_t targetUs = 0, currentUs = 0, samples = 0, minimumUs = 0, maximumUs = 0;
        // Session totals; retained names preserve existing trace/test callers.
        uint64_t observed = 0, eligible = 0, ineligible = 0, absorbed = 0, missed = 0;
        uint64_t cumulativeAbsorbed = 0, cumulativeMissed = 0;
        uint64_t lastBufferMissUs = 0;
        uint64_t lastNetworkMissUs = 0, lastDecoderMissUs = 0;
        uint64_t cumulativeNetworkMisses = 0, cumulativeDecoderMisses = 0;
        uint64_t recentReleaseSamples = 0, recentReleaseMisses = 0;
        uint64_t effectiveFallRateUsPerSecond = 0;
        // Outcomes use each frame's actual buffer and tolerance; the
        // controller supplies session-wide reporting across rate layers.
        uint64_t recentEligible = 0, recentAbsorbed = 0, recentMissed = 0;
        // Reporting only: recent responsiveness alongside the full learning history.
        uint64_t reportingWindowUs = 0, reportingEligible = 0;
        uint64_t reportingAbsorbed = 0, reportingMissed = 0;
        uint64_t raises = 0, lowers = 0, epochResets = 0;
        uint64_t observationSpanUs = 0, validPairCoverageUs = 0;
        uint64_t probabilityPpm = 0, windowUs = 0, toleranceUs = 0, vrrHeadroomUs = 0;
        int64_t readinessBudgetUs = 0, clockOffsetUs = 0;
        uint64_t headroomTargetUnclippedUs = 0;
        bool tailUncertain = true, evidenceValid = false;
        bool historyTruncated = false, expiredUnknown = false, clipped = false;
        bool initialized = false, lastInputEligible = false, growthBlocked = false;
        int64_t lastInputRequiredUs = 0;
        uint64_t lastInputObservationUs = 0;
    };
    // Activation/source epoch should seed current from the applied buffer.
    void reset(uint64_t current = 0) {
        *this = InputGoalBuffer{};
        m_Stats.currentUs = m_Stats.targetUs = current;
        m_Stats.initialized = true;
    }
    // Select the smallest buffer that meets the goal over the requested window,
    // or the smallest buffer with the best achievable score. Aim inside the
    // allowed loss so that reacting to a dip can still meet the profile goal.
    void setGoalSolver(uint64_t windowUs, uint64_t tailPerMille = 1000) {
        m_GoalWindowUs = windowUs; m_GoalTailPerMille = std::clamp<uint64_t>(tailPerMille, 1, 1000);
    }
    bool layerTransitionActive() const { return m_LayerTransitionActive; }
    bool fastLayerDrain() const { return m_LayerTransitionActive && m_LayerFastDrain; }
    // A changed source phase, cadence, or normalized-input basis invalidates
    // prior requirements. Retain current protection while relearning its size.
    void breakSequence() {
        m_Samples.clear();
        m_Stats.targetUs = m_Stats.currentUs;
        m_Stats.samples = m_Stats.observationSpanUs = m_Stats.validPairCoverageUs = 0;
        m_Stats.recentEligible = m_Stats.recentAbsorbed = m_Stats.recentMissed = 0;
        updateReleaseFeedback(m_LastClockUs);
        m_Stats.headroomTargetUnclippedUs = m_Stats.currentUs;
        m_Stats.tailUncertain = true;
        m_Stats.evidenceValid = m_Stats.historyTruncated = m_Stats.clipped = false;
        ++m_Stats.epochResets;
        m_LastEvaluationUs = m_LastTruncatedUs = m_ReleaseFraction = 0;
        m_PreviousValid = false;
        m_Direction = 0;
        m_LayerTransitionActive = false;
        ++m_Epoch;
        // Keep elapsed/last-eligible clocks and diagnostics: recurring phase
        // breaks must not rearm the unknown-evidence retention window forever.
    }
    // A source phase discontinuity keeps the slot basis: input lateness keeps
    // its level across it (2026-10-10 22:55 capture: median 3.10 -> 3.16 ms).
    // Only frame adjacency breaks; evidence and coverage outcomes are kept.
    void breakAdjacency() {
        m_PreviousValid = false;
        ++m_Epoch;
    }
    // A new rate layer starts from a copy of its neighbour's evidence. The
    // copied requirements seed its target; their outcomes belonged to the
    // other rate, so they leave this layer's coverage.
    void markEvidenceAsSeed() {
        for (auto& sample : m_Samples) {
            if (sample.seed) continue;
            --m_Stats.recentEligible;
            m_Stats.recentAbsorbed -= sample.absorbed;
            m_Stats.recentMissed -= sample.missed;
            sample.seed = true;
            sample.absorbed = sample.missed = false;
        }
    }
    // Session totals and stage diagnostics continue across rate-band layers;
    // evidence, protection and coverage outcomes stay with each band.
    void continueTotalsFrom(const InputGoalBuffer& previous) {
        const auto& s = previous.m_Stats;
        m_Stats.observed = s.observed; m_Stats.eligible = s.eligible; m_Stats.ineligible = s.ineligible;
        m_Stats.absorbed = s.absorbed; m_Stats.missed = s.missed;
        m_Stats.cumulativeNetworkMisses = s.cumulativeNetworkMisses;
        m_Stats.cumulativeDecoderMisses = s.cumulativeDecoderMisses;
        m_Stats.lastBufferMissUs = s.lastBufferMissUs;
        m_Stats.lastNetworkMissUs = s.lastNetworkMissUs;
        m_Stats.lastDecoderMissUs = s.lastDecoderMissUs;
        m_Stats.raises = s.raises; m_Stats.lowers = s.lowers;
        m_Stats.epochResets = s.epochResets;
    }
    // Source-rate change that gained VRR headroom: protection learned at the
    // old rate overstates what the new rate needs by exactly that gain.
    void creditHeadroom(uint64_t gainedUs) {
        if (!m_Stats.initialized || !gainedUs) return;
        const auto reduce = [&](uint64_t value) {
            return std::max(m_Stats.minimumUs, value - std::min(value, gainedUs));
        };
        m_Stats.currentUs = reduce(m_Stats.currentUs);
        m_Stats.targetUs = reduce(m_Stats.targetUs);
        m_ReleaseFraction = 0;
    }
    // Source-rate change that lost VRR headroom: the retained evidence already
    // sets the target at the new rate (call after advance()), so step to it at
    // once, by at most the loss, instead of rising at 1 ms/s and missing every
    // frame meanwhile. The step coincides with the cadence change.
    void raiseForHeadroomLoss(uint64_t lostUs) {
        if (!m_Stats.initialized || !lostUs) return;
        const uint64_t raised = std::min({m_Stats.targetUs, m_Stats.maximumUs,
            m_Stats.currentUs + std::min(lostUs, m_Stats.maximumUs)});
        if (raised <= m_Stats.currentUs) return;
        m_Stats.currentUs = raised;
        ++m_Stats.raises;
        m_ReleaseFraction = 0;
    }
    // Selecting a layer restores its learned buffer, but only a substantial
    // increase may jump. A decrease keeps the applied buffer and its learned
    // destination while normal release drains it. Recorded policy 9 instead
    // spends at most T per fresh source frame.
    void transitionFrom(uint64_t previousUs) {
        const uint64_t learnedUs = m_Stats.currentUs;
        previousUs = std::max(previousUs, m_Stats.minimumUs);
        m_LayerTransitionActive = false;
        if (learnedUs == previousUs ||
            (learnedUs > previousUs && learnedUs - previousUs > m_Stats.toleranceUs)) return;
        m_LayerTransitionTargetUs = learnedUs;
        m_LayerTransitionActive = true;
        m_LayerFastDrain = previousUs > learnedUs && previousUs - learnedUs > m_Stats.toleranceUs;
        m_Stats.currentUs = previousUs;
        m_Stats.targetUs = learnedUs;
        m_ReleaseFraction = 0;
    }
    void prepareForLayerStorage() {
        if (m_LayerTransitionActive) {
            // A draining transition is still carrying the previous layer's
            // excess; do not save that excess as this layer's learned queue.
            m_Stats.currentUs = m_Stats.targetUs = m_LayerTransitionTargetUs;
            m_LayerTransitionActive = false;
        }
    }
    uint64_t demand(uint64_t applied) const {
        return m_Stats.initialized ? m_Stats.currentUs : applied;
    }
    Stats stats() const {
        auto result = m_Stats;
        result.cumulativeAbsorbed = result.absorbed;
        result.cumulativeMissed = result.missed;
        return result;
    }
    // Raw graph-stage misses remain cumulative diagnostics. Recent miss counts
    // use eligible causal outcomes; goal pressure also gates quantile-driven growth.
    void noteBufferMiss(uint64_t atUs, bool networkMiss, bool decoderMiss) {
        if (!atUs || (!networkMiss && !decoderMiss)) return;
        if (networkMiss) {
            ++m_Stats.cumulativeNetworkMisses;
            m_Stats.lastNetworkMissUs = std::max(m_Stats.lastNetworkMissUs, atUs);
        }
        if (decoderMiss) {
            ++m_Stats.cumulativeDecoderMisses;
            m_Stats.lastDecoderMissUs = std::max(m_Stats.lastDecoderMissUs, atUs);
        }
        m_Stats.lastBufferMissUs = std::max(m_Stats.lastBufferMissUs, atUs);
    }
    // Decision-time aging: no input observation or continuity changes.
    // Unknown current-frame capacity permits release, never another rise.
    // The full overload applies profile changes before demand() is consulted.
    void advance(uint64_t atUs, uint64_t minimum, uint64_t maximum,
                 uint64_t probabilityPpm, uint64_t windowUs,
                 uint64_t riseRate, uint64_t fallRate, int64_t readinessBudgetUs = 0, uint64_t toleranceUs = 0,
                 uint64_t vrrHeadroomUs = 0, int64_t clockOffsetUs = 0) {
        if (!atUs || (m_LastClockUs && atUs < m_LastClockUs)) return;
        configure(minimum, maximum, probabilityPpm, windowUs, riseRate, fallRate, readinessBudgetUs, toleranceUs, vrrHeadroomUs, clockOffsetUs);
        const uint64_t elapsed = advanceClock(atUs);
        expire(atUs);
        evaluate(atUs);
        move(elapsed, false);
    }
    void advance(uint64_t atUs) {
        if (!m_Stats.initialized || !m_Stats.windowUs) return;
        advance(atUs, m_Stats.minimumUs, m_Stats.maximumUs, m_Stats.probabilityPpm,
            m_Stats.windowUs, m_RiseRate, m_FallRate, m_Stats.readinessBudgetUs, m_Stats.toleranceUs,
            m_Stats.vrrHeadroomUs, m_Stats.clockOffsetUs);
    }
    void observe(uint64_t frame, uint64_t observationUs, uint64_t slotUs,
                 int64_t requiredUs, bool valid, uint64_t minimum,
                 uint64_t maximum, uint64_t probabilityPpm,
                 uint64_t windowUs, uint64_t riseRate, uint64_t fallRate,
                 int64_t readinessBudgetUs = 0, uint64_t toleranceUs = 0,
                 uint64_t vrrHeadroomUs = 0, int64_t clockOffsetUs = 0,
                 bool qualifyingHitch = true) {
        configure(minimum, maximum, probabilityPpm, windowUs, riseRate, fallRate, readinessBudgetUs, toleranceUs, vrrHeadroomUs, clockOffsetUs);
        // An observation can follow advance() at the same timestamp. Actual
        // duplicate/nonmonotonic observations still invalidate their history.
        if (!observationUs || (m_LastObservationUs && observationUs <= m_LastObservationUs) ||
            (m_LastClockUs && observationUs < m_LastClockUs)) {
            reset(m_Stats.currentUs);
            return;
        }
        const uint64_t observationElapsed = m_LastObservationUs ?
            std::min<uint64_t>(observationUs - m_LastObservationUs, 100000) : 0;
        m_LastObservationUs = observationUs;
        const uint64_t elapsed = advanceClock(observationUs);
        expire(observationUs);
        m_Stats.lastInputRequiredUs = requiredUs;
        m_Stats.lastInputObservationUs = observationUs;
        ++m_Stats.observed;
        // Tolerance is permissible excess, not standing buffer protection.
        const uint64_t excess = requiredUs > 0 ?
            uint64_t(requiredUs) - std::min(uint64_t(requiredUs), toleranceUs) : 0;
        // Physical VRR room may make a late input harmless, but cannot erase
        // a hitch that actually happened. New scoring uses B+T; the caller
        // separately proves whether the input overrun caused a hitch.
        const uint64_t headroom = excess;
        // An input beyond the cap still contributes its measured timing cost.
        const bool eligible = valid && slotUs;
        m_Stats.lastInputEligible = eligible;
        const bool absorbed = eligible && headroom > 0 && headroom <= m_Stats.currentUs;
        const bool missed = eligible && headroom > m_Stats.currentUs && qualifyingHitch;
        if (eligible) {
            ++m_Stats.eligible;
            m_Stats.absorbed += absorbed;
            m_Stats.missed += missed;
        } else ++m_Stats.ineligible;
        if (missed) m_LayerTransitionActive = false;
        const bool adjacent = eligible && m_PreviousValid && frame == m_PreviousFrame + 1 &&
            slotUs > m_PreviousSlotUs && slotUs - m_PreviousSlotUs < 1000000;
        if (!adjacent) ++m_Epoch;
        if (eligible) {
            m_LastEligibleUs = observationUs;
            const int64_t rawRequired = readinessBudgetUs > 0 &&
                requiredUs > std::numeric_limits<int64_t>::max() - readinessBudgetUs ?
                std::numeric_limits<int64_t>::max() : readinessBudgetUs < 0 &&
                requiredUs < std::numeric_limits<int64_t>::min() - readinessBudgetUs ?
                std::numeric_limits<int64_t>::min() : requiredUs + readinessBudgetUs;
            m_Samples.push_back({observationUs, slotUs, offsetSum(rawRequired, clockOffsetUs), m_Epoch, absorbed, missed});
            ++m_Stats.recentEligible;
            m_Stats.recentAbsorbed += absorbed;
            m_Stats.recentMissed += missed;
        }
        m_PreviousFrame = frame;
        m_PreviousSlotUs = slotUs;
        m_PreviousValid = eligible;
        while (m_Samples.size() > MaximumSamples) {
            removeFront();
            m_LastTruncatedUs = observationUs;
        }
        evaluate(observationUs);
        // Invalid observations discard growth time and fractional growth credit;
        // decision-time advance must neither grow nor consume observation time.
        if (!eligible && m_Stats.targetUs > m_Stats.currentUs) m_ReleaseFraction = 0;
        move(m_Stats.targetUs > m_Stats.currentUs ? observationElapsed : elapsed, eligible);
    }
private:
    static int64_t offsetSum(int64_t value, int64_t offset) {
        constexpr auto lo = std::numeric_limits<int64_t>::min();
        constexpr auto hi = std::numeric_limits<int64_t>::max();
        if (offset > 0 && value > hi - offset) return hi;
        if (offset < 0 && value < lo - offset) return lo;
        return value + offset;
    }
    static int64_t offsetDifference(int64_t value, int64_t offset) {
        constexpr auto lo = std::numeric_limits<int64_t>::min();
        constexpr auto hi = std::numeric_limits<int64_t>::max();
        if (offset > 0 && value < lo + offset) return lo;
        if (offset < 0 && value > hi + offset) return hi;
        return value - offset;
    }
    void configure(uint64_t minimum, uint64_t maximum, uint64_t probabilityPpm,
                   uint64_t windowUs, uint64_t riseRate, uint64_t fallRate, int64_t budget, uint64_t tolerance,
                   uint64_t vrrHeadroom, int64_t clockOffset) {
        minimum = std::min(minimum, maximum);
        if (!m_Stats.initialized) {
            m_Stats.initialized = true;
            m_Stats.currentUs = m_Stats.targetUs = minimum;
        }
        m_Stats.minimumUs = minimum;
        m_Stats.maximumUs = maximum;
        // A shrinking source-rate cap changes demand, not the applied queue
        // in one step. Policy 9 slews existing excess down to the new cap.
        m_Stats.currentUs = std::max(m_Stats.currentUs, minimum);
        m_Stats.targetUs = std::clamp(m_Stats.targetUs, minimum, maximum);
        m_Stats.probabilityPpm = std::clamp<uint64_t>(probabilityPpm, 1, 999999);
        m_Stats.windowUs = std::clamp(windowUs, MinimumWindowUs, MaximumWindowUs);
        m_Stats.readinessBudgetUs = budget;
        m_Stats.toleranceUs = tolerance;
        m_Stats.vrrHeadroomUs = vrrHeadroom;
        m_Stats.clockOffsetUs = clockOffset;
        m_RiseRate = std::min<uint64_t>(riseRate, 1000000);
        m_FallRate = std::min<uint64_t>(fallRate, 1000000);

    }
    uint64_t advanceClock(uint64_t atUs) {
        const uint64_t elapsed = m_LastClockUs ? std::min<uint64_t>(atUs - m_LastClockUs, 100000) : 0;
        m_LastClockUs = atUs;
        if (!m_FirstObservationUs) m_FirstObservationUs = atUs;
        return elapsed;
    }
    void removeFront() {
        const auto& sample = m_Samples.front();
        if (!sample.seed) {
            --m_Stats.recentEligible;
            m_Stats.recentAbsorbed -= sample.absorbed;
            m_Stats.recentMissed -= sample.missed;
        }
        m_Samples.pop_front();
    }
    void expire(uint64_t atUs) {
        while (!m_Samples.empty() && atUs - m_Samples.front().at >= m_Stats.windowUs) {
            removeFront();
        }
    }
    void updateReleaseFeedback(uint64_t atUs) {
        m_Stats.recentReleaseSamples = m_Stats.recentReleaseMisses = 0;
        for (auto it = m_Samples.rbegin(); it != m_Samples.rend(); ++it) {
            if (atUs - it->at >= ReleaseFeedbackWindowUs) break;
            if (it->seed) continue;
            ++m_Stats.recentReleaseSamples;
            m_Stats.recentReleaseMisses += it->missed;
        }
        m_Stats.effectiveFallRateUsPerSecond = m_FallRate;
    }
    void evaluate(uint64_t atUs) {
        updateReleaseFeedback(atUs);
        const uint64_t windowUs = m_Stats.windowUs;
        m_Stats.samples = m_Samples.size();
        m_Stats.observationSpanUs = m_Samples.empty() ? 0 : atUs - m_Samples.front().at;
        m_Stats.historyTruncated = m_LastTruncatedUs && atUs - m_LastTruncatedUs < windowUs;
        // Eligibility belongs to each sample, not the current observation.
        m_Stats.evidenceValid = !m_Samples.empty();
        m_Stats.expiredUnknown = m_Samples.empty() && atUs -
            (m_LastEligibleUs ? m_LastEligibleUs : m_FirstObservationUs) >= windowUs;
        if (m_Samples.empty()) {
            m_Stats.headroomTargetUnclippedUs = 0;
            m_Stats.validPairCoverageUs = 0;
            m_Stats.tailUncertain = true;
            m_Stats.clipped = false;
            if (m_Stats.expiredUnknown) m_Stats.targetUs = m_Stats.minimumUs;
            else m_ReleaseFraction = 0;
            return;
        }
        evaluateGoal(atUs);
    }
    void evaluateGoal(uint64_t atUs) {
        if (m_LastEvaluationUs && atUs - m_LastEvaluationUs < GoalEvaluationIntervalUs &&
                m_EvaluatedProbabilityPpm == m_Stats.probabilityPpm && m_EvaluatedWindowUs == m_Stats.windowUs)
            return;
        m_LastEvaluationUs = atUs;
        m_EvaluatedProbabilityPpm = m_Stats.probabilityPpm;
        m_EvaluatedWindowUs = m_Stats.windowUs;
        const uint64_t spanUs = std::min(m_GoalWindowUs ? m_GoalWindowUs : m_Stats.windowUs, m_Stats.windowUs);
        // Project retained source-coordinate requirements into the current mapping.
        const auto lateness = [&](const Sample& sample) {
            return offsetDifference(offsetDifference(sample.required, m_Stats.clockOffsetUs),
                                    m_Stats.readinessBudgetUs);
        };
        m_GoalSolver.clear();
        const Sample* previous = nullptr;
        for (const auto& sample : m_Samples) {
            if (sample.seed || atUs - sample.at >= spanUs) { previous = nullptr; continue; }
            if (previous && sample.epoch == previous->epoch && sample.slot > previous->slot &&
                    sample.slot - previous->slot < 1000000)
                m_GoalSolver.add(lateness(*previous), lateness(sample), sample.slot - previous->slot,
                                 m_Stats.toleranceUs);
            previous = &sample;
        }
        m_Stats.validPairCoverageUs = m_GoalSolver.intendedUs();
        m_Stats.tailUncertain = m_GoalSolver.intendedUs() < GoalMinimumCoverageUs;
        if (m_Stats.tailUncertain) {
            m_Stats.targetUs = m_Stats.currentUs;
            return;
        }
        const uint64_t aimPpm = 1000000 - (1000000 - m_Stats.probabilityPpm) * m_GoalTailPerMille / 1000;
        const auto solution = m_GoalSolver.solve(aimPpm, m_Stats.toleranceUs,
            m_Stats.minimumUs, m_Stats.maximumUs, GoalStepUs);
        m_Stats.headroomTargetUnclippedUs = solution.bufferUs;
        m_Stats.clipped = !solution.goalReachable;
        m_Stats.targetUs = std::clamp(solution.bufferUs, m_Stats.minimumUs, m_Stats.maximumUs);
    }
    void move(uint64_t elapsed, bool growthAllowed) {
        const int direction = m_Stats.targetUs > m_Stats.currentUs ? 1 :
            m_Stats.targetUs < m_Stats.currentUs ? -1 : 0;
        if (direction != m_Direction) m_ReleaseFraction = 0;
        m_Direction = direction;
        m_Stats.growthBlocked = direction > 0 && !growthAllowed;
        if (!direction || (direction > 0 && !growthAllowed)) return;
        m_ReleaseFraction += elapsed * (direction > 0 ? m_RiseRate : m_FallRate);
        const uint64_t step = m_ReleaseFraction / 1000000;
        m_ReleaseFraction %= 1000000;
        const uint64_t distance = direction > 0 ? m_Stats.targetUs - m_Stats.currentUs :
            m_Stats.currentUs - m_Stats.targetUs;
        const uint64_t movement = std::min(step, distance);
        if (direction > 0) { m_Stats.currentUs += movement; if (movement) ++m_Stats.raises; }
        else { m_Stats.currentUs -= movement; if (movement) ++m_Stats.lowers; }
    }

    uint64_t m_LayerTransitionTargetUs = 0;
    bool m_LayerTransitionActive = false, m_LayerFastDrain = false;
    struct Sample { uint64_t at, slot; int64_t required; uint64_t epoch; bool absorbed, missed; bool seed = false; };
    std::deque<Sample> m_Samples;
    static constexpr uint64_t GoalEvaluationIntervalUs = 500000;
    static constexpr uint64_t GoalMinimumCoverageUs = 1000000;
    static constexpr uint64_t GoalStepUs = 250;
    uint64_t m_GoalWindowUs = 0, m_GoalTailPerMille = 1000;
    InputGoalSolver m_GoalSolver;
    Stats m_Stats;
    uint64_t m_FirstObservationUs = 0, m_LastObservationUs = 0, m_LastEligibleUs = 0;
    uint64_t m_LastClockUs = 0, m_RiseRate = 0, m_FallRate = 0;
    uint64_t m_LastEvaluationUs = 0, m_LastTruncatedUs = 0, m_Epoch = 0;
    uint64_t m_PreviousFrame = 0, m_PreviousSlotUs = 0;
    uint64_t m_EvaluatedProbabilityPpm = 0, m_EvaluatedWindowUs = 0, m_ReleaseFraction = 0;
    bool m_PreviousValid = false;
    int m_Direction = 0;
};
}
