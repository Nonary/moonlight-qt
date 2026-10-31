#pragma once
#include "inputlayercadence.h"
#include "inputcoveragehistory.h"
#include "extrabufferslew.h"

#include "vrrtypes.h"
#include "reserve.h"
#include "workload.h"
#include "prediction.h"
#include "intervalbuffer.h"
#include "inputbuffer.h"
#include "phasetrajectory.h"

#include <cstddef>
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <limits>
#include <vector>

// This list is also the replay/trace parameter schema. Keeping the JSON name,
// C++ member, type, and production default together prevents those copies from
// drifting while avoiding hand-written serialization for every field. The
// display-period latch terms default to zero so production uses the absolute
// headroom thresholds; non-zero ratios remain available to replay captures
// made with display-scaled protection.
#define VRR_TIMING_PARAMETER_FIELDS(X) \
    X(uint64_t, latency_fix_enabled, latencyFixEnabled, 0) \
    X(uint64_t, latency_fix_all_rates, latencyFixAllRates, 0) \
    X(uint64_t, latency_fix_delay_period_per_mille, latencyFixDelayPeriodPerMille, 500) \
    X(uint64_t, playout_delay_cap_source_period_per_mille, playoutDelayCapSourcePeriodPerMille, 0) \
    X(uint64_t, playout_delay_cap_uses_observed_period, playoutDelayCapUsesObservedPeriod, 0) \
    X(uint64_t, bounded_catchup_enabled, boundedCatchupEnabled, 0) \
    X(uint64_t, normalized_source_mapping, normalizedSourceMapping, 0) \
    /* Bounded catch-up carry ceiling: nonzero caps it at this many us and, */ \
    /* with the share, at that share of the source period. Zero: one period. */ \
    X(uint64_t, playout_catchup_carry_limit_us, playoutCatchupCarryLimitUs, 0) \
    X(uint64_t, playout_catchup_carry_limit_period_per_mille, playoutCatchupCarryLimitPeriodPerMille, 0) \
    /* Unbounded credit: nonzero scales the physical VRR room by this share. */ \
    X(uint64_t, playout_input_recovery_credit_room_per_mille, playoutInputRecoveryCreditRoomPerMille, 0) \
    /* Nonzero: a never-seen rate layer is seeded from the previous layer only */ \
    /* when that layer's period is at most this many per mille of the new one. */ \
    X(uint64_t, playout_input_layer_seed_ratio_per_mille, playoutInputLayerSeedRatioPerMille, 0) \
    /* Input-buffer learning ignores the largest `trim` requirements until the */ \
    /* profile tail is supported. */ \
    X(uint64_t, playout_input_percentile_enabled, playoutInputPercentileEnabled, 0) \
    /* Only preventable input misses that caused an interval hitch score/grow. */ \
    /* Stable target adaptation is independent of miss eligibility. */ \
    X(uint64_t, playout_input_goal_window_us, playoutInputGoalWindowUs, 0) \
    /* Share of the goal's allowed loss the solver aims for; 0 aims at the goal. */ \
    X(uint64_t, playout_input_goal_tail_per_mille, playoutInputGoalTailPerMille, 0) \
    /* Nonzero: input readiness uses decoder output plus the GPU decode time */ \
    /* of promptly collected frames, so pacing-queue gating is not jitter. */ \
    X(uint64_t, playout_input_intrinsic_decode_gpu, playoutInputIntrinsicDecodeGpu, 0) \
    X(uint64_t, playout_input_percentile_target_per_million, playoutInputPercentileTargetPerMillion, 990000) \
    X(uint64_t, playout_input_percentile_window_us, playoutInputPercentileWindowUs, 60000000) \
    X(uint64_t, playout_input_percentile_tolerance_us, playoutInputPercentileToleranceUs, 0) \
    X(uint64_t, playout_input_percentile_rise_us_per_second, playoutInputPercentileRiseUsPerSecond, 1000) \
    X(uint64_t, playout_input_percentile_fall_us_per_second, playoutInputPercentileFallUsPerSecond, 250) \
    X(uint64_t, playout_capacity_telemetry, playoutCapacityTelemetry, 0) \
    X(uint64_t, playout_gpu_readiness_adaptation, playoutGpuReadinessAdaptation, 0) \
    X(uint64_t, playout_gpu_readiness_window_us, playoutGpuReadinessWindowUs, 10000000) \
    X(unsigned int, playout_gpu_readiness_percentile, playoutGpuReadinessPercentile, 99) \
    X(uint64_t, playout_gpu_readiness_margin_us, playoutGpuReadinessMarginUs, 500) \
    X(uint64_t, playout_gpu_readiness_attack_us, playoutGpuReadinessAttackUs, 1000) \
    X(uint64_t, playout_gpu_readiness_release_us_per_second, playoutGpuReadinessReleaseUsPerSecond, 250) \
    X(uint64_t, playout_gpu_readiness_maximum_us, playoutGpuReadinessMaximumUs, 12000) \
    X(uint64_t, playout_prediction_only, playoutPredictionOnly, 0) \
    /* Production source mapping is anchored before worker/backend waits. */ \
    X(uint64_t, playout_source_mapping_decoder_output, playoutSourceMappingDecoderOutput, 0) \
    /* Bounded per-frame recovery allowance. */ \
    X(uint64_t, playout_catchup_per_mille, playoutCatchupPerMille, 0) \
    /* Rescue bounded late frames and recover from actual submission lateness. */ \
    X(uint64_t, playout_late_recovery, playoutLateRecovery, 0) \
    X(uint64_t, playout_interval_tolerance_us, playoutIntervalToleranceUs, 0) \
    /* Qualification requires both elapsed time and consecutive samples. */ \
    X(uint64_t, playout_interval_initial_warmup_us, playoutIntervalInitialWarmupUs, 1000000) \
    X(size_t, playout_interval_initial_minimum_samples, playoutIntervalInitialMinimumSamples, 2) \
    X(uint64_t, playout_mean_miss_hold_us, playoutMeanMissHoldUs, 4000000) \
    X(uint64_t, playout_mean_miss_release_us_per_second, playoutMeanMissReleaseUsPerSecond, 200) \
    /* Nonzero floors the adaptive playout delay at this per-mille percentile */ \
    /* of recent timestamp-playout ready offsets (decode completion after the */ \
    /* mapped source slot), so release cannot drain below what current frames */ \
    /* need; rare stalls cannot raise a percentile, and it follows load down. */ \
    X(uint64_t, playout_readiness_floor_per_mille, playoutReadinessFloorPerMille, 0) \
    X(uint64_t, playout_readiness_floor_window_us, playoutReadinessFloorWindowUs, 10000000) \
    X(uint64_t, playout_on_time_target_per_million, playoutOnTimeTargetPerMillion, 990000) \
    X(uint64_t, playout_readiness_window_us, playoutReadinessWindowUs, 3000000) \
    X(uint64_t, playout_require_display_events, playoutRequireDisplayEvents, 0) \
    X(uint64_t, playout_submission_estimate_fallback, playoutSubmissionEstimateFallback, 0) \
    X(uint64_t, playout_native_hitch_adaptation, playoutNativeHitchAdaptation, 0) \
    X(uint64_t, playout_readiness_driven_adaptation, playoutReadinessDrivenAdaptation, 0) \
    X(uint64_t, playout_stable_smoothness_reference, playoutStableSmoothnessReference, 0) \
    X(uint64_t, render_start_preserve_learned_lead, renderStartPreserveLearnedLead, 0) \
    X(uint64_t, playout_smoothness_feedback_enabled, playoutSmoothnessFeedbackEnabled, 0) \
    X(uint64_t, playout_prediction_enabled, playoutPredictionEnabled, 0) \
    /* Slot latching with optional diagnostic safety headroom. */ \
    X(uint64_t, playout_per_frame_latch, playoutPerFrameLatch, 0) \
    /* Constant native synchronization is a presenter capability, not a latch choice. */ \
    X(uint64_t, native_synchronized_presentation, nativeSynchronizedPresentation, 0) \
    X(uint64_t, playout_adaptive_only, playoutAdaptiveOnly, 0) \
    X(uint64_t, playout_rate_protection_enabled, playoutRateProtectionEnabled, 0) \
    X(uint64_t, playout_history_enabled, playoutHistoryEnabled, 0) \
    X(uint64_t, maximum_forward_movement_us, maximumForwardMovementUs, 1000000) \
    X(uint64_t, render_lead_floor_us, renderLeadFloorUs, 1000) \
    X(uint64_t, render_lead_slack_us, renderLeadSlackUs, 0) \
    X(unsigned int, render_baseline_percentile, renderBaselinePercentile, 50) \
    X(uint64_t, pacing_latency_budget_divisor, pacingLatencyBudgetDivisor, 2) \
    X(uint64_t, pacing_latency_extra_period_numerator, pacingLatencyExtraPeriodNumerator, 0) \
    X(uint64_t, pacing_latency_extra_period_denominator, pacingLatencyExtraPeriodDenominator, 1) \
    X(uint64_t, presentation_safety_us, presentationSafetyUs, 0) \
    X(uint64_t, source_playout_delay_us, sourcePlayoutDelayUs, 0) \
    X(uint64_t, timestamp_playout_enabled, timestampPlayoutEnabled, 0) \
    X(uint64_t, playout_offset_window_us, playoutOffsetWindowUs, 3000000) \
    X(uint64_t, playout_offset_slew_us, playoutOffsetSlewUs, 20) \
    /* Source-clock mapping eligibility and bounded slew. */ \
    X(uint64_t, playout_offset_cadence_gate, playoutOffsetCadenceGate, 0) \
    X(uint64_t, playout_offset_slew_us_per_second, playoutOffsetSlewUsPerSecond, 0) \
    X(uint64_t, playout_offset_source_clock, playoutOffsetSourceClock, 0) \
    X(uint64_t, playout_offset_maximum_step_us, playoutOffsetMaximumStepUs, 100) \
    X(size_t, playout_offset_warmup_samples, playoutOffsetWarmupSamples, 64) \
    X(uint64_t, playout_delay_adaptive, playoutDelayAdaptive, 0) \
    X(uint64_t, playout_delay_start_us, playoutDelayStartUs, 0) \
    X(uint64_t, playout_delay_minimum_us, playoutDelayMinimumUs, 1000) \
    X(uint64_t, playout_delay_maximum_us, playoutDelayMaximumUs, 12000) \
    X(uint64_t, playout_delay_percentile_per_mille, playoutDelayPercentilePerMille, 980) \
    X(uint64_t, playout_delay_margin_us, playoutDelayMarginUs, 300) \
    X(uint64_t, playout_delay_tolerance_us, playoutDelayToleranceUs, 0) \
    X(uint64_t, playout_delay_attack_us, playoutDelayAttackUs, 50) \
    X(uint64_t, playout_delay_release_us, playoutDelayReleaseUs, 10) \
    X(size_t, playout_delay_minimum_samples, playoutDelayMinimumSamples, 250) \
    X(size_t, playout_delay_release_samples, playoutDelayReleaseSamples, 500) \
    X(size_t, playout_delay_reservoir_samples, playoutDelayReservoirSamples, 1024) \
    X(uint64_t, playout_band_width_hz, playoutBandWidthHz, 20) \
    X(uint64_t, playout_band_stale_us, playoutBandStaleUs, 120000000) \
    X(uint64_t, playout_stall_exclusion_us, playoutStallExclusionUs, 25000) \
    X(uint64_t, playout_burst_exclusion_per_mille, playoutBurstExclusionPerMille, 0) \
    X(uint64_t, playout_smoothing_gain_per_mille, playoutSmoothingGainPerMille, 0) \
    X(uint64_t, playout_smoothing_period_alpha_per_mille, playoutSmoothingPeriodAlphaPerMille, 50) \
    X(uint64_t, playout_smoothing_max_lag_us, playoutSmoothingMaxLagUs, 8000) \
    X(uint64_t, playout_smoothing_snap_per_mille, playoutSmoothingSnapPerMille, 1000) \
    X(uint64_t, smoothing_mode, smoothingMode, VrrSmoothingOff) \
    X(uint64_t, playout_smoothing_recovery_us, playoutSmoothingRecoveryUs, 200000) \
    /* Zero maximum disables the learned smoothed-cadence readiness reserve. */ \
    X(uint64_t, playout_smoothing_reserve_max_us, playoutSmoothingReserveMaxUs, 0) \
    X(uint64_t, playout_smoothing_reserve_tolerance_us, playoutSmoothingReserveToleranceUs, 0) \
    X(uint64_t, playout_smoothing_reserve_percentile_per_mille, playoutSmoothingReservePercentilePerMille, 0) \
    X(uint64_t, playout_smoothing_reserve_release_us_per_second, playoutSmoothingReserveReleaseUsPerSecond, 0) \
    /* Zero retains interval-EMA-only period tracking for smoothed cadence. */ \
    X(uint64_t, playout_smoothing_period_feedback_per_million, playoutSmoothingPeriodFeedbackPerMillion, 0) \
    /* Zero drops the retiming to the raw slot at a cadence reset; nonzero */ \
    /* eases it there by at most this many microseconds per frame. */ \
    X(uint64_t, playout_smoothing_reset_slew_us, playoutSmoothingResetSlewUs, 0) \
    /* Absorb keeps a fixed presentation reserve. */ \
    X(uint64_t, playout_smoothing_absorb_reserve_us, playoutSmoothingAbsorbReserveUs, 0) \
    X(uint64_t, playout_delay_start_period_per_mille, playoutDelayStartPeriodPerMille, 0) \
    X(uint64_t, playout_delay_maximum_period_per_mille, playoutDelayMaximumPeriodPerMille, 0) \
    X(uint64_t, playout_offset_reseed_frames, playoutOffsetReseedFrames, 1) \
    X(uint64_t, playout_delay_slew_across_bands, playoutDelaySlewAcrossBands, 0) \
    X(uint64_t, playout_prepare_on_arrival, playoutPrepareOnArrival, 0) \
    X(uint64_t, render_start_after_submission_us, renderStartAfterSubmissionUs, 0) \
    X(uint64_t, render_start_minimum_lead_us, renderStartMinimumLeadUs, 1500) \
    X(uint64_t, playout_stall_burst_exclusion, playoutStallBurstExclusion, 0) \
    /* Native and queued presenters have different spacing ownership. */ \
    X(uint64_t, latched_floor_disabled, latchedFloorDisabled, 0) \
    /* Zero anchors display spacing at every Present call; one anchors a */ \
    /* latched present at its predicted flip (prior anchor + display period). */ \
    X(uint64_t, latched_flip_anchor, latchedFlipAnchor, 0) \
    /* Waiting-frame capacity for admission and the delay budget; 0 keeps */ \
    /* the conservative three-frame fallback (VrrMaximumQueuedFrames). */ \
    X(uint64_t, playout_queue_frames, playoutQueueFrames, 0) \
    /* Nonzero latches the first present at a gap this long, so */ \
    /* it cannot tear against a driver-managed LFC repeat after a long gap. */ \
    X(uint64_t, vrr_floor_latch_gap_us, vrrFloorLatchGapUs, 0) \
    /* Nonzero lets the presenter latch a planned tearing present when native */ \
    /* frame statistics show its predecessor still pending or scanning out. */ \
    X(uint64_t, native_flip_protection, nativeFlipProtection, 0) \
    X(uint64_t, readiness_ceiling_us, readinessCeilingUs, 10000) \
    X(uint64_t, minimum_readiness_reserve_us, minimumReadinessReserveUs, 500) \
    X(uint64_t, cold_start_readiness_demand_us, coldStartReadinessDemandUs, 1500) \
    X(uint64_t, arrival_spread_guard_us, arrivalSpreadGuardUs, 900) \
    X(uint64_t, readiness_acquire_step_us, readinessAcquireStepUs, 1000) \
    X(uint64_t, readiness_learning_window_us, readinessLearningWindowUs, 0) \
    X(uint64_t, readiness_floor_period_numerator, readinessPeriodFloorNumerator, 0) \
    X(uint64_t, readiness_floor_period_denominator, readinessPeriodFloorDenominator, 1) \
    X(uint64_t, retain_readiness_on_phase_reset, retainReadinessOnPhaseReset, 0) \
    X(uint64_t, maximum_render_wake_lead_us, maximumRenderWakeLeadUs, 2000) \
    X(uint64_t, maximum_target_wake_lead_us, maximumTargetWakeLeadUs, 500) \
    X(uint64_t, minimum_guard_us, minimumGuardUs, 100) \
    X(uint64_t, latch_enter_headroom_us, latchedPresentationHeadroomUs, 225) \
    X(uint64_t, latch_exit_headroom_us, latchedPresentationExitHeadroomUs, 400) \
    X(size_t, cadence_stability_latch_frames, cadenceStabilityLatchFrames, 64) \
    X(uint64_t, latch_enter_headroom_period_numerator, latchedPresentationHeadroomPeriodNumerator, 0) \
    X(uint64_t, latch_enter_headroom_period_denominator, latchedPresentationHeadroomPeriodDenominator, 1) \
    X(uint64_t, latch_exit_headroom_period_numerator, latchedPresentationExitHeadroomPeriodNumerator, 0) \
    X(uint64_t, latch_exit_headroom_period_denominator, latchedPresentationExitHeadroomPeriodDenominator, 1) \
    X(uint64_t, maximum_base_guard_us, maximumBaseGuardUs, 250) \
    X(uint64_t, maximum_adaptive_guard_us, maximumAdaptiveGuardUs, 1000) \
    X(uint64_t, guard_step_us, guardStepUs, 50) \
    X(size_t, guard_decay_frames, guardDecayFrames, 120) \
    X(size_t, scheduler_learning_samples, schedulerLearningSamples, 19) \
    X(size_t, readiness_learning_samples, readinessLearningSamples, 16) \
    X(size_t, preparation_learning_samples, preparationLearningSamples, 96) \
    X(size_t, minimum_readiness_samples, minimumReadinessSamples, 16) \
    X(size_t, minimum_cadence_samples, minimumCadenceSamples, 6) \
    X(size_t, maximum_cadence_samples, maximumCadenceSamples, 512) \
    X(size_t, rate_candidate_samples, rateCandidateSamples, 3) \
    X(uint64_t, rate_candidate_minimum_us, rateCandidateMinimumUs, 0) \
    X(uint64_t, loose_cadence_window_us, looseCadenceWindowUs, 350000) \
    X(uint64_t, tight_cadence_window_us, tightCadenceWindowUs, 1000000) \
    X(uint64_t, major_cadence_ratio_numerator, majorCadenceRatioNumerator, 7) \
    X(uint64_t, major_cadence_ratio_denominator, majorCadenceRatioDenominator, 2) \
    X(uint64_t, candidate_cadence_ratio_numerator, candidateCadenceRatioNumerator, 2) \
    X(uint64_t, candidate_cadence_ratio_denominator, candidateCadenceRatioDenominator, 1) \
    X(unsigned int, material_rate_change_percent, materialRateChangePercent, 12) \
    X(size_t, phase_error_frames, phaseErrorFrames, 3) \
    X(unsigned int, preparation_percentile, preparationPercentile, 99) \
    X(unsigned int, scheduler_percentile, schedulerPercentile, 95) \
    X(unsigned int, readiness_low_percentile, readinessLowPercentile, 0) \
    X(unsigned int, readiness_tight_percentile, readinessTightPercentile, 100) \
    X(unsigned int, readiness_loose_percentile, readinessLoosePercentile, 80) \
    X(uint64_t, readiness_attack_numerator, readinessAttackNumerator, 1) \
    X(uint64_t, readiness_attack_denominator, readinessAttackDenominator, 1) \
    X(uint64_t, readiness_release_numerator, readinessReleaseNumerator, 1) \
    X(uint64_t, readiness_release_denominator, readinessReleaseDenominator, 32) \
    X(uint64_t, usable_headroom_numerator, usableHeadroomNumerator, 3) \
    X(uint64_t, usable_headroom_denominator, usableHeadroomDenominator, 4) \
    X(uint64_t, loose_headroom_display_periods, looseHeadroomDisplayPeriods, 2) \
    X(uint64_t, base_guard_divisor, baseGuardDivisor, 96) \
    X(uint64_t, preparation_initial_sample_excluded, preparationInitialSampleExcluded, 0) \
    X(uint64_t, preparation_prediction_lead, preparationPredictionLead, 0) \
    X(uint64_t, playout_delay_start_seed_us, playoutDelayStartSeedUs, 0) \
    X(uint64_t, playout_epoch_rate_ratio_per_mille, playoutEpochRateRatioPerMille, 0) \
    X(uint64_t, playout_epoch_sustain_us, playoutEpochSustainUs, 0) \
    X(uint64_t, playout_delay_decrease_slew_us, playoutDelayDecreaseSlewUs, 0) \
    X(uint64_t, playout_epoch_confirm_us, playoutEpochConfirmUs, 0) \
    X(uint64_t, playout_smoothing_readiness_bound, playoutSmoothingReadinessBound, 0) \
    /* Native future deadlines use the logical display clock, not CPU enqueue. */ \
    X(uint64_t, native_scheduled_presentation, nativeScheduledPresentation, 0) \
    /* Worker-paced smoothing uses its own bounded retiming law, not T/second. */ \
    X(uint64_t, playout_smoothing_own_movement, playoutSmoothingOwnMovement, 0) \
    /* Producer-clock GPU service is learned only from prompt collection. */ \
    X(uint64_t, normalized_source_intrinsic_decode_gpu, normalizedSourceIntrinsicDecodeGpu, 0) \
    /* Recorded display-ceiling recovery behavior. */ \
    X(uint64_t, playout_ceiling_phase_replacement, playoutCeilingPhaseReplacement, 0) \
    /* Normalize short paced sends only for offset learning. */ \
    X(uint64_t, playout_offset_send_span_normalization, playoutOffsetSendSpanNormalization, 0)

