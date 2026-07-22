#include "vrrpacingworker.h"

#include "vrr/vrrtargetwaiter.h"
#include "vrr/vrrtimingcontroller.h"

#include <Limelight.h>

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace {

// Keep enough decoded successors to absorb the short gap-then-burst delivery
// pattern seen near the panel ceiling. Capacity remains bounded and evicts the
// oldest queued successor under sustained pressure, so it cannot accumulate
// an unbounded latency backlog.
constexpr size_t kMaximumQueuedFrames = 3;
// ~64 seconds of rows at 120 FPS. When the writer thread cannot keep up the
// pacing thread drops rows rather than ever waiting on diagnostics.
constexpr size_t kMaximumTraceQueueRows = 8192;
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
    "trace_schema,arrival_sequence,frame,rtp_timestamp,rtp_valid,decode_complete_us,pacer_arrival_us,"
    "arrival_queue_depth_before,arrival_queue_depth_after,queue_accepted,dequeue_us,queue_discontinuity,decision_valid,decision_us,"
    "display_refresh_hz,stream_rate_hz,display_period_us,can_latch_present,sender_interval_us,source_rate_hz,source_period_us,"
    "source_time_us,ready_offset_us,readiness_budget_us,timing_budget_us,render_lead_us,"
    "render_wake_lead_us,target_wake_lead_us,guard_us,headroom_us,render_start_us,render_wait_final_us,render_wait_overshoot_us,"
    "render_scheduler_delay_us,render_scheduler_delay_valid,render_deadline_already_elapsed,"
    "prepare_start_us,prepare_end_us,prepare_us,target_us,target_wait_final_us,target_wait_overshoot_us,target_scheduler_delay_us,target_scheduler_delay_valid,target_deadline_already_elapsed,"
    "present_start_us,submission_boundary_us,presenter_submission_time_used,present_end_us,present_call_us,submit_error_us,submission_spacing_us,"
    "spacing_margin_us,spacing_deficit_us,spacing_guard_feedback_us,spacing_corrected,had_prior_submission,tear_classification,tear_risk,completion_queue_depth,disposition,dropped,presented,cancelled,"
    "submission_id_valid,submission_id,latch_valid,latch_submission_id,latch_time_us,latch_present_refresh_seq,latch_sync_refresh_seq,latched_present,"
    "used_rtp_timestamp,cadence_eligible,source_rate_changed,phase_discontinuity,rebased,deep_trace,"
    "native_present_timing_valid,native_present_start_us,native_present_end_us,native_present_call_us,"
    "present_count_before_valid,present_count_before,frame_stats_before_valid,frame_stats_before_present_count,frame_stats_before_time_us,frame_stats_before_present_refresh_seq,frame_stats_before_sync_refresh_seq,"
    "gpu_ready_timing_valid,gpu_ready_wait_start_us,gpu_ready_time_us,gpu_ready_wait_us,"
    "decision_end_us,controller_call_us,stale_check_us,stale_age_us,render_wait_entry_us,target_wait_entry_us,spacing_check_us,presentation_floor_us,correction_wait_start_us,correction_wait_end_us,terminal_time_us,"
    "readiness_phase_us,readiness_demand_us,applied_readiness_reserve_us,cadence_sample_count,rate_candidate_sample_count,readiness_sample_count,preparation_sample_count,render_scheduler_sample_count,target_scheduler_sample_count,clean_spacing_frames,phase_error_frames,readiness_model_valid"
    VRR_TIMING_PARAMETER_FIELDS(VRR_TRACE_PARAMETER_HEADER)
    "\n";
#undef VRR_TRACE_PARAMETER_HEADER
constexpr uint32_t kVrrWindowStateMask =
    WINDOW_STATE_CHANGE_MINIMIZED |
    WINDOW_STATE_CHANGE_RESTORED |
    WINDOW_STATE_CHANGE_SUSPENDED;

int64_t signedDifference(uint64_t left, uint64_t right)
{
    if (left >= right) {
        const uint64_t difference = left - right;
        return difference > static_cast<uint64_t>(
                   std::numeric_limits<int64_t>::max()) ?
            std::numeric_limits<int64_t>::max() :
            static_cast<int64_t>(difference);
    }

    const uint64_t difference = right - left;
    if (difference > static_cast<uint64_t>(
                         std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::min();
    }
    return -static_cast<int64_t>(difference);
}

uint64_t positiveDifference(uint64_t actualUs, uint64_t targetUs)
{
    return actualUs > targetUs ? actualUs - targetUs : 0;
}

uint64_t submissionBoundaryUs(const VrrPresentFeedback& feedback,
                              uint64_t operationStartUs,
                              uint64_t operationEndUs,
                              bool& usedPresenterSubmissionTime)
{
    usedPresenterSubmissionTime = false;
    if (!feedback.presented) {
        return 0;
    }

    // A backend may wait for physical scanout inside presentAdaptive(). Use
    // its exact native-call timestamp only when it belongs to this operation;
    // stale or cross-epoch feedback must not poison every future spacing floor.
    if (feedback.submissionTimeValid &&
            operationStartUs <= operationEndUs &&
            feedback.submissionTimeUs >= operationStartUs &&
            feedback.submissionTimeUs <= operationEndUs) {
        usedPresenterSubmissionTime = true;
        return feedback.submissionTimeUs;
    }

    // Presenters without native timing are required to be thin. Anchoring at
    // entry prevents an unrelated blocking return from adding a display period
    // to every subsequent frame.
    return operationStartUs;
}

#ifdef _WIN32
bool isUncPath(const char* path)
{
    return path != nullptr &&
        ((path[0] == '\\' && path[1] == '\\') ||
         (path[0] == '/' && path[1] == '/'));
}
#endif

} // namespace

VrrPacingWorker::VrrPacingWorker(IVrrFramePresenter* presenter,
                                 const VrrSessionConfig& config,
                                 PacerTelemetry* telemetry) :
    m_Presenter(presenter),
    m_Telemetry(telemetry),
    m_Config(config),
    m_CanLatchPresentation(presenter != nullptr &&
                           presenter->canLatchAdaptivePresent()),
    m_TimingController(std::make_unique<VrrTimingController>(
        config, m_CanLatchPresentation))
{
    const char* deepTraceEnv = SDL_getenv("MOONLIGHT_VRR_DEEP_TRACE");
    m_DeepTraceEnabled = deepTraceEnv != nullptr && deepTraceEnv[0] == '1';

    VrrTargetWaiterHooks hooks;
    hooks.nowUs = []() {
        return LiGetMicroseconds();
    };
    hooks.yield = []() {
        std::this_thread::yield();
    };
    m_TargetWaiter = std::make_unique<VrrTargetWaiter>(std::move(hooks));
}

VrrPacingWorker::~VrrPacingWorker()
{
    {
        QMutexLocker lock(&m_FrameQueueLock);
        m_Stopping.store(true);
        m_FrameQueueNotEmpty.wakeAll();
    }

    if (m_WorkerThread != nullptr) {
        SDL_WaitThread(m_WorkerThread, nullptr);
        m_WorkerThread = nullptr;
    }

    discardQueuedFrames(false, TraceDisposition::ShutdownDiscard);
    closeTrace();
}

bool VrrPacingWorker::start()
{
    if (m_Presenter == nullptr ||
        m_Presenter->checkSupport() != VrrFallbackReason::NoFallback) {
        return false;
    }

    // Enable capture before the producer can submit its first frame. Opening
    // from run() left a small startup race that made session replay incomplete.
    openTraceIfRequested();

    m_WorkerThread = SDL_CreateThread(VrrPacingWorker::threadProc,
                                      "PacerVRR", this);
    if (m_WorkerThread == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create VRR pacing worker: %s", SDL_GetError());
        closeTrace();
        return false;
    }

    if (m_Telemetry != nullptr) {
        m_Telemetry->beginVrrSession();
    }

    return true;
}

void VrrPacingWorker::submit(PacedFrame&& frame)
{
    if (!frame) {
        return;
    }

    QueuedFrame incoming;
    incoming.frame = std::move(frame);
    if (m_TraceAcceptingRows.load()) {
        incoming.trace.arrivalUs = LiGetMicroseconds();
        incoming.trace.arrivalSequence =
            m_TraceArrivalSequence.fetch_add(1) + 1;
    }

    QueuedFrame droppedFrame;
    TraceDisposition droppedDisposition = TraceDisposition::ArrivalRejected;
    bool queuedFrame = false;
    {
        QMutexLocker lock(&m_FrameQueueLock);
        incoming.trace.queueDepthBefore = m_FrameQueue.size();
        // Close the race where suspension can begin after the optimistic
        // check above but before this producer acquires the queue lock.
        if (m_Stopping.load() || m_Suspended.load()) {
            incoming.trace.queueDepthAfter = m_FrameQueue.size();
            droppedFrame = std::move(incoming);
        }
        else {
            if (m_FrameQueue.size() >= kMaximumQueuedFrames) {
                droppedFrame = std::move(m_FrameQueue.front());
                m_FrameQueue.pop_front();
                droppedDisposition = TraceDisposition::QueueCapacity;
                // Without a rebase, a multi-frame RTP jump to the freshest
                // successor can be mistaken for time still left to wait.
                m_QueueDiscontinuity.store(true);
            }
            incoming.trace.queueAccepted = true;
            m_FrameQueue.emplace_back(std::move(incoming));
            m_FrameQueue.back().trace.queueDepthAfter = m_FrameQueue.size();
            queuedFrame = true;
        }
    }

    if (droppedFrame) {
        writeTrace(droppedFrame, VrrTimingDecision {},
                   VrrPresentFeedback {}, FrameTelemetry {},
                   droppedDisposition, false);
        noteDrop();
    }
    if (queuedFrame) {
        m_FrameQueueNotEmpty.wakeOne();
    }
}

void VrrPacingWorker::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    if (info == nullptr) {
        return;
    }

    const uint32_t flags = info->stateChangeFlags & kVrrWindowStateMask;
    if (flags == 0) {
        return;
    }

    if (flags & (WINDOW_STATE_CHANGE_MINIMIZED | WINDOW_STATE_CHANGE_SUSPENDED)) {
        m_Suspended.store(true);
        discardQueuedFrames(true, TraceDisposition::SuspensionDiscard);
    }
    if (flags & WINDOW_STATE_CHANGE_RESTORED) {
        m_Suspended.store(false);
    }

    m_PendingWindowStateFlags.fetch_or(flags);
    m_FrameQueueNotEmpty.wakeAll();
}

