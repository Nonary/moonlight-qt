#pragma once
#include "rollingquantile.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <utility>
#include <vector>

namespace Vrr13 {
// Input coverage controller. The hitch-only policy scores actual preventable
// input-caused hitches; historical policy scores raw overruns. probabilityPpm is a frame
// probability; it must never be substituted for interval severity quality.
// requiredUs is a causal input offset from the budget-adjusted input slot,
// while slotUs is the unbuffered source slot used only for continuity.
// Intentional decoder
// backlog and policy holds removed by the caller. The caller owns capacity,
// packet-loss and basis eligibility. Hitch-only scoring additionally excludes
// input beyond the selected maximum as unpreventable within this policy.
//
// vrrHeadroomUs is recovery credit supplied by the caller. Hitch-only scoring
// never uses it to erase an actual miss; demand may use it while the goal is
// met. Historical callers retain their recorded credit rules. Requirements
// may also carry a clockOffsetUs so retained measurements share an immutable
// source coordinate; historical coverage outcomes keep their observed budget.
class InputPercentileBuffer {
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
        uint64_t holdUntilUs = 0, holdRemainingUs = 0, lastBufferMissUs = 0;
        uint64_t lastNetworkMissUs = 0, lastDecoderMissUs = 0;
        uint64_t cumulativeNetworkMisses = 0, cumulativeDecoderMisses = 0;
        uint64_t recentReleaseSamples = 0, recentReleaseMisses = 0, recentProposedMisses = 0;
        uint64_t releaseFactorPpm = 1000000, effectiveFallRateUsPerSecond = 0;
        uint64_t effectiveFallRateMilliUsPerSecond = 0;
        // Outcomes observed with each frame's actual buffer/tolerance inside
        // the effective trailing profile window. They are historical outcomes,
        // not counterfactual coverage under a newly selected tolerance.
        // The controller's policy-8 telemetry substitutes its session-wide
        // trailing outcomes here; this buffer's internal counters stay local.
        uint64_t recentEligible = 0, recentAbsorbed = 0, recentMissed = 0;
        // Reporting only: recent responsiveness alongside the full learning history.
        uint64_t reportingWindowUs = 0, reportingEligible = 0;
        uint64_t reportingAbsorbed = 0, reportingMissed = 0;
        uint64_t raises = 0, lowers = 0, unknownRelease = 0, epochResets = 0;
        uint64_t observationSpanUs = 0, validPairCoverageUs = 0;
        uint64_t probabilityPpm = 0, windowUs = 0, toleranceUs = 0, vrrHeadroomUs = 0;
        int64_t readinessBudgetUs = 0, clockOffsetUs = 0;
        uint64_t headroomTargetUnclippedUs = 0;
        int64_t rawRequiredQuantileUs = 0;
        bool tailUncertain = true, evidenceValid = false;
        bool historyTruncated = false, expiredUnknown = false, clipped = false;
        bool initialized = false, lastInputEligible = false, growthBlocked = false;
        int64_t lastInputRequiredUs = 0;
        uint64_t lastInputObservationUs = 0;
    };
    // Activation/source epoch should seed current from the applied buffer.
    void reset(uint64_t current = 0) {
        *this = InputPercentileBuffer{};
        m_Stats.currentUs = m_Stats.targetUs = current;
        m_Stats.initialized = true;
    }
    // Until the profile tail is first supported, protection follows the
    // largest eligible requirement at once instead of rising at 1 ms/s. The
    // slow ramp missed 144 and 165 frames in the first 10 s of the 2026-10-10
    // 22:48/22:55 sessions; tracking the maximum missed 8 and 16.
    // A buffer resumed from evidence it already trusts skips learning.
    void endLearning() { m_Learning = false; }
    // Keep the learned percentile and configured fall rate in charge. Miss
    // eligibility and the evidence required for growth remain independent.
    void setStableAdaptation(bool stable) { m_StableAdaptation = stable; }
    // While learning, ignore the largest `trim` requirements so one startup
    // or scene-load outlier cannot set protection.
    void setLearningTrim(size_t trim) { m_LearningTrim = trim; }
    bool learning() const { return m_Learning; }
    // A newly created empty rate layer has no valid past to restore. Permit
    // fresh causal misses to reacquire protection only until one expected tail
    // observation is supported, and never beyond its original history window.
    // This is an actuation budget, not a sample, success or learned protection.
    void beginFreshLayerReacquisition(uint64_t atUs, uint64_t probabilityPpm, uint64_t windowUs) {
        if (m_Stats.samples || m_LastEligibleUs) return;
        const uint64_t tail = 1000000 - std::clamp<uint64_t>(probabilityPpm, 1, 999999);
        m_ReacquisitionRemaining = (1000000 + tail - 1) / tail;
        windowUs = std::clamp(windowUs, MinimumWindowUs, MaximumWindowUs);
        m_ReacquisitionUntilUs = atUs + std::min(windowUs, std::numeric_limits<uint64_t>::max() - atUs);
    }
    // Observation-clock timestamp of usable evidence. Ineligible outcomes
    // update diagnostics but cannot extend the learning history's lifetime.
    uint64_t lastEligibleObservationUs() const { return m_LastEligibleUs; }
    uint64_t reacquisitionRemaining() const { return m_ReacquisitionRemaining; }
    uint64_t reacquisitionUntilUs() const { return m_ReacquisitionUntilUs; }
    struct SlewAudit { uint64_t atUs=0, authorizedTargetUs=0, beforeUs=0; int64_t deltaUs=0; };
    SlewAudit lastSlewAudit() const { return m_SlewAudit; }
    bool layerTransitionActive() const { return m_LayerTransitionActive; }
    bool fastLayerDrain() const { return (m_LayerTransitionActive && m_LayerFastDrain) ||
        (m_RateDropDrain && m_RateDropFastDrain); }
    void setRateDropDrain(bool active) {
        m_RateDropDrain = active;
        if (!active) m_RateDropFastDrain = false;
    }
    // A changed source phase, cadence, or normalized-input basis invalidates
    // prior requirements. Retain current protection while relearning its size.
    void breakSequence() {
        m_Samples.clear();
        m_Requirements.clear();
        m_MissedRequirements.clear();
        m_ValidPairCoverageUs = 0;
        m_Stats.targetUs = m_Stats.currentUs;
        m_Stats.samples = m_Stats.observationSpanUs = m_Stats.validPairCoverageUs = 0;
        m_Stats.recentEligible = m_Stats.recentAbsorbed = m_Stats.recentMissed = 0;
        updateReleaseFeedback(m_LastClockUs);
        m_SourceRelativeMissQuantileUs = 0;
        m_SourceRelativeQuantileUs = m_Stats.rawRequiredQuantileUs = 0;
        m_Stats.headroomTargetUnclippedUs = m_Stats.currentUs;
        m_Stats.tailUncertain = true;
        m_Stats.evidenceValid = m_Stats.historyTruncated = m_Stats.clipped = false;
        ++m_Stats.epochResets;
        m_Learning = true;
        m_LastEvaluationUs = m_LastTruncatedUs = m_ReleaseFraction = 0;
        m_PreviousValid = false;
        m_Direction = 0;
        m_ReleaseIntervalUs = 0;
        m_ReleaseStepRemainingUs = 0;
        m_LayerTransitionActive = false;
        restartReleaseFeedback();
        ++m_Epoch;
        // Keep elapsed/last-eligible clocks and diagnostics: recurring phase
        // breaks must not rearm the unknown-evidence retention window forever.
    }
    // A source phase discontinuity keeps the slot basis: input lateness keeps
    // its level across it (2026-10-10 22:55 capture: median 3.10 -> 3.16 ms).
    // Only frame adjacency breaks; evidence and coverage outcomes are kept.
    void breakAdjacency() {
        m_PreviousValid = false;
        m_ReleaseIntervalUs = 0;
        m_ReleaseStepRemainingUs = 0;
        restartReleaseFeedback();
        ++m_Epoch;
    }
    // A new rate layer starts from a copy of its neighbour's evidence. The
    // copied requirements seed its target; their outcomes belonged to the
    // other rate, so they leave this layer's coverage.
    void markEvidenceAsSeed() {
        m_MissedRequirements.clear();
        for (auto& sample : m_Samples) {
            if (sample.seed) continue;
            --m_Stats.recentEligible;
            m_Stats.recentAbsorbed -= sample.absorbed;
            m_Stats.recentMissed -= sample.missed;
            sample.seed = true;
            sample.absorbed = sample.missed = false;
        }
        restartReleaseFeedback();
    }
    // Session totals and stage diagnostics continue across rate-band layers;
    // evidence, protection and coverage outcomes stay with each band.
    void continueTotalsFrom(const InputPercentileBuffer& previous) {
        const auto& s = previous.m_Stats;
        m_Stats.observed = s.observed; m_Stats.eligible = s.eligible; m_Stats.ineligible = s.ineligible;
        m_Stats.absorbed = s.absorbed; m_Stats.missed = s.missed;
        m_Stats.cumulativeNetworkMisses = s.cumulativeNetworkMisses;
        m_Stats.cumulativeDecoderMisses = s.cumulativeDecoderMisses;
        m_Stats.lastBufferMissUs = s.lastBufferMissUs;
        m_Stats.lastNetworkMissUs = s.lastNetworkMissUs;
        m_Stats.lastDecoderMissUs = s.lastDecoderMissUs;
        m_Stats.raises = s.raises; m_Stats.lowers = s.lowers;
        m_Stats.unknownRelease = s.unknownRelease; m_Stats.epochResets = s.epochResets;
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
        restartReleaseFeedback();
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
    // An abrupt source-rate drop changes every frame interval at once, so a
    // step to the evidence-backed target is masked by the cadence change.
    // Take it then instead of slewing at the rise and fall rates.
    void followTarget() {
        if (!m_Stats.initialized || m_Stats.targetUs == m_Stats.currentUs) return;
        if (m_Stats.targetUs > m_Stats.currentUs) ++m_Stats.raises;
        else ++m_Stats.lowers;
        m_Stats.currentUs = m_Stats.targetUs;
        m_ReleaseFraction = 0;
    }
    // Applied protection is a local duration. Only movement of the actual
    // producer mapping changes that duration; drift normalization of retained
    // evidence is not another movement of the playback deadline.
    void remapProtection(int64_t previousMappingUs, int64_t mappingUs) {
        const auto project = [&](uint64_t value) {
            if (mappingUs >= previousMappingUs) {
                const uint64_t delta = uint64_t(mappingUs) - uint64_t(previousMappingUs);
                return value - std::min(value, delta);
            }
            const uint64_t delta = uint64_t(previousMappingUs) - uint64_t(mappingUs);
            return value + std::min(std::numeric_limits<uint64_t>::max() - value, delta);
        };
        m_Stats.currentUs = std::clamp(project(m_Stats.currentUs), m_Stats.minimumUs, m_Stats.maximumUs);
        m_Stats.targetUs = std::clamp(project(m_Stats.targetUs), m_Stats.minimumUs, m_Stats.maximumUs);
        if (previousMappingUs != mappingUs) restartReleaseFeedback();
    }
    // Selecting a layer restores its learned buffer, but only a substantial
    // increase may jump. A decrease keeps the applied buffer and its learned
    // destination while normal release drains it. Recorded policy 9 instead
    // spends at most T per fresh source frame.
    void transitionFrom(uint64_t previousUs) {
        if (m_HitchMissesOnly < 9) return;
        const uint64_t learnedUs = m_Stats.currentUs;
        previousUs = std::max(previousUs, m_Stats.minimumUs);
        m_LayerTransitionActive = false;
        m_LayerDrainStepRemainingUs = 0;
        if (learnedUs == previousUs ||
            (learnedUs > previousUs && learnedUs - previousUs > m_Stats.toleranceUs)) return;
        m_LayerTransitionTargetUs = learnedUs;
        m_LayerTransitionActive = true;
        m_LayerFastDrain = previousUs > learnedUs && previousUs - learnedUs > m_Stats.toleranceUs;
        m_Stats.currentUs = previousUs;
        m_Stats.targetUs = learnedUs;
        m_ReleaseFraction = 0;
        restartReleaseFeedback();
    }
    void prepareForLayerStorage() {
        setRateDropDrain(false);
        if (m_HitchMissesOnly >= 9 && m_LayerTransitionActive) {
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
        m_Stats.holdUntilUs = m_Stats.holdRemainingUs = 0;
    }
    // Decision-time aging: no input observation or continuity changes.
    // Unknown current-frame capacity permits release, never another rise.
    // The full overload applies profile changes before demand() is consulted.
    void advance(uint64_t atUs, uint64_t minimum, uint64_t maximum,
                 uint64_t probabilityPpm, uint64_t windowUs,
                 uint64_t riseRate, uint64_t fallRate, int64_t readinessBudgetUs = 0, uint64_t toleranceUs = 0,
                 uint64_t vrrHeadroomUs = 0, int64_t clockOffsetUs = 0, uint64_t hitchMissesOnly = 0) {
        if (!atUs || (m_LastClockUs && atUs < m_LastClockUs)) return;
        m_HitchMissesOnly = hitchMissesOnly;
        configure(minimum, maximum, probabilityPpm, windowUs, riseRate, fallRate, readinessBudgetUs, toleranceUs, vrrHeadroomUs, clockOffsetUs);
        const uint64_t elapsed = advanceClock(atUs);
        evaluate(atUs, expire(atUs));
        move(elapsed, false);
    }
    void advance(uint64_t atUs) {
        if (!m_Stats.initialized || !m_Stats.windowUs) return;
        advance(atUs, m_Stats.minimumUs, m_Stats.maximumUs, m_Stats.probabilityPpm,
            m_Stats.windowUs, m_RiseRate, m_FallRate, m_Stats.readinessBudgetUs, m_Stats.toleranceUs,
            m_Stats.vrrHeadroomUs, m_Stats.clockOffsetUs, m_HitchMissesOnly);
    }
    void observe(uint64_t frame, uint64_t observationUs, uint64_t slotUs,
                 int64_t requiredUs, bool valid, uint64_t minimum,
                 uint64_t maximum, uint64_t probabilityPpm,
                 uint64_t windowUs, uint64_t riseRate, uint64_t fallRate,
                 int64_t readinessBudgetUs = 0, uint64_t toleranceUs = 0,
                 uint64_t vrrHeadroomUs = 0, int64_t clockOffsetUs = 0,
                 bool qualifyingHitch = true, uint64_t hitchMissesOnly = 0) {
        m_HitchMissesOnly = hitchMissesOnly;
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
        bool changed = expire(observationUs);
        m_Stats.lastInputRequiredUs = requiredUs;
        m_Stats.lastInputObservationUs = observationUs;
        ++m_Stats.observed;
        // Tolerance is permissible excess, not standing buffer protection.
        // Subtract once rather than adding to current (which could overflow).
        // VRR headroom is subtracted the same way: it absorbs lateness for free.
        const uint64_t excess = requiredUs > 0 ?
            uint64_t(requiredUs) - std::min(uint64_t(requiredUs), toleranceUs) : 0;
        // Physical VRR room may make a late input harmless, but cannot erase
        // a hitch that actually happened. New scoring uses B+T; the caller
        // separately proves whether the input overrun caused a hitch.
        const uint64_t headroom = m_HitchMissesOnly ? excess :
            excess - std::min(excess, m_Stats.vrrHeadroomUs);
        // Inputs beyond the selected maximum cannot be protected within this
        // policy. They remain raw diagnostics, not failures or invented clean
        // samples in the preventable-miss goal.
        const bool eligible = valid && slotUs &&
            (!m_HitchMissesOnly || headroom <= m_Stats.maximumUs);
        m_Stats.lastInputEligible = eligible;
        const bool absorbed = eligible && headroom > 0 && headroom <= m_Stats.currentUs;
        const bool missed = eligible && headroom > m_Stats.currentUs &&
            (!m_HitchMissesOnly || qualifyingHitch);
        if (eligible) {
            ++m_Stats.eligible;
            m_Stats.absorbed += absorbed;
            m_Stats.missed += missed;
        } else ++m_Stats.ineligible;
        if (m_HitchMissesOnly >= 9 && missed) {
            restartReleaseFeedback();
            m_LayerTransitionActive = false;
        }
        const bool adjacent = eligible && m_PreviousValid && frame == m_PreviousFrame + 1 &&
            slotUs > m_PreviousSlotUs && slotUs - m_PreviousSlotUs < 1000000;
        if (m_HitchMissesOnly >= 7) {
            m_ReleaseIntervalUs = adjacent ? slotUs - m_PreviousSlotUs : 0;
            m_ReleaseStepRemainingUs = std::min({m_ReleaseIntervalUs / 200,
                m_Stats.toleranceUs / 2, m_Stats.vrrHeadroomUs});
        }
        if (!adjacent) ++m_Epoch;
        if (m_HitchMissesOnly >= 9)
            m_LayerDrainStepRemainingUs = adjacent ? m_Stats.toleranceUs : 0;
        if (eligible) {
            m_LastEligibleUs = observationUs;
            const int64_t rawRequired = readinessBudgetUs > 0 &&
                requiredUs > std::numeric_limits<int64_t>::max() - readinessBudgetUs ?
                std::numeric_limits<int64_t>::max() : readinessBudgetUs < 0 &&
                requiredUs < std::numeric_limits<int64_t>::min() - readinessBudgetUs ?
                std::numeric_limits<int64_t>::min() : requiredUs + readinessBudgetUs;
            const int64_t normalizedRequired = offsetSum(rawRequired, clockOffsetUs);
            if (!m_Samples.empty() && m_Samples.back().epoch == m_Epoch)
                m_ValidPairCoverageUs += slotUs - m_Samples.back().slot;
            m_Samples.push_back({observationUs, slotUs, normalizedRequired, m_Epoch, absorbed, missed});
            m_Requirements.add(normalizedRequired);
            if (missed) m_MissedRequirements.add(normalizedRequired);
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
            changed = true;
        }
        evaluate(observationUs, changed, missed ? headroom : 0);
        // A proven hitch already spent this input delay. Retain that phase
        // for subsequent frames instead of spending seconds below the known
        // requirement. Never step past this frame's measured demand: an older
        // larger target or harmless late input cannot authorize the jump.
        const uint64_t attackTarget = m_HitchMissesOnly >= 2 ?
            (missed ? std::min(m_Stats.targetUs, headroom) : m_Stats.currentUs) : m_Stats.targetUs;
        // Only an explicitly new, empty rate layer may reacquire rapidly.
        // Its fixed eligible-input budget and original profile-window deadline
        // survive phase breaks; invalid callbacks cannot rearm either bound.
        const bool freshLearningAttack = m_HitchMissesOnly == 10 && m_Learning &&
            m_ReacquisitionRemaining && observationUs < m_ReacquisitionUntilUs;
        if ((m_HitchMissesOnly < 7 || freshLearningAttack) &&
                (m_Learning || m_HitchMissesOnly >= 2) &&
                eligible && attackTarget > m_Stats.currentUs) {
            m_Stats.currentUs = attackTarget;
            ++m_Stats.raises;
            m_ReleaseFraction = 0;
        }
        if (eligible && m_ReacquisitionRemaining) --m_ReacquisitionRemaining;
        if (observationUs >= m_ReacquisitionUntilUs) m_ReacquisitionRemaining = 0;
        if (m_Learning && m_Stats.samples && !m_Stats.tailUncertain) {
            m_Learning = false;
            m_ReacquisitionRemaining = 0;
        }
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
        if ((m_HitchMissesOnly == 3 || m_HitchMissesOnly == 4) &&
                m_Stats.windowUs && clockOffset != m_Stats.clockOffsetUs) {
            // Evidence already lives in the immutable source coordinate.
            // Retained protection must live there too: moving the mapping
            // forward must remove the same amount from B, rather than adding
            // that delay again while B drains at the ordinary release rate.
            // The reverse transform retains the same source deadline; it is
            // not fresh protection or a new miss. Bounds still apply below.
            const auto project = [&](uint64_t value) {
                if (clockOffset > m_Stats.clockOffsetUs) {
                    const uint64_t delta = uint64_t(clockOffset) - uint64_t(m_Stats.clockOffsetUs);
                    return value - std::min(value, delta);
                }
                const uint64_t delta = uint64_t(m_Stats.clockOffsetUs) - uint64_t(clockOffset);
                return value + std::min(std::numeric_limits<uint64_t>::max() - value, delta);
            };
            const uint64_t tail = 1000000 - std::clamp<uint64_t>(probabilityPpm, 1, 999999);
            const bool goalMet = m_Stats.recentEligible &&
                m_Stats.recentMissed * 1000000 <= m_Stats.recentEligible * tail;
            // A rate correction must not spend protection while proven
            // misses already exceed the selected allowance. Resume giving
            // back surplus mapping delay only after coverage has recovered.
            if (m_HitchMissesOnly < 4 || clockOffset < m_Stats.clockOffsetUs || goalMet)
                m_Stats.currentUs = project(m_Stats.currentUs);
            m_Stats.targetUs = project(m_Stats.targetUs);
        }
        m_Stats.minimumUs = minimum;
        m_Stats.maximumUs = maximum;
        // A shrinking source-rate cap changes demand, not the applied queue
        // in one step. Policy 9 slews existing excess down to the new cap.
        m_Stats.currentUs = m_HitchMissesOnly >= 9 ? std::max(m_Stats.currentUs, minimum) :
            std::clamp(m_Stats.currentUs, minimum, maximum);
        m_Stats.targetUs = std::clamp(m_Stats.targetUs, minimum, maximum);
        m_Stats.probabilityPpm = std::clamp<uint64_t>(probabilityPpm, 1, 999999);
        m_Stats.windowUs = std::clamp(windowUs, MinimumWindowUs, MaximumWindowUs);
        m_Stats.readinessBudgetUs = budget;
        m_Stats.toleranceUs = tolerance;
        m_Stats.vrrHeadroomUs = vrrHeadroom;
        m_Stats.clockOffsetUs = clockOffset;
        m_RiseRate = std::min<uint64_t>(riseRate, 1000000);
        m_FallRate = std::min<uint64_t>(fallRate, 1000000);
        // Policy 9 accidentally changed ordinary adaptation as well as layers.
        // Policy 10 restores the pre-existing rise and healthy release law.
        if (m_HitchMissesOnly == 9) {
            m_RiseRate = std::min(m_RiseRate, tolerance);
            m_FallRate = std::min(m_FallRate, tolerance);
        }
    }
    uint64_t advanceClock(uint64_t atUs) {
        const uint64_t elapsed = m_LastClockUs ? std::min<uint64_t>(atUs - m_LastClockUs, 100000) : 0;
        m_LastClockUs = atUs;
        if (!m_FirstObservationUs) m_FirstObservationUs = atUs;
        return elapsed;
    }
    void removeFront(bool updateIndex = true) {
        const auto& sample = m_Samples.front();
        if (m_Samples.size() > 1 && sample.epoch == m_Samples[1].epoch)
            m_ValidPairCoverageUs -= m_Samples[1].slot - sample.slot;
        if (updateIndex) {
            m_Requirements.remove(sample.required);
            if (sample.missed && !sample.seed) m_MissedRequirements.remove(sample.required);
        }
        if (!sample.seed) {
            --m_Stats.recentEligible;
            m_Stats.recentAbsorbed -= sample.absorbed;
            m_Stats.recentMissed -= sample.missed;
        }
        m_Samples.pop_front();
    }
    bool expire(uint64_t atUs) {
        if (!m_Samples.empty() && atUs - m_Samples.back().at >= m_Stats.windowUs) {
            m_Samples.clear();
            m_Requirements.clear();
            m_MissedRequirements.clear();
            m_ValidPairCoverageUs = 0;
            m_Stats.recentEligible = m_Stats.recentAbsorbed = m_Stats.recentMissed = 0;
            return true;
        }
        // Ordinary expiry removes one or two values. A long advance retires
        // the same prefix with one exact compaction of each order index.
        if (m_Samples.size() > 16 && atUs - m_Samples[16].at >= m_Stats.windowUs) {
            std::vector<int64_t> requirements, missed;
            for (const auto& sample : m_Samples) {
                if (atUs - sample.at < m_Stats.windowUs) break;
                requirements.push_back(sample.required);
                if (sample.missed && !sample.seed) missed.push_back(sample.required);
            }
            const size_t count = requirements.size();
            m_Requirements.removeBatch(std::move(requirements));
            m_MissedRequirements.removeBatch(std::move(missed));
            for (size_t i = 0; i < count; ++i) removeFront(false);
            return true;
        }
        bool changed = false;
        while (!m_Samples.empty() && atUs - m_Samples.front().at >= m_Stats.windowUs) {
            removeFront();
            changed = true;
        }
        return changed;
    }
    uint64_t recentMissesAt(uint64_t atUs, uint64_t candidate) const {
        uint64_t misses = 0;
        for (auto it = m_Samples.rbegin(); it != m_Samples.rend(); ++it) {
            if (atUs - it->at >= ReleaseFeedbackWindowUs) break;
            if (m_HitchMissesOnly) {
                misses += it->missed;
                continue;
            }
            const int64_t relativeRequired = offsetDifference(it->required, m_Stats.clockOffsetUs);
            const uint64_t required = relativeRequired > m_Stats.readinessBudgetUs ?
                uint64_t(relativeRequired) - uint64_t(m_Stats.readinessBudgetUs) : 0;
            const uint64_t excess = required - std::min(required, m_Stats.toleranceUs);
            misses += excess - std::min(excess, m_Stats.vrrHeadroomUs) > candidate;
        }
        return misses;
    }
    void updateReleaseFeedback(uint64_t atUs) {
        m_Stats.holdUntilUs = m_Stats.holdRemainingUs = 0;
        m_Stats.recentReleaseSamples = 0;
        for (auto it = m_Samples.rbegin(); it != m_Samples.rend(); ++it) {
            if (atUs - it->at >= ReleaseFeedbackWindowUs) break;
            if (m_HitchMissesOnly >= 7 && it->seed) continue;
            ++m_Stats.recentReleaseSamples;
        }
        m_Stats.recentReleaseMisses = recentMissesAt(atUs, m_Stats.currentUs);
        m_Stats.recentProposedMisses = m_Stats.recentReleaseMisses;
        updateReleaseRate(m_Stats.recentReleaseMisses);
    }
    void updateReleaseRate(uint64_t /*misses*/) {
        // The selected long-window quantile owns buffer demand. Short-window
        // counts diagnose misses; they do not redefine the user's tail target.
        // Output catch-up is independently bounded by PhaseTrajectory.
        m_Stats.releaseFactorPpm = 1000000;
        m_Stats.effectiveFallRateUsPerSecond = m_FallRate;
        m_Stats.effectiveFallRateMilliUsPerSecond = m_FallRate * 1000;
        if (canProbeRelease() && (m_HitchMissesOnly != 9 || m_EvidenceBackedRelease)) {
            // Release at no more than 0.5% of source cadence, half the selected
            // tolerance, and the available VRR credit per interval. Presentation
            // recovery still enforces the shared physical/cadence floor.
            const uint64_t step = std::min({m_ReleaseIntervalUs / 200,
                m_Stats.toleranceUs / 2, m_Stats.vrrHeadroomUs});
            uint64_t rate = step * 1000000 / m_ReleaseIntervalUs;
            // Measured surplus may drain faster than a speculative trial, but
            // never faster than the selected growth rate in the current layer.
            if (m_HitchMissesOnly == 9) rate = std::min(rate, m_RiseRate);
            m_Stats.effectiveFallRateUsPerSecond = std::max(m_FallRate, rate);
            m_Stats.effectiveFallRateMilliUsPerSecond = m_Stats.effectiveFallRateUsPerSecond * 1000;
        }
    }
    bool canProbeRelease() const {
        const uint64_t tail = 1000000 - m_Stats.probabilityPpm;
        return !m_StableAdaptation && m_HitchMissesOnly >= 7 && m_ReleaseIntervalUs && m_Stats.vrrHeadroomUs &&
            m_Stats.toleranceUs &&
            m_Stats.recentEligible && m_Stats.recentReleaseSamples >= 8 &&
            (m_HitchMissesOnly >= 9 ||
             m_Stats.recentMissed * 1000000 <= m_Stats.recentEligible * tail) &&
            m_Stats.recentReleaseMisses == 0 && m_LastEligibleUs &&
            m_LastClockUs >= m_LastEligibleUs && m_LastClockUs - m_LastEligibleUs < 100000 &&
            m_Stats.validPairCoverageUs >= 500000 && !m_Stats.historyTruncated;
    }
    void restartReleaseFeedback() {
        m_ReleaseTrialActive = false;
        m_ReleaseTrialWaiting = true;
        m_ReleaseTrialAtUs = m_LastClockUs;
        m_ReleaseTrialEligible = m_Stats.eligible;
        m_EvidenceBackedRelease = false;
    }
    void applyFeedbackBoundedRelease(uint64_t atUs, uint64_t measuredTargetUs) {
        // The ordinary target covers the measured input quantile without
        // assuming that unused VRR room would have prevented a proven hitch.
        // Only that evidence permits continuous accelerated release.
        // The previous target may itself be an unfinished trial. Its lower
        // value is not new evidence authorizing continuous release.
        if (m_Stats.targetUs < m_Stats.currentUs && measuredTargetUs >= m_Stats.currentUs)
            m_Stats.targetUs = m_Stats.currentUs;
        m_EvidenceBackedRelease = m_Stats.targetUs < m_Stats.currentUs;
        if (m_Stats.targetUs != m_Stats.currentUs) {
            restartReleaseFeedback();
            m_EvidenceBackedRelease = m_Stats.targetUs < m_Stats.currentUs;
            return;
        }
        if (!canProbeRelease()) return;
        const uint64_t tail = 1000000 - m_Stats.probabilityPpm;
        const uint64_t needed = (1000000 + tail - 1) / tail;
        if (m_ReleaseTrialActive && m_Stats.currentUs <= m_ReleaseTrialFloorUs) {
            // Feedback starts after the reduction has finished. Outcomes at
            // the larger buffer do not validate the smaller one.
            m_ReleaseTrialActive = false;
            m_ReleaseTrialWaiting = true;
            m_ReleaseTrialAtUs = atUs;
            m_ReleaseTrialEligible = m_Stats.eligible;
        }
        if (m_ReleaseTrialWaiting) {
            if (atUs - m_ReleaseTrialAtUs < ReleaseFeedbackWindowUs ||
                m_Stats.eligible - m_ReleaseTrialEligible < needed) return;
            m_ReleaseTrialWaiting = false;
        }
        if (!m_ReleaseTrialActive) {
            const uint64_t probe = std::min<uint64_t>(100, m_Stats.toleranceUs / 4);
            m_ReleaseTrialFloorUs = m_Stats.currentUs -
                std::min(probe, m_Stats.currentUs - m_Stats.minimumUs);
            m_ReleaseTrialActive = true;
        }
        m_Stats.targetUs = std::min(m_Stats.targetUs, m_ReleaseTrialFloorUs);
    }
    void evaluate(uint64_t atUs, bool removedSamples, uint64_t missDemandUs = 0) {
        const uint64_t previousTargetUs = m_Stats.targetUs;
        updateReleaseFeedback(atUs);
        const uint64_t windowUs = m_Stats.windowUs;
        const uint64_t probabilityPpm = m_Stats.probabilityPpm;
        m_Stats.samples = m_Samples.size();
        m_Stats.observationSpanUs = m_Samples.empty() ? 0 : atUs - m_Samples.front().at;
        m_Stats.historyTruncated = m_LastTruncatedUs && atUs - m_LastTruncatedUs < windowUs;
        // Historical eligibility belongs to each sample, not to the current
        // tick. Invalid input cannot freeze an expired quantile or its target.
        m_Stats.evidenceValid = !m_Samples.empty();
        m_Stats.expiredUnknown = m_Samples.empty() && atUs -
            (m_LastEligibleUs ? m_LastEligibleUs : m_FirstObservationUs) >= windowUs;
        if (m_Samples.empty()) {
            m_SourceRelativeMissQuantileUs = 0;
            m_SourceRelativeQuantileUs = m_Stats.rawRequiredQuantileUs = 0;
            m_Stats.headroomTargetUnclippedUs = 0;
            m_Stats.validPairCoverageUs = 0;
            m_Stats.tailUncertain = true;
            m_Stats.clipped = false;
            if (m_Stats.expiredUnknown) m_Stats.targetUs = m_Stats.minimumUs;
            else m_ReleaseFraction = 0;
            return;
        }
        const uint64_t tail = 1000000 - probabilityPpm;
        const uint64_t needed = (1000000 + tail - 1) / tail;
        const bool belowMissGoal = m_Stats.recentEligible &&
            m_Stats.recentMissed * 1000000 > m_Stats.recentEligible * tail;
        // Learning protects each new maximum at once, so it is not throttled;
        // the history is still short (below the tail's supporting samples).
        if (m_Learning || removedSamples || !m_LastEvaluationUs || m_Samples.size() == needed ||
            (m_HitchMissesOnly >= 2 && missDemandUs) ||
            atUs - m_LastEvaluationUs >= 100000 ||
            m_EvaluatedProbabilityPpm != probabilityPpm || m_EvaluatedWindowUs != windowUs) {
            m_LastEvaluationUs = atUs;
            m_EvaluatedProbabilityPpm = probabilityPpm;
            m_EvaluatedWindowUs = windowUs;
            m_Stats.validPairCoverageUs = m_ValidPairCoverageUs;
            // One expected tail observation is support, not a confidence bound.
            m_Stats.tailUncertain = m_Requirements.size() < needed || m_Stats.historyTruncated;
            const size_t rank = m_Stats.tailUncertain ?
                m_Requirements.size() - std::min(m_LearningTrim, m_Requirements.size() - 1) :
                size_t((probabilityPpm * m_Requirements.size() + 999999) / 1000000);
            m_SourceRelativeQuantileUs = m_Requirements.atRank(rank);
            m_SourceRelativeMissQuantileUs = 0;
            if (m_HitchMissesOnly >= 2 && belowMissGoal && !m_MissedRequirements.empty()) {
                // Leave only the profile's permitted tail unprotected. Unlike
                // the raw-input learning quantile, proven playback misses must
                // not be trimmed away or replaced by harmless late inputs.
                const size_t allowed = size_t(m_Stats.recentEligible * tail / 1000000);
                const size_t missRank = m_MissedRequirements.size() - std::min(allowed, m_MissedRequirements.size() - 1);
                m_SourceRelativeMissQuantileUs = m_MissedRequirements.atRank(missRank);
            }
        }
        // Retained requirements use an immutable RTP/source coordinate. Only
        // this projection into the current local clock changes with mapping.
        // Observed historical misses continue to use each original budget.
        m_Stats.rawRequiredQuantileUs = offsetDifference(m_SourceRelativeQuantileUs, m_Stats.clockOffsetUs);
        // Unsigned subtraction preserves the positive signed difference even
        // when the normalized requirement and budget span the signed range.
        const int64_t budget = m_Stats.readinessBudgetUs;
        const uint64_t required = m_Stats.rawRequiredQuantileUs > budget ?
            uint64_t(m_Stats.rawRequiredQuantileUs) - uint64_t(budget) : 0;
        const uint64_t excess = required - std::min(required, m_Stats.toleranceUs);
        // Credit unused physical recovery room while the actual miss goal is
        // met, including on slower rate layers. Once preventable hitches put
        // it below goal, that room did not prevent them and cannot discount
        // the protection needed to cover their input tail.
        const uint64_t credit = m_HitchMissesOnly == 9 || (m_HitchMissesOnly && belowMissGoal) ?
            0 : m_Stats.vrrHeadroomUs;
        uint64_t requested = excess - std::min(excess, credit);
        if (m_HitchMissesOnly >= 2 && belowMissGoal) {
            const int64_t missRequired = offsetDifference(m_SourceRelativeMissQuantileUs, m_Stats.clockOffsetUs);
            const uint64_t missDemand = missRequired > budget ? uint64_t(missRequired) - uint64_t(budget) : 0;
            const uint64_t protection = missDemand - std::min(missDemand, m_Stats.toleranceUs);
            requested = std::max(requested, protection);
        }
        m_Stats.headroomTargetUnclippedUs = requested;
        m_Stats.targetUs = std::clamp(requested, m_Stats.minimumUs, m_Stats.maximumUs);
        m_Stats.clipped = requested > m_Stats.maximumUs;
        // The quantile sizes protection, but only actual preventable hitches
        // below the selected goal authorize an increase. Raw lateness on clean
        // intervals cannot buy latency, including during initial learning.
        if (m_HitchMissesOnly && !belowMissGoal) {
            // Recovery of the historical percentage cannot revoke a growth
            // request that an actual hitch already justified. A lower measured
            // requirement can still reduce that request.
            const uint64_t authorized = m_HitchMissesOnly >= 9 && m_Stats.recentReleaseMisses ?
                std::max(m_Stats.currentUs, previousTargetUs) : m_Stats.currentUs;
            m_Stats.targetUs = std::min(m_Stats.targetUs, authorized);
            if (m_HitchMissesOnly != 9 && canProbeRelease()) {
                // A raw late-input quantile is not proof that this protection
                // remains necessary. Try a small reduction while actual input
                // hitch coverage is healthy; fresh misses stop the probe.
                const uint64_t probe = std::min<uint64_t>(100, m_Stats.toleranceUs / 4);
                const uint64_t candidate = m_Stats.currentUs -
                    std::min(probe, m_Stats.currentUs - m_Stats.minimumUs);
                m_Stats.targetUs = std::min(m_Stats.targetUs, candidate);
            }
        }
        else if (m_HitchMissesOnly) {
            // Clean inputs cannot enlarge a previously justified request just
            // because older misses still leave the history below goal. Only
            // this actual miss may raise it, and only to its measured demand.
            m_Stats.targetUs = std::min(m_Stats.targetUs,
                std::max(previousTargetUs, missDemandUs));
        }
        if (m_HitchMissesOnly >= 9) {
            if (m_LayerTransitionActive) {
                if (m_LayerTransitionTargetUs < m_Stats.currentUs)
                    m_LayerTransitionTargetUs = std::min(m_LayerTransitionTargetUs, m_Stats.targetUs);
                m_Stats.targetUs = std::clamp(m_LayerTransitionTargetUs, m_Stats.minimumUs, m_Stats.maximumUs);
            }
            else if (m_HitchMissesOnly == 9) applyFeedbackBoundedRelease(atUs,
                    std::clamp(requested, m_Stats.minimumUs, m_Stats.maximumUs));
            if (m_HitchMissesOnly == 9) updateReleaseRate(m_Stats.recentReleaseMisses);
        }
    }
    void move(uint64_t elapsed, bool growthAllowed) {
        m_SlewAudit={m_LastClockUs,
            !growthAllowed && m_Stats.targetUs>m_Stats.currentUs ? m_Stats.currentUs : m_Stats.targetUs,
            m_Stats.currentUs,0};
        struct FinishAudit {
            Stats& stats; SlewAudit& audit;
            ~FinishAudit() {
                const bool up=stats.currentUs>=audit.beforeUs;
                const uint64_t delta=up?stats.currentUs-audit.beforeUs:audit.beforeUs-stats.currentUs;
                const int64_t bounded=int64_t(std::min(delta,uint64_t(std::numeric_limits<int64_t>::max())));
                audit.deltaUs=up?bounded:-bounded;
            }
        } finishAudit{m_Stats,m_SlewAudit};
        const int direction = m_Stats.targetUs > m_Stats.currentUs ? 1 :
            m_Stats.targetUs < m_Stats.currentUs ? -1 : 0;
        if (direction != m_Direction) m_ReleaseFraction = 0;
        m_Direction = direction;
        m_Stats.growthBlocked = direction > 0 && !growthAllowed;
        if (!direction || (direction > 0 && !growthAllowed)) return;
        if (direction < 0 && m_RateDropDrain &&
            m_Stats.currentUs - m_Stats.targetUs > m_Stats.toleranceUs) m_RateDropFastDrain = true;
        if (direction < 0 && m_HitchMissesOnly == 9 && fastLayerDrain()) {
            const uint64_t movement = std::min(m_LayerDrainStepRemainingUs,
                m_Stats.currentUs - m_Stats.targetUs);
            m_LayerDrainStepRemainingUs -= movement;
            m_Stats.currentUs -= movement;
            if (movement) ++m_Stats.lowers;
            if (m_Stats.currentUs == m_Stats.targetUs) m_LayerTransitionActive = false;
            m_ReleaseFraction = 0;
            return;
        }
        // Preserve fractional soft rates (e.g. 37.5 us/s). Both products are
        // bounded: elapsed <= 100 ms, rate <= 1e6, factor <= 1e6.
        const uint64_t rate = direction > 0 ? m_RiseRate : m_Stats.effectiveFallRateUsPerSecond;
        constexpr uint64_t FractionDenominator = 1000000000000ULL;
        if (direction < 0) {
            // Assess the unattenuated next step before spending protection.
            // One conservative pass avoids alternating factors within a tick.
            const uint64_t provisionalStep = (m_ReleaseFraction + elapsed * rate * 1000000) /
                FractionDenominator;
            const uint64_t provisional = m_Stats.currentUs -
                std::min(provisionalStep, m_Stats.currentUs - m_Stats.targetUs);
            m_Stats.recentProposedMisses = recentMissesAt(m_LastClockUs, provisional);
            updateReleaseRate(std::max(m_Stats.recentReleaseMisses, m_Stats.recentProposedMisses));
        }
        const uint64_t factor = direction > 0 ? 1000000 : m_Stats.releaseFactorPpm;
        m_ReleaseFraction += elapsed * rate * factor;
        const uint64_t step = m_ReleaseFraction / FractionDenominator;
        m_ReleaseFraction %= FractionDenominator;
        if (direction > 0) {
            const uint64_t movement = std::min(step, m_Stats.targetUs - m_Stats.currentUs);
            m_Stats.currentUs += movement;
            if (movement) ++m_Stats.raises;
        } else {
            uint64_t movement = std::min(step, m_Stats.currentUs - m_Stats.targetUs);
            if (canProbeRelease()) {
                // advance() and observe() share one interval's allowance. A
                // late decision or repeated calls cannot spend it twice.
                movement = std::min(movement, m_ReleaseStepRemainingUs);
                m_ReleaseStepRemainingUs -= movement;
            }
            const uint64_t candidate = m_Stats.currentUs - movement;
            m_Stats.currentUs = candidate;
            if (movement) {
                ++m_Stats.lowers;
                if (m_Stats.expiredUnknown) ++m_Stats.unknownRelease;
            }
        }
        if (m_LayerTransitionActive && m_Stats.currentUs == m_Stats.targetUs)
            m_LayerTransitionActive = false;
    }
    int64_t m_SourceRelativeQuantileUs = 0, m_SourceRelativeMissQuantileUs = 0;
    bool m_StableAdaptation = false;
    uint64_t m_ReleaseIntervalUs = 0;
    uint64_t m_ReleaseStepRemainingUs = 0;
    uint64_t m_ReleaseTrialFloorUs = 0, m_ReleaseTrialAtUs = 0, m_ReleaseTrialEligible = 0;
    bool m_ReleaseTrialActive = false, m_ReleaseTrialWaiting = false, m_EvidenceBackedRelease = false;
    uint64_t m_LayerTransitionTargetUs = 0, m_LayerDrainStepRemainingUs = 0;
    bool m_LayerTransitionActive = false, m_LayerFastDrain = false;
    bool m_RateDropDrain = false, m_RateDropFastDrain = false;
    struct Sample { uint64_t at, slot; int64_t required; uint64_t epoch; bool absorbed, missed; bool seed = false; };
    std::deque<Sample> m_Samples;
    ExactRollingQuantile m_Requirements, m_MissedRequirements;
    uint64_t m_ValidPairCoverageUs = 0;
    Stats m_Stats;
    uint64_t m_FirstObservationUs = 0, m_LastObservationUs = 0, m_LastEligibleUs = 0;
    uint64_t m_LastClockUs = 0, m_RiseRate = 0, m_FallRate = 0;
    SlewAudit m_SlewAudit;
    uint64_t m_LastEvaluationUs = 0, m_LastTruncatedUs = 0, m_Epoch = 0;
    uint64_t m_PreviousFrame = 0, m_PreviousSlotUs = 0;
    uint64_t m_EvaluatedProbabilityPpm = 0, m_EvaluatedWindowUs = 0, m_ReleaseFraction = 0;
    bool m_PreviousValid = false;
    bool m_Learning = true;
    uint64_t m_ReacquisitionRemaining = 0, m_ReacquisitionUntilUs = 0;
    // 0: historical raw input; 1: historical hitch gate; 2: retain the
    // protection justified by qualifying misses while below the goal;
    // 3: also project retained protection with its source-clock coordinate.
    // 4: forward projection additionally requires measured coverage at goal;
    // the caller limits this policy to rate-rise settling and uses 2 elsewhere.
    // 5/6: the caller projects the actual live mapping separately; history's
    // drift-normalized coordinate never directly moves applied protection.
    uint64_t m_HitchMissesOnly = 0;
    size_t m_LearningTrim = 0;
    int m_Direction = 0;
};
}
