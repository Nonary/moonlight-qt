#include "vrrpacingworker.h"

#include <Limelight.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cstdio>
#include <limits>

namespace {

// ~64 seconds of rows at 120 FPS. When the writer thread cannot keep up the
// pacing thread drops rows rather than ever waiting on diagnostics.
// Each compressed chunk covers only a few seconds, limiting crash loss while
// still turning repeated timestamps and controller state into very small,
// infrequent physical writes.
constexpr int kTraceChunkBytes = 256 * 1024;
// Always preserve at least an hour, including the maximum supported 480 FPS
// stream cadence. The physical cap takes effect only after that duration, so
// an unusually incompressible trace remains complete rather than silently
// trading away replay fidelity. Chunk compression keeps normal captures far
// below this limit.
constexpr uint64_t kMinimumTraceDurationUs = 60ULL * 60ULL * 1000000ULL;
constexpr uint64_t kMaximumTraceBytes = 512ULL * 1024ULL * 1024ULL;
constexpr char kTraceMagic[] = "MLVRR1\n";
#define VRR_TRACE_PARAMETER_HEADER(type, jsonName, memberName, defaultValue) \
    ",param_" #jsonName
constexpr char kTraceHeader[] =
    "trace_schema,arrival_sequence,frame,rtp_timestamp,rtp_valid,decode_complete_us,"
    "frame_receive_us,frame_reassembled_us,decode_submit_us,pacer_arrival_us,"
    "arrival_queue_depth_before,arrival_queue_depth_after,queue_accepted,dequeue_us,queue_discontinuity,decision_valid,decision_us,"
    "display_refresh_hz,stream_rate_hz,additional_queued_frame,display_period_us,can_latch_present,sender_interval_us,source_rate_hz,source_period_us,"
    "source_time_us,ready_offset_us,readiness_budget_us,timing_budget_us,render_lead_us,gpu_readiness_lead_us,"
    "render_wake_lead_us,target_wake_lead_us,guard_us,headroom_us,render_start_us,render_wait_final_us,render_wait_overshoot_us,"
    "render_scheduler_delay_us,render_scheduler_delay_valid,render_deadline_already_elapsed,"
    "render_wait_initial_us,render_wait_active_budget_us,render_wait_coarse_sleep_count,render_wait_coarse_requested_total_us,render_wait_coarse_requested_wake_us,render_wait_coarse_return_us,render_wait_coarse_clock_stalled,render_wait_active_entered,render_wait_active_start_us,render_wait_active_limit_us,render_wait_active_yield_count,render_wait_active_clock_stalled,render_wait_active_yield_limit_reached,"
    "prepare_start_us,prepare_end_us,prepare_us,target_us,target_wait_final_us,target_wait_overshoot_us,target_scheduler_delay_us,target_scheduler_delay_valid,target_deadline_already_elapsed,"
    "target_wait_initial_us,target_wait_active_budget_us,target_wait_coarse_sleep_count,target_wait_coarse_requested_total_us,target_wait_coarse_requested_wake_us,target_wait_coarse_return_us,target_wait_coarse_clock_stalled,target_wait_active_entered,target_wait_active_start_us,target_wait_active_limit_us,target_wait_active_yield_count,target_wait_active_clock_stalled,target_wait_active_yield_limit_reached,"
    "present_start_us,submission_boundary_us,presenter_submission_time_valid,presenter_submission_time_us,presenter_submission_time_used,present_end_us,present_call_us,submit_error_us,submission_spacing_us,"
    "spacing_margin_us,spacing_deficit_us,spacing_guard_feedback_us,spacing_corrected,had_prior_submission,tear_classification,tear_risk,completion_queue_depth,disposition,dropped,presented,cancelled,"
    "submission_id_valid,submission_id,latch_valid,latch_submission_id,latch_time_us,latch_time_kind,latch_present_refresh_seq,latch_sync_refresh_seq,latched_present,"
    "used_rtp_timestamp,cadence_eligible,source_rate_changed,phase_discontinuity,rebased,external_rebase_applied,external_rebase_flags,midframe_window_state_flags,deep_trace,"
    "native_present_timing_valid,native_present_start_us,native_present_end_us,native_present_call_us,"
    "present_count_before_valid,present_count_before,frame_stats_before_valid,frame_stats_before_present_count,frame_stats_before_time_us,frame_stats_before_present_refresh_seq,frame_stats_before_sync_refresh_seq,"
    "gpu_ready_attempted,gpu_ready_signal_result_valid,gpu_ready_signal_result,gpu_ready_set_event_result_valid,gpu_ready_set_event_result,gpu_ready_wait_result_valid,gpu_ready_wait_result,"
    "gpu_ready_timing_valid,gpu_ready_signal_start_us,gpu_ready_signal_end_us,gpu_ready_flush_start_us,gpu_ready_flush_end_us,gpu_ready_set_event_start_us,gpu_ready_set_event_end_us,gpu_ready_poll_start_us,gpu_ready_poll_end_us,gpu_ready_fence_value,gpu_ready_poll_completed_value,gpu_ready_completed_before_wait,gpu_ready_completion_lower_bound_us,gpu_ready_completion_upper_bound_us,gpu_ready_completion_uncertainty_us,gpu_ready_wait_start_us,gpu_ready_time_us,gpu_ready_wait_us,"
    "decision_end_us,controller_call_us,stale_check_us,stale_age_us,render_wait_entry_us,target_wait_entry_us,spacing_check_us,presentation_floor_us,spacing_recheck_us,spacing_corrected_floor_us,correction_wait_start_us,correction_wait_end_us,terminal_time_us,"
    "native_backend_valid,native_backend,native_present_result_valid,native_present_result,native_present_parameters_valid,native_present_sync_interval,native_present_flags,"
    "native_vrr_state_valid,native_tearing_supported,native_borderless_flip_model,native_same_gpu_output,native_render_adapter_luid_valid,native_render_adapter_luid,native_swap_chain_allows_tearing,"
    "native_tearing_feature_query_result_valid,native_tearing_feature_query_result,native_tearing_feature_allows_tearing,native_swap_chain_desc_query_result_valid,native_swap_chain_desc_query_result,native_swap_chain_flags,native_swap_chain_swap_effect,native_fullscreen_state_query_result_valid,native_fullscreen_state_query_result,native_fullscreen_exclusive,native_window_flags,"
    "native_present_ready_available,native_foreground_window,native_vrr_fallback_reason,native_desktop_monitor_count,"
    "native_vblank_virtualization_probe_complete,native_vblank_virtualization_call_available,native_vblank_virtualization_result_valid,native_vblank_virtualization_result,native_vblank_virtualization_disabled,"
    "native_display_config_query_result_valid,native_display_config_query_result,native_display_path_valid,native_display_path_flags,native_display_target_available,native_display_source_adapter_luid,native_display_source_id,native_display_target_adapter_luid,native_display_target_id,native_display_output_technology,native_display_rotation,native_display_scaling,native_display_path_refresh_numerator,native_display_path_refresh_denominator,"
    "native_display_signal_valid,native_display_signal_pixel_rate_hz,native_display_signal_hsync_numerator,native_display_signal_hsync_denominator,native_display_signal_vsync_numerator,native_display_signal_vsync_denominator,native_display_signal_active_width,native_display_signal_active_height,native_display_signal_total_width,native_display_signal_total_height,native_display_signal_additional_info_raw,native_display_signal_scanline_ordering,"
    "native_raster_sampling_requested,native_raster_open_result_valid,native_raster_open_result,native_raster_source_valid,native_raster_vidpn_source_id,"
    "native_raster_before_query_result_valid,native_raster_before_query_result,native_raster_before_query_start_us,native_raster_before_query_end_us,native_raster_before_in_vertical_blank,native_raster_before_scanline,"
    "native_raster_after_query_result_valid,native_raster_after_query_result,native_raster_after_query_start_us,native_raster_after_query_end_us,native_raster_after_in_vertical_blank,native_raster_after_scanline,"
    "submission_id_query_result_valid,submission_id_query_result,submission_id_query_start_us,submission_id_query_end_us,frame_stats_query_result_valid,frame_stats_query_result,frame_stats_query_start_us,frame_stats_query_end_us,latch_raw_sync_qpc_valid,latch_raw_sync_qpc_ticks,latch_raw_sync_qpc_frequency_hz,"
    "latch_qpc_correlation_valid,latch_qpc_correlation_reference_ticks,latch_qpc_correlation_reference_time_us,latch_qpc_correlation_span_ticks,"
    "readiness_phase_us,readiness_demand_us,applied_readiness_reserve_us,render_baseline_us,render_insurance_us,gpu_readiness_applied_us,pacing_latency_budget_us,cadence_sample_count,rate_candidate_sample_count,readiness_sample_count,preparation_sample_count,render_scheduler_sample_count,target_scheduler_sample_count,clean_spacing_frames,phase_error_frames,readiness_model_valid,playout_delay_us,cadence_smoothing_us,missed_ticks,"
    "decode_sync_wait_us,prepare_timing_valid,prepare_decode_sync_us,prepare_acquire_us,prepare_render_us,prepare_flush_us,"
    "gap_fills_before,gap_fill_last_us,original_target_us,playout_initial_profile,original_scanout_us,predicted_scanout_us,compositor_lead_us,recovery_headroom_us,smoothness_protection_us,requested_playout_delay_us,submission_smoothness_samples,submission_smoothness_misses,native_smoothness_samples,native_smoothness_misses,playout_capacity_limited,presentation_uncertainty_us"
    VRR_TIMING_PARAMETER_FIELDS(VRR_TRACE_PARAMETER_HEADER)
    ",decoder_output_us,session_latency_mode,session_readiness_hitch_feedback,calibration_loaded,initial_cached_samples,history_version,history_state_valid,history_samples,history_misses,history_duration_us,history_can_release"
    ",session_latency_oscillation,latency_test_phase,session_allow_tearing"
    ",buffer_cap_us,buffer_queue_limit_us,buffer_preset_cap_us,playout_offset_us,presentation_floor_push_us"
    ",buffer_update_valid,buffer_update_at_us,buffer_update_frame,buffer_action,buffer_attributed_frame"
    ",buffer_request_before_us,buffer_request_after_us,buffer_interval_error_us,buffer_lateness_us"
    ",buffer_attempted_increase_us,buffer_clipped_increase_us,buffer_hold_remaining_us,buffer_cooldown_remaining_us"
    ",buffer_calibration_complete,buffer_calibration_samples,buffer_calibration_coverage_us"
    ",prepared_ahead,stage_start_us,stage_decode_ready_us,stage_decode_wait_us,stage_render_start_us,stage_render_end_us,stage_ready_us"
    ",flip_protection_checked,flip_protection_query_result,flip_protection_query_start_us,flip_protection_query_end_us"
    ",flip_protection_pending,flip_protection_reference_us,flip_protection_latched"
    ",decode_hold_us,decode_readiness_observed"
    ",native_scheduled_presentation,native_target_us,scheduling_boundary_us"
    ",native_target_100ns,native_target_reference_100ns,native_target_clock_before_us,native_target_clock_after_us,native_target_uncertainty_us"
    ",native_outstanding_presents,native_retiring_present_id,native_skipped_frames,native_canceled_frames,native_displayed_duration_100ns,native_status_id,native_status,native_dropped_status_frames"
    ",lost_packets"
    ",input_release_allowed,input_release_held,input_release_unknown,input_evidence_us,input_candidate_us,input_candidate_loss_per_million,input_current_loss_per_million"
    ",input_percentile_target_us,input_percentile_current_us,input_percentile_samples,input_percentile_window_us,input_percentile_probability_ppm,input_percentile_tail_uncertain,input_percentile_evidence_valid,input_percentile_clipped,input_percentile_expired_unknown,input_percentile_history_truncated,input_percentile_eligible,input_percentile_ineligible,input_percentile_absorbed,input_percentile_missed,input_percentile_raises,input_percentile_lowers,input_percentile_unknown_release,input_percentile_epoch_resets,input_percentile_recent_eligible,input_percentile_recent_absorbed,input_percentile_recent_missed,input_percentile_last_input_eligible,input_percentile_last_input_required_us,input_percentile_last_input_observation_us,input_percentile_growth_blocked,input_network_buffer_miss,input_decode_buffer_miss,input_buffer_miss_observation_us,input_percentile_hold_until_us,input_percentile_hold_remaining_us,input_percentile_last_buffer_miss_us,input_percentile_last_network_miss_us,input_percentile_last_decoder_miss_us,input_percentile_network_misses,input_percentile_decoder_misses,input_percentile_release_samples,input_percentile_release_misses,input_percentile_release_factor_ppm,input_percentile_effective_fall_rate_us_per_second,input_percentile_proposed_misses,input_percentile_effective_fall_rate_milli_us_per_second"
    ",trajectory_nominal_us,trajectory_existing_us,trajectory_target_us,trajectory_added_us,trajectory_carry_us,trajectory_minimum_interval_us,trajectory_physical_period_us,trajectory_allowance_us,trajectory_intended_interval_us,trajectory_plans,trajectory_limited,trajectory_resets,trajectory_resync_drops,trajectory_deferred,trajectory_continuity,trajectory_no_headroom,trajectory_resync_recommended,trajectory_resync_dropped,trajectory_resync_deferred,trajectory_successor_frame,trajectory_successor_proof"
    ",trace_model_kind,buffer_audit_revision"
#define VRR_BUFFER_BEFORE_HEADER(type, name, member) ",buffer_before_" #name
    VRR_BUFFER_AUDIT_FIELDS(VRR_BUFFER_BEFORE_HEADER)
#undef VRR_BUFFER_BEFORE_HEADER
#define VRR_BUFFER_AFTER_HEADER(type, name, member) ",buffer_after_" #name
    VRR_BUFFER_AUDIT_FIELDS(VRR_BUFFER_AFTER_HEADER)
#undef VRR_BUFFER_AFTER_HEADER
    "\n";
#undef VRR_TRACE_PARAMETER_HEADER

#ifdef _WIN32
bool isUncPath(const QString& path)
{
    return path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//"));
}
#endif


} // namespace

