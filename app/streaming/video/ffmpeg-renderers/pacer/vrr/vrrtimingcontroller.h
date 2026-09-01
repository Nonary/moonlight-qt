#pragma once

#include "vrrtypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

// This list is also the replay/trace parameter schema. Keeping the JSON name,
// C++ member, type, and production default together prevents those copies from
// drifting while avoiding hand-written serialization for every field. The
// display-period latch terms default to zero so production uses the absolute
// headroom thresholds; non-zero ratios remain available to replay captures
// made with display-scaled protection.
#define VRR_TIMING_PARAMETER_FIELDS(X) \
    X(uint64_t, maximum_forward_movement_us, maximumForwardMovementUs, 1000000) \
    X(uint64_t, render_lead_floor_us, renderLeadFloorUs, 1000) \
    X(uint64_t, render_lead_ceiling_us, renderLeadCeilingUs, 0) \
    X(uint64_t, render_lead_slack_us, renderLeadSlackUs, 0) \
    X(unsigned int, render_baseline_percentile, renderBaselinePercentile, 50) \
    X(uint64_t, pacing_latency_budget_divisor, pacingLatencyBudgetDivisor, 2) \
    X(uint64_t, pacing_latency_extra_period_numerator, pacingLatencyExtraPeriodNumerator, 0) \
    X(uint64_t, pacing_latency_extra_period_denominator, pacingLatencyExtraPeriodDenominator, 1) \
    X(uint64_t, pacing_latency_queue_mode_extra, pacingLatencyQueueModeExtra, 1) \
    X(uint64_t, presentation_safety_us, presentationSafetyUs, 0) \
    X(uint64_t, source_playout_delay_us, sourcePlayoutDelayUs, 0) \
    X(uint64_t, timestamp_playout_enabled, timestampPlayoutEnabled, 0) \
    X(uint64_t, playout_offset_window_us, playoutOffsetWindowUs, 3000000) \
    X(uint64_t, playout_offset_slew_us, playoutOffsetSlewUs, 20) \
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
    X(uint64_t, latch_base_guard_exit, latchedPresentationBaseGuardExit, 0) \
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
    X(uint64_t, base_guard_divisor, baseGuardDivisor, 96)

// Every value that changes VRR policy remains replaceable by replay without
// rebuilding the controller. Production callers use these defaults.
struct VrrTimingParameters {
#define VRR_DECLARE_TIMING_PARAMETER(type, jsonName, memberName, defaultValue) \
    type memberName = defaultValue;
    VRR_TIMING_PARAMETER_FIELDS(VRR_DECLARE_TIMING_PARAMETER)
#undef VRR_DECLARE_TIMING_PARAMETER
};

// Resolve mode-dependent production policy once for both the live worker and
// the replay baseline. Candidate replay configs may still override any field.
VrrTimingParameters vrrTimingParametersForSession(
    const VrrSessionConfig& config);

struct VrrTimingDiagnostics {
    int64_t readinessPhaseUs = 0;
    uint64_t readinessDemandUs = 0;
    uint64_t appliedReadinessReserveUs = 0;
    uint64_t renderBaselineUs = 0;
    uint64_t renderInsuranceUs = 0;
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
    uint64_t sourceTimeUs = 0;
    uint64_t sourceIntervalUs = 0;
    uint64_t sourcePeriodUs = 0;

    int64_t readyOffsetUs = 0;
    int64_t readinessBudgetUs = 0;
    // The source playout delay this target was built with: the adaptive
    // per-band delay under timestamp playout, else the fixed parameter.
    uint64_t playoutDelayUs = 0;

    uint64_t renderStartUs = 0;
    uint64_t targetUs = 0;
    uint64_t guardUs = 0;
    uint64_t headroomUs = 0;
    uint64_t timingBudgetUs = 0;
    uint64_t renderLeadUs = 0;
    uint64_t renderWakeLeadUs = 0;
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
    void notePreparationDuration(uint64_t preparationDurationUs);
    void noteSchedulerDelays(uint64_t renderDelayUs,
                             uint64_t targetDelayUs,
                             bool targetDelayValid);