int VrrPacingWorker::threadProc(void* context)
{
    return static_cast<VrrPacingWorker*>(context)->run();
}

int VrrPacingWorker::run()
{
#if SDL_VERSION_ATLEAST(2, 0, 9)
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL) < 0) {
#else
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
#endif
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set VRR pacing worker priority: %s",
                    SDL_GetError());
    }

    while (!isStopping()) {
        consumeWindowStateNotifications();

        QueuedFrame queuedFrame;
        bool queueDiscontinuity = false;
        if (!dequeueFrame(queuedFrame, queueDiscontinuity)) {
            break;
        }
        queuedFrame.trace.dequeueUs = LiGetMicroseconds();
        queuedFrame.trace.queueDiscontinuity = queueDiscontinuity;
        PacedFrame& frame = queuedFrame.frame;

        consumeWindowStateNotifications();
        if (isStopping()) {
            if (frame) {
                writeTrace(queuedFrame, VrrTimingDecision {},
                           VrrPresentFeedback {}, FrameTelemetry {},
                           TraceDisposition::ShutdownDiscard, false);
                noteDrop();
            }
            continue;
        }
        if (presentationSuspended()) {
            if (frame) {
                writeTrace(queuedFrame, VrrTimingDecision {},
                           VrrPresentFeedback {}, FrameTelemetry {},
                           TraceDisposition::SuspensionDiscard, false);
                noteDrop();
            }
            // dequeueFrame() wakes without a frame so suspension can be
            // delivered to the backend. Once delivered, block here until a
            // restore or shutdown notification changes the atomic state.
            QMutexLocker lock(&m_FrameQueueLock);
            while (!isStopping() && m_Suspended.load()) {
                m_FrameQueueNotEmpty.wait(&m_FrameQueueLock);
            }
            continue;
        }
        if (!frame) {
            // dequeueFrame() also wakes the worker without a frame so window
            // state can be delivered while suspended.
            continue;
        }

        if (m_RebaseOnNextFrame) {
            m_TimingController->rebase();
            m_RebaseOnNextFrame = false;
        }
        // A local latest-frame queue replacement is not a source epoch
        // change. Frame-number and cumulative RTP movement let the timing
        // controller advance across the omitted frame without throwing away
        // its learned cadence.
        (void) queueDiscontinuity;

        const uint64_t decisionTimeUs = LiGetMicroseconds();
        VrrTimingDecision decision = m_TimingController->schedule(
            frame, decisionTimeUs);
        FrameTelemetry telemetry;
        telemetry.decisionTimeUs = decisionTimeUs;
        telemetry.decisionEndUs = LiGetMicroseconds();

        // schedule() deliberately clamps an overdue target to the current
        // one-slot deadline. That is right for the newest frame, but it makes
        // the later target-relative stale check unable to see time already
        // spent waiting in this worker's queue. If a fresher successor exists,
        // skip a frame that is already more than one source interval old before
        // rendering it; its RTP/frame delta remains in the controller, so the
        // successor preserves cadence without re-anchoring the whole model.
        const uint64_t scheduleNowUs = LiGetMicroseconds();
        telemetry.staleCheckUs = scheduleNowUs;
        const uint64_t scheduleAgeUs = scheduleNowUs >=
                frame.decodeCompleteUs() ?
            scheduleNowUs - frame.decodeCompleteUs() : 0;
        telemetry.staleAgeUs = scheduleAgeUs;
        if (decision.sourcePeriodUs != 0 &&
            scheduleAgeUs > decision.sourcePeriodUs && hasQueuedFrame()) {
            writeTrace(queuedFrame, decision, VrrPresentFeedback {}, telemetry,
                       TraceDisposition::Stale);
            noteDrop();
            m_TimingController->noteSubmission(false, false, 0);
            continue;
        }

        telemetry.renderWaitEntryUs = LiGetMicroseconds();
        const VrrTargetWaitResult renderWait =
            m_TargetWaiter->waitUntil(decision.renderStartUs);
        telemetry.renderWaitFinalUs = renderWait.finalNowUs;
        telemetry.renderSchedulerDelayUs = renderWait.schedulerDelayUs;
        telemetry.renderSchedulerDelayValid = renderWait.schedulerDelayValid;
        telemetry.renderDeadlineAlreadyElapsed =
            renderWait.deadlineAlreadyElapsed;
        // A deadline that was already in the past is not an OS wake delay.
        // This matters when a decoded frame arrives after its projected render
        // start: readiness/render telemetry owns that lateness instead.
        telemetry.renderWaitOvershootUs = renderWait.deadlineAlreadyElapsed ?
            0 : positiveDifference(renderWait.finalNowUs,
                                   decision.renderStartUs);
        consumeWindowStateNotifications();
        if (presentationSuspended() || isStopping()) {
            telemetry.presentStartUs = LiGetMicroseconds();
            VrrPresentFeedback feedback = m_Presenter->cancelFrame();
            telemetry.presentEndUs = LiGetMicroseconds();
            telemetry.presentDurationUs =
                telemetry.presentEndUs >= telemetry.presentStartUs ?
                    telemetry.presentEndUs - telemetry.presentStartUs : 0;
            feedback.cancelled = true;
            recordSubmission(decision, feedback, telemetry.presentStartUs,
                             telemetry.presentEndUs,
                             telemetry);
            if (m_Telemetry != nullptr) {
                m_Telemetry->recordVrrOutcome(feedback.presented,
                                              feedback.cancelled);
            }
            writeTrace(queuedFrame, decision, feedback, telemetry,
                       TraceDisposition::Interrupted);
            noteDrop();
            continue;
        }

        // A frame can become stale while the worker waits for its render
        // start. Leave the surface unprepared and let the next iteration start
        // fresh rather than rendering an avoidably old image.
        uint64_t nowUs = LiGetMicroseconds();
        const uint64_t staleHorizonAfterWaitUs = decision.sourcePeriodUs;
        if (staleHorizonAfterWaitUs != 0 &&
            nowUs > decision.targetUs + staleHorizonAfterWaitUs &&
            hasQueuedFrame()) {
            writeTrace(queuedFrame, decision, VrrPresentFeedback {}, telemetry,
                       TraceDisposition::Stale);
            noteDrop();
            m_TimingController->rebase();
            continue;
        }

        telemetry.preparationStartUs = LiGetMicroseconds();
        const VrrPrepareResult preparation =
            m_Presenter->prepareFrame(frame.frame());
        telemetry.preparationEndUs = LiGetMicroseconds();
        telemetry.preparationDurationUs =
            telemetry.preparationEndUs >= telemetry.preparationStartUs ?
                telemetry.preparationEndUs - telemetry.preparationStartUs : 0;
        m_TimingController->notePreparationDuration(
            telemetry.preparationDurationUs);

        if (!preparation.prepared || presentationSuspended() || isStopping()) {
            VrrPresentFeedback feedback = preparation.feedback;
            uint64_t submissionOperationStartUs =
                telemetry.preparationStartUs;
            uint64_t submissionOperationEndUs =
                telemetry.preparationEndUs;
            const bool mustCancel = !feedback.presented &&
                (preparation.prepared || preparation.cancellationMaySubmit ||
                 !feedback.cancelled);
            if (mustCancel) {
                // A presenter may need to submit an acquired image in order
                // to abandon it. It reports only that neutral fact; the worker
                // owns the target and display-spacing policy.
                if (preparation.cancellationMaySubmit) {
                    waitForSubmissionFloor(decision, telemetry);
                }
                telemetry.presentStartUs = LiGetMicroseconds();
                VrrPresentFeedback cancelFeedback = m_Presenter->cancelFrame();
                telemetry.presentEndUs = LiGetMicroseconds();
                telemetry.presentDurationUs =
                    telemetry.presentEndUs >= telemetry.presentStartUs ?
                        telemetry.presentEndUs - telemetry.presentStartUs : 0;
                if (cancelFeedback.presented) {
                    feedback = cancelFeedback;
                    submissionOperationStartUs = telemetry.presentStartUs;
                    submissionOperationEndUs = telemetry.presentEndUs;
                }
            }
            feedback.cancelled = true;
            recordSubmission(decision, feedback, submissionOperationStartUs,
                             submissionOperationEndUs,
                             telemetry);
            if (m_Telemetry != nullptr) {
                m_Telemetry->recordVrrOutcome(feedback.presented,
                                              feedback.cancelled);
            }
            writeTrace(queuedFrame, decision, feedback, telemetry,
                       preparation.prepared ? TraceDisposition::Interrupted :
                                              TraceDisposition::PreparationFailed);
            noteDrop();
            deferFrame(std::move(frame));
            continue;
        }

        telemetry.targetWaitEntryUs = LiGetMicroseconds();
        const VrrTargetWaitResult targetWait =
            m_TargetWaiter->waitUntil(decision.targetUs,
                                      decision.targetWakeLeadUs);
        telemetry.targetWaitFinalUs = targetWait.finalNowUs;
        telemetry.targetWaitOvershootUs = targetWait.deadlineAlreadyElapsed ?
            0 : positiveDifference(targetWait.finalNowUs,
                                   decision.targetUs);
        telemetry.targetSchedulerDelayUs = targetWait.schedulerDelayUs;
        telemetry.targetSchedulerDelayValid =
            targetWait.schedulerDelayValid;
        telemetry.targetDeadlineAlreadyElapsed =
            targetWait.deadlineAlreadyElapsed;
        consumeWindowStateNotifications();
        if (presentationSuspended() || isStopping()) {
            if (preparation.cancellationMaySubmit) {
                waitForSubmissionFloor(decision, telemetry);
            }
            telemetry.presentStartUs = LiGetMicroseconds();
            VrrPresentFeedback feedback = m_Presenter->cancelFrame();
            telemetry.presentEndUs = LiGetMicroseconds();
            telemetry.presentDurationUs =
                telemetry.presentEndUs >= telemetry.presentStartUs ?
                    telemetry.presentEndUs - telemetry.presentStartUs : 0;
            feedback.cancelled = true;
            recordSubmission(decision, feedback, telemetry.presentStartUs,
                             telemetry.presentEndUs,
                             telemetry);
            if (m_Telemetry != nullptr) {
                m_Telemetry->recordVrrOutcome(feedback.presented,
                                              feedback.cancelled);
            }
            writeTrace(queuedFrame, decision, feedback, telemetry,
                       TraceDisposition::Interrupted);
            noteDrop();
            deferFrame(std::move(frame));
            continue;
        }

        // Recheck both mathematical floors immediately before Present. The
        // waiter deliberately has a bounded active phase, so a pathological
        // clock must not turn an early return into an early submission.
        uint64_t beforePresentUs = LiGetMicroseconds();
        telemetry.spacingCheckUs = beforePresentUs;
        uint64_t earliestSubmissionUs =
            m_TimingController->earliestSubmissionUs();
        m_TimingController->noteSpacingDeficit(0);
        if (earliestSubmissionUs != 0 &&
                beforePresentUs < earliestSubmissionUs) {
            telemetry.spacingDeficitUs =
                earliestSubmissionUs - beforePresentUs;
            telemetry.spacingCorrected = true;
        }

        const uint64_t presentationFloorUs = std::max(decision.targetUs,
                                                       earliestSubmissionUs);
        telemetry.presentationFloorUs = presentationFloorUs;
        while (beforePresentUs < presentationFloorUs) {
            m_TargetWaiter->waitUntil(presentationFloorUs);
            beforePresentUs = LiGetMicroseconds();
        }

        const bool hadPriorSubmission =
            m_TimingController->hasLastSubmission();
        const uint64_t priorSubmissionUs =
            m_TimingController->lastSubmissionUs();
        telemetry.presentStartUs = LiGetMicroseconds();
        if (hadPriorSubmission) {
            telemetry.presentSpacingUs =
                telemetry.presentStartUs >= priorSubmissionUs ?
                    telemetry.presentStartUs - priorSubmissionUs : 0;
            const uint64_t minimumUntornUs = priorSubmissionUs +
                m_TimingController->displayPeriodUs();
            telemetry.spacingMarginUs = signedDifference(
                telemetry.presentStartUs, minimumUntornUs);

            // A second check protects against a clock anomaly between the
            // first check and the actual call boundary.
            if (telemetry.spacingMarginUs < 0) {
                const uint64_t deficitUs = static_cast<uint64_t>(
                    -(telemetry.spacingMarginUs + 1)) + 1;
                telemetry.spacingDeficitUs = std::max(
                    telemetry.spacingDeficitUs, deficitUs);
                telemetry.spacingGuardFeedbackUs = deficitUs;
                telemetry.spacingCorrected = true;
                m_TimingController->noteSpacingDeficit(deficitUs);
                const uint64_t correctedFloorUs =
                    m_TimingController->earliestSubmissionUs();
                telemetry.correctionWaitStartUs = LiGetMicroseconds();
                telemetry.presentStartUs = LiGetMicroseconds();
                while (telemetry.presentStartUs < correctedFloorUs) {
                    m_TargetWaiter->waitUntil(correctedFloorUs);
                    telemetry.presentStartUs = LiGetMicroseconds();
                }
                telemetry.correctionWaitEndUs = telemetry.presentStartUs;
                telemetry.presentSpacingUs =
                    telemetry.presentStartUs >= priorSubmissionUs ?
                        telemetry.presentStartUs - priorSubmissionUs : 0;
                telemetry.spacingMarginUs = signedDifference(
                    telemetry.presentStartUs, minimumUntornUs);
            }
        }

        m_TimingController->noteSchedulerDelays(
            telemetry.renderWaitOvershootUs,
            targetWait.schedulerDelayUs,
            targetWait.schedulerDelayValid);

        VrrPresentRequest presentRequest;
        presentRequest.latchedPresentation = decision.latchedPresentation;
        presentRequest.collectDiagnostics = m_DeepTraceEnabled;

        telemetry.presentStartUs = LiGetMicroseconds();
        VrrPresentFeedback feedback =
            m_Presenter->presentAdaptive(presentRequest);
        telemetry.presentEndUs = LiGetMicroseconds();
        telemetry.presentDurationUs =
            telemetry.presentEndUs >= telemetry.presentStartUs ?
                telemetry.presentEndUs - telemetry.presentStartUs : 0;
        recordSubmission(decision, feedback, telemetry.presentStartUs,
                         telemetry.presentEndUs,
                         telemetry);
        if (m_Telemetry != nullptr) {
            VrrTelemetrySample sample;
            sample.decisionTimeUs = decisionTimeUs;
            sample.pacerTimeUs =
                telemetry.preparationStartUs >= frame.decodeCompleteUs() ?
                    telemetry.preparationStartUs - frame.decodeCompleteUs() : 0;
            sample.renderTimeUs = telemetry.preparationDurationUs +
                telemetry.presentDurationUs;
            sample.prepareLate = telemetry.preparationEndUs > decision.targetUs;
            sample.preparationLatenessUs = sample.prepareLate ?
                telemetry.preparationEndUs - decision.targetUs : 0;
            sample.targetWaitEntryLate = !sample.prepareLate &&
                telemetry.preparationEndUs < decision.targetUs &&
                targetWait.deadlineAlreadyElapsed;
            sample.submitErrorUs = telemetry.submitErrorUs;
            sample.spacingCorrected = telemetry.spacingCorrected;
            sample.presented = feedback.presented;
            sample.cancelled = feedback.cancelled;
            sample.readinessBudgetUs = decision.readinessBudgetUs;
            sample.timingBudgetUs = decision.timingBudgetUs;
            sample.renderLeadUs = decision.renderLeadUs;
            sample.renderWakeLeadUs = decision.renderWakeLeadUs;
            sample.targetWakeLeadUs = decision.targetWakeLeadUs;
            sample.guardUs = decision.guardUs;
            sample.sourcePeriodUs = decision.sourcePeriodUs;
            m_Telemetry->recordVrrFrame(sample);
        }

        const bool outputDropped = !feedback.presented || feedback.cancelled;
        if (outputDropped) {
            noteDrop();
        }
        writeTrace(queuedFrame, decision, feedback, telemetry,
                   outputDropped ?
                       TraceDisposition::OutputDropped :
                       TraceDisposition::Presented);
        deferFrame(std::move(frame));
    }

    // Release any native state retained between preparation and presentation.
    m_Presenter->cancelFrame();
    return 0;
}