int VrrPacingWorker::traceThreadProc(void* context)
{
    return static_cast<VrrPacingWorker*>(context)->traceRun();
}

int VrrPacingWorker::traceRun()
{
    TraceRow row;
    while (true) {
        if (m_TraceQueue->pop(row)) {
#ifdef VRR_WORKER_REPLAY
            replayTraceConsumed.fetch_add(1, std::memory_order_relaxed);
#endif
            if (m_TraceAcceptingRows.load() ||
                    (m_TraceStopping.load() && !m_TraceWriteFailed && !m_TraceSizeCapped))
                writeTraceRow(row);
            continue;
        }
        if (m_TraceStopping.load() && m_TraceProducersActive.load() == 0) {
            // A producer could have published between the first empty read
            // and the active-count check. Drain that final row before exiting.
            if (m_TraceQueue->pop(row)) {
                if (!m_TraceWriteFailed && !m_TraceSizeCapped) writeTraceRow(row);
                continue;
            }
            if (m_TraceFormat == TraceFormat::ChunkedCompressed) {
                flushTraceChunk();
            }
            return 0;
        }
        // Poll only the background writer. Producers never wait for it or
        // contend with a reader-held mutex while delivering a frame.
        SDL_Delay(2);
    }
}

void VrrPacingWorker::writeTraceRow(const TraceRow& row)
{
    m_TraceLatestArrivalUs = std::max(m_TraceLatestArrivalUs,
                                      row.input.arrivalUs);
    const VrrTimingDecision& decision = row.decision;
    const VrrTimingDiagnostics& diagnostics = row.diagnostics;
    // The writer must never read mutable controller parameters. Reconstruct
    // this row's policy from immutable session settings and its captured mode.
    const VrrTimingParameters& parameters = m_TraceParameters;
    // Loaded before either worker starts; retain the actual startup input
    // without reading mutable controller state on the trace writer thread.
    const VrrPresentFeedback& feedback = row.feedback;
    const FrameTelemetry& telemetry = row.telemetry;
    const uint64_t nativePresentDurationUs =
        feedback.nativePresentTimingValid &&
        feedback.nativePresentEndUs >= feedback.nativePresentStartUs ?
            feedback.nativePresentEndUs - feedback.nativePresentStartUs : 0;
    const uint64_t gpuReadyWaitUs = feedback.gpuReadyTimingValid &&
        feedback.gpuReadyTimeUs >= feedback.gpuReadyWaitStartUs ?
            feedback.gpuReadyTimeUs - feedback.gpuReadyWaitStartUs : 0;
    const uint64_t gpuReadyCompletionLowerBoundUs =
        feedback.gpuReadyTimingValid ?
            (feedback.gpuReadyCompletedBeforeWait ?
                 feedback.gpuReadySignalStartUs :
                 feedback.gpuReadyPollStartUs) :
            0;
    const uint64_t gpuReadyCompletionUpperBoundUs =
        feedback.gpuReadyTimingValid ?
            (feedback.gpuReadyCompletedBeforeWait ?
                 feedback.gpuReadyPollEndUs :
                 feedback.gpuReadyTimeUs) :
            0;
    const uint64_t gpuReadyCompletionUncertaintyUs =
        gpuReadyCompletionUpperBoundUs >=
                gpuReadyCompletionLowerBoundUs ?
            gpuReadyCompletionUpperBoundUs -
                gpuReadyCompletionLowerBoundUs :
            0;
    const uint64_t displayPeriodUs = m_Config.displayRefreshHz > 0 ?
        (1000000ULL + static_cast<uint64_t>(m_Config.displayRefreshHz) / 2) /
            static_cast<uint64_t>(m_Config.displayRefreshHz) : 0;

    QByteArray line;
    line.reserve(2048);
    auto separator = [&line]() {
        if (!line.isEmpty()) {
            line.append(',');
        }
    };
    auto addUnsigned = [&line, &separator](uint64_t value) {
        separator();
        line.append(QByteArray::number(value));
    };
    auto addSigned = [&line, &separator](int64_t value) {
        separator();
        line.append(QByteArray::number(value));
    };
    auto addBool = [&addUnsigned](bool value) {
        addUnsigned(value ? 1 : 0);
    };
    auto addText = [&line, &separator](const char* value) {
        separator();
        line.append(value);
    };

    addUnsigned(5);
    addUnsigned(row.input.arrivalSequence);
    addSigned(row.frameNumber);
    addUnsigned(row.rtpTimestamp);
    addBool(row.timestampValid);
    addUnsigned(row.decodeCompleteUs);
    addUnsigned(row.receiveUs);
    addUnsigned(row.reassembledUs);
    addUnsigned(row.decodeSubmitUs);
    addUnsigned(row.input.arrivalUs);
    addUnsigned(row.input.queueDepthBefore);
    addUnsigned(row.input.queueDepthAfter);
    addBool(row.input.queueAccepted);
    addUnsigned(row.input.dequeueUs);
    addBool(row.input.queueDiscontinuity);
    addBool(row.decisionValid);
    addUnsigned(telemetry.decisionTimeUs);
    addSigned(m_Config.displayRefreshHz);
    addSigned(m_Config.streamRateHz);
    addBool(m_Config.allowAdditionalQueuedFrame);
    addUnsigned(displayPeriodUs);
    addBool(m_CanLatchPresentation);
    addUnsigned(decision.sourceIntervalUs);
    separator();
    line.append(QByteArray::number(
        decision.sourcePeriodUs == 0 ? 0.0 :
            1000000.0 / static_cast<double>(decision.sourcePeriodUs), 'f', 3));
    addUnsigned(decision.sourcePeriodUs);
    addUnsigned(decision.sourceTimeUs);
    addSigned(decision.readyOffsetUs);
    addSigned(decision.readinessBudgetUs);
    addUnsigned(decision.timingBudgetUs);
    addUnsigned(decision.renderLeadUs);
    addUnsigned(decision.gpuReadinessLeadUs);
    addUnsigned(decision.renderWakeLeadUs);
    addUnsigned(decision.targetWakeLeadUs);
    addUnsigned(decision.guardUs);
    addUnsigned(decision.headroomUs);
    addUnsigned(decision.renderStartUs);
    addUnsigned(telemetry.renderWaitFinalUs);
    addUnsigned(telemetry.renderWaitOvershootUs);
    addUnsigned(telemetry.renderSchedulerDelayUs);
    addBool(telemetry.renderSchedulerDelayValid);
    addBool(telemetry.renderDeadlineAlreadyElapsed);
    addUnsigned(telemetry.renderWait.initialNowUs);
    addUnsigned(telemetry.renderWait.activeWaitUs);
    addUnsigned(telemetry.renderWait.coarseSleepCount);
    addUnsigned(telemetry.renderWait.coarseSleepRequestedUs);
    addUnsigned(telemetry.renderWait.coarseSleepRequestedWakeUs);
    addUnsigned(telemetry.renderWait.coarseSleepReturnUs);
    addBool(telemetry.renderWait.coarseSleepClockStalled);
    addBool(telemetry.renderWait.activeWaitEntered);
    addUnsigned(telemetry.renderWait.activeWaitStartUs);
    addUnsigned(telemetry.renderWait.activeWaitLimitUs);
    addUnsigned(telemetry.renderWait.activeWaitYieldCount);
    addBool(telemetry.renderWait.activeWaitClockStalled);
    addBool(telemetry.renderWait.activeWaitYieldLimitReached);
    addUnsigned(telemetry.preparationStartUs);
    addUnsigned(telemetry.preparationEndUs);
    addUnsigned(telemetry.preparationDurationUs);
    addUnsigned(decision.targetUs);
    addUnsigned(telemetry.targetWaitFinalUs);
    addUnsigned(telemetry.targetWaitOvershootUs);
    addUnsigned(telemetry.targetSchedulerDelayUs);
    addBool(telemetry.targetSchedulerDelayValid);
    addBool(telemetry.targetDeadlineAlreadyElapsed);
    addUnsigned(telemetry.targetWait.initialNowUs);
    addUnsigned(telemetry.targetWait.activeWaitUs);
    addUnsigned(telemetry.targetWait.coarseSleepCount);
    addUnsigned(telemetry.targetWait.coarseSleepRequestedUs);
    addUnsigned(telemetry.targetWait.coarseSleepRequestedWakeUs);
    addUnsigned(telemetry.targetWait.coarseSleepReturnUs);
    addBool(telemetry.targetWait.coarseSleepClockStalled);
    addBool(telemetry.targetWait.activeWaitEntered);
    addUnsigned(telemetry.targetWait.activeWaitStartUs);
    addUnsigned(telemetry.targetWait.activeWaitLimitUs);
    addUnsigned(telemetry.targetWait.activeWaitYieldCount);
    addBool(telemetry.targetWait.activeWaitClockStalled);
    addBool(telemetry.targetWait.activeWaitYieldLimitReached);
    addUnsigned(telemetry.presentStartUs);
    addUnsigned(telemetry.submissionBoundaryUs);
    addBool(feedback.submissionTimeValid);
    addUnsigned(feedback.submissionTimeUs);
    addBool(telemetry.usedPresenterSubmissionTime);
    addUnsigned(telemetry.presentEndUs);
    addUnsigned(telemetry.presentDurationUs);
    addSigned(telemetry.submitErrorUs);
    addUnsigned(telemetry.presentSpacingUs);
    addSigned(telemetry.spacingMarginUs);
    addUnsigned(telemetry.spacingDeficitUs);
    addUnsigned(telemetry.spacingGuardFeedbackUs);
    addBool(telemetry.spacingCorrected);
    addBool(telemetry.hadPriorSubmission);
    addText(tearClassification(row));
    addBool(feedback.presented && !decision.latchedPresentation &&
            !feedback.flipProtectionLatched &&
            telemetry.hadPriorSubmission && telemetry.spacingMarginUs < 0);
    addUnsigned(row.completionQueueDepth);
    addText(traceDispositionName(row.disposition));
    addBool(row.disposition != TraceDisposition::Presented);
    addBool(feedback.presented);
    addBool(feedback.cancelled);
    addBool(feedback.submissionIdValid);
    addUnsigned(feedback.submissionId);
    addBool(feedback.latchSampleValid);
    addUnsigned(feedback.latchSubmissionId);
    addUnsigned(feedback.latchTimeUs);
    addUnsigned(static_cast<uint64_t>(feedback.latchTimeKind));
    addUnsigned(feedback.latchPresentRefreshSequence);
    addUnsigned(feedback.latchRefreshSequence);
    addBool(decision.latchedPresentation);
    addBool(decision.usedRtpTimestamp);
    addBool(decision.cadenceEligible);
    addBool(decision.sourceRateChanged);
    addBool(decision.phaseDiscontinuity);
    addBool(decision.rebased);
    addBool(telemetry.externalRebaseApplied);
    addUnsigned(telemetry.externalRebaseFlags);
    addUnsigned(telemetry.midframeWindowStateFlags);
    addBool(m_DeepTraceEnabled);
    addBool(feedback.nativePresentTimingValid);
    addUnsigned(feedback.nativePresentStartUs);
    addUnsigned(feedback.nativePresentEndUs);
    addUnsigned(nativePresentDurationUs);
    addBool(feedback.presentCountBeforeValid);
    addUnsigned(feedback.presentCountBefore);
    addBool(feedback.frameStatsBeforeValid);
    addUnsigned(feedback.frameStatsBeforePresentCount);
    addUnsigned(feedback.frameStatsBeforeTimeUs);
    addUnsigned(feedback.frameStatsBeforePresentRefreshSequence);
    addUnsigned(feedback.frameStatsBeforeRefreshSequence);
    addBool(feedback.gpuReadyAttempted);
    addBool(feedback.gpuReadySignalResultValid);
    addSigned(feedback.gpuReadySignalResult);
    addBool(feedback.gpuReadySetEventResultValid);
    addSigned(feedback.gpuReadySetEventResult);
    addBool(feedback.gpuReadyWaitResultValid);
    addUnsigned(feedback.gpuReadyWaitResult);
    addBool(feedback.gpuReadyTimingValid);
    addUnsigned(feedback.gpuReadySignalStartUs);
    addUnsigned(feedback.gpuReadySignalEndUs);
    addUnsigned(feedback.gpuReadyFlushStartUs);
    addUnsigned(feedback.gpuReadyFlushEndUs);
    addUnsigned(feedback.gpuReadySetEventStartUs);
    addUnsigned(feedback.gpuReadySetEventEndUs);
    addUnsigned(feedback.gpuReadyPollStartUs);
    addUnsigned(feedback.gpuReadyPollEndUs);
    addUnsigned(feedback.gpuReadyFenceValue);
    addUnsigned(feedback.gpuReadyPollCompletedValue);
    addBool(feedback.gpuReadyCompletedBeforeWait);
    addUnsigned(gpuReadyCompletionLowerBoundUs);
    addUnsigned(gpuReadyCompletionUpperBoundUs);
    addUnsigned(gpuReadyCompletionUncertaintyUs);
    addUnsigned(feedback.gpuReadyWaitStartUs);
    addUnsigned(feedback.gpuReadyTimeUs);
    addUnsigned(gpuReadyWaitUs);
    addUnsigned(telemetry.decisionEndUs);
    addUnsigned(telemetry.decisionEndUs >= telemetry.decisionTimeUs ?
        telemetry.decisionEndUs - telemetry.decisionTimeUs : 0);
    addUnsigned(telemetry.staleCheckUs);
    addUnsigned(telemetry.staleAgeUs);
    addUnsigned(telemetry.renderWaitEntryUs);
    addUnsigned(telemetry.targetWaitEntryUs);
    addUnsigned(telemetry.spacingCheckUs);
    addUnsigned(telemetry.presentationFloorUs);
    addUnsigned(telemetry.spacingRecheckUs);
    addUnsigned(telemetry.spacingCorrectedFloorUs);
    addUnsigned(telemetry.correctionWaitStartUs);
    addUnsigned(telemetry.correctionWaitEndUs);
    addUnsigned(row.terminalTimeUs);
    addBool(feedback.nativeBackendValid);
    addUnsigned(static_cast<uint32_t>(feedback.nativeBackend));
    addBool(feedback.nativePresentResultValid);
    addSigned(feedback.nativePresentResult);
    addBool(feedback.nativePresentParametersValid);
    addUnsigned(feedback.nativePresentSyncInterval);
    addUnsigned(feedback.nativePresentFlags);
    addBool(feedback.nativeVrrStateValid);
    addBool(feedback.nativeTearingSupported);
    addBool(feedback.nativeBorderlessFlipModel);
    addBool(feedback.nativeSameGpuOutput);
    addBool(feedback.nativeRenderAdapterLuidValid);
    addUnsigned(feedback.nativeRenderAdapterLuid);
    addBool(feedback.nativeSwapChainAllowsTearing);
    addBool(feedback.nativeTearingFeatureQueryResultValid);
    addSigned(feedback.nativeTearingFeatureQueryResult);
    addBool(feedback.nativeTearingFeatureAllowsTearing);
    addBool(feedback.nativeSwapChainDescQueryResultValid);
    addSigned(feedback.nativeSwapChainDescQueryResult);
    addUnsigned(feedback.nativeSwapChainFlags);
    addUnsigned(feedback.nativeSwapChainSwapEffect);
    addBool(feedback.nativeFullscreenStateQueryResultValid);
    addSigned(feedback.nativeFullscreenStateQueryResult);
    addBool(feedback.nativeFullscreenExclusive);
    addUnsigned(feedback.nativeWindowFlags);
    addBool(feedback.nativePresentReadyAvailable);
    addBool(feedback.nativeForegroundWindow);
    addUnsigned(static_cast<uint32_t>(
        feedback.nativeVrrFallbackReason));
    addUnsigned(feedback.nativeDesktopMonitorCount);
    addBool(feedback.nativeVblankVirtualizationProbeComplete);
    addBool(feedback.nativeVblankVirtualizationCallAvailable);
    addBool(feedback.nativeVblankVirtualizationResultValid);
    addSigned(feedback.nativeVblankVirtualizationResult);
    addBool(feedback.nativeVblankVirtualizationDisabled);
    addBool(feedback.nativeDisplayConfigQueryResultValid);
    addUnsigned(feedback.nativeDisplayConfigQueryResult);
    addBool(feedback.nativeDisplayPathValid);
    addUnsigned(feedback.nativeDisplayPathFlags);
    addBool(feedback.nativeDisplayTargetAvailable);
    addUnsigned(feedback.nativeDisplaySourceAdapterLuid);
    addUnsigned(feedback.nativeDisplaySourceId);
    addUnsigned(feedback.nativeDisplayTargetAdapterLuid);
    addUnsigned(feedback.nativeDisplayTargetId);
    addUnsigned(feedback.nativeDisplayOutputTechnology);
    addUnsigned(feedback.nativeDisplayRotation);
    addUnsigned(feedback.nativeDisplayScaling);
    addUnsigned(feedback.nativeDisplayPathRefreshNumerator);
    addUnsigned(feedback.nativeDisplayPathRefreshDenominator);
    addBool(feedback.nativeDisplaySignalValid);
    addUnsigned(feedback.nativeDisplaySignalPixelRateHz);
    addUnsigned(feedback.nativeDisplaySignalHSyncNumerator);
    addUnsigned(feedback.nativeDisplaySignalHSyncDenominator);
    addUnsigned(feedback.nativeDisplaySignalVSyncNumerator);
    addUnsigned(feedback.nativeDisplaySignalVSyncDenominator);
    addUnsigned(feedback.nativeDisplaySignalActiveWidth);
    addUnsigned(feedback.nativeDisplaySignalActiveHeight);
    addUnsigned(feedback.nativeDisplaySignalTotalWidth);
    addUnsigned(feedback.nativeDisplaySignalTotalHeight);
    addUnsigned(feedback.nativeDisplaySignalAdditionalInfoRaw);
    addUnsigned(feedback.nativeDisplaySignalScanLineOrdering);
    addBool(feedback.nativeRasterSamplingRequested);
    addBool(feedback.nativeRasterOpenResultValid);
    addSigned(feedback.nativeRasterOpenResult);
    addBool(feedback.nativeRasterSourceValid);
    addUnsigned(feedback.nativeRasterVidPnSourceId);
    addBool(feedback.nativeRasterBeforePresent.queryResultValid);
    addSigned(feedback.nativeRasterBeforePresent.queryResult);
    addUnsigned(feedback.nativeRasterBeforePresent.queryStartUs);
    addUnsigned(feedback.nativeRasterBeforePresent.queryEndUs);
    addBool(feedback.nativeRasterBeforePresent.inVerticalBlank);
    addUnsigned(feedback.nativeRasterBeforePresent.scanLine);
    addBool(feedback.nativeRasterAfterPresent.queryResultValid);
    addSigned(feedback.nativeRasterAfterPresent.queryResult);
    addUnsigned(feedback.nativeRasterAfterPresent.queryStartUs);
    addUnsigned(feedback.nativeRasterAfterPresent.queryEndUs);
    addBool(feedback.nativeRasterAfterPresent.inVerticalBlank);
    addUnsigned(feedback.nativeRasterAfterPresent.scanLine);
    addBool(feedback.submissionIdQueryResultValid);
    addSigned(feedback.submissionIdQueryResult);
    addUnsigned(feedback.submissionIdQueryStartUs);
    addUnsigned(feedback.submissionIdQueryEndUs);
    addBool(feedback.frameStatsQueryResultValid);
    addSigned(feedback.frameStatsQueryResult);
    addUnsigned(feedback.frameStatsQueryStartUs);
    addUnsigned(feedback.frameStatsQueryEndUs);
    addBool(feedback.latchRawSyncQpcValid);
    addUnsigned(feedback.latchRawSyncQpcTicks);
    addUnsigned(feedback.latchRawSyncQpcFrequency);
    addBool(feedback.latchQpcCorrelationValid);
    addUnsigned(feedback.latchQpcCorrelationReferenceTicks);
    addUnsigned(feedback.latchQpcCorrelationReferenceTimeUs);
    addUnsigned(feedback.latchQpcCorrelationSpanTicks);
    addSigned(diagnostics.readinessPhaseUs);
    addUnsigned(diagnostics.readinessDemandUs);
    addUnsigned(diagnostics.appliedReadinessReserveUs);
    addUnsigned(diagnostics.renderBaselineUs);
    addUnsigned(diagnostics.renderInsuranceUs);
    // This field is the completed present-ready wait for this row. The
    // learned lead used by the next decision is recorded separately as
    // gpu_readiness_lead_us above.
    addUnsigned(gpuReadyWaitUs);
    addUnsigned(diagnostics.pacingLatencyBudgetUs);
    addUnsigned(diagnostics.cadenceSamples);
    addUnsigned(diagnostics.rateCandidateSamples);
    addUnsigned(diagnostics.readinessSamples);
    addUnsigned(diagnostics.preparationSamples);
    addUnsigned(diagnostics.renderSchedulerSamples);
    addUnsigned(diagnostics.targetSchedulerSamples);
    addUnsigned(diagnostics.cleanSpacingFrames);
    addUnsigned(diagnostics.phaseErrorFrames);
    addBool(diagnostics.readinessModelValid);
    addUnsigned(decision.playoutDelayUs);
    addSigned(decision.cadenceSmoothingUs);
    addUnsigned(decision.missedTicks);
    addUnsigned(telemetry.decodeSyncWaitUs);
    addBool(telemetry.prepareTimingValid);
    addUnsigned(telemetry.prepareDecodeSyncUs);
    addUnsigned(telemetry.prepareAcquireUs);
    addUnsigned(telemetry.prepareRenderUs);
    addUnsigned(telemetry.prepareFlushUs);
    // Reserved historical gap-fill columns preserve trace compatibility.
    addUnsigned(0);
    addUnsigned(0);
    addUnsigned(decision.originalTargetUs);
    separator();
    line.append(m_InitialPlayoutProfile);
    addUnsigned(decision.originalScanoutUs);
    addUnsigned(decision.predictedScanoutUs);
    addUnsigned(decision.compositorLeadUs);
    addUnsigned(decision.recoveryHeadroomUs);
    addUnsigned(decision.smoothnessProtectionUs);
    addUnsigned(decision.requestedPlayoutDelayUs);
    addUnsigned(decision.submissionSmoothnessSamples);
    addUnsigned(decision.submissionSmoothnessMisses);
    addUnsigned(decision.nativeSmoothnessSamples);
    addUnsigned(decision.nativeSmoothnessMisses);
    addUnsigned(decision.playoutCapacityLimited);
    addUnsigned(feedback.presentationUncertaintyUs);
#define VRR_ADD_TRACE_PARAMETER(type, jsonName, memberName, defaultValue) \
    addUnsigned(static_cast<uint64_t>(parameters.memberName));
    VRR_TIMING_PARAMETER_FIELDS(VRR_ADD_TRACE_PARAMETER)
#undef VRR_ADD_TRACE_PARAMETER
    addUnsigned(row.decoderOutputUs);
    addSigned(row.latencyMode);
    addUnsigned(m_Config.readinessHitchFeedback);
    addUnsigned(m_CalibrationLoaded);
    addUnsigned(m_InitialCachedSamples);
    addSigned(m_HistoryVersion);
    addUnsigned(row.decisionValid);
    addUnsigned(row.historySamples);
    addUnsigned(row.historyMisses);
    addUnsigned(row.historyDurationUs);
    addUnsigned(row.historyCanRelease);
    addUnsigned(0); // Retired oscillation diagnostic column.
    addUnsigned(0);
    addUnsigned(m_Config.allowTearing);
    addUnsigned(decision.playoutDelayMaximumUs);
    addUnsigned(decision.playoutQueueLimitUs);
    addUnsigned(decision.playoutPresetCapUs);
    addSigned(decision.playoutOffsetUs);
    addUnsigned(decision.presentationFloorPushUs);
    const auto& update = row.bufferStats.update;
    // Stale/failed rows may precede the observer call. Never attribute the
    // preceding frame's update to them, nor inspect worker state on producers.
    addUnsigned(row.decisionValid && parameters.playoutResponsiveBuffer >= 6 &&
                update.frame == static_cast<uint64_t>(row.frameNumber) && update.atUs != 0);
    addUnsigned(update.atUs);
    addUnsigned(update.frame);
    addUnsigned(static_cast<uint64_t>(update.action));
    addUnsigned(update.attributedFrame);
    addUnsigned(update.beforeUs);
    addUnsigned(update.requestedUs);
    addUnsigned(update.intervalErrorUs);
    addUnsigned(update.latenessUs);
    addUnsigned(update.attemptedIncreaseUs);
    addUnsigned(update.clippedIncreaseUs);
    addUnsigned(update.holdRemainingUs);
    addUnsigned(update.cooldownRemainingUs);
    addBool(row.bufferStats.initialCalibrationComplete);
    addUnsigned(row.bufferStats.calibrationSamples);
    addUnsigned(row.bufferStats.calibrationCoverageUs);
    addUnsigned(row.telemetry.preparedAhead);
    addUnsigned(row.telemetry.preparationStage.startUs);
    addUnsigned(row.telemetry.preparationStage.decodeReadyUs);
    addUnsigned(row.telemetry.preparationStage.decodeWaitUs);
    addUnsigned(row.telemetry.preparationStage.renderStartUs);
    addUnsigned(row.telemetry.preparationStage.renderEndUs);
    addUnsigned(row.telemetry.preparationStage.readyUs);
    addBool(feedback.flipProtectionChecked);
    addSigned(feedback.flipProtectionQueryResult);
    addUnsigned(feedback.flipProtectionQueryStartUs);
    addUnsigned(feedback.flipProtectionQueryEndUs);
    addBool(feedback.flipProtectionPending);
    addUnsigned(feedback.flipProtectionReferenceUs);
    addBool(feedback.flipProtectionLatched);
    addUnsigned(row.decodeHoldUs);
    addBool(row.telemetry.decodeReadinessObserved);
    addBool(row.telemetry.nativeScheduledPresentation);
    addUnsigned(row.feedback.nativeTargetValid ? row.feedback.nativeTargetUs : 0);
    addUnsigned(row.telemetry.schedulingBoundaryUs);
    addUnsigned(row.feedback.nativeTargetTime100ns);
    addUnsigned(row.feedback.nativeTargetReferenceTime100ns);
    addUnsigned(row.feedback.nativeTargetClockBeforeUs);
    addUnsigned(row.feedback.nativeTargetClockAfterUs);
    addUnsigned(row.feedback.nativeTargetUncertaintyUs);
    addUnsigned(row.feedback.nativeOutstandingPresents);
    addUnsigned(row.feedback.nativeRetiringPresentId);
    addUnsigned(row.feedback.nativeSkippedFrames);
    addUnsigned(row.feedback.nativeCanceledFrames);
    addUnsigned(row.feedback.nativeDisplayedDuration100ns);
    addUnsigned(row.feedback.nativeStatusId);
    addUnsigned(row.feedback.nativeStatus);
    addUnsigned(row.feedback.nativeDroppedStatusFrames);
    addUnsigned(row.lostPackets);
    addUnsigned(row.bufferStats.inputReleaseAllowed);
    addUnsigned(row.bufferStats.inputReleaseHeld);
    addUnsigned(row.bufferStats.inputReleaseUnknown);
    addUnsigned(row.bufferStats.inputEvidenceUs);
    addUnsigned(row.bufferStats.inputCandidateUs);
    addUnsigned(uint64_t(row.bufferStats.inputCandidateLoss * 1000000));
    addUnsigned(uint64_t(row.bufferStats.inputCurrentLoss * 1000000));
    addUnsigned(row.bufferStats.inputPercentile.targetUs);
    addUnsigned(row.bufferStats.inputPercentile.currentUs);
    addUnsigned(row.bufferStats.inputPercentile.samples);
    addUnsigned(row.bufferStats.inputPercentile.windowUs);
    addUnsigned(row.bufferStats.inputPercentile.probabilityPpm);
    addUnsigned(row.bufferStats.inputPercentile.tailUncertain);
    addUnsigned(row.bufferStats.inputPercentile.evidenceValid);
    addUnsigned(row.bufferStats.inputPercentile.clipped);
    addUnsigned(row.bufferStats.inputPercentile.expiredUnknown);
    addUnsigned(row.bufferStats.inputPercentile.historyTruncated);
    addUnsigned(row.bufferStats.inputPercentile.eligible);
    addUnsigned(row.bufferStats.inputPercentile.ineligible);
    addUnsigned(row.bufferStats.inputPercentile.absorbed);
    addUnsigned(row.bufferStats.inputPercentile.missed);
    addUnsigned(row.bufferStats.inputPercentile.raises);
    addUnsigned(row.bufferStats.inputPercentile.lowers);
    addUnsigned(row.bufferStats.inputPercentile.unknownRelease);
    addUnsigned(row.bufferStats.inputPercentile.epochResets);
    addUnsigned(row.bufferStats.inputPercentile.recentEligible);
    addUnsigned(row.bufferStats.inputPercentile.recentAbsorbed);
    addUnsigned(row.bufferStats.inputPercentile.recentMissed);
    addUnsigned(row.bufferStats.inputPercentile.lastInputEligible);
    addSigned(row.bufferStats.inputPercentile.lastInputRequiredUs);
    addUnsigned(row.bufferStats.inputPercentile.lastInputObservationUs);
    addUnsigned(row.bufferStats.inputPercentile.growthBlocked);
    addUnsigned(row.telemetry.inputNetworkBufferMiss);
    addUnsigned(row.telemetry.inputDecodeBufferMiss);
    addUnsigned(row.telemetry.inputBufferMissObservationUs);
    addUnsigned(row.bufferStats.inputPercentile.holdUntilUs);
    addUnsigned(row.bufferStats.inputPercentile.holdRemainingUs);
    addUnsigned(row.bufferStats.inputPercentile.lastBufferMissUs);
    addUnsigned(row.bufferStats.inputPercentile.lastNetworkMissUs);
    addUnsigned(row.bufferStats.inputPercentile.lastDecoderMissUs);
    addUnsigned(row.bufferStats.inputPercentile.cumulativeNetworkMisses);
    addUnsigned(row.bufferStats.inputPercentile.cumulativeDecoderMisses);
    addUnsigned(row.bufferStats.inputPercentile.recentReleaseSamples);
    addUnsigned(row.bufferStats.inputPercentile.recentReleaseMisses);
    addUnsigned(row.bufferStats.inputPercentile.releaseFactorPpm);
    addUnsigned(row.bufferStats.inputPercentile.effectiveFallRateUsPerSecond);
    addUnsigned(row.bufferStats.inputPercentile.recentProposedMisses);
    addUnsigned(row.bufferStats.inputPercentile.effectiveFallRateMilliUsPerSecond);
    addUnsigned(row.decision.trajectory.nominalUs);
    addUnsigned(row.decision.trajectory.existingUs);
    addUnsigned(row.decision.trajectory.targetUs);
    addUnsigned(row.decision.trajectory.addedUs);
    addUnsigned(row.decision.trajectory.carryUs);
    addUnsigned(row.decision.trajectory.minimumIntervalUs);
    addUnsigned(row.decision.trajectory.physicalPeriodUs);
    addUnsigned(row.decision.trajectory.allowanceUs);
    addUnsigned(row.decision.trajectory.intendedIntervalUs);
    addUnsigned(row.decision.trajectory.plans);
    addUnsigned(row.decision.trajectory.limited);
    addUnsigned(row.decision.trajectory.resets);
    addUnsigned(row.decision.trajectory.resyncDrops);
    addUnsigned(row.decision.trajectory.deferred);
    addUnsigned(row.decision.trajectory.continuity);
    addUnsigned(row.decision.trajectory.noHeadroom);
    addUnsigned(row.decision.trajectory.resyncRecommended);
    addUnsigned(row.telemetry.trajectoryResyncDropped);
    addUnsigned(row.telemetry.trajectoryResyncDeferred);
    addSigned(row.telemetry.trajectorySuccessorFrame);
    addUnsigned(row.telemetry.trajectorySuccessorProof);
#ifdef VRR_WORKER_REPLAY
    addUnsigned(2); // Virtual production-worker closed loop, never a live capture.
#else
    addUnsigned(0);
#endif
    addUnsigned(row.telemetry.bufferAuditValid ? 1 : 0);
    const auto addAudit = [&](const VrrBufferAuditSnapshot& audit) {
#define VRR_WRITE_BUFFER_AUDIT(type, name, member) \
        if constexpr (std::is_signed<type>::value) addSigned(audit.member); \
        else addUnsigned(audit.member);
        VRR_BUFFER_AUDIT_FIELDS(VRR_WRITE_BUFFER_AUDIT)
#undef VRR_WRITE_BUFFER_AUDIT
    };
    addAudit(row.telemetry.bufferBefore);
    addAudit(row.bufferAfter);
    line.append('\n');

    if (m_TraceFormat == TraceFormat::ChunkedCompressed) {
        m_TraceDecodedHash.addData(line);
        m_TraceChunk.append(line);
        if (m_TraceChunk.size() >= kTraceChunkBytes) {
            flushTraceChunk();
        }
        return;
    }

    const size_t bytesWritten = std::fwrite(
        line.constData(), 1, static_cast<size_t>(line.size()), m_TraceFile);
    m_TraceBytesWritten += bytesWritten;
    if (bytesWritten != static_cast<size_t>(line.size())) {
        m_TraceWriteFailed = true;
        m_TraceAcceptingRows.store(false);
    }
    else {
        m_TraceDecodedHash.addData(line);
        if (m_TraceBytesWritten >= kMaximumTraceBytes &&
                minimumTraceDurationCaptured()) {
            m_TraceSizeCapped = true;
            m_TraceAcceptingRows.store(false);
        }
    }
}

