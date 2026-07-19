#include "vrrpacingworker.h"

#include "vrr/vrrtargetwaiter.h"
#include "vrr/vrrtimingcontroller.h"

#include <Limelight.h>

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace {

constexpr size_t kMaximumQueuedFrames = 1;
// ~64 seconds of rows at 120 FPS. When the writer thread cannot keep up the
// pacing thread drops rows rather than ever waiting on diagnostics.
constexpr size_t kMaximumTraceQueueRows = 8192;
// A forgotten diagnostic session must not write indefinitely. This includes
// the normal and deep-trace columns and is enforced only by the writer thread.
constexpr uint64_t kMaximumTraceBytes = 128ULL * 1024ULL * 1024ULL;
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
                                 PVIDEO_STATS videoStats) :
    m_Presenter(presenter),
    m_VideoStats(videoStats),
    m_TimingController(std::make_unique<VrrTimingController>(config))
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

    discardQueuedFrames(false);
    closeTrace();
}

bool VrrPacingWorker::start()
{
    if (m_Presenter == nullptr ||
        m_Presenter->checkSupport() != VrrFallbackReason::None) {
        return false;
    }

    m_WorkerThread = SDL_CreateThread(VrrPacingWorker::threadProc,
                                      "PacerVRR", this);
    if (m_WorkerThread == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create VRR pacing worker: %s", SDL_GetError());
        return false;
    }

    return true;
}