bool VrrPacingWorker::dequeueFrame(QueuedFrame& frame,
                                   bool& queueDiscontinuity)
{
    QMutexLocker lock(&m_FrameQueueLock);
    while (!isStopping() && !m_Suspended.load() && m_FrameQueue.empty()) {
        m_FrameQueueNotEmpty.wait(&m_FrameQueueLock);
    }

    if (isStopping()) {
        return false;
    }
    if (m_Suspended.load()) {
        return true;
    }

    queueDiscontinuity = m_QueueDiscontinuity.exchange(false);
    frame = std::move(m_FrameQueue.front());
    m_FrameQueue.pop_front();
    return true;
}

bool VrrPacingWorker::hasQueuedFrame()
{
    QMutexLocker lock(&m_FrameQueueLock);
    return !m_FrameQueue.empty();
}

void VrrPacingWorker::discardQueuedFrames(
    bool countDrops, TraceDisposition disposition)
{
    std::deque<QueuedFrame> discardedFrames;
    {
        QMutexLocker lock(&m_FrameQueueLock);
        discardedFrames.swap(m_FrameQueue);
        m_QueueDiscontinuity.store(false);
    }

    for (const QueuedFrame& frame : discardedFrames) {
        writeTrace(frame, VrrTimingDecision {}, VrrPresentFeedback {},
                   FrameTelemetry {}, disposition, false);
        if (countDrops) {
            noteDrop();
        }
    }
}