void VrrPacingWorker::flushTraceChunk(bool enforceSizeCap)
{
    if (m_TraceChunk.isEmpty() || m_TraceFile == nullptr) {
        return;
    }

    const QByteArray compressed = qCompress(m_TraceChunk, 6);
    const uint32_t compressedBytes = static_cast<uint32_t>(compressed.size());
    const uint64_t recordBytes = sizeof(compressedBytes) + compressedBytes;

    const unsigned char lengthBytes[4] = {
        static_cast<unsigned char>(compressedBytes & 0xff),
        static_cast<unsigned char>((compressedBytes >> 8) & 0xff),
        static_cast<unsigned char>((compressedBytes >> 16) & 0xff),
        static_cast<unsigned char>((compressedBytes >> 24) & 0xff),
    };
    const size_t lengthWritten = std::fwrite(
        lengthBytes, 1, sizeof(lengthBytes), m_TraceFile);
    const size_t payloadWritten = std::fwrite(
        compressed.constData(), 1, static_cast<size_t>(compressed.size()),
        m_TraceFile);
    if (lengthWritten != sizeof(lengthBytes) ||
        payloadWritten != static_cast<size_t>(compressed.size())) {
        m_TraceWriteFailed = true;
        m_TraceAcceptingRows.store(false);
    }
    else {
        m_TraceBytesWritten += recordBytes;
        // A completed chunk is independently recoverable after a crash. This
        // is a low-frequency write performed only by the background thread.
        if (std::fflush(m_TraceFile) != 0) {
            m_TraceWriteFailed = true;
            m_TraceAcceptingRows.store(false);
        }
        else if (enforceSizeCap &&
                 m_TraceBytesWritten >= kMaximumTraceBytes &&
                 minimumTraceDurationCaptured()) {
            m_TraceSizeCapped = true;
            m_TraceAcceptingRows.store(false);
        }
    }
    m_TraceChunk.clear();
}