// Current smoothing modes. Numeric values are serialized, not revision selectors.
enum VrrSmoothingMode : uint64_t {
    VrrSmoothingOff = 0,
    VrrSmoothingWorkerPhaseLocked = 2,
    VrrSmoothingNativeToleranceBounded = 4,
    VrrSmoothingAbsorb = 7,
};

// Every value that changes VRR policy remains replaceable by replay without
// rebuilding the controller. Production callers use these defaults.
struct VrrTimingParameters {
#define VRR_DECLARE_TIMING_PARAMETER(type, jsonName, memberName, defaultValue) \
    type memberName = defaultValue;
    VRR_TIMING_PARAMETER_FIELDS(VRR_DECLARE_TIMING_PARAMETER)
#undef VRR_DECLARE_TIMING_PARAMETER
};

// Stable serialization of the effective policy for calibration identity.
std::string vrrCalibrationPolicy(const VrrTimingParameters& parameters);

// Resolve mode-dependent production policy once for both the live worker and
// the replay baseline. Candidate replay configs may still override any field.
VrrTimingParameters vrrTimingParametersForSession(
    const VrrSessionConfig& config, bool nativeSynchronizedPresentation = false,
    bool nativeScheduledPresentation = false);

struct VrrTimingDiagnostics {
    int64_t readinessPhaseUs = 0;
    uint64_t readinessDemandUs = 0;
    uint64_t appliedReadinessReserveUs = 0;
    uint64_t renderBaselineUs = 0;
    uint64_t renderInsuranceUs = 0;
    uint64_t gpuReadinessLeadUs = 0;
    uint64_t pacingLatencyBudgetUs = 0;
    size_t cadenceSamples = 0;
    size_t rateCandidateSamples = 0;
    size_t readinessSamples = 0;
    size_t preparationSamples = 0;
    size_t renderSchedulerSamples = 0;
    size_t targetSchedulerSamples = 0;
    size_t cleanSpacingFrames = 0;
    size_t phaseErrorFrames = 0;
    bool readinessModelValid = false;
};