void VrrPacingWorker::consumeWindowStateNotifications()
{
    if (m_PendingWindowStateFlags.exchange(0) == 0) {
        return;
    }

    // Minimize/restore can race while this worker is draining notifications.
    // Reconcile to the authoritative atomic state rather than applying a stale
    // flag snapshot in event order.
    bool suspended = m_Suspended.load();
    while (true) {
        const uint32_t newerFlags = m_PendingWindowStateFlags.exchange(0);
        const bool latestSuspended = m_Suspended.load();
        if (newerFlags == 0 && latestSuspended == suspended) {
            break;
        }
        suspended = latestSuspended;
    }

    if (suspended != m_PresenterSuspended) {
        m_Presenter->setSuspended(suspended);
        m_PresenterSuspended = suspended;
    }
    m_RebaseOnNextFrame = true;
}

bool VrrPacingWorker::presentationSuspended() const
{
    // If UI state changed just after notification draining, either side being
    // suspended is enough to prevent a misclassified finish. The next loop
    // reconciles the presenter to the newest authoritative state.
    return m_Suspended.load() || m_PresenterSuspended;
}

bool VrrPacingWorker::isStopping() const
{
    return m_Stopping.load();
}

void VrrPacingWorker::waitForSubmissionFloor(
    const VrrTimingDecision& decision, FrameTelemetry& telemetry)
{
    const uint64_t earliestSubmissionUs =
        m_TimingController->earliestSubmissionUs();
    const uint64_t submissionFloorUs = std::max(decision.targetUs,
                                                 earliestSubmissionUs);
    uint64_t nowUs = LiGetMicroseconds();
    if (nowUs >= submissionFloorUs) {
        return;
    }

    if (earliestSubmissionUs != 0 && nowUs < earliestSubmissionUs) {
        telemetry.spacingDeficitUs = std::max(
            telemetry.spacingDeficitUs, earliestSubmissionUs - nowUs);
        telemetry.spacingCorrected = true;
    }

    const VrrTargetWaitResult wait = m_TargetWaiter->waitUntil(
        submissionFloorUs, decision.targetWakeLeadUs);
    telemetry.targetWaitFinalUs = std::max(telemetry.targetWaitFinalUs,
                                            wait.finalNowUs);
    telemetry.targetDeadlineAlreadyElapsed =
        telemetry.targetDeadlineAlreadyElapsed || wait.deadlineAlreadyElapsed;
    if (wait.schedulerDelayValid) {
        telemetry.targetSchedulerDelayUs = std::max(
            telemetry.targetSchedulerDelayUs, wait.schedulerDelayUs);
        telemetry.targetSchedulerDelayValid = true;
    }
    if (!wait.deadlineAlreadyElapsed) {
        telemetry.targetWaitOvershootUs = std::max(
            telemetry.targetWaitOvershootUs,
            positiveDifference(wait.finalNowUs, submissionFloorUs));
    }

    // The waiter deliberately bounds each active phase. Re-enter it until the
    // shared monotonic clock confirms the floor; an incomplete wait must never
    // become permission to submit.
    nowUs = LiGetMicroseconds();
    while (nowUs < submissionFloorUs) {
        m_TargetWaiter->waitUntil(submissionFloorUs);
        nowUs = LiGetMicroseconds();
    }
}