bool VrrPacingWorker::minimumTraceDurationCaptured() const
{
    return m_TraceLatestArrivalUs >= m_TraceStartUs &&
        m_TraceLatestArrivalUs - m_TraceStartUs >= kMinimumTraceDurationUs;
}

const char* VrrPacingWorker::traceDispositionName(
    TraceDisposition disposition)
{
    switch (disposition) {
    case TraceDisposition::Presented:
        return "presented";
    case TraceDisposition::OutputDropped:
        return "output_dropped";
    case TraceDisposition::QueueCapacity:
        return "queue_capacity";
    case TraceDisposition::QueueStale:
        return "queue_stale";
    case TraceDisposition::ArrivalRejected:
        return "arrival_rejected";
    case TraceDisposition::SuspensionDiscard:
        return "suspension_discard";
    case TraceDisposition::ShutdownDiscard:
        return "shutdown_discard";
    case TraceDisposition::Interrupted:
        return "interrupted";
    case TraceDisposition::Stale:
        return "stale";
    case TraceDisposition::PreparationFailed:
        return "preparation_failed";
    }
    return "unknown";
}

const char* VrrPacingWorker::tearClassification(const TraceRow& row) const
{
    if (!row.feedback.presented) {
        return "not_presented";
    }
    if ((row.decision.latchedPresentation ||
         row.feedback.flipProtectionLatched) && m_CanLatchPresentation) {
        return "confirmed_safe_latched";
    }
    if (!row.telemetry.hadPriorSubmission) {
        return "first_submission_unknown";
    }
    if (row.telemetry.spacingMarginUs < 0) {
        return "adaptive_interval_violation";
    }
    // This proves that the client respected the panel-period floor. It is not
    // a literal hardware tear observation: a nonfunctional VRR path or an
    // unreported scanout transition can still require external validation.
    return "adaptive_interval_safe";
}

