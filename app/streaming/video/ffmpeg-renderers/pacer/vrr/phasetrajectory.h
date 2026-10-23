#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace Vrr13 {
// Presentation recovery owns no input observations or buffer demand. Plans
// consume only known slots/ownership; acceptance, not planning, commits phase.
//
// Carry is deliberate lateness spread over successors after a genuinely late
// presentation: the late frame is held on screen longer, then successors run
// slightly fast until phase is regained, instead of compressing one interval
// or skipping. Spacing accuracy outranks recovery speed: each successor repays
// 1/DrainCarryDivisor of the remaining carry, at least MinimumDrainPerMille of
// its interval, never beyond the allowance (the user's interval tolerance) and
// never beyond the VRR room above the panel's fastest refresh. Replaying the
// 2026-10-10 captures, this matched a constant 1% drain's spacing errors with
// up to 1.8 ms less latency; a constant tolerance-sized drain added ~2 points of
// >0.75 ms spacing errors.
// Every submission is late by the scheduler's usual wake-up
// overshoot, and a successor will be too, so that common offset leaves the
// displayed spacing unchanged. Only lateness beyond the recent median, and
// beyond the allowance, creates carry. Anchoring on the raw actual submission
// previously added the overshoot to every frame and ratcheted carry up to ~2
// source periods until the stale policy dropped a frame (~1 drop/s and 16 ms
// extra latency on the 2026-10-10 captures). Carry is
// capped at one source period; beyond it, the successor accepts compression
// down to the caller's physical/ownership floor. Recovery never skips content:
// late frames are held and shown, never dropped to regain phase.
class PhaseTrajectory {
public:
    static constexpr uint64_t DrainCarryDivisor = 32;
    static constexpr uint64_t MinimumDrainPerMille = 5;
    struct Stats {
        uint64_t nominalUs = 0, existingUs = 0, targetUs = 0, addedUs = 0;
        // Recovery's requested phase before this frame's readiness bound.
        // Diagnostic only: current input lateness is not recovery demand.
        uint64_t requestedTargetUs = 0;
        uint64_t carryUs = 0, minimumIntervalUs = 0, physicalPeriodUs = 0;
        uint64_t allowanceUs = 0, intendedIntervalUs = 0;
        uint64_t plans = 0, limited = 0, resets = 0, resyncDrops = 0, deferred = 0, capped = 0;
        bool continuity = false, noHeadroom = false, resyncRecommended = false;
    };
    Stats plan(uint64_t frame, uint64_t intendedSlot, uint64_t nominal,
               uint64_t existing, uint64_t readyBound, uint64_t physicalPeriod,
               uint64_t ownershipFloor, uint64_t allowance, uint64_t sourcePeriod,
               bool valid, uint64_t carryLimit = 0) {
        ++m_Stats.plans;
        m_Stats.nominalUs = nominal;
        m_Stats.existingUs = existing;
        m_Stats.targetUs = std::max(existing, ownershipFloor);
        m_Stats.requestedTargetUs = std::max(nominal, ownershipFloor);
        m_Stats.physicalPeriodUs = physicalPeriod;
        m_Stats.allowanceUs = allowance;
        m_Stats.intendedIntervalUs = 0;
        m_Stats.minimumIntervalUs = physicalPeriod;
        m_Allowance = allowance;
        m_Stats.continuity = valid && m_HaveAnchor && frame == m_Frame + 1 &&
            intendedSlot > m_Slot;
        if (!valid || (m_HaveAnchor && !m_Stats.continuity)) breakSequence();
        if (m_Stats.continuity) {
            const uint64_t interval = intendedSlot - m_Slot;
            m_Stats.intendedIntervalUs = interval;
            // physicalPeriod bounds the repayment to the available VRR room.
            const uint64_t previousCarry = m_Anchor > m_CommittedNominal ? m_Anchor - m_CommittedNominal : 0;
            const uint64_t drain = m_DrainAtAllowance ? std::min(allowance, previousCarry) :
                std::min(allowance, std::max(interval * MinimumDrainPerMille / 1000,
                                             previousCarry / DrainCarryDivisor));
            m_Stats.minimumIntervalUs = std::max(physicalPeriod,
                interval > drain ? interval - drain : 0);
            const uint64_t phaseFloor = add(m_Anchor, m_Stats.minimumIntervalUs);
            const uint64_t period = sourcePeriod ? sourcePeriod : physicalPeriod;
            const uint64_t carryCeiling = add(nominal, carryLimit ? std::min(carryLimit, period) : period);
            if (phaseFloor > carryCeiling) ++m_Stats.capped;
            m_Stats.targetUs = std::max(m_Stats.targetUs, std::min(phaseFloor, carryCeiling));
            m_Stats.requestedTargetUs = std::max(m_Stats.requestedTargetUs,
                                                std::min(phaseFloor, carryCeiling));
        }
        m_PlannedFrame = frame;
        m_PlannedNominal = nominal;
        m_PlannedTarget = m_Stats.targetUs;
        m_Stats.addedUs = m_Stats.targetUs - existing;
        m_Stats.carryUs = m_Stats.targetUs > nominal ? m_Stats.targetUs - nominal : 0;
        const uint64_t interval = m_Stats.continuity ? m_Stats.intendedIntervalUs : sourcePeriod;
        m_Stats.noHeadroom = interval && physicalPeriod >= interval;
        // Never trade a shown frame for phase: the carry ceiling bounds recovery.
        (void) readyBound;
        m_Stats.resyncRecommended = false;
        if (m_Stats.addedUs) ++m_Stats.limited;
        return m_Stats;
    }
    void commit(uint64_t frame, uint64_t intendedSlot, uint64_t accepted,
                uint64_t actualEnqueue, bool submitted, bool cancelled, bool valid) {
        if (!submitted || cancelled || !valid || !accepted) {
            breakSequence();
            return;
        }
        m_Frame = frame;
        m_Slot = intendedSlot;
        // Carry is measured against this frame's planned nominal target.
        m_CommittedNominal = m_PlannedFrame == frame && m_PlannedNominal ? m_PlannedNominal : intendedSlot;
        // Measure lateness against the target this trajectory planned for the
        // frame. On immediate (Wayland mailbox) paths the caller's accepted
        // time is the actual submission itself, so it carries the wake-up
        // overshoot and must not become the anchor. A native accepted future
        // deadline later than the plan is real display lateness.
        const bool planned = m_PlannedFrame == frame && m_PlannedTarget;
        const uint64_t reference = planned ? m_PlannedTarget : accepted;
        const uint64_t shown = std::max(accepted, actualEnqueue);
        const uint64_t late = shown > reference ? shown - reference : 0;
        // The usual overshoot is shared by the successor; only the excess over
        // it, when beyond the allowance, raises the anchor.
        const uint64_t baseline = latenessBaseline();
        const uint64_t excess = late > baseline ? late - baseline : 0;
        m_Anchor = excess > m_Allowance ? add(reference, excess) : reference;
        m_Lateness[m_LatenessNext] = late;
        m_LatenessNext = (m_LatenessNext + 1) % m_Lateness.size();
        m_LatenessCount = std::min(m_LatenessCount + 1, m_Lateness.size());
        m_HaveAnchor = true;
    }
    void noteResyncDrop() { ++m_Stats.resyncDrops; breakSequence(); }
    // The shared buffer budget can restrict intentional recovery after plan().
    // Commit must anchor the plan that will actually be submitted.
    void applyTargetLimit(uint64_t target, uint64_t requested) {
        m_PlannedTarget = m_Stats.targetUs = target;
        m_Stats.requestedTargetUs = requested;
        m_Stats.addedUs = target - std::min(target, m_Stats.existingUs);
        m_Stats.carryUs = target - std::min(target, m_Stats.nominalUs);
    }
    void noteDeferred() { ++m_Stats.deferred; }
    // Repay up to the whole allowance per successor rather than 1/32 of the
    // carry; still bounded by the panel's fastest refresh.
    void setDrainAtAllowance(bool enabled) { m_DrainAtAllowance = enabled; }
    void breakSequence() {
        if (m_HaveAnchor) ++m_Stats.resets;
        m_HaveAnchor = false;
    }
    void reset() { *this = PhaseTrajectory{}; }
    Stats stats() const { return m_Stats; }
    // The smallest recent submission lateness (wake-up overshoot). The next
    // submission will be at least this late as well, so a physical floor
    // measured from the previous actual submission may start this much
    // earlier without beating the panel's fastest refresh.
    uint64_t minimumRecentLatenessUs() const {
        if (!m_LatenessCount) return 0;
        return *std::min_element(m_Lateness.begin(), m_Lateness.begin() + m_LatenessCount);
    }
    // Committed phase of `frame`: its planned target, or its actual
    // submission less the usual overshoot when beyond the allowance.
    bool anchoredAt(uint64_t frame) const { return m_HaveAnchor && m_Frame == frame; }
    uint64_t anchorUs() const { return m_Anchor; }
private:
    uint64_t latenessBaseline() const {
        if (!m_LatenessCount) return 0;
        auto sorted = m_Lateness;
        const auto middle = sorted.begin() + m_LatenessCount / 2;
        std::nth_element(sorted.begin(), middle, sorted.begin() + m_LatenessCount);
        return *middle;
    }
    static uint64_t add(uint64_t a, uint64_t b) {
        return b > std::numeric_limits<uint64_t>::max() - a ?
            std::numeric_limits<uint64_t>::max() : a + b;
    }
    Stats m_Stats;
    uint64_t m_Frame = 0, m_Slot = 0, m_Anchor = 0, m_Allowance = 0;
    uint64_t m_PlannedFrame = 0, m_PlannedNominal = 0, m_PlannedTarget = 0, m_CommittedNominal = 0;
    // Recent submission lateness; a scheduler property, kept across breaks.
    std::array<uint64_t, 31> m_Lateness{};
    size_t m_LatenessNext = 0, m_LatenessCount = 0;
    bool m_HaveAnchor = false, m_DrainAtAllowance = false;
};
}