void VrrPacingWorker::recordSubmission(
    const VrrTimingDecision& decision,
    const VrrPresentFeedback& feedback,
    uint64_t operationStartUs,
    uint64_t operationEndUs,
    FrameTelemetry& telemetry)
{
    telemetry.submissionBoundaryUs = submissionBoundaryUs(
        feedback, operationStartUs, operationEndUs,
        telemetry.usedPresenterSubmissionTime);
    telemetry.hadPriorSubmission =
        m_TimingController->hasLastSubmission();

    if (feedback.presented) {
        telemetry.submitErrorUs = signedDifference(
            telemetry.submissionBoundaryUs, decision.targetUs);

        if (telemetry.hadPriorSubmission) {
            const uint64_t priorSubmissionUs =
                m_TimingController->lastSubmissionUs();
            telemetry.presentSpacingUs =
                telemetry.submissionBoundaryUs >= priorSubmissionUs ?
                    telemetry.submissionBoundaryUs - priorSubmissionUs : 0;
            const uint64_t minimumUntornUs = priorSubmissionUs +
                m_TimingController->displayPeriodUs();
            telemetry.spacingMarginUs = signedDifference(
                telemetry.submissionBoundaryUs, minimumUntornUs);
        }
    }

    m_TimingController->noteSubmission(
        feedback.presented, feedback.cancelled,
        telemetry.submissionBoundaryUs);
}