// Platform-neutral, feed-forward VRR timing. The controller projects the
// sender clock into the local monotonic epoch and learns bounded readiness,
// render, scheduler, and spacing budgets. It contains no renderer/native API
// types; the worker translates platform observations into neutral timing
// feedback.
struct VrrTimingDecision {
    Vrr13::PhaseTrajectory::Stats trajectory;
    uint64_t replacementWaitUs = 0;
    uint64_t frameNumber = 0;
    uint64_t smoothnessProtectionUs = 0;
    uint64_t requestedPlayoutDelayUs = 0;
    uint64_t submissionSmoothnessSamples = 0, submissionSmoothnessMisses = 0;
    uint64_t nativeSmoothnessSamples = 0, nativeSmoothnessMisses = 0;
    bool playoutCapacityLimited = false;
    uint64_t originalScanoutUs = 0;
    uint64_t predictedScanoutUs = 0;
    uint64_t compositorLeadUs = 0;
    uint64_t recoveryHeadroomUs = 0;
    // The original smoothed slot, before readiness and display-floor clamps.
    // Observation only: a late frame must never rewrite its own deadline.
    uint64_t originalTargetUs = 0;
    uint64_t sourceTimeUs = 0;
    uint64_t sourceIntervalUs = 0;
    uint64_t sourcePeriodUs = 0;
    // Derived diagnostics only; no policy input or trace schema field.
    uint64_t diagnosticSourceTimestampUs = 0;
    bool diagnosticSourceTimestampValid = false;