    // A positive deficit means the worker reached the presentation boundary
    // before the display-spacing floor. The worker corrects the current frame;
    // this feedback adjusts one bounded guard for future frames.
    void noteSpacingDeficit(uint64_t deficitUs);

    // The worker records its own call boundary and supplies only the neutral
    // lifecycle result. The timing controller has no renderer/native types.
    void noteSubmission(bool submitted, bool cancelled,
                        uint64_t submissionUs);

    uint64_t timingBudgetUs() const;
    int64_t readinessBudgetUs() const;
    uint64_t headroomUs() const;
    uint64_t sourcePeriodUs() const;
    uint64_t displayPeriodUs() const;
    uint64_t guardUs() const;
    uint64_t renderLeadUs() const;
    uint64_t targetWakeLeadUs() const;
    uint64_t earliestSubmissionUs() const;
    uint64_t lastSubmissionUs() const;
    bool hasLastSubmission() const;
    // Timestamp playout: the applied sender-to-local clock offset and whether
    // the last scheduled frame used the fixed-delay timestamp path.
    int64_t playoutOffsetUs() const;
    bool timestampPlayoutActive() const;
    // Adaptive playout delay: the delay currently applied, the rate band it
    // belongs to (fitted source rate divided by the band width), and how many
    // lateness samples that band has admitted.
    uint64_t playoutDelayUs() const;
    unsigned int playoutBandIndex() const;
    uint64_t playoutBandSamples() const;
    const VrrTimingParameters& parameters() const;
    VrrTimingDiagnostics diagnostics() const;

private:
    struct PendingFrame {
        bool valid = false;
        bool cadenceEligible = false;
        bool hasPreparationDuration = false;
        int64_t readyOffsetUs = 0;
        uint64_t preparationDurationUs = 0;
    };

    struct CadenceObservation {
        uint64_t intervalUs = 0;
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
        uint64_t decodeCompleteUs = 0;
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
    int64_t observePlayoutOffset(uint64_t decodeCompleteUs,
                                 int64_t offsetUs);
    static uint64_t rtpTicksToUs(uint64_t ticks);
    void updatePlayoutDelay(const PacedFrame& frame,
                            const CadenceObservation& cadence,
                            bool rebased, int64_t readyOffsetUs,
                            uint64_t nowUs);
    uint64_t effectivePlayoutDelayUs() const;
    uint64_t playoutDelayStartUs() const;
    uint64_t playoutDelayMinimumUs() const;
    uint64_t playoutDelayMaximumUs() const;

    void clearTimeline(bool retainLearnedBudgets);
    void initializeTimeline(const PacedFrame& frame);
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
    bool pacingLatencyPolicyEnabled() const;
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
    uint64_t m_LastSubmissionUs = 0;
    unsigned int m_CleanSpacingFrames = 0;
    unsigned int m_PhaseErrorFrames = 0;

    std::deque<PlayoutOffsetSample> m_PlayoutOffsets;
    bool m_PlayoutOffsetValid = false;
    int64_t m_AppliedPlayoutOffsetUs = 0;
    uint64_t m_PlayoutSamplesSeen = 0;
    bool m_TimestampPlayoutActive = false;
    std::map<unsigned int, PlayoutBand> m_PlayoutBands;
    unsigned int m_PlayoutBandIndex = 0;
    bool m_PlayoutBandValid = false;
    uint64_t m_AppliedPlayoutDelayUs = 0;
    uint64_t m_LastDecodeCompleteUs = 0;
    bool m_HaveLastDecodeComplete = false;

    std::deque<CadenceSample> m_CadenceSamples;
    std::deque<CadenceSample> m_RateCandidateSamples;
    std::deque<int64_t> m_ReadyOffsets;
    std::deque<uint64_t> m_PreparationDurations;
    std::deque<uint64_t> m_RenderSchedulerDelays;
    std::deque<uint64_t> m_TargetSchedulerDelays;

    PendingFrame m_Pending;
};