void VrrPacingWorker::deferFrame(PacedFrame&& frame)
{
    // Keep the last frame alive until a subsequent result so a decoder-owned
    // surface cannot be recycled while GPU work from this present still reads
    // it. The move assignment frees the older deferred frame outside queues.
    m_DeferredFrame = std::move(frame);
}

void VrrPacingWorker::noteDrop()
{
    if (m_Telemetry != nullptr) {
        m_Telemetry->recordVrrDrop();
    }
}

void VrrPacingWorker::writeTrace(const QueuedFrame& queuedFrame,
                                 const VrrTimingDecision& decision,
                                 const VrrPresentFeedback& feedback,
                                 const FrameTelemetry& telemetry,
                                 TraceDisposition disposition,
                                 bool decisionValid)
{
    if (queuedFrame.trace.arrivalSequence == 0 ||
            !m_TraceAcceptingRows.load()) {
        return;
    }

    size_t queueDepth;
    {
        QMutexLocker lock(&m_FrameQueueLock);
        queueDepth = m_FrameQueue.size();
    }

    const PacedFrame& frame = queuedFrame.frame;
    TraceRow row;
    row.frameNumber = frame.frameNumber();
    row.rtpTimestamp = frame.rtpTimestamp();
    row.timestampValid = frame.timestampValid();
    row.decodeCompleteUs = frame.decodeCompleteUs();
    row.input = queuedFrame.trace;
    row.decision = decision;
    row.diagnostics = m_TimingController->diagnostics();
    row.feedback = feedback;
    row.telemetry = telemetry;
    row.completionQueueDepth = queueDepth;
    row.disposition = disposition;
    row.decisionValid = decisionValid;
    row.terminalTimeUs = LiGetMicroseconds();

    {
        QMutexLocker lock(&m_TraceLock);
        if (m_TraceQueue.size() >= kMaximumTraceQueueRows) {
            ++m_TraceDroppedRows;
            return;
        }
        m_TraceQueue.emplace_back(std::move(row));
    }
    m_TraceQueueNotEmpty.wakeOne();
}