    int64_t readyOffsetUs = 0;
    // How far the display floor pushed the target past the frame's own slot.
    uint64_t presentationFloorPushUs = 0;
    // Requested display/recovery phase before this frame's readiness clamp.
    // This describes policy phase, not the marginal wait left after readiness.
    // Derived diagnostics only; neither changes scheduling or trace fields.
    uint64_t requestedPhaseTargetUs = 0;
    // How far late-frame recovery deliberately held the target past its own
    // slot: the VRR17 recovery floor, or bounded catch-up's added carry.
    uint64_t recoveryAddedUs = 0;
    int64_t readinessBudgetUs = 0;
    // The source playout delay this target was built with: the adaptive
    // per-band delay under timestamp playout, else the fixed parameter.
    uint64_t playoutDelayUs = 0;
    // Independent limits, sampled with the target. These are budgets, not
    // additional measured latency to add to queue residence or render time.
    uint64_t playoutDelayMaximumUs = 0;
    uint64_t playoutQueueLimitUs = 0;
    uint64_t playoutPresetCapUs = 0;
    int64_t playoutOffsetUs = 0;
    int64_t cadenceSmoothingUs = 0;
    // Net phase requested by smoothing/display/recovery. Do not subtract a
    // different amount of already-incurred input lateness on each frame.
    // A new readiness overrun itself is not deliberate extra protection.
    uint64_t plannedExtraDelayUs() const {
        const uint64_t phaseUs = std::max(originalTargetUs, requestedPhaseTargetUs) - originalTargetUs;
        if (cadenceSmoothingUs >= 0)
            return phaseUs + std::min(uint64_t(cadenceSmoothingUs),
                std::numeric_limits<uint64_t>::max() - phaseUs);
        const uint64_t earlyUs = uint64_t(-(cadenceSmoothingUs + 1)) + 1;
        return phaseUs - std::min(phaseUs, earlyUs);
    }

    uint64_t renderStartUs = 0;
    uint64_t targetUs = 0;
    uint64_t guardUs = 0;
    uint64_t headroomUs = 0;
    uint64_t timingBudgetUs = 0;
    uint64_t renderLeadUs = 0;
    uint64_t renderWakeLeadUs = 0;
    // Preparation begins this much earlier than the non-GPU render budget
    // requires. It is a learned, bounded head start and never moves targetUs.
    uint64_t gpuReadinessLeadUs = 0;
    uint64_t targetWakeLeadUs = 0;

    bool latchedPresentation = false;
    bool usedRtpTimestamp = false;
    bool cadenceEligible = false;
    bool sourceRateChanged = false;
    bool phaseDiscontinuity = false;
    bool rebased = false;
};