void VrrPacingWorker::openTraceIfRequested()
{
    const QString tracePath = qEnvironmentVariable("MOONLIGHT_VRR_TRACE");
    if (tracePath.isEmpty()) {
        return;
    }

#ifdef _WIN32
    // A buffered stdio stream still flushes synchronously when its buffer
    // fills. Keep diagnostic I/O off the time-critical worker's network path.
    if (isUncPath(tracePath)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "MOONLIGHT_VRR_TRACE must use a local path; refusing network trace: %s",
                    qPrintable(tracePath));
        return;
    }

#endif

    // The launcher owns one path for the application lifetime, while each
    // reconnect creates a new worker. Preserve the completed connection before
    // reusing that path so launchers and their latest-trace links still name
    // the current capture. A failed archive must never fall through to truncate.
    QFile previousTrace(tracePath);
    if (previousTrace.exists()) {
        const QFileInfo traceInfo(previousTrace);
        const QString extension = traceInfo.suffix().isEmpty() ? QString() :
            QStringLiteral(".") + traceInfo.suffix();
        QString archivePath;
        for (unsigned connection = 1; ; ++connection) {
            archivePath = traceInfo.absoluteDir().filePath(
                traceInfo.completeBaseName() +
                QStringLiteral("-connection-%1").arg(connection) + extension);
            if (!QFileInfo::exists(archivePath)) break;
        }
        if (!previousTrace.rename(archivePath)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to preserve previous VRR trace; tracing disabled: %s",
                        qPrintable(tracePath));
            return;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR trace: preserved earlier connection at %s",
                    QFile::encodeName(archivePath).constData());
    }