int VrrPacingWorker::traceThreadProc(void* context)
{
    return static_cast<VrrPacingWorker*>(context)->traceRun();
}

int VrrPacingWorker::traceRun()
{
    std::deque<TraceRow> batch;
    while (true) {
        {
            QMutexLocker lock(&m_TraceLock);
            while (m_TraceQueue.empty() && !m_TraceStopping.load()) {
                m_TraceQueueNotEmpty.wait(&m_TraceLock);
            }
            batch.swap(m_TraceQueue);
        }

        if (batch.empty() && m_TraceStopping.load()) {
            if (m_TraceFormat == TraceFormat::ChunkedCompressed) {
                flushTraceChunk();
            }
            return 0;
        }
        for (const TraceRow& row : batch) {
            if (!m_TraceAcceptingRows.load()) {
                break;
            }
            writeTraceRow(row);
        }
        batch.clear();
    }
}

void VrrPacingWorker::writeTraceRow(const TraceRow& row)
{
    m_TraceLatestArrivalUs = std::max(m_TraceLatestArrivalUs,
                                      row.input.arrivalUs);
    const VrrTimingDecision& decision = row.decision;
    const VrrTimingDiagnostics& diagnostics = row.diagnostics;
    const VrrTimingParameters& parameters = m_TimingController->parameters();
    const VrrPresentFeedback& feedback = row.feedback;
    const FrameTelemetry& telemetry = row.telemetry;
    const uint64_t nativePresentDurationUs =
        feedback.nativePresentTimingValid &&
        feedback.nativePresentEndUs >= feedback.nativePresentStartUs ?
            feedback.nativePresentEndUs - feedback.nativePresentStartUs : 0;
    const uint64_t gpuReadyWaitUs = feedback.gpuReadyTimingValid &&
        feedback.gpuReadyTimeUs >= feedback.gpuReadyWaitStartUs ?
            feedback.gpuReadyTimeUs - feedback.gpuReadyWaitStartUs : 0;
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
    addUnsigned(telemetry.preparationStartUs);
    addUnsigned(telemetry.preparationEndUs);
    addUnsigned(telemetry.preparationDurationUs);
    addUnsigned(decision.targetUs);
    addUnsigned(telemetry.targetWaitFinalUs);
    addUnsigned(telemetry.targetWaitOvershootUs);
    addUnsigned(telemetry.targetSchedulerDelayUs);
    addBool(telemetry.targetSchedulerDelayValid);
    addBool(telemetry.targetDeadlineAlreadyElapsed);
    addUnsigned(telemetry.presentStartUs);
    addUnsigned(telemetry.submissionBoundaryUs);
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
    addUnsigned(feedback.latchPresentRefreshSequence);
    addUnsigned(feedback.latchRefreshSequence);
    addBool(decision.latchedPresentation);
    addBool(decision.usedRtpTimestamp);
    addBool(decision.cadenceEligible);
    addBool(decision.sourceRateChanged);
    addBool(decision.phaseDiscontinuity);
    addBool(decision.rebased);
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
    addBool(feedback.gpuReadyTimingValid);
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
    addUnsigned(telemetry.correctionWaitStartUs);
    addUnsigned(telemetry.correctionWaitEndUs);
    addUnsigned(row.terminalTimeUs);
    addSigned(diagnostics.readinessPhaseUs);
    addUnsigned(diagnostics.readinessDemandUs);
    addUnsigned(diagnostics.appliedReadinessReserveUs);
    addUnsigned(diagnostics.cadenceSamples);
    addUnsigned(diagnostics.rateCandidateSamples);
    addUnsigned(diagnostics.readinessSamples);
    addUnsigned(diagnostics.preparationSamples);
    addUnsigned(diagnostics.renderSchedulerSamples);
    addUnsigned(diagnostics.targetSchedulerSamples);
    addUnsigned(diagnostics.cleanSpacingFrames);
    addUnsigned(diagnostics.phaseErrorFrames);
    addBool(diagnostics.readinessModelValid);
#define VRR_ADD_TRACE_PARAMETER(type, jsonName, memberName, defaultValue) \
    addUnsigned(static_cast<uint64_t>(parameters.memberName));
    VRR_TIMING_PARAMETER_FIELDS(VRR_ADD_TRACE_PARAMETER)
#undef VRR_ADD_TRACE_PARAMETER
    line.append('\n');

    if (m_TraceFormat == TraceFormat::ChunkedCompressed) {
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
        m_TraceAcceptingRows.store(false);
    }
    else if (m_TraceBytesWritten >= kMaximumTraceBytes &&
             minimumTraceDurationCaptured()) {
        m_TraceSizeCapped = true;
        m_TraceAcceptingRows.store(false);
    }
}