class VrrTimingController {
public:
    // A blocking wait measures producer work only when collection started
    // promptly after decoder output. A late poll includes our own queue time.
    static constexpr uint64_t PreciseCompletionWaitUs = 200;
    static constexpr uint64_t PromptCollectionUs = 1000;
    static bool promptDecodeCompletion(uint64_t outputUs, uint64_t observedUs, uint64_t waitUs)
    {
        if (!outputUs || observedUs <= outputUs || waitUs < PreciseCompletionWaitUs) return false;
        const uint64_t collectedUs = observedUs - std::min(observedUs, waitUs);
        return collectedUs <= outputUs || collectedUs - outputUs <= PromptCollectionUs;
    }
    // Late observations use only independently learned service, bounded by
    // observed completion. Without a sample, output is the accounting origin;
    // actual readiness still gates scheduling and presentation separately.
    static uint64_t estimateDecodeCompleteUs(uint64_t decoderOutputUs, uint64_t observedCompleteUs,
                                             uint64_t syncWaitUs, uint64_t typicalGpuUs)
    {
        if (observedCompleteUs <= decoderOutputUs) return decoderOutputUs;
        if (promptDecodeCompletion(decoderOutputUs, observedCompleteUs, syncWaitUs)) return observedCompleteUs;
        return decoderOutputUs + std::min(typicalGpuUs, observedCompleteUs - decoderOutputUs);
    }
    uint64_t typicalDecodeGpuUs() const { return m_DecodeGpuTypicalUs; }
    explicit VrrTimingController(const VrrSessionConfig& config,
                                 bool canLatchPresentation = true);
    VrrTimingController(const VrrSessionConfig& config,
                        bool canLatchPresentation,
                        const VrrTimingParameters& parameters);

    void reset();

    // Starts a fresh source epoch while retaining learned render/wake budgets
    // and the last submission instant used by the display-spacing floor.
    void rebase();

    VrrTimingDecision schedule(const PacedFrame& frame, uint64_t nowUs);

    // Samples affect subsequent frames only. The current presentation target
    // never moves after rendering has begun.
    void notePreparationDuration(uint64_t preparationDurationUs,
                                 uint64_t acquisitionWaitUs = 0,
                                 uint64_t preparationCompleteUs = 0,
                                 uint64_t gpuReadyWaitUs = 0);
    // Successful renderer fence waits are feedback for future preparation
    // starts. Failed/unknown waits are deliberately not learned.
    void noteGpuReadyWait(uint64_t waitUs, bool completed,
                          uint64_t completionUs = 0);
    // A backend may verify render completion only at the final native-present
    // boundary after useful overlap with the cadence hold. Feed its residual
    // CPU wait to future lead learning and its conservative service bound to
    // the growth gate. The observation time is not the frame's readiness time.
    void noteDeferredGpuReady(uint64_t waitUs, bool completed,
                              uint64_t completionUs = 0,
                              uint64_t serviceUpperBoundUs = 0,
                              bool readinessWasPending = false);
    void noteSchedulerDelays(uint64_t renderDelayUs,
                             uint64_t targetDelayUs,
                             bool targetDelayValid);

    // A positive deficit means the worker reached the presentation boundary
    // before the display-spacing floor. The worker corrects the current frame;
    // this feedback adjusts one bounded guard for future frames.
    void noteSpacingDeficit(uint64_t deficitUs);

    // submissionUs is the scheduling boundary (possibly a future accepted
    // deadline). Supply the actual CPU enqueue separately for diagnostics.
    // Immediate presenters may omit it because both boundaries are identical.
    void noteSubmission(bool submitted, bool cancelled,
                        uint64_t submissionUs, uint64_t actualSubmissionUs = 0);
    // Worker supplies actual graph-stage misses against the applied buffer
    // line for diagnostics. Release feedback uses corrected causal observations.
    void noteInputBufferMiss(uint64_t atUs, bool networkMiss, bool decoderMiss) {
        if (m_Parameters.playoutInputPercentileEnabled)
            m_InputBuffer.noteBufferMiss(atUs, networkMiss, decoderMiss);
    }
    void noteTrajectoryResyncDrop() {
        m_PhaseTrajectory.noteResyncDrop();
        if (m_Parameters.playoutCeilingPhaseReplacement != 0)
            m_ExtraBufferSlew.shedRecovery();
        m_CeilingLastPhaseValid = false;
    }
    void noteTrajectoryDeferred() {
        m_PhaseTrajectory.noteDeferred();
        m_FullRatePhaseCount = 0;
        m_CeilingShedding = false;
    }
    Vrr13::PhaseTrajectory::Stats trajectoryStats() const { return m_PhaseTrajectory.stats(); }
    void notePresentation(const Vrr13::PresentationObservation& observation);
    uint64_t nativeCadenceIntervals() const { return m_NativeCadenceIntervals; }
    uint64_t estimatedCadenceIntervals() const { return m_EstimatedCadenceIntervals; }
    uint64_t estimatedCadenceHitches() const { return m_EstimatedCadenceHitches; }
    uint64_t nativeCadenceHitches() const { return m_NativeCadenceHitches; }
    Vrr13::SmoothnessFeedback::Sample smoothnessSample(const VrrTimingDecision& decision) const;
    Vrr13::IntervalBuffer::Stats intervalStats() const {
        auto stats = m_IntervalBuffer.stats();
        stats.inputPercentileEnabled = m_Parameters.playoutInputPercentileEnabled != 0;
        if (stats.inputPercentileEnabled) {
            stats.inputPercentile = m_InputBuffer.stats();
            // Report the session's trailing actual outcomes across
            // all rates. Adaptation continues to use the active layer's own
            // stats internally; switching layers cannot erase playback history.
            m_InputCoverageHistory.report(stats.inputPercentile, true);
            stats.update.beforeUs = m_InputPercentileBeforeUs;
            stats.update.requestedUs = stats.inputPercentile.currentUs;
            stats.update.minimumUs = stats.inputPercentile.minimumUs;
            stats.update.maximumUs = stats.inputPercentile.maximumUs;
            stats.update.holdRemainingUs = 0;
            stats.update.cooldownRemainingUs = 0;
            stats.update.attemptedIncreaseUs = stats.inputPercentile.currentUs > m_InputPercentileBeforeUs ?
                stats.inputPercentile.currentUs - m_InputPercentileBeforeUs : 0;
            stats.update.clippedIncreaseUs = 0;
            using Action = Vrr13::IntervalBuffer::Action;
            stats.update.action = stats.inputPercentile.growthBlocked ? Action::NotAbsorbable :
                stats.inputPercentile.clipped ? Action::Capped :
                stats.inputPercentile.expiredUnknown ? Action::InputPercentileUnknown :
                !stats.inputPercentile.evidenceValid ? Action::NotAbsorbable :
                stats.inputPercentile.targetUs > stats.inputPercentile.currentUs ? Action::InputPercentileGrow :
                stats.inputPercentile.targetUs < stats.inputPercentile.currentUs ? Action::InputPercentileRelease :
                Action::InputPercentileHold;
        }
        stats.present = m_PresentTiming.stats();
        stats.submissionCadence = m_SubmissionCadence.stats();
        stats.displayCadence = m_DisplayCadence.stats();
        return stats;
    }
    uint64_t intervalToleranceUs() const { return m_IntervalBuffer.stats().toleranceUs; }
    uint64_t typicalRenderUs() const;
    uint64_t recoveryHeadroomUs() const;