#ifdef _WIN32
    // Use the checked CRT variant on Windows so enabling diagnostics does not
    // introduce a deprecation warning in the normal application build.
    if (_wfopen_s(&m_TraceFile, reinterpret_cast<const wchar_t*>(tracePath.utf16()), L"wb") != 0) {
        m_TraceFile = nullptr;
    }
#else
    m_TraceFile = std::fopen(QFile::encodeName(tracePath).constData(), "wb");
#endif
    if (m_TraceFile == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to open MOONLIGHT_VRR_TRACE file: %s", qPrintable(tracePath));
        return;
    }

    // A .csv suffix explicitly requests the directly readable compatibility
    // format. The recommended .vrrtrace format compresses independent chunks
    // and typically reduces a full session by an order of magnitude.
    m_TraceFormat = tracePath.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive) ?
        TraceFormat::Csv : TraceFormat::ChunkedCompressed;

    // Amortize local diagnostic writes instead of flushing on every frame.
    // fclose() commits the CSV tail; compressed chunks flush independently.
    std::setvbuf(m_TraceFile, nullptr, _IOFBF, 1024 * 1024);

    m_TraceBytesWritten = 0;
    m_TraceChunk.clear();
    m_TraceQueue = std::make_unique<Vrr13::TraceQueue<TraceRow, 8192>>();
    m_TraceDecodedHash.reset();
    if (m_TraceFormat == TraceFormat::ChunkedCompressed) {
        const size_t magicBytes = sizeof(kTraceMagic) - 1;
        if (std::fwrite(kTraceMagic, 1, magicBytes, m_TraceFile) != magicBytes) {
            std::fclose(m_TraceFile);
            m_TraceFile = nullptr;
            return;
        }
        m_TraceBytesWritten = magicBytes;
        m_TraceChunk.append(kTraceHeader);
    }
    else {
        const size_t headerBytes = sizeof(kTraceHeader) - 1;
        if (std::fwrite(kTraceHeader, 1, headerBytes, m_TraceFile) !=
                headerBytes) {
            std::fclose(m_TraceFile);
            m_TraceFile = nullptr;
            return;
        }
        m_TraceBytesWritten = headerBytes;
    }
    m_TraceDecodedHash.addData(
        kTraceHeader, sizeof(kTraceHeader) - 1);

    // All formatting and I/O happen on this thread; the pacing worker only
    // enqueues row copies. Without it, a buffered flush would periodically
    // stall the TIME_CRITICAL thread and perturb the timing being measured.
    m_TraceStopping.store(false);
    m_TraceSizeCapped = false;
    m_TraceWriteFailed = false;
    m_TraceDroppedRows.store(0, std::memory_order_relaxed);
    m_TraceRowsEnqueued.store(0, std::memory_order_relaxed);
    m_TraceArrivalSequence.store(0);
    m_TraceStartUs = LiGetMicroseconds();
    m_TraceLatestArrivalUs = m_TraceStartUs;
    m_TraceThread = SDL_CreateThread(VrrPacingWorker::traceThreadProc,
                                     "VrrTrace", this);
    if (m_TraceThread == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Disabling VRR trace: writer thread failed: %s",
                    SDL_GetError());
        std::fclose(m_TraceFile);
        m_TraceFile = nullptr;
        m_TraceChunk.clear();
        return;
    }
    m_TraceAcceptingRows.store(true);
}