void VrrPacingWorker::flushTraceChunk()
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
        m_TraceAcceptingRows.store(false);
    }
    else {
        m_TraceBytesWritten += recordBytes;
        // A completed chunk is independently recoverable after a crash. This
        // is a low-frequency write performed only by the background thread.
        std::fflush(m_TraceFile);
        if (m_TraceBytesWritten >= kMaximumTraceBytes &&
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
    if (row.decision.latchedPresentation && m_CanLatchPresentation) {
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
    const char* tracePath = SDL_getenv("MOONLIGHT_VRR_TRACE");
    if (tracePath == nullptr || tracePath[0] == '\0') {
        return;
    }

#ifdef _WIN32
    // A buffered stdio stream still flushes synchronously when its buffer
    // fills. Keep diagnostic I/O off the time-critical worker's network path.
    if (isUncPath(tracePath)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "MOONLIGHT_VRR_TRACE must use a local path; refusing network trace: %s",
                    tracePath);
        return;
    }

    // Use the checked CRT variant on Windows so enabling diagnostics does not
    // introduce a deprecation warning in the normal application build.
    if (fopen_s(&m_TraceFile, tracePath, "wb") != 0) {
        m_TraceFile = nullptr;
    }
#else
    m_TraceFile = std::fopen(tracePath, "wb");
#endif
    if (m_TraceFile == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to open MOONLIGHT_VRR_TRACE file: %s", tracePath);
        return;
    }

    // A .csv suffix explicitly requests the directly readable compatibility
    // format. The recommended .vrrtrace format compresses independent chunks
    // and typically reduces a full session by an order of magnitude.
    m_TraceFormat = QByteArray(tracePath).toLower().endsWith(".csv") ?
        TraceFormat::Csv : TraceFormat::ChunkedCompressed;

    // Amortize local diagnostic writes instead of flushing on every frame.
    // fclose() commits the CSV tail; compressed chunks flush independently.
    std::setvbuf(m_TraceFile, nullptr, _IOFBF, 1024 * 1024);

    m_TraceBytesWritten = 0;
    m_TraceChunk.clear();
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

    // All formatting and I/O happen on this thread; the pacing worker only
    // enqueues row copies. Without it, a buffered flush would periodically
    // stall the TIME_CRITICAL thread and perturb the timing being measured.
    m_TraceStopping.store(false);
    m_TraceSizeCapped = false;
    m_TraceDroppedRows = 0;
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
    if (m_TraceThread != nullptr) {
        {
            QMutexLocker lock(&m_TraceLock);
            m_TraceStopping.store(true);
        }
        m_TraceQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_TraceThread, nullptr);
        m_TraceThread = nullptr;
    }
    m_TraceAcceptingRows.store(false);

    if (m_TraceDroppedRows != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR trace dropped %zu rows to protect pacing",
                    m_TraceDroppedRows);
        m_TraceDroppedRows = 0;
    }

    if (m_TraceSizeCapped) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR trace was capped at 512 MiB after preserving at least one hour");
        m_TraceSizeCapped = false;
    }

    if (m_TraceFile != nullptr) {
        std::fclose(m_TraceFile);
        m_TraceFile = nullptr;
    }
    m_TraceChunk.clear();
}