    uint64_t timingBudgetUs() const;
    int64_t readinessBudgetUs() const;
    uint64_t inputRecoveryCreditUs() const;
    uint64_t catchupCarryLimitUs() const;
    uint64_t headroomUs() const;
    uint64_t sourcePeriodUs() const;
    uint64_t inputLayerPeriodUs() const { return m_InputLayerPeriodUs; }
    uint64_t displayPeriodUs() const;
    uint64_t guardUs() const;
    uint64_t renderLeadUs() const;
    uint64_t gpuReadinessLeadUs() const;
    uint64_t targetWakeLeadUs() const;
    uint64_t earliestSubmissionUs() const;
    uint64_t lastSubmissionUs() const;
    // Frames that may wait in the pacing queue (admission and delay budget).
    size_t queuedFrameCapacity() const;
    // Display-spacing reference for the next present (see latchedFlipAnchor).
    uint64_t spacingAnchorUs() const;
    // Reference the pending present must clear by one display period to be
    // untorn: the predecessor's flip for a tearing present, its call otherwise.
    uint64_t untornReferenceUs() const;
    // The presenter latched the pending (planned tearing) present because
    // native statistics showed its predecessor still pending or scanning out.
    // observedFlipUs is the predecessor's refresh start, zero while pending.
    // Call immediately before noteSubmission() for that same present.
    void noteNativeFlipProtection(uint64_t observedFlipUs);
    bool hasLastSubmission() const;
    // Timestamp playout: the applied sender-to-local clock offset and whether
    // the last scheduled frame used the fixed-delay timestamp path.
    int64_t playoutOffsetUs() const;
    bool timestampPlayoutActive() const;
    // Adaptive playout delay: the delay currently applied, the rate band it
    // belongs to (fitted source rate divided by the band width), and how many
    // lateness samples that band has admitted.
    uint64_t playoutDelayUs() const;
    // Learned Reduce judder readiness reserve; already included in each
    // decision's cadenceSmoothingUs, never in playoutDelayUs.
    uint64_t smoothingReserveUs() const { return m_Parameters.smoothingMode == 7 ?
        absorbReserveUs() : m_SmoothingReserveUs; }
    // Absorb microstutter reserve at the current source period; included in
    // each decision's cadenceSmoothingUs, never in playoutDelayUs.
    uint64_t absorbReserveUs() const;
    uint64_t smoothingPhaseLimitUs() const;
    unsigned int playoutBandIndex() const;
    uint64_t playoutBandSamples() const;
    const VrrTimingParameters& parameters() const;
    VrrTimingDiagnostics diagnostics() const;
    const Vrr13::Reserve& playoutHistory() const { return m_PlayoutHistory; }
    bool loadPlayoutHistory(const std::vector<int64_t>& profile) {
        return !m_HaveTimeline && m_PlayoutHistory.loadProfile(profile);
    }
    // Start at the delay a previous session with the same calibration key
    // settled at. Recorded as a parameter so replay reproduces the start.
    void seedPlayoutDelayStart(uint64_t delayUs) {
        if (!m_HaveTimeline) m_Parameters.playoutDelayStartSeedUs = delayUs;
    }
    uint64_t playoutQueueLimitUs() const;
    bool latencyFixActive() const { return m_LatencyFixActive; }

private:
    Vrr13::PhaseTrajectory m_PhaseTrajectory;
    std::array<uint64_t, 32> m_FullRatePhase {};
    size_t m_FullRatePhaseCount = 0, m_FullRatePhaseNext = 0;
    uint64_t m_CeilingEarnedPhaseUs = 0, m_CeilingLastPhaseUs = 0, m_CeilingLastRemovableUs = 0;
    uint64_t m_CeilingShedFromUs = 0, m_CeilingEarnedPeakUs = 0, m_CeilingEarnedAtUs = 0;
    static constexpr uint64_t kCeilingEarnedHoldUs = 5000000;
    size_t m_CeilingFramesSinceShed = SIZE_MAX / 2;
    bool m_CeilingLastPhaseValid = false, m_CeilingShedding = false;
    Vrr13::ExtraBufferSlew m_ExtraBufferSlew;
    bool m_BufferLayerChanged = false, m_BufferLayerDrain = false;
    bool m_HaveMappingBasis = false;
    bool m_CorrectedMappingBasis = false;
    void updateLatencyFixState();
    uint64_t latencyFixDelayLimitUs() const;
    uint64_t playoutDelayCapUs() const;
    uint64_t gpuReadinessCeilingUs() const;
    bool m_LatencyFixActive = false;
    uint64_t m_CadenceStableSinceUs = 0;
    uint64_t m_PreviousSmoothingIntervalUs = 0;
    int64_t m_SourceGapSmoothingUs = 0;
    std::array<uint64_t, 4> m_SmoothingCadenceIntervals{};
    size_t m_SmoothingCadenceCount = 0;
    size_t m_SmoothingCadenceIndex = 0;
    struct PendingFrame {
        Vrr13::SmoothnessFeedback::Sample smoothness;
        Vrr13::ReadinessPrediction::Probe prediction;
        uint64_t renderSchedulerUs = 0;
        bool valid = false;
        bool cadenceEligible = false;
        bool hasPreparationDuration = false;
        int64_t readyOffsetUs = 0;
        uint64_t preparationDurationUs = 0;
        uint64_t rawPreparationDurationUs = 0;
        uint64_t acquisitionWaitUs = 0;
        uint64_t decodeSyncWaitUs = 0;
        uint64_t deferredGpuServiceUs = 0;
        uint64_t deferredGpuWaitUs = 0;
        uint64_t deferredGpuReadyUs = 0;
        uint64_t preparationCompleteUs = 0;
        uint64_t intervalIntendedUs = 0;
        uint64_t inputSlotUs = 0;
        uint64_t inputObservationUs = 0;
        int64_t inputReadinessBudgetUs = 0;
        int64_t inputRequiredUs = 0;
        int64_t inputClockOffsetUs = 0;
        bool inputValid = false;
        bool intervalValid = false;
        bool trajectoryValid = false;
    };

    struct CadenceObservation {
        uint64_t intervalUs = 0;
        uint64_t confirmedInputLayerPeriodUs = 0;
        uint32_t rtpDelta = 0;
        uint64_t frameDelta = 1;
        bool usedRtpTimestamp = false;
        bool eligible = false;
        bool sourceRateChanged = false;
        bool phaseDiscontinuity = false;
        bool needsRebase = false;
    };

    struct CadenceSample {
        uint64_t frameOrdinal = 0;
        uint64_t rtpTicks = 0;
    };

    struct PlayoutOffsetSample {
        uint64_t observationUs = 0;
        int64_t offsetUs = 0;
    };

    // Per-rate-band lateness reservoir and the delay learned from it.
    struct PlayoutBand {
        std::vector<uint64_t> latenessUs;
        size_t nextIndex = 0;
        uint64_t samplesSeen = 0;
        uint64_t lastUsedUs = 0;
        uint64_t appliedDelayUs = 0;
        bool applied = false;
    };

