#pragma once
#include <cstdint>
#include <type_traits>

// Versioned, scalar-only snapshots. Never used as policy input. Capture the
// decision and post-feedback phases separately; they are not interchangeable.
#define VRR_BUFFER_AUDIT_FIELDS(X) \
    X(uint64_t, at_us, atUs) \
    X(uint64_t, applied_us, appliedUs) \
    X(uint64_t, target_us, targetUs) \
    X(uint64_t, current_us, currentUs) \
    X(uint64_t, layer_id, layerId) \
    X(uint64_t, layer_period_us, layerPeriodUs) \
    X(uint64_t, layer_switches, layerSwitches) \
    X(uint64_t, layer_creates, layerCreates) \
    X(uint64_t, layer_restores, layerRestores) \
    X(uint64_t, layer_expirations, layerExpirations) \
    X(uint64_t, cached_layers, cachedLayers) \
    X(uint64_t, last_eligible_us, lastEligibleUs) \
    X(uint64_t, last_observation_us, lastObservationUs) \
    X(uint64_t, eligible_age_us, eligibleAgeUs) \
    X(uint64_t, samples, samples) \
    X(uint64_t, eligible, eligible) \
    X(uint64_t, missed, missed) \
    X(uint64_t, window_us, windowUs) \
    X(uint64_t, probability_ppm, probabilityPpm) \
    X(uint64_t, tolerance_us, toleranceUs) \
    X(uint64_t, minimum_us, minimumUs) \
    X(uint64_t, maximum_us, maximumUs) \
    X(uint64_t, vrr_headroom_us, vrrHeadroomUs) \
    X(int64_t, raw_quantile_us, rawQuantileUs) \
    X(uint64_t, unclipped_target_us, unclippedTargetUs) \
    X(int64_t, readiness_budget_us, readinessBudgetUs) \
    X(int64_t, clock_offset_us, clockOffsetUs) \
    X(uint64_t, reacquisition_remaining, reacquisitionRemaining) \
    X(uint64_t, reacquisition_until_us, reacquisitionUntilUs) \
    X(uint64_t, feedback_frame, feedbackFrame) \
    X(uint64_t, eligibility_reasons, eligibilityReasons) \
    X(uint64_t, last_slew_at_us, slewAtUs) \
    X(uint64_t, last_slew_authorized_target_us, slewAuthorizedTargetUs) \
    X(uint64_t, last_slew_before_us, slewBeforeUs) \
    X(int64_t, last_slew_delta_us, slewDeltaUs) \
    X(uint64_t, flags, flags)

struct VrrBufferAuditSnapshot {
#define VRR_DECLARE_BUFFER_AUDIT(type, name, member) type member = 0;
    VRR_BUFFER_AUDIT_FIELDS(VRR_DECLARE_BUFFER_AUDIT)
#undef VRR_DECLARE_BUFFER_AUDIT
    enum Flag : uint64_t {
        Learning=1, TailUncertain=2, EvidenceValid=4, Clipped=8,
        ExpiredUnknown=16, GrowthBlocked=32, LayerTransition=64,
        FastDrain=128, Reacquiring=256, LastInputEligible=512
    };
    enum EligibilityReason : uint64_t {
        NotSubmitted=1, Cancelled=2, InvalidInterval=4, NoPreparation=8,
        InvalidInputMapping=16, SourceIneligible=32, CapacityUnknown=64,
        ServiceOverloaded=128, NoCausalHitch=256, BeyondCap=512, MissingSlot=1024
    };
};
static_assert(std::is_trivially_copyable<VrrBufferAuditSnapshot>::value,
              "Audit snapshots must remain bounded scalar copies");