void VrrPacingWorker::closeTrace()
{
    // Close admission before asking the writer to drain. Otherwise a producer
    // could enqueue after the writer observes an empty stopping queue, leaving
    // a normal shutdown with an unwritten row behind the footer.
    if (m_TraceThread != nullptr) {
        m_TraceAcceptingRows.store(false);
        m_TraceStopping.store(true);
        SDL_WaitThread(m_TraceThread, nullptr);
        m_TraceThread = nullptr;
    }
    else {
        m_TraceAcceptingRows.store(false);
    }

    const size_t droppedRows =
        m_TraceDroppedRows.exchange(0, std::memory_order_relaxed);
    const uint64_t rowsEnqueued =
        m_TraceRowsEnqueued.exchange(0, std::memory_order_relaxed);
    const uint64_t arrivalsAllocated =
        m_TraceArrivalSequence.load(std::memory_order_relaxed);
#ifdef VRR_WORKER_REPLAY
    const bool replayHadTrace = m_TraceFile != nullptr;
    const bool replayWasCapped = m_TraceSizeCapped;
    bool replayCloseFailed = false;
#endif
    if (m_TraceFile != nullptr) {
        if (m_TraceFormat == TraceFormat::Csv &&
                std::fflush(m_TraceFile) != 0) {
            m_TraceWriteFailed = true;
        }
        const QByteArray footer =
            QByteArrayLiteral(
                "#vrr_trace_footer,format_version=2,clean_shutdown=1,"
                "arrival_sequence_allocated=") +
            QByteArray::number(arrivalsAllocated) +
            QByteArrayLiteral(",rows_enqueued=") +
            QByteArray::number(rowsEnqueued) +
            QByteArrayLiteral(",rows_dropped=") +
            QByteArray::number(static_cast<qulonglong>(droppedRows)) +
            QByteArrayLiteral(",size_capped=") +
            QByteArray::number(m_TraceSizeCapped ? 1 : 0) +
            QByteArrayLiteral(",write_failed=") +
            QByteArray::number(m_TraceWriteFailed ? 1 : 0) +
            QByteArrayLiteral(",decoded_sha256=") +
            m_TraceDecodedHash.result().toHex() +
            QByteArrayLiteral("\n");
        if (m_TraceFormat == TraceFormat::ChunkedCompressed) {
            m_TraceChunk.append(footer);
            // The footer is metadata, not a captured row. Crossing the size
            // threshold by these few bytes did not truncate the capture.
            flushTraceChunk(false);
        }
        else {
            const size_t footerBytes = static_cast<size_t>(footer.size());
            if (std::fwrite(
                    footer.constData(), 1, footerBytes, m_TraceFile) !=
                    footerBytes ||
                    std::fflush(m_TraceFile) != 0) {
                m_TraceWriteFailed = true;
            }
        }
    }
    if (droppedRows != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR trace dropped %zu rows to protect pacing",
                    droppedRows);
    }

    if (m_TraceSizeCapped) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR trace was capped at 512 MiB after preserving at least one hour");
        m_TraceSizeCapped = false;
    }
    if (m_TraceWriteFailed) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR trace reported a write failure");
    }

    if (m_TraceFile != nullptr) {
        if (std::fclose(m_TraceFile) != 0) {
#ifdef VRR_WORKER_REPLAY
            replayCloseFailed = true;
#endif
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "VRR trace close reported a write failure");
        }
        m_TraceFile = nullptr;
    }
#ifdef VRR_WORKER_REPLAY
    if (replayTraceResult) {
        *replayTraceResult = replayHadTrace && !replayWasCapped &&
            !m_TraceWriteFailed && !replayCloseFailed && droppedRows == 0 &&
            arrivalsAllocated > 0 && rowsEnqueued == arrivalsAllocated;
    }
#endif
    m_TraceChunk.clear();
    m_TraceWriteFailed = false;
}