    bool timestampPlayoutEnabled() const;
    void resetPlayoutOffsets();
    int64_t inputClockOffsetUs(uint64_t nowUs);
    void updateInputDriftFit(uint64_t nowUs);
    int64_t observePlayoutOffset(uint64_t observationUs, int64_t offsetUs,
                                 bool cadenceEligible = true,
                                 bool phaseDiscontinuity = false);
    static uint64_t rtpTicksToUs(uint64_t ticks);
    void updatePlayoutDelay(const PacedFrame& frame,
                            const CadenceObservation& cadence,
                            bool rebased, int64_t readyOffsetUs,
                            uint64_t nowUs);
    uint64_t effectivePlayoutDelayUs() const;
    // Cadence smoothing: returns the signed adjustment to add to the raw
    // mapped slot for this frame, tracking the source period and pulling
    // toward the raw slot by the configured gain. Resets on discontinuities.
    int64_t cadenceSmoothingAdjustUs(const CadenceObservation& cadence,
                                     bool rebased, uint64_t rawBasisUs,
                                     uint64_t playoutDelayUs,
                                     uint64_t reserveUs = 0);
    // Smoothed cadence is only useful where frames can be ready. The reserve
    // moves the smoothed schedule later by the recent shortfall that exceeds
    // the tolerated lateness; it is zero unless its parameters enable it.
    bool smoothingReserveEnabled() const;
    int64_t bufferedSmoothingUs(int64_t smoothingUs) const;
    void observeSmoothingReserve(bool engaged, int64_t shortfallUs,
                                 uint64_t nowUs);
    void resetCadenceSmoothing();
    uint64_t playoutDelayStartUs() const;
    uint64_t playoutDelayMinimumUs() const;
    void noteReadinessFloorSample(uint64_t submissionUs, int64_t readyOffsetUs);
    uint64_t playoutDelayMaximumUs() const;
    void noteIntervalBufferEpoch(uint64_t atUs);
    void updatePlayoutHistory(const PacedFrame& frame,
                              const CadenceObservation& cadence,
                              bool rebased, int64_t requiredUs, uint64_t nowUs);
    static uint64_t scaledPerMille(uint64_t value, uint64_t perMille);

    void clearTimeline(bool retainLearnedBudgets);
    void initializeTimeline(const PacedFrame& frame);
    uint64_t sourceMappingUs(const PacedFrame& frame) const;
    uint64_t estimatedDecodeCompleteUs(const PacedFrame& frame) const;
    void learnDecodeGpu(const PacedFrame& frame);
    uint64_t sendSpanCorrectionUs(const PacedFrame& frame);
    CadenceObservation observeCadence(const PacedFrame& frame);
    void observeRtpCadence(uint32_t rtpDelta,
                           CadenceObservation& observation);
    void appendCadenceSample(std::deque<CadenceSample>& samples,
                             const CadenceSample& sample);
    uint64_t fittedSourcePeriodQ16(
        const std::deque<CadenceSample>& samples) const;
    uint64_t cadenceWindowUs() const;
    bool isMajorCadenceDeparture(uint64_t intervalUs,
                                 uint64_t frameDelta) const;
    bool acceptSourcePeriodQ16(uint64_t periodUsQ16);
    void anchorSourceTime(uint64_t sourceTimeUs);
    void updateLearnedBudgets();
    void updateReadinessModel();
    void applyReadinessBudget(bool acquireReserve,
                              bool immediateAcquisition = false);
    void clampReadinessReserveToPolicy();

    uint64_t pacingLatencyBudgetUs() const;
    uint64_t minimumReadinessReserveUs() const;
    uint64_t renderInsuranceCeilingUs() const;
    uint64_t renderInsuranceUs() const;
    uint64_t readinessReserveCeilingUs() const;
    uint64_t readinessPeriodFloorUs() const;
    size_t readinessLearningSampleLimit() const;
    uint64_t renderLeadFloorUs() const;
    uint64_t renderLeadCeilingUs() const;
    uint64_t readinessCeilingUs() const;
    uint64_t guardCeilingUs() const;
    uint64_t latchedPresentationHeadroomUs() const;
    bool rateProtectedPresentation() const;
    uint64_t latchedPresentationExitHeadroomUs() const;
    uint64_t scaledDisplayPeriodUs(uint64_t numerator,
                                   uint64_t denominator) const;
    static uint64_t periodForRate(int rateHz, uint64_t fallbackUs);
    static uint64_t periodForRateQ16(int rateHz, uint64_t fallbackQ16);
    static uint64_t saturatingAdd(uint64_t left, uint64_t right);
    static uint64_t addSigned(uint64_t value, int64_t adjustment);
    static int64_t signedDifference(uint64_t left, uint64_t right);
    static uint64_t roundedQ16(uint64_t valueQ16);
    static bool withinPercent(uint64_t value, uint64_t reference,
                              unsigned int percent);

    VrrSessionConfig m_Config;
    VrrTimingParameters m_Parameters;
    uint64_t m_ConfiguredStreamPeriodUs = 0;
    uint64_t m_ConfiguredStreamPeriodQ16 = 0;
    uint64_t m_DisplayPeriodUs = 0;
    uint64_t m_BaseGuardUs = 0;
    uint64_t m_GuardUs = 0;

    uint64_t m_SourcePeriodUs = 0;
    uint64_t m_SourcePeriodUsQ16 = 0;
    int64_t m_ReadinessBudgetUs = 0;
    int64_t m_ReadinessPhaseUs = 0;
    uint64_t m_ReadinessDemandUs = 0;
    uint64_t m_AppliedReadinessReserveUs = 0;
    bool m_ReadinessModelValid = false;
    uint64_t m_RenderBaselineUs = 0;
    uint64_t m_RenderLeadUs = 0;
    uint64_t m_RenderWakeLeadUs = 0;
    uint64_t m_TargetWakeLeadUs = 0;
    uint64_t m_GpuReadinessLeadUs = 0;
    struct GpuReadinessSample {
        uint64_t completionUs = 0;
        uint64_t waitUs = 0;
    };
    std::deque<GpuReadinessSample> m_GpuReadinessSamples;
    std::vector<uint64_t> m_GpuReadinessSortedWaits;
    uint64_t m_LastGpuReadinessUpdateUs = 0;
    bool m_CanLatchPresentation = true;
    bool m_LatchedPresentation = false;
    size_t m_CadenceStabilityLatchFramesRemaining = 0;

    bool m_HaveTimeline = false;
    uint64_t m_SourceTimeUs = 0;
    uint64_t m_SourceTimeUsQ16 = 0;
    uint64_t m_SourceFrameOrdinal = 0;
    uint64_t m_UnwrappedRtpTicks = 0;
    int m_LastFrameNumber = -1;
    bool m_HaveLastFrameNumber = false;
    uint32_t m_LastRtpTimestamp = 0;
    bool m_LastTimestampValid = false;
    uint64_t m_RtpConversionRemainder = 0;
    uint64_t m_FrameConversionRemainder = 0;
    bool m_LastCadenceUsedRtp = false;

    bool m_HaveLastSubmission = false;
    bool m_CatchupActive = false;
    uint64_t m_LastSubmissionUs = 0;
    // Earliest time the last submission can reach scanout; equals
    // m_LastSubmissionUs unless latchedFlipAnchor projects a latched flip.
    uint64_t m_SpacingAnchorUs = 0;
    // Pending native latch of the next submission (nativeFlipProtection).
    bool m_NativeLatchPending = false;
    uint64_t m_NativeLatchFlipUs = 0;
    unsigned int m_CleanSpacingFrames = 0;
    unsigned int m_PhaseErrorFrames = 0;