void VrrPacingWorker::submit(PacedFrame&& frame)
{
    if (!frame) {
        return;
    }

    if (m_Stopping.load() || m_Suspended.load()) {
        noteDrop();
        return;
    }

    PacedFrame droppedFrame;
    bool queuedFrame = false;
    {
        QMutexLocker lock(&m_FrameQueueLock);
        // Close the race where suspension can begin after the optimistic
        // check above but before this producer acquires the queue lock.
        if (m_Stopping.load() || m_Suspended.load()) {
            droppedFrame = std::move(frame);
        }
        else {
            if (m_FrameQueue.size() >= kMaximumQueuedFrames) {
                droppedFrame = std::move(m_FrameQueue.front());
                m_FrameQueue.pop_front();
                // Without a rebase, a multi-frame RTP jump to the freshest
                // successor can be mistaken for time still left to wait.
                m_QueueDiscontinuity.store(true);
            }
            m_FrameQueue.emplace_back(std::move(frame));
            queuedFrame = true;
        }
    }

    if (droppedFrame) {
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
        discardQueuedFrames(true);
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

    openTraceIfRequested();

    while (!isStopping()) {
        consumeWindowStateNotifications();

        PacedFrame frame;
        bool queueDiscontinuity = false;
        if (!dequeueFrame(frame, queueDiscontinuity)) {
            break;
        }

        consumeWindowStateNotifications();
        if (isStopping()) {
            if (frame) {
                noteDrop();
            }
            continue;
        }
        if (presentationSuspended()) {
            if (frame) {
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

        uint64_t nowUs = LiGetMicroseconds();
        VrrTimingDecision decision = m_TimingController->schedule(frame, nowUs);
        FrameTelemetry telemetry;
        const VrrTargetWaitResult renderWait =
            m_TargetWaiter->waitUntil(decision.renderStartUs);
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
            writeTrace(frame, decision, feedback, telemetry,
                       queuedFrameCount(), true);
            noteDrop();
            continue;
        }

        // A frame can become stale while the worker waits for its render
        // start. Leave the surface unprepared and let the next iteration start
        // fresh rather than rendering an avoidably old image.
        nowUs = LiGetMicroseconds();
        const uint64_t staleHorizonAfterWaitUs = staleToleranceUs(decision);
        if (staleHorizonAfterWaitUs != 0 &&
            nowUs > decision.targetUs + staleHorizonAfterWaitUs &&
            hasQueuedFrame()) {
            writeTrace(frame, decision, VrrPresentFeedback {}, telemetry,
                       queuedFrameCount(), true);
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
            writeTrace(frame, decision, feedback, telemetry,
                       queuedFrameCount(), true);
            noteDrop();
            deferFrame(std::move(frame));
            continue;
        }

        const VrrTargetWaitResult targetWait =
            m_TargetWaiter->waitUntil(decision.targetUs,
                                      decision.targetWakeLeadUs);
        telemetry.targetWaitOvershootUs = targetWait.deadlineAlreadyElapsed ?
            0 : positiveDifference(targetWait.finalNowUs,
                                   decision.targetUs);
        telemetry.targetSchedulerDelayUs = targetWait.schedulerDelayUs;
        telemetry.targetSchedulerDelayValid =
            targetWait.schedulerDelayValid;
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
            writeTrace(frame, decision, feedback, telemetry,
                       queuedFrameCount(), true);
            noteDrop();
            deferFrame(std::move(frame));
            continue;
        }

        // Recheck both mathematical floors immediately before Present. The
        // waiter deliberately has a bounded active phase, so a pathological
        // clock must not turn an early return into an early submission.
        uint64_t beforePresentUs = LiGetMicroseconds();
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
                telemetry.spacingCorrected = true;
                m_TimingController->noteSpacingDeficit(deficitUs);
                const uint64_t correctedFloorUs =
                    m_TimingController->earliestSubmissionUs();
                telemetry.presentStartUs = LiGetMicroseconds();
                while (telemetry.presentStartUs < correctedFloorUs) {
                    m_TargetWaiter->waitUntil(correctedFloorUs);
                    telemetry.presentStartUs = LiGetMicroseconds();
                }
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
        if (m_VideoStats != nullptr) {
            m_VideoStats->totalPacerTimeUs +=
                telemetry.preparationStartUs >= frame.decodeCompleteUs() ?
                    telemetry.preparationStartUs - frame.decodeCompleteUs() : 0;
            m_VideoStats->totalRenderTimeUs +=
                telemetry.preparationDurationUs + telemetry.presentDurationUs;
            if (targetWait.deadlineAlreadyElapsed ||
                telemetry.preparationEndUs > decision.targetUs) {
                ++m_VideoStats->vrrReadinessMisses;
            }
            if (telemetry.spacingCorrected) {
                ++m_VideoStats->vrrSpacingCorrections;
            }
            m_VideoStats->vrrTimingBudgetUs =
                m_TimingController->timingBudgetUs();
            m_VideoStats->vrrSchedulerDelayUs =
                m_TimingController->wakeLeadUs();
            m_VideoStats->vrrGuardUs = m_TimingController->guardUs();
            if (feedback.presented && !feedback.cancelled) {
                ++m_VideoStats->renderedFrames;
            }
        }

        const bool outputDropped = !feedback.presented || feedback.cancelled;
        if (outputDropped) {
            noteDrop();
        }
        writeTrace(frame, decision, feedback, telemetry,
                   queuedFrameCount(), outputDropped);
        deferFrame(std::move(frame));
    }

    // Release any native state retained between preparation and presentation.
    m_Presenter->cancelFrame();
    return 0;
}

bool VrrPacingWorker::dequeueFrame(PacedFrame& frame,
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

size_t VrrPacingWorker::queuedFrameCount()
{
    QMutexLocker lock(&m_FrameQueueLock);
    return m_FrameQueue.size();
}

void VrrPacingWorker::discardQueuedFrames(bool countDrops)
{
    std::deque<PacedFrame> discardedFrames;
    {
        QMutexLocker lock(&m_FrameQueueLock);
        discardedFrames.swap(m_FrameQueue);
        m_QueueDiscontinuity.store(false);
    }

    if (countDrops) {
        for (size_t i = 0; i < discardedFrames.size(); ++i) {
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

uint64_t VrrPacingWorker::staleToleranceUs(
    const VrrTimingDecision& decision) const
{
    return decision.sourcePeriodUs;
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
    telemetry.submissionIdValid = feedback.submissionIdValid;
    telemetry.submissionId = feedback.submissionId;
    telemetry.latchSampleValid = feedback.latchSampleValid;
    telemetry.latchSubmissionId = feedback.latchSubmissionId;
    telemetry.latchTimeUs = feedback.latchTimeUs;
    telemetry.latchRefreshSequence = feedback.latchRefreshSequence;

    if (feedback.presented) {
        telemetry.submitErrorUs = signedDifference(
            telemetry.submissionBoundaryUs, decision.targetUs);

        if (m_TimingController->hasLastSubmission()) {
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
    if (m_VideoStats != nullptr) {
        ++m_VideoStats->pacerDroppedFrames;
        ++m_VideoStats->vrrPacingDroppedFrames;
    }
}

void VrrPacingWorker::writeTrace(const PacedFrame& frame,
                                 const VrrTimingDecision& decision,
                                 const VrrPresentFeedback& feedback,
                                 const FrameTelemetry& telemetry,
                                 size_t queueDepth,
                                 bool dropped)
{
    if (!m_TraceAcceptingRows.load()) {
        return;
    }

    TraceRow row;
    row.frameNumber = frame.frameNumber();
    row.decodeCompleteUs = frame.decodeCompleteUs();
    row.decision = decision;
    row.feedback = feedback;
    row.telemetry = telemetry;
    row.queueDepth = queueDepth;
    row.dropped = dropped;

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
    const VrrTimingDecision& decision = row.decision;
    const VrrPresentFeedback& feedback = row.feedback;
    const FrameTelemetry& telemetry = row.telemetry;
    const size_t queueDepth = row.queueDepth;
    const bool dropped = row.dropped;

    const double sourceRateHz = decision.sourcePeriodUs == 0 ? 0.0 :
        1000000.0 / static_cast<double>(decision.sourcePeriodUs);
    const int baseBytes = std::fprintf(m_TraceFile,
                 "%d,%llu,%llu,%.3f,%llu,%llu,%lld,%lld,%llu,%llu,%llu,"
                 "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                 "%d,%llu,%llu,%d,%llu,%llu,%lld,%llu,%lld,%llu,%d,%zu,%d,%d,%d,"
                 "%d,%llu,%d,%llu,%llu,%llu,%d,%d",
                 row.frameNumber,
                 static_cast<unsigned long long>(decision.sourceIntervalUs),
                 static_cast<unsigned long long>(row.decodeCompleteUs),
                 sourceRateHz,
                 static_cast<unsigned long long>(decision.sourcePeriodUs),
                 static_cast<unsigned long long>(decision.sourceTimeUs),
                 static_cast<long long>(decision.readyOffsetUs),
                 static_cast<long long>(decision.readinessBudgetUs),
                 static_cast<unsigned long long>(decision.timingBudgetUs),
                 static_cast<unsigned long long>(decision.renderLeadUs),
                 static_cast<unsigned long long>(decision.renderWakeLeadUs),
                 static_cast<unsigned long long>(decision.targetWakeLeadUs),
                 static_cast<unsigned long long>(decision.guardUs),
                 static_cast<unsigned long long>(decision.headroomUs),
                 static_cast<unsigned long long>(decision.renderStartUs),
                 static_cast<unsigned long long>(telemetry.renderWaitOvershootUs),
                 static_cast<unsigned long long>(telemetry.preparationStartUs),
                 static_cast<unsigned long long>(telemetry.preparationEndUs),
                 static_cast<unsigned long long>(telemetry.preparationDurationUs),
                 static_cast<unsigned long long>(decision.targetUs),
                 static_cast<unsigned long long>(telemetry.targetWaitOvershootUs),
                 static_cast<unsigned long long>(telemetry.targetSchedulerDelayUs),
                 telemetry.targetSchedulerDelayValid ? 1 : 0,
                 static_cast<unsigned long long>(telemetry.presentStartUs),
                 static_cast<unsigned long long>(telemetry.submissionBoundaryUs),
                 telemetry.usedPresenterSubmissionTime ? 1 : 0,
                 static_cast<unsigned long long>(telemetry.presentEndUs),
                 static_cast<unsigned long long>(telemetry.presentDurationUs),
                 static_cast<long long>(telemetry.submitErrorUs),
                 static_cast<unsigned long long>(telemetry.presentSpacingUs),
                 static_cast<long long>(telemetry.spacingMarginUs),
                 static_cast<unsigned long long>(telemetry.spacingDeficitUs),
                 telemetry.spacingCorrected ? 1 : 0,
                 queueDepth,
                 dropped ? 1 : 0,
                 feedback.presented ? 1 : 0,
                 feedback.cancelled ? 1 : 0,
                 telemetry.submissionIdValid ? 1 : 0,
                 static_cast<unsigned long long>(telemetry.submissionId),
                 telemetry.latchSampleValid ? 1 : 0,
                 static_cast<unsigned long long>(telemetry.latchSubmissionId),
                 static_cast<unsigned long long>(telemetry.latchTimeUs),
                 static_cast<unsigned long long>(telemetry.latchRefreshSequence),
                 decision.latchedPresentation ? 1 : 0,
                 decision.phaseDiscontinuity ? 1 : 0);

    const uint64_t nativePresentDurationUs =
        feedback.nativePresentTimingValid &&
        feedback.nativePresentEndUs >= feedback.nativePresentStartUs ?
            feedback.nativePresentEndUs - feedback.nativePresentStartUs : 0;
    const int diagnosticBytes = std::fprintf(
        m_TraceFile,
        ",%d,%d,%llu,%llu,%llu,%d,%llu,%d,%llu,%llu,%llu,"
        "%d,%llu,%llu,%llu\n",
        m_DeepTraceEnabled ? 1 : 0,
        feedback.nativePresentTimingValid ? 1 : 0,
        static_cast<unsigned long long>(feedback.nativePresentStartUs),
        static_cast<unsigned long long>(feedback.nativePresentEndUs),
        static_cast<unsigned long long>(nativePresentDurationUs),
        feedback.presentCountBeforeValid ? 1 : 0,
        static_cast<unsigned long long>(feedback.presentCountBefore),
        feedback.frameStatsBeforeValid ? 1 : 0,
        static_cast<unsigned long long>(feedback.frameStatsBeforePresentCount),
        static_cast<unsigned long long>(feedback.frameStatsBeforeTimeUs),
        static_cast<unsigned long long>(feedback.frameStatsBeforeRefreshSequence),
        feedback.gpuReadyTimingValid ? 1 : 0,
        static_cast<unsigned long long>(feedback.gpuReadyWaitStartUs),
        static_cast<unsigned long long>(feedback.gpuReadyTimeUs),
        static_cast<unsigned long long>(
            feedback.gpuReadyTimingValid &&
            feedback.gpuReadyTimeUs >= feedback.gpuReadyWaitStartUs ?
                feedback.gpuReadyTimeUs - feedback.gpuReadyWaitStartUs : 0));

    if (baseBytes > 0) {
        m_TraceBytesWritten += static_cast<uint64_t>(baseBytes);
    }
    if (diagnosticBytes > 0) {
        m_TraceBytesWritten += static_cast<uint64_t>(diagnosticBytes);
    }
    if (m_TraceBytesWritten >= kMaximumTraceBytes) {
        m_TraceSizeCapped = true;
        m_TraceAcceptingRows.store(false);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR trace reached the 128 MiB safety cap; stopping diagnostics");
    }
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
    if (fopen_s(&m_TraceFile, tracePath, "w") != 0) {
        m_TraceFile = nullptr;
    }
#else
    m_TraceFile = std::fopen(tracePath, "w");
#endif
    if (m_TraceFile == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to open MOONLIGHT_VRR_TRACE file: %s", tracePath);
        return;
    }

    // Amortize local diagnostic writes instead of flushing on every frame.
    // fclose() commits the buffered tail.
    std::setvbuf(m_TraceFile, nullptr, _IOFBF, 1024 * 1024);

    // All formatting and I/O happen on this thread; the pacing worker only
    // enqueues row copies. Without it, a buffered flush would periodically
    // stall the TIME_CRITICAL thread and perturb the timing being measured.
    m_TraceStopping.store(false);
    m_TraceBytesWritten = 0;
    m_TraceSizeCapped = false;
    m_TraceThread = SDL_CreateThread(VrrPacingWorker::traceThreadProc,
                                     "VrrTrace", this);
    if (m_TraceThread == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Disabling VRR trace: writer thread failed: %s",
                    SDL_GetError());
        std::fclose(m_TraceFile);
        m_TraceFile = nullptr;
        return;
    }

    const int headerBytes = std::fprintf(m_TraceFile,
                  "frame,sender_interval_us,decode_complete_us,source_rate_hz,source_period_us,"
                  "source_time_us,ready_offset_us,readiness_budget_us,timing_budget_us,render_lead_us,"
                  "render_wake_lead_us,target_wake_lead_us,guard_us,headroom_us,render_start_us,render_wait_overshoot_us,"
                  "prepare_start_us,prepare_end_us,prepare_us,target_us,target_wait_overshoot_us,target_scheduler_delay_us,target_scheduler_delay_valid,"
                  "present_start_us,submission_boundary_us,presenter_submission_time_used,present_end_us,present_call_us,submit_error_us,submission_spacing_us,"
                  "spacing_margin_us,spacing_deficit_us,spacing_corrected,queue_depth,dropped,presented,cancelled,"
                  "submission_id_valid,submission_id,latch_valid,latch_submission_id,latch_time_us,latch_refresh_seq,latched_present,phase_discontinuity,"
                  "deep_trace,"
                  "native_present_timing_valid,native_present_start_us,native_present_end_us,native_present_call_us,"
                  "present_count_before_valid,present_count_before,frame_stats_before_valid,frame_stats_before_present_count,frame_stats_before_time_us,frame_stats_before_refresh_seq,"
                  "gpu_ready_timing_valid,gpu_ready_wait_start_us,gpu_ready_time_us,gpu_ready_wait_us\n");
    if (headerBytes > 0) {
        m_TraceBytesWritten = static_cast<uint64_t>(headerBytes);
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
                    "VRR trace was capped at 128 MiB to limit diagnostic disk writes");
        m_TraceSizeCapped = false;
    }

    if (m_TraceFile != nullptr) {
        std::fclose(m_TraceFile);
        m_TraceFile = nullptr;
    }
}