    std::deque<PlayoutOffsetSample> m_PlayoutOffsets;
    bool m_PlayoutOffsetValid = false;
    int64_t m_AppliedPlayoutOffsetUs = 0;
    uint64_t m_PlayoutSamplesSeen = 0;
    bool m_PlayoutOffsetClockValid = false;
    uint64_t m_LastPlayoutOffsetObservationUs = 0;
    uint64_t m_PlayoutOffsetSlewRemainder = 0;
    bool m_TimestampPlayoutActive = false;
    // The applied offset is still slewing toward its window minimum.
    bool m_PlayoutOffsetConverging = false;
    // Decision time of the last abrupt source-rate drop, zero when none is open,
    // and the anchor period of the last layer that settled before it.
    uint64_t m_InputRateDropAtUs = 0;
    uint64_t m_InputRateRiseAtUs = 0;
    uint64_t m_InputSettledLayerPeriodUs = 0;
    // Input history coordinate: applied offset movement minus the drift rate
    // (trailing least-squares slope in mode 3), continuous across reseeds.
    uint64_t m_PlayoutOffsetEpoch = 0;
    bool m_InputClockValid = false;
    uint64_t m_InputClockEpoch = 0;
    int64_t m_InputClockLastOffsetUs = 0;
    uint64_t m_InputClockLastUs = 0;
    int64_t m_InputClockCoordinateUs = 0;
    int64_t m_InputClockRawUs = 0;
    int64_t m_InputClockDriftRemainder = 0;
    struct InputDriftPoint { uint64_t atUs; int64_t rawUs; };
    std::deque<InputDriftPoint> m_InputDriftPoints;
    uint64_t m_InputDriftLastFitUs = 0;
    int64_t m_InputDriftFitPpb = 0;
    std::map<unsigned int, PlayoutBand> m_PlayoutBands;
    Vrr13::Reserve m_PlayoutHistory;
    Vrr13::WorkloadEpisode m_WorkloadEpisode;
    Vrr13::ReadinessPrediction m_ReadinessPrediction;
    Vrr13::IntervalBuffer m_IntervalBuffer;
    Vrr13::InputGoalBuffer m_InputBuffer;
    // VRR headroom last applied to the input buffer; rescaled on rate change.
    uint64_t m_InputHeadroomUs = 0;
    // Layered input buffers, one per source-rate range. Lateness depends on
    // the rate (2026-10-10 22:55: median 2.9 ms at 110 FPS, -0.35 ms at
    // 30 FPS), so each layer keeps its own evidence, protection and coverage.
    // The active layer lives in m_InputBuffer, anchored at
    // m_InputLayerPeriodUs; stored layers wait for their rate to return.
    struct InputLayer {
        uint64_t periodUs = 0;
        Vrr13::InputGoalBuffer buffer;
    };
    enum class InputLayerChange { None, Restored, Created };
    std::vector<InputLayer> m_InputLayers;
    uint64_t m_InputLayerPeriodUs = 0;
    uint64_t m_InputLayerEligibleAtStart = 0;
    Vrr13::InputLayerCadence m_InputLayerCadence;
    Vrr13::InputCoverageHistory m_InputCoverageHistory;
    InputLayerChange updateInputLayer(uint64_t nowUs, const CadenceObservation& cadence);
    // Promptly collected blocking completions only. One shared service estimate
    // keeps source mapping and input accounting independent of our own queue.
    std::array<uint64_t, 64> m_DecodeGpuSamples {};
    size_t m_DecodeGpuNext = 0, m_DecodeGpuCount = 0;
    uint64_t m_DecodeGpuTypicalUs = 0;
    std::array<uint32_t, 512> m_SendSpanSamples {};
    size_t m_SendSpanNext = 0, m_SendSpanCount = 0;
    uint64_t m_SendSpanTypicalUs = 0;
    uint64_t m_InputPercentileBeforeUs = 0;
    // Interval-buffer demand remembered per source-rate epoch, keyed by
    // quarter-octave rate bucket.
    std::vector<std::pair<int, uint64_t>> m_EpochDemands;
    uint64_t m_EpochRateMilliHz = 0;
    uint64_t m_EpochSinceUs = 0;
    uint64_t m_EpochCandidateMilliHz = 0;
    uint64_t m_EpochCandidateSinceUs = 0;
    Vrr13::PresentationPrediction m_PresentationPrediction;
    Vrr13::PresentTiming m_PresentTiming;
    Vrr13::CadenceDetection m_SubmissionCadence, m_DisplayCadence;
    Vrr13::SmoothnessFeedback m_SubmissionSmoothness, m_NativeSmoothness;
    // Lifetime counters for decoder-owned reporting windows. These do not
    // expire with the controller's rolling adaptation histogram.
    uint64_t m_NativeCadenceIntervals = 0, m_NativeCadenceHitches = 0;
    uint64_t m_EstimatedCadenceIntervals = 0, m_EstimatedCadenceHitches = 0;
    bool hasRecentNativeFeedback(uint64_t now) const;
    const Vrr13::SmoothnessFeedback& activeSmoothnessFeedback(uint64_t now) const;
    uint64_t m_RequestedPlayoutDelayUs = 0;
    uint64_t m_UnclampedRequestedPlayoutDelayUs = 0;
    bool m_FeedbackModeValid = false, m_FeedbackLatched = false;
    uint64_t m_LastHistoryArrivalUs = 0;
    unsigned int m_PlayoutBandIndex = 0;
    bool m_PlayoutBandValid = false;
    uint64_t m_AppliedPlayoutDelayUs = 0;
    bool m_AppliedPlayoutDelayValid = false;
    uint64_t m_LastDecodeCompleteUs = 0;
    bool m_HaveLastDecodeComplete = false;
    // Cadence smoothing state: the presented slot the schedule continues
    // from (target minus lead and safety) and the tracked source period.
    bool m_HaveSmoothedBasis = false;
    uint64_t m_LastSmoothedBasisUs = 0;
    uint64_t m_SmoothedPeriodUs = 0;
    int64_t m_SmoothedPeriodRemainder = 0;
    int64_t m_SmoothedPeriodFeedbackRemainder = 0;
    // Whether this frame's slot came from the smoother rather than a reset.
    bool m_SmoothingEngaged = false;
    // Retiming (excluding the reserve) given to the last scheduled frame; a
    // cadence reset eases from here when playoutSmoothingResetSlewUs is set.
    int64_t m_LastSmoothingRetimingUs = 0;
    // Learned smoothed-cadence reserve and the recent lateness the smoother
    // caused by placing frames before their raw slots, which it is chosen from.
    uint64_t m_SmoothingReserveUs = 0;
    uint64_t m_SmoothingReserveReleaseRemainder = 0;
    uint64_t m_LastSmoothingReserveUpdateUs = 0;
    std::array<int64_t, 128> m_SmoothingShortfalls{};
    size_t m_SmoothingShortfallCount = 0;
    size_t m_SmoothingShortfallIndex = 0;
    unsigned int m_FutureProjectionFrames = 0;
    // Frames still to exclude from the lateness reservoir after an arrival
    // stall: the backlog that the stall held up, not the link's jitter.
    uint64_t m_BurstExclusionFrames = 0;

    std::deque<CadenceSample> m_CadenceSamples;
    std::deque<CadenceSample> m_RateCandidateSamples;
    std::deque<int64_t> m_ReadyOffsets;
    // playoutReadinessFloorPerMille: (submission, ready offset) window.
    std::deque<std::pair<uint64_t, int64_t>> m_ReadinessFloorSamples;
    uint64_t m_ReadinessFloorUs = 0;
    uint64_t m_ReadinessFloorUpdatedUs = 0;
    std::deque<uint64_t> m_PreparationDurations;
    bool m_PreparationInitialSampleSeen = false;
    std::deque<uint64_t> m_RenderSchedulerDelays;
    std::deque<uint64_t> m_TargetSchedulerDelays;

    PendingFrame m_Pending;
};
