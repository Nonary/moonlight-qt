#include "vrr/profile.h"
#include "vrr/profilecodec.h"
#include "vrrpacingworker.h"
#include "../../videothreadpriority.h"
#include "../../timinggraph.h"

#include "vrr/vrrtargetwaiter.h"
#include "vrr/vrrtimingcontroller.h"
#include "vrr/vrrframedroppolicy.h"

#include <Limelight.h>
#include <QFileInfo>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace {

VrrSessionConfig prepareProductionConfig(VrrSessionConfig config)
{
    config = vrrProductionSessionConfig(config);
    for (const char* name : {"MOONLIGHT_VRR_EXPERIMENTAL_ADAPTIVE_BUFFER",
                             "MOONLIGHT_VRR_BOUNDED_CATCHUP",
                             "MOONLIGHT_VRR_INPUT_ACCOUNTING"}) {
        if (qEnvironmentVariableIsSet(name))
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Retired VRR switch %s is ignored", name);
    }
    const auto target = config.experimentalInputPercentilePpm;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Adaptive buffer: client-timing goal %.2f%% (network and decode jitter only); least buffer meeting it over the last %llu s, aiming at 70%% of the allowed loss; beyond reach maximize timing quality within the buffer cap; rise 4 ms/s, fall 1 ms/s; history %llu s",
        target / 10000.0,
        static_cast<unsigned long long>(target >= 999000 ? 120 : target >= 995000 ? 60 : 20),
        static_cast<unsigned long long>(config.timingOptions.resolved(config.latencyMode).historySeconds));
    return config;
}

// Slack between the published PyroWave reassembly deadline and the latest
// on-time reassembly, covering receive-thread wake-up and depacketizing.
constexpr uint64_t kReceiveDeadlineMarginUs = 250;
constexpr uint32_t kVrrWindowStateMask =
    WINDOW_STATE_CHANGE_SIZE |
    WINDOW_STATE_CHANGE_DISPLAY |
    WINDOW_STATE_CHANGE_MINIMIZED |
    WINDOW_STATE_CHANGE_RESTORED |
    WINDOW_STATE_CHANGE_SUSPENDED;
constexpr uint32_t kVrrDisplayEpochStateMask =
    WINDOW_STATE_CHANGE_SIZE |
    WINDOW_STATE_CHANGE_DISPLAY;

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


} // namespace

VrrPacingWorker::VrrPacingWorker(IVrrFramePresenter* presenter,
                                 const VrrSessionConfig& config,
                                 PacerTelemetry* telemetry) :
    m_Presenter(presenter),
    m_Telemetry(telemetry),
    m_Config(prepareProductionConfig(config)),
    m_CanLatchPresentation(presenter != nullptr &&
                           presenter->canLatchAdaptivePresent()),
    m_NativeSynchronizedPresentation(m_CanLatchPresentation &&
        presenter->alwaysSynchronizesAdaptivePresent()),
    m_NativeScheduledPresentation(m_NativeSynchronizedPresentation &&
        presenter->supportsScheduledPresentation()),
    m_TimingController(std::make_unique<VrrTimingController>(
        m_Config, m_CanLatchPresentation,
        vrrTimingParametersForSession(m_Config, m_NativeSynchronizedPresentation,
                                     m_NativeScheduledPresentation)))
{
    if (!m_Config.calibrationKey.empty()) {
        m_Config.calibrationKey = Vrr13::calibrationKey(
            QString::fromStdString(m_Config.calibrationKey),
            QByteArray::fromStdString(vrrCalibrationPolicy(m_TimingController->parameters())),
            m_CanLatchPresentation, m_Config.allowTearing).toStdString();
    }
    // Settings enables tracing after SDL initialization. SDL2-compat may cache
    // its environment, so read the current process value just like the path.
    m_DeepTraceEnabled = qEnvironmentVariable("MOONLIGHT_VRR_DEEP_TRACE").startsWith(QLatin1Char('1'));
    m_QueueCapacity = m_TimingController->queuedFrameCapacity();

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

    if (m_Presenter) m_Presenter->interruptFrameWait(true);

    if (m_WorkerThread != nullptr) {
        SDL_WaitThread(m_WorkerThread, nullptr);
        m_WorkerThread = nullptr;
    }

    if (m_Presenter) {
        if (m_NativeScheduledPresentation) m_Presenter->clearScheduledPresentation();
        m_Presenter->stopFramePreparation();
    }

    VrrReceiveDeadline::clear();
    VrrReceiveDeadline::clearPresentWindow();
    discardQueuedFrames(false, TraceDisposition::ShutdownDiscard);
    closeTrace();
    if (m_WorkerStarted && !m_Config.calibrationKey.empty() && !m_CalibrationInvalidated.load()) {
        // Current DXGI observations do not always carry the presentation instant.
        // Until native coverage can qualify a run, cache history only, never
        // promote inferred success into a proven low-latency startup.
        Vrr13::saveProfile(QString::fromStdString(m_Config.calibrationPath),
                          QString::fromStdString(m_Config.calibrationKey),
                          m_TimingController->playoutHistory());
        // A median over the settled part of the session, not the final value,
        // which may still carry a transient.
        if (m_SettledDelaySamples.size() >= kMinimumSettledDelaySamples) {
            auto samples = m_SettledDelaySamples;
            std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
            Vrr13::saveStartDelay(startDelayPath(),
                                  QString::fromStdString(m_Config.calibrationKey),
                                  samples[samples.size() / 2]);
        }
    }
}

bool VrrPacingWorker::start()
{
    if (m_Presenter == nullptr ||
        m_Presenter->checkSupport() != VrrFallbackReason::NoFallback) {
        return false;
    }

    if (!m_Config.calibrationKey.empty()) {
        Vrr13::Reserve prior(m_TimingController->playoutHistory().version());
        if (Vrr13::loadProfile(QString::fromStdString(m_Config.calibrationPath),
                              QString::fromStdString(m_Config.calibrationKey), prior)) {
            m_CalibrationLoaded = m_TimingController->loadPlayoutHistory(prior.profile());
        }
        m_StartDelaySeedUs = Vrr13::loadStartDelay(startDelayPath(),
            QString::fromStdString(m_Config.calibrationKey));
        m_TimingController->seedPlayoutDelayStart(m_StartDelaySeedUs);
    }
    m_InitialPlayoutProfile = encodeVrrPlayoutProfile(m_TimingController->playoutHistory());
    m_TraceParameters = m_TimingController->parameters();
    m_InitialCachedSamples = m_TimingController->playoutHistory().cachedEvidence();
    m_HistoryVersion = m_TimingController->playoutHistory().version();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "VRR capture policy: latency_mode=%d history_version=%d calibration_loaded=%d cached_samples=%llu start_delay_seed_us=%llu",
                m_Config.latencyMode,
                m_HistoryVersion, int(m_CalibrationLoaded),
                static_cast<unsigned long long>(m_InitialCachedSamples),
                static_cast<unsigned long long>(m_StartDelaySeedUs));

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

    m_WorkerStarted = true;
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

    // Capture an asynchronous decoder boundary before a newer frame can add
    // work behind it. A later render must wait for this frame, not everything
    // the decoder happened to queue before the pacing worker ran.
    frame.setDecodeBoundary(m_Presenter->captureDecodeBoundary());

    QueuedFrame incoming;
    incoming.frame = std::move(frame);
    // Admission age is pacing state even when no diagnostic capture is open.
    incoming.trace.arrivalUs = LiGetMicroseconds();
    if (m_TraceAcceptingRows.load()) {
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
            if (m_FrameQueue.size() >= m_QueueCapacity) {
                droppedFrame = std::move(m_FrameQueue.front());
                m_FrameQueue.pop_front();
                droppedDisposition = TraceDisposition::QueueCapacity;
                // Without a rebase, a multi-frame RTP jump to the freshest
                // successor can be mistaken for time still left to wait.
                m_QueueDiscontinuity.store(true);
            }
            incoming.trace.queueAccepted = true;
            incoming.preparation = m_Presenter->queueFramePreparation(
                incoming.frame.frame(), incoming.frame.decodeBoundary());
            m_FrameQueue.emplace_back(std::move(incoming));
            m_FrameQueue.back().trace.queueDepthAfter = m_FrameQueue.size();
            queuedFrame = true;
        }
        m_FrameQueueDepth.store(m_FrameQueue.size(), std::memory_order_relaxed);
    }

    if (droppedFrame) {
        recordFrameCompletion(droppedFrame, VrrTimingDecision {},
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
    if (flags & kVrrDisplayEpochStateMask) m_CalibrationInvalidated.store(true);
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
    if (m_Presenter) m_Presenter->interruptFrameWait(false);
    m_FrameQueueNotEmpty.wakeAll();
}

int VrrPacingWorker::threadProc(void* context)
{
    return static_cast<VrrPacingWorker*>(context)->run();
}

int VrrPacingWorker::run()
{
    const VideoThreadPriority priority("VRRPacer", VideoThreadPriority::Role::Deadline);

    QueuedFrame reservedFrame;
    bool reservedDiscontinuity = false;
    while (!isStopping()) {
        consumeWindowStateNotifications();

        QueuedFrame queuedFrame;
        bool queueDiscontinuity = false;
        if (reservedFrame) {
            queuedFrame = std::move(reservedFrame);
            queueDiscontinuity = reservedDiscontinuity;
        }
        else if (!dequeueFrame(queuedFrame, queueDiscontinuity)) {
            break;
        }
        if (queuedFrame.trace.dequeueUs == 0)
            queuedFrame.trace.dequeueUs = LiGetMicroseconds();
        queuedFrame.trace.queueDiscontinuity = queueDiscontinuity;
        PacedFrame& frame = queuedFrame.frame;

        consumeWindowStateNotifications();
        if (isStopping()) {
            if (frame) {
                recordFrameCompletion(queuedFrame, VrrTimingDecision {},
                           VrrPresentFeedback {}, FrameTelemetry {},
                           TraceDisposition::ShutdownDiscard, false);
                noteDrop();
            }
            continue;
        }
        if (presentationSuspended()) {
            if (frame) {
                recordFrameCompletion(queuedFrame, VrrTimingDecision {},
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

        bool externalRebaseApplied = false;
        uint32_t externalRebaseFlags = 0;
        // A local latest-frame queue replacement is not a source epoch
        // change. Frame-number and cumulative RTP movement let the timing
        // controller advance across the omitted frame without throwing away
        // its learned cadence.
        (void) queueDiscontinuity;

        // Establish this frame's exact decoder dependency before preparing
        // it. Some backends wait here; others queue a GPU dependency and
        // return immediately. Preserve only CPU time actually spent waiting
        // as service telemetry.
        const bool preparedAhead = queuedFrame.preparation &&
            queuedFrame.preparation->wait([this]() {
                return isStopping() || m_Suspended.load() ||
                    m_PendingWindowStateFlags.load() != 0;
            });
        if (isStopping() || m_Suspended.load() ||
                m_PendingWindowStateFlags.load() != 0) {
            recordFrameCompletion(queuedFrame, VrrTimingDecision{},
                VrrPresentFeedback{}, FrameTelemetry{},
                isStopping() ? TraceDisposition::ShutdownDiscard :
                               TraceDisposition::SuspensionDiscard, false);
            noteDrop();
            continue;
        }
        auto decodeReadiness = preparedAhead ? VrrDecodeReadiness{} :
            queuedFrame.reservedReadinessValid ? queuedFrame.reservedReadiness :
                m_Presenter->observeDecodeReadiness(frame.frame(), frame.decodeBoundary());
        if (!queuedFrame.reservedReadinessValid)
            decodeReadiness.waitUs += queuedFrame.reservedReadiness.waitUs;
        const uint64_t decodeSyncWaitUs = decodeReadiness.waitUs;
        frame.noteDecodeSyncWaitUs(decodeSyncWaitUs);
        if (decodeReadiness.asynchronousOutput) {
            frame.setDecoderOutputComplete(false);
        }
        if (preparedAhead) {
            frame.noteGpuReadyUs(queuedFrame.preparation->timing.decodeReadyUs);
        }
        if (decodeReadiness.completionObserved) {
            // This is an upper bound on completion, sampled after the native
            // wait. Adding the wait duration to decoder output is wrong when
            // the frame already spent time in the pacing queue: it can place
            // completion before the worker even began waiting.
            frame.noteGpuReadyUs(queuedFrame.reservedReadinessValid ?
                queuedFrame.reservedReadinessObservedUs : LiGetMicroseconds());
        }

        if (m_RebaseOnNextFrame) {
            m_TimingController->rebase();
            m_RebaseOnNextFrame = false;
            externalRebaseFlags = m_RebaseOnNextFrameFlags;
            m_RebaseOnNextFrameFlags = 0;
            externalRebaseApplied = true;
        }
        const uint64_t decisionTimeUs = LiGetMicroseconds();
        VrrTimingDecision decision = m_TimingController->schedule(
            frame, decisionTimeUs);
        publishReceiveDeadline(frame, decision);
        noteSettledDelay(decisionTimeUs, decision.playoutDelayUs);
        FrameTelemetry telemetry;
        telemetry.preparedAhead = preparedAhead;
        if (preparedAhead) telemetry.preparationStage = queuedFrame.preparation->timing;
        telemetry.decodeSyncWaitUs = decodeSyncWaitUs;
        telemetry.decodeReadinessObserved = decodeReadiness.completionObserved;
        telemetry.decisionTimeUs = decisionTimeUs;
        telemetry.decisionEndUs = LiGetMicroseconds();
        telemetry.externalRebaseApplied = externalRebaseApplied;
        telemetry.externalRebaseFlags = externalRebaseFlags;

        // Use the same source mapping and actual buffer line as the graph.
        // A late output alone proves a decode miss; an early asynchronous
        // output cannot prove completion. Render/presentation errors do not
        // become producer jitter observations. Stage counters alone do not
        // slow release; that feedback uses corrected causal input evidence.
        Overlay::TimingGraphPoint bufferPoint;
        bufferPoint.inputPercentileEnabled = m_TimingController->parameters().playoutInputPercentileEnabled != 0;
        bufferPoint.inputToleranceUs = m_TimingController->parameters().playoutInputPercentileToleranceUs;
        bufferPoint.sourceTimeUs = decision.sourceTimeUs;
        bufferPoint.bufferUs = decision.playoutDelayUs;
        bufferPoint.cadenceRetimingUs = decision.cadenceSmoothingUs + decision.readinessBudgetUs;
        bufferPoint.sourceTimingValid = decision.usedRtpTimestamp && !decision.rebased &&
            !decision.phaseDiscontinuity && !externalRebaseApplied;
        bufferPoint.networkReadyUs = frame.reassembledUs();
        bufferPoint.decoderOutputUs = frame.decoderOutputUs();
        bufferPoint.decoderReadyUs = preparedAhead ? telemetry.preparationStage.decodeReadyUs :
            (decodeReadiness.completionObserved || frame.decoderOutputComplete() ?
                frame.decodeCompleteUs() : 0);
        const auto bufferSample = Overlay::bufferGraphSample(bufferPoint);
        telemetry.inputNetworkBufferMiss = bufferSample.networkValid &&
            bufferSample.networkUs > bufferSample.bufferLimitUs;
        telemetry.inputDecodeBufferMiss =
            (bufferSample.readyValid && bufferSample.decoderReadyUs > bufferSample.bufferLimitUs) ||
            (bufferSample.outputValid && bufferSample.decoderOutputUs > bufferSample.bufferLimitUs);
        if (telemetry.inputNetworkBufferMiss || telemetry.inputDecodeBufferMiss) {
            telemetry.inputBufferMissObservationUs = telemetry.decisionEndUs;
            m_TimingController->noteInputBufferMiss(telemetry.inputBufferMissObservationUs,
                telemetry.inputNetworkBufferMiss, telemetry.inputDecodeBufferMiss);
        }

        if (decision.trajectory.resyncRecommended) {
            const uint64_t waitUs = decision.replacementWaitUs != 0 ? decision.replacementWaitUs :
                m_TimingController->parameters().playoutCeilingPhaseReplacement != 0 ?
                    std::min<uint64_t>(20000, decision.sourcePeriodUs * 2) : 0;
            const unsigned replacementWaitMs = unsigned((waitUs + 999) / 1000);
            if (reserveReadySuccessor(preparedAhead, reservedFrame, reservedDiscontinuity, replacementWaitMs)) {
                telemetry.trajectorySuccessorFrame = reservedFrame.frame.frameNumber();
                telemetry.trajectorySuccessorProof = reservedFrame.preparation ? 1 : 2;
                telemetry.staleCheckUs = LiGetMicroseconds();
                const uint64_t ageBoundary =
                    m_TimingController->parameters().playoutSourceMappingDecoderOutput != 0 ?
                        frame.decoderOutputUs() : frame.decodeCompleteUs();
                telemetry.staleAgeUs = telemetry.staleCheckUs >= ageBoundary ?
                    telemetry.staleCheckUs - ageBoundary : 0;
                telemetry.trajectoryResyncDropped = true;
                m_TimingController->noteTrajectoryResyncDrop();
                recordFrameCompletion(queuedFrame, decision, VrrPresentFeedback {}, telemetry,
                                      TraceDisposition::Stale);
                noteDrop();
                m_TimingController->noteSubmission(false, false, 0);
                continue;
            }
            telemetry.trajectoryResyncDeferred = true;
            m_TimingController->noteTrajectoryDeferred();
        }

        // schedule() deliberately clamps an overdue target to the current
        // one-slot deadline. That is right for the newest frame, but it makes
        // the later target-relative stale check unable to see time already
        // spent waiting in this worker's transport queue. One interval of age
        // is ordinary single-worker occupancy, so two source intervals are
        // tolerated. Sustained overload still drops older frames whenever a
        // fresher successor exists, and queue capacity remains the hard bound.
        // A third interval was once granted to a "smoothness" option; on a
        // render-bound client it only deepened the standing backlog.
        const uint64_t scheduleNowUs = LiGetMicroseconds();
        telemetry.staleCheckUs = scheduleNowUs;
        const uint64_t ageBoundaryUs =
            m_TimingController->parameters().playoutSourceMappingDecoderOutput != 0 ?
                frame.decoderOutputUs() : frame.decodeCompleteUs();
        const uint64_t scheduleAgeUs = scheduleNowUs >= ageBoundaryUs ?
            scheduleNowUs - ageBoundaryUs : 0;
        telemetry.staleAgeUs = scheduleAgeUs;
        const bool latencyFix = m_TimingController->latencyFixActive();
        // The playout delay is intentional queue residence, not staleness.
        // So is bounded trajectory carry (at most one source period): it
        // spreads a late predecessor over successors and must not get those
        // successors dropped as stale.
        const uint64_t protectedDelayUs = decision.playoutDelayUs +
            std::min(decision.trajectory.carryUs, decision.sourcePeriodUs);
        // Clock mapping and latency reporting retain the full elapsed age.
        // Discard policy excludes only this image's explicit decode wait:
        // replacing a now-ready image with an unverified successor can repeat
        // forever under GPU contention. Actual pre-wait queue residence still
        // counts, and already-expired queue fronts are rejected before decode.
        const uint64_t ageOriginUs = latencyFix ?
            queuedFrame.trace.arrivalUs :
            (m_TimingController->parameters().playoutSourceMappingDecoderOutput != 0 ?
                frame.decoderOutputUs() : frame.decodeCompleteUs());
        const uint64_t ageUs = VrrFrameDropPolicy::ageExcludingDecodeWaitUs(
            scheduleNowUs, ageOriginUs, decodeSyncWaitUs);
        // These checks shed work before expensive rendering. Offscreen
        // preparation has already finished that work: a newer queued source
        // is not a ready replacement. Repeatedly dropping completed outputs
        // here can freeze video indefinitely under sustained GPU load. Keep
        // queue admission/expiry bounded, but present the active ready image.
        if (!preparedAhead && hasQueuedFrame() && VrrFrameDropPolicy::beforeRender(
                decision, m_TimingController->displayPeriodUs(), ageUs,
                latencyFix, protectedDelayUs,
                m_TimingController->parameters().playoutLateRecovery != 0)) {
            recordFrameCompletion(queuedFrame, decision, VrrPresentFeedback {}, telemetry,
                       TraceDisposition::Stale);
            noteDrop();
            m_TimingController->noteSubmission(false, false, 0);
            continue;
        }

        telemetry.renderWaitEntryUs = LiGetMicroseconds();
        const VrrTargetWaitResult renderWait =
            m_TargetWaiter->waitUntil(decision.renderStartUs);
        telemetry.renderWait = renderWait;
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
        uint32_t midframeWindowStateFlags =
            consumeWindowStateNotifications();
        telemetry.midframeWindowStateFlags |=
            midframeWindowStateFlags;
        bool displayEpochInterrupted =
            (midframeWindowStateFlags &
                kVrrDisplayEpochStateMask) != 0;
        if (displayEpochInterrupted ||
                presentationSuspended() || isStopping()) {
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
            recordFrameCompletion(queuedFrame, decision, feedback, telemetry,
                       TraceDisposition::Interrupted);
            noteDrop();
            continue;
        }

        // A frame can become stale while the worker waits for its render
        // start. Leave the surface unprepared and let the next iteration start
        // fresh rather than rendering an avoidably old image.
        uint64_t nowUs = LiGetMicroseconds();
        if (!preparedAhead && hasQueuedFrame() && VrrFrameDropPolicy::afterRenderWait(
                decision, ageOriginUs, nowUs, latencyFix,
                decodeSyncWaitUs, protectedDelayUs)) {
            recordFrameCompletion(queuedFrame, decision, VrrPresentFeedback {}, telemetry,
                       TraceDisposition::Stale);
            noteDrop();
            if (latencyFix) {
                // The tick is freed for the successor; the clock mapping is
                // still valid and a rebase would only restart its warm-up.
                m_TimingController->noteSubmission(false, false, 0);
            }
            else {
                m_TimingController->rebase();
            }
            continue;
        }

        VrrPresentRequest presentRequest;
        presentRequest.latchedPresentation = decision.latchedPresentation;
        presentRequest.collectDiagnostics = m_DeepTraceEnabled;

        telemetry.preparationStartUs = LiGetMicroseconds();
        const VrrPrepareResult preparation = preparedAhead ?
            m_Presenter->activatePreparedFrame(queuedFrame.preparation,
                frame.frame(), frame.decodeBoundary(), presentRequest) :
            m_Presenter->prepareFrame(frame.frame(), frame.decodeBoundary(), presentRequest);
        telemetry.preparationEndUs = LiGetMicroseconds();
        telemetry.preparationDurationUs =
            telemetry.preparationEndUs >= telemetry.preparationStartUs ?
                telemetry.preparationEndUs - telemetry.preparationStartUs : 0;
        telemetry.prepareTimingValid = preparation.timingValid;
        telemetry.prepareDecodeSyncUs = preparation.decodeSyncUs;
        telemetry.prepareAcquireUs = preparation.acquireUs;
        telemetry.prepareRenderUs = preparation.renderUs;
        telemetry.prepareFlushUs = preparation.flushUs;
        bool gpuReadyCompleted =
            preparation.feedback.gpuReadyTimingValid &&
            preparation.feedback.gpuReadyWaitResultValid &&
            preparation.feedback.gpuReadyWaitResult == 0 &&
            preparation.feedback.gpuReadyTimeUs >=
                preparation.feedback.gpuReadyWaitStartUs;
        uint64_t gpuReadyWaitUs = gpuReadyCompleted ?
            preparation.feedback.gpuReadyTimeUs -
                preparation.feedback.gpuReadyWaitStartUs : 0;
        m_PreparationCost.observe(telemetry.preparationStartUs, telemetry.preparationEndUs);
        m_TimingController->notePreparationDuration(
            telemetry.preparationDurationUs,
            preparation.timingValid ? preparation.acquireUs : 0,
            telemetry.preparationEndUs, gpuReadyWaitUs);
        m_TimingController->noteGpuReadyWait(
            gpuReadyWaitUs, gpuReadyCompleted,
            preparation.feedback.gpuReadyTimeUs);

        midframeWindowStateFlags =
            consumeWindowStateNotifications();
        telemetry.midframeWindowStateFlags |=
            midframeWindowStateFlags;
        displayEpochInterrupted =
            displayEpochInterrupted ||
            (midframeWindowStateFlags &
                kVrrDisplayEpochStateMask) != 0;
        if (!preparation.prepared || displayEpochInterrupted ||
                presentationSuspended() || isStopping()) {
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
                // The cancellation call is the authoritative final backend
                // outcome even when its native submit/fence operation fails.
                // Keeping only successful cancellation submissions hid the
                // exact failure evidence from diagnostic traces.
                feedback = cancelFeedback;
                if (cancelFeedback.nativeBackendValid) {
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
            recordFrameCompletion(queuedFrame, decision, feedback, telemetry,
                       preparation.prepared ? TraceDisposition::Interrupted :
                                              TraceDisposition::PreparationFailed);
            noteDrop();
            deferFrame(std::move(frame));
            continue;
        }

        if (preparation.sourceFrameReusable) {
            // The backend has completed every GPU read from this decoder
            // surface. Release it before the target wait so high-resolution
            // pacing cannot exhaust the decoder surface pool.
            AVFrame* reusableFrame = frame.release();
            av_frame_free(&reusableFrame);
        }

        telemetry.nativeScheduledPresentation = m_NativeScheduledPresentation;
        VrrTargetWaitResult targetWait;
        const auto holdCadence = [&]() {
            do {
                telemetry.targetWaitEntryUs = LiGetMicroseconds();
                targetWait = m_TargetWaiter->waitUntil(decision.targetUs,
                                                      decision.targetWakeLeadUs);
                // Keep the last complete wait observation intact. Combining
                // one attempt's raw fields with a later clock sample corrupts
                // exact replay, especially for an already-elapsed deadline.
            } while (m_NativeScheduledPresentation &&
                     targetWait.finalNowUs < decision.targetUs);
            telemetry.targetWait = targetWait;
            telemetry.targetWaitFinalUs = targetWait.finalNowUs;
            telemetry.targetWaitOvershootUs = targetWait.deadlineAlreadyElapsed ?
                0 : positiveDifference(targetWait.finalNowUs, decision.targetUs);
            telemetry.targetSchedulerDelayUs = targetWait.schedulerDelayUs;
            telemetry.targetSchedulerDelayValid = targetWait.schedulerDelayValid;
            telemetry.targetDeadlineAlreadyElapsed = targetWait.deadlineAlreadyElapsed;
        };
        if (!m_NativeScheduledPresentation) holdCadence();
        midframeWindowStateFlags =
            consumeWindowStateNotifications();
        telemetry.midframeWindowStateFlags |=
            midframeWindowStateFlags;
        displayEpochInterrupted =
            displayEpochInterrupted ||
            (midframeWindowStateFlags &
                kVrrDisplayEpochStateMask) != 0;
        if (displayEpochInterrupted ||
                presentationSuspended() || isStopping()) {
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
            recordFrameCompletion(queuedFrame, decision, feedback, telemetry,
                       TraceDisposition::Interrupted);
            noteDrop();
            deferFrame(std::move(frame));
            continue;
        }

        // Recheck both mathematical floors immediately before Present. The
        // waiter deliberately has a bounded active phase, so a pathological
        // clock must not turn an early return into an early submission.
        const bool hadPriorSubmission = m_TimingController->hasLastSubmission();
        const uint64_t priorSubmissionUs = m_TimingController->lastSubmissionUs();
        if (!m_NativeScheduledPresentation) {
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

            telemetry.spacingRecheckUs = LiGetMicroseconds();
            telemetry.presentStartUs = telemetry.spacingRecheckUs;
            if (hadPriorSubmission) {
                telemetry.presentSpacingUs =
                    telemetry.presentStartUs >= priorSubmissionUs ?
                        telemetry.presentStartUs - priorSubmissionUs : 0;
                // A tearing present must clear the previous frame's flip, which
                // for a latched predecessor can be later than its Present call.
                const uint64_t minimumUntornUs =
                    m_TimingController->untornReferenceUs() +
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
                    telemetry.spacingCorrectedFloorUs = correctedFloorUs;
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
        }
        else {
            telemetry.spacingCheckUs = LiGetMicroseconds();
            telemetry.spacingRecheckUs = telemetry.spacingCheckUs;
            // The controller reserved display spacing. CPU enqueue is
            // intentionally early and must not be checked against a flip floor.
            presentRequest.displayTargetUs = decision.targetUs;
        }

        // The mathematical floor and its correction can add another
        // display-period wait after the primary target checkpoint. Drain
        // notifications once more at the actual submission boundary so a
        // minimize or a renderer-accepted display epoch cannot slip through
        // that final wait and be presented under the old decision.
        midframeWindowStateFlags =
            consumeWindowStateNotifications();
        telemetry.midframeWindowStateFlags |=
            midframeWindowStateFlags;
        displayEpochInterrupted =
            displayEpochInterrupted ||
            (midframeWindowStateFlags &
                kVrrDisplayEpochStateMask) != 0;
        if (displayEpochInterrupted ||
                presentationSuspended() || isStopping()) {
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
            recordFrameCompletion(queuedFrame, decision, feedback, telemetry,
                       TraceDisposition::Interrupted);
            noteDrop();
            deferFrame(std::move(frame));
            continue;
        }

        if (!m_NativeScheduledPresentation) m_TimingController->noteSchedulerDelays(
            telemetry.renderWaitOvershootUs,
            targetWait.schedulerDelayUs,
            targetWait.schedulerDelayValid);

        // Require native scanout protection even before the first accepted ID
        // and after resets. Missing evidence must synchronize, not silently
        // admit the adaptive path under a submission-time estimate.
        presentRequest.flipProtectionWindowUs =
            m_TimingController->parameters().nativeFlipProtection != 0 &&
                m_CanLatchPresentation &&
                !decision.latchedPresentation ?
            m_TimingController->displayPeriodUs() : 0;
        telemetry.presentStartUs = LiGetMicroseconds();
        VrrPresentFeedback feedback =
            m_Presenter->presentAdaptive(presentRequest);
        telemetry.presentEndUs = LiGetMicroseconds();
        // Our Present is queued ahead of any decode submitted from now on.
        VrrReceiveDeadline::clearPresentWindow();
        m_PresentCallCost.observe(telemetry.presentStartUs, telemetry.presentEndUs);
        telemetry.presentDurationUs =
            telemetry.presentEndUs >= telemetry.presentStartUs ?
                telemetry.presentEndUs - telemetry.presentStartUs : 0;
        if (!gpuReadyCompleted) {
            const bool deferredGpuReadyCompleted =
                feedback.gpuReadyTimingValid &&
                feedback.gpuReadyWaitResultValid &&
                feedback.gpuReadyWaitResult == 0 &&
                feedback.gpuReadyTimeUs >= feedback.gpuReadyWaitStartUs;
            if (deferredGpuReadyCompleted) {
                gpuReadyCompleted = true;
                gpuReadyWaitUs = feedback.gpuReadyTimeUs -
                    feedback.gpuReadyWaitStartUs;
                const uint64_t completionUpperBoundUs =
                    feedback.gpuReadyCompletedBeforeWait ?
                        feedback.gpuReadyPollEndUs :
                        feedback.gpuReadyTimeUs;
                m_TimingController->noteDeferredGpuReady(
                    gpuReadyWaitUs, true, completionUpperBoundUs,
                    completionUpperBoundUs >= telemetry.preparationStartUs ?
                        completionUpperBoundUs - telemetry.preparationStartUs : 0,
                    !feedback.gpuReadyCompletedBeforeWait);
            }
        }
        if (m_NativeScheduledPresentation) {
            // Submission and retirement are now owned by the native queue.
            // Hold only this worker's next dequeue until the logical deadline,
            // avoiding a speculative extra queue of future frames.
            holdCadence();
            m_TimingController->noteSchedulerDelays(
                telemetry.renderWaitOvershootUs,
                targetWait.schedulerDelayUs, targetWait.schedulerDelayValid);
        }
        recordSubmission(decision, feedback, telemetry.presentStartUs,
                         telemetry.presentEndUs,
                         telemetry);
        if (m_Telemetry != nullptr) {
            VrrTelemetrySample sample;
            sample.graph.submitted = feedback.presented && !feedback.cancelled;
            sample.graph.submissionUs = telemetry.submissionBoundaryUs;
            sample.graph.targetUs = decision.targetUs;
            sample.graph.sourcePeriodUs = decision.sourcePeriodUs;
            sample.graph.bufferUs = decision.playoutDelayUs;
            sample.graph.extraDelayUs = decision.plannedExtraDelayUs();
            sample.graph.requestedBufferUs = decision.requestedPlayoutDelayUs;
            const auto graphInterval = m_TimingController->intervalStats();
            const auto& input = graphInterval.inputPercentile;
            sample.graph.inputPercentileEnabled = graphInterval.inputPercentileEnabled;
            // noteSubmission just committed this frame at its CPU enqueue time.
            // Never publish a previous frame's eligibility after an early return,
            // reset or rejected observation. Sampled GPU completion remains raw.
            sample.graph.producerEligible = graphInterval.inputPercentileEnabled &&
                input.lastInputObservationUs == telemetry.submissionBoundaryUs && input.lastInputEligible;
            sample.graph.producerRequiredUs = input.lastInputRequiredUs;
            sample.graph.inputVrrHeadroomUs = input.vrrHeadroomUs;
            sample.graph.inputTargetUs = input.targetUs;
            sample.graph.inputTargetValid = input.evidenceValid;
            sample.graph.inputTargetCapped = input.clipped;
            sample.graph.inputProbabilityPpm = input.probabilityPpm;
            sample.graph.inputWindowUs = input.windowUs;
            // Profile allowance remains known while reset evidence is unavailable.
            sample.graph.inputToleranceUs = m_TimingController->parameters().playoutInputPercentileToleranceUs;
            sample.graph.inputCoverageWindowUs = input.reportingWindowUs;
            sample.graph.inputSamples = input.reportingWindowUs ? input.reportingEligible : input.recentEligible;
            sample.graph.inputAbsorbed = input.reportingWindowUs ? input.reportingAbsorbed : input.recentAbsorbed;
            sample.graph.inputMissed = input.reportingWindowUs ? input.reportingMissed : input.recentMissed;
            sample.graph.trajectoryCarryUs = decision.trajectory.carryUs;
            sample.graph.toleranceUs = m_TimingController->intervalToleranceUs();
            sample.graph.sourceTimeUs = decision.sourceTimeUs;
            sample.graph.cadenceRetimingUs = decision.cadenceSmoothingUs + decision.readinessBudgetUs;
            sample.graph.sourceTimingValid = decision.usedRtpTimestamp &&
                !decision.rebased && !decision.phaseDiscontinuity && !externalRebaseApplied;
            sample.graph.networkReadyUs = frame.reassembledUs();
            sample.graph.decoderOutputUs = frame.decoderOutputUs();
            // Preserve known completion only. An asynchronous decoder output
            // alone cannot prove that its GPU work fit inside the buffer.
            sample.graph.decoderReadyUs = preparedAhead ? telemetry.preparationStage.decodeReadyUs :
                (decodeReadiness.completionObserved || frame.decoderOutputComplete() ?
                    frame.decodeCompleteUs() : 0);
            sample.graph.discontinuity = decision.rebased || decision.phaseDiscontinuity || externalRebaseApplied;
            sample.graph.backend = feedback.nativeBackendValid ? uint32_t(feedback.nativeBackend) : 0;
            sample.graph.idValid = feedback.submissionIdValid;
            sample.graph.submissionId = feedback.submissionId;
            sample.graph.displayValid = feedback.latchSampleValid &&
                feedback.latchTimeKind == Vrr13::PresentationTimeKind::DisplayEvent;
            sample.graph.displayId = feedback.latchSubmissionId;
            sample.graph.displayUs = feedback.latchTimeUs;
            // DXGI exposes an image's refresh identity separately from its
            // sampled refresh clock. The graph joins those identities and
            // labels the result as refresh timing; strict display-event
            // diagnostics and controller adaptation retain their own policy.
            sample.graph.preparationReadyUs = telemetry.preparationEndUs;
            sample.graph.refreshDisjoint = feedback.frameStatsDisjoint;
            sample.graph.refreshValid =
                feedback.nativeBackend == VrrNativePresentationBackend::Dxgi &&
                feedback.latchSampleValid &&
                feedback.latchTimeKind == Vrr13::PresentationTimeKind::RefreshReference &&
                feedback.latchRawSyncQpcValid && feedback.latchQpcCorrelationValid &&
                feedback.latchRawSyncQpcFrequency != 0 &&
                feedback.nativeVblankVirtualizationDisabled;
            if (sample.graph.refreshValid) {
                sample.graph.refreshReportedId = feedback.latchSubmissionId;
                sample.graph.refreshPresentSequence = feedback.latchPresentRefreshSequence;
                sample.graph.refreshSyncSequence = feedback.latchRefreshSequence;
                sample.graph.refreshTimeUs = feedback.latchTimeUs;
                sample.graph.refreshObservedUs = telemetry.presentEndUs;
                sample.graph.refreshUncertaintyUs =
                    feedback.latchQpcCorrelationSpanTicks * 1000000 /
                        feedback.latchRawSyncQpcFrequency;
            }
            sample.queueResidenceUs = positiveDifference(queuedFrame.trace.dequeueUs,
                                                         queuedFrame.trace.arrivalUs);
            sample.decodeWaitUs = telemetry.decodeSyncWaitUs;
            if (preparedAhead) {
                const auto& stage = telemetry.preparationStage;
                sample.decodeWaitUs = stage.decodeWaitUs;
                sample.queueResidenceUs =
                    positiveDifference(stage.startUs, queuedFrame.trace.arrivalUs) +
                    positiveDifference(queuedFrame.trace.dequeueUs, stage.readyUs);
            }
            sample.bufferUs = decision.playoutDelayUs;
            // Both overlays use the same frame's net positive correction.
            sample.smoothingUs = sample.graph.extraDelayUs;
            sample.submissionUs = telemetry.submissionBoundaryUs;
            sample.motionDiscontinuity = decision.rebased || externalRebaseApplied;
            sample.decisionTimeUs = decisionTimeUs;
            sample.clientProcessingTimeUs =
                telemetry.presentEndUs >= frame.decoderOutputUs() ?
                    telemetry.presentEndUs - frame.decoderOutputUs() : 0;
            sample.renderingTimeUs = telemetry.preparationDurationUs +
                telemetry.presentDurationUs;
            if (preparedAhead) {
                sample.renderingTimeUs += positiveDifference(
                    telemetry.preparationStage.readyUs,
                    telemetry.preparationStage.renderStartUs);
            }
            sample.preparationUs = telemetry.preparationDurationUs;
            sample.presentCallUs = telemetry.presentDurationUs;
            sample.gpuReadyWaitUs = gpuReadyWaitUs;
            sample.gpuReadyWaitValid = gpuReadyCompleted;
            if (preparedAhead) {
                const auto& stage = telemetry.preparationStage;
                sample.preparationUs += positiveDifference(stage.readyUs, stage.renderStartUs);
                sample.gpuReadyWaitUs += positiveDifference(stage.readyUs, stage.renderEndUs);
                sample.gpuReadyWaitValid = true;
            }
            sample.latched = decision.latchedPresentation ||
                feedback.flipProtectionLatched;
            sample.bufferCapUs = decision.playoutDelayMaximumUs;
            sample.gpuReadinessLeadUs = decision.gpuReadinessLeadUs;
            const uint64_t readinessDeadlineUs =
                decision.originalTargetUs;
            sample.prepareLate = telemetry.preparationEndUs > readinessDeadlineUs;
            sample.cadenceIntervals = m_TimingController->nativeCadenceIntervals();
            sample.cadenceHitches = m_TimingController->nativeCadenceHitches();
            sample.estimatedCadenceIntervals = m_TimingController->estimatedCadenceIntervals();
            sample.estimatedCadenceHitches = m_TimingController->estimatedCadenceHitches();
            sample.preparationLatenessUs = sample.prepareLate ?
                telemetry.preparationEndUs - readinessDeadlineUs : 0;
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
        recordFrameCompletion(queuedFrame, decision, feedback, telemetry,
                   outputDropped ?
                       TraceDisposition::OutputDropped :
                       TraceDisposition::Presented);
        deferFrame(std::move(frame));
    }

    if (reservedFrame) {
        recordFrameCompletion(reservedFrame, VrrTimingDecision {},
            VrrPresentFeedback {}, FrameTelemetry {}, TraceDisposition::ShutdownDiscard, false);
        noteDrop();
    }
    // Release any native state retained between preparation and presentation.
    m_Presenter->cancelFrame();
    return 0;
}

bool VrrPacingWorker::dequeueFrame(QueuedFrame& frame,
                                   bool& queueDiscontinuity)
{
    std::deque<QueuedFrame> expiredFrames;
    QMutexLocker lock(&m_FrameQueueLock);
    while (!isStopping() && !m_Suspended.load() &&
            m_PendingWindowStateFlags.load() == 0 && m_FrameQueue.empty()) {
        m_FrameQueueNotEmpty.wait(&m_FrameQueueLock);
    }

    if (isStopping()) {
        return false;
    }
    if (m_Suspended.load() || m_PendingWindowStateFlags.load() != 0 ||
            m_FrameQueue.empty()) {
        // A display epoch must reach the presenter even when no successor
        // frame arrives. Return an empty wake to the notification drain.
        return true;
    }

    const uint64_t nowUs = LiGetMicroseconds();
    const auto trajectory = m_TimingController->trajectoryStats();
    const uint64_t protectedDelayUs = m_TimingController->playoutDelayUs() +
        std::min(trajectory.carryUs, m_TimingController->sourcePeriodUs());
    while (m_FrameQueue.size() > 1 && !m_RebaseOnNextFrame &&
            VrrFrameDropPolicy::beforeDecodeWait(
                m_FrameQueue[0].frame, m_FrameQueue[1].frame,
                m_FrameQueue[0].trace.arrivalUs, nowUs,
                m_TimingController->sourcePeriodUs(),
                protectedDelayUs)) {
        expiredFrames.push_back(std::move(m_FrameQueue.front()));
        m_FrameQueue.pop_front();
    }

    queueDiscontinuity = m_QueueDiscontinuity.exchange(false);
    frame = std::move(m_FrameQueue.front());
    m_FrameQueue.pop_front();
    m_FrameQueueDepth.store(m_FrameQueue.size(), std::memory_order_relaxed);
    lock.unlock();
    // No backend has read these images. Release them outside the queue lock
    // without waiting for decode or perturbing the controller's source clock.
    for (const auto& expired : expiredFrames) {
        recordFrameCompletion(expired, VrrTimingDecision{}, VrrPresentFeedback{},
                              FrameTelemetry{}, TraceDisposition::QueueStale, false);
        noteDrop();
    }
    return true;
}

bool VrrPacingWorker::hasQueuedFrame()
{
    QMutexLocker lock(&m_FrameQueueLock);
    return !m_FrameQueue.empty();
}

bool VrrPacingWorker::reserveReadySuccessor(bool preparedCurrent,
                                           QueuedFrame& frame,
                                           bool& queueDiscontinuity,
                                           unsigned waitMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    QMutexLocker lock(&m_FrameQueueLock);
    while (waitMs && m_FrameQueue.empty() && !isStopping() && !m_Suspended.load() &&
            m_PendingWindowStateFlags.load() == 0 && std::chrono::steady_clock::now() < deadline)
        m_FrameQueueNotEmpty.wait(&m_FrameQueueLock, 1);
    if (m_FrameQueue.empty()) return false;
    const auto& next = m_FrameQueue.front();
    const bool ready = next.preparation ? next.preparation->readyForPresentation() :
        !preparedCurrent && next.frame.decoderOutputComplete();
    if (!ready && !waitMs) return false;
    // Transfer ownership under the proof's queue lock. A concurrent producer
    // must not evict the proven replacement between checking and dropping.
    queueDiscontinuity = m_QueueDiscontinuity.exchange(false);
    frame = std::move(m_FrameQueue.front());
    m_FrameQueue.pop_front();
    m_FrameQueueDepth.store(m_FrameQueue.size(), std::memory_order_relaxed);
    lock.unlock();
    if (ready) return true;
    // The reserved image remains owned even on failure. Present the current
    // image and process this successor normally next; never drop an image on
    // the strength of queue presence alone.
    if (frame.preparation) {
        while (!frame.preparation->readyForPresentation() && !isStopping() &&
                !presentationSuspended() && m_PendingWindowStateFlags.load() == 0 &&
                std::chrono::steady_clock::now() < deadline)
            SDL_Delay(1);
        return frame.preparation->readyForPresentation();
    }
    if (preparedCurrent || isStopping() || presentationSuspended()) return false;
    frame.trace.dequeueUs = LiGetMicroseconds();
    frame.reservedReadiness = m_Presenter->observeDecodeReadiness(
        frame.frame.frame(), frame.frame.decodeBoundary());
    frame.reservedReadinessObservedUs = LiGetMicroseconds();
    frame.reservedReadinessValid = frame.reservedReadiness.completionObserved;
    return frame.reservedReadiness.completionObserved;
}

void VrrPacingWorker::discardQueuedFrames(
    bool countDrops, TraceDisposition disposition)
{
    std::deque<QueuedFrame> discardedFrames;
    {
        QMutexLocker lock(&m_FrameQueueLock);
        discardedFrames.swap(m_FrameQueue);
        m_FrameQueueDepth.store(0, std::memory_order_relaxed);
        m_QueueDiscontinuity.store(false);
    }

    for (const QueuedFrame& frame : discardedFrames) {
        recordFrameCompletion(frame, VrrTimingDecision {}, VrrPresentFeedback {},
                   FrameTelemetry {}, disposition, false);
        if (countDrops) {
            noteDrop();
        }
    }
}

uint32_t VrrPacingWorker::consumeWindowStateNotifications()
{
    uint32_t consumedFlags =
        m_PendingWindowStateFlags.exchange(0);
    if (consumedFlags == 0) {
        return 0;
    }

    // Minimize/restore can race while this worker is draining notifications.
    // Reconcile to the authoritative atomic state rather than applying a stale
    // flag snapshot in event order.
    bool suspended = m_Suspended.load();
    while (true) {
        const uint32_t newerFlags = m_PendingWindowStateFlags.exchange(0);
        consumedFlags |= newerFlags;
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
    if (m_NativeScheduledPresentation) {
        // Accepted deadlines belong to this window/display epoch. Cancel any
        // queued updates before preparing work for the reconciled new epoch.
        m_Presenter->clearScheduledPresentation();
    }
    m_RebaseOnNextFrame = true;
    m_RebaseOnNextFrameFlags |= consumedFlags;
    return consumedFlags;
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
    if (telemetry.targetWaitEntryUs == 0) {
        telemetry.targetWaitEntryUs = nowUs;
    }
    telemetry.spacingCheckUs = nowUs;
    telemetry.presentationFloorUs = submissionFloorUs;
    if (nowUs >= submissionFloorUs) {
        telemetry.targetWaitFinalUs = std::max(
            telemetry.targetWaitFinalUs, nowUs);
        telemetry.targetDeadlineAlreadyElapsed = true;
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
    telemetry.targetWaitFinalUs = std::max(
        telemetry.targetWaitFinalUs, nowUs);
    if (!wait.deadlineAlreadyElapsed) {
        telemetry.targetWaitOvershootUs = std::max(
            telemetry.targetWaitOvershootUs,
            positiveDifference(nowUs, submissionFloorUs));
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
            const uint64_t priorSubmissionUs = telemetry.nativeScheduledPresentation ?
                m_LastActualSubmissionUs : m_TimingController->lastSubmissionUs();
            telemetry.presentSpacingUs =
                telemetry.submissionBoundaryUs >= priorSubmissionUs ?
                    telemetry.submissionBoundaryUs - priorSubmissionUs : 0;
            const uint64_t minimumUntornUs = priorSubmissionUs +
                m_TimingController->displayPeriodUs();
            telemetry.spacingMarginUs = signedDifference(
                telemetry.submissionBoundaryUs, minimumUntornUs);
        }
    }

    telemetry.schedulingBoundaryUs = telemetry.submissionBoundaryUs;
    if (telemetry.nativeScheduledPresentation && feedback.presented &&
            !feedback.cancelled && feedback.nativeTargetValid) {
        telemetry.schedulingBoundaryUs = std::max(
            telemetry.submissionBoundaryUs, feedback.nativeTargetUs);
    }
    if (feedback.presented) m_LastActualSubmissionUs = telemetry.submissionBoundaryUs;

    if (feedback.flipProtectionLatched) {
        m_TimingController->noteNativeFlipProtection(
            feedback.flipProtectionReferenceUs);
    }
    m_TimingController->noteSubmission(
        feedback.presented, feedback.cancelled,
        telemetry.schedulingBoundaryUs, telemetry.submissionBoundaryUs);
    Vrr13::PresentationObservation observation;
    observation.smoothness = m_TimingController->smoothnessSample(decision);
    observation.submitted = feedback.presented && !feedback.cancelled;
    observation.idValid = feedback.submissionIdValid;
    observation.id = feedback.submissionId;
    observation.submission = telemetry.submissionBoundaryUs;
    observation.ready = telemetry.preparationEndUs;
    observation.deadline = decision.originalScanoutUs;
    // Vulkan, composition and Metal do not select DXGI latch modes. A requested
    // latch transition must not reset their feedback matching history.
    observation.latched = (feedback.nativeBackend == VrrNativePresentationBackend::Vulkan ||
                           feedback.nativeBackend == VrrNativePresentationBackend::Composition ||
                           feedback.nativeBackend == VrrNativePresentationBackend::Metal) ?
        false : decision.latchedPresentation || feedback.flipProtectionLatched;
    observation.dxgi = feedback.nativeBackend == VrrNativePresentationBackend::Dxgi;
    observation.sampleValid = feedback.latchSampleValid &&
        (!observation.dxgi || (feedback.latchQpcCorrelationValid && feedback.latchRawSyncQpcFrequency));
    observation.sampleId = feedback.latchSubmissionId;
    observation.sampleTime = feedback.latchTimeUs;
    observation.timeKind = feedback.latchTimeKind;
    observation.observed = operationEndUs;
    observation.presentRefresh = feedback.latchPresentRefreshSequence;
    observation.syncRefresh = feedback.latchRefreshSequence;
    observation.uncertainty = feedback.latchRawSyncQpcFrequency ?
        feedback.latchQpcCorrelationSpanTicks * 1000000 / feedback.latchRawSyncQpcFrequency :
        feedback.presentationUncertaintyUs;
    m_TimingController->notePresentation(observation);
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

QString VrrPacingWorker::startDelayPath() const
{
    return QFileInfo(QString::fromStdString(m_Config.calibrationPath)).absolutePath() +
        QStringLiteral("/vrr-start-delay.json");
}

void VrrPacingWorker::noteSettledDelay(uint64_t nowUs, uint64_t delayUs)
{
    // Sample once per second after the first half minute: early delay still
    // reflects the starting value rather than what this setup needs.
    if (!m_FirstDecisionUs) m_FirstDecisionUs = nowUs;
    if (nowUs - m_FirstDecisionUs < kSettledDelayWarmupUs ||
            (m_LastSettledDelaySampleUs && nowUs - m_LastSettledDelaySampleUs < 1000000) ||
            m_SettledDelaySamples.size() >= 36000) {
        return;
    }
    m_LastSettledDelaySampleUs = nowUs;
    m_SettledDelaySamples.push_back(delayUs);
}

void VrrPacingWorker::publishReceiveDeadline(const PacedFrame& frame,
                                             const VrrTimingDecision& decision)
{
    // A frame is on time when its decode-complete boundary reaches the mapped
    // source slot plus the playout delay (the lateness the delay calibrator
    // sees, before cadence retiming). Later frames follow at the RTP rate from
    // this anchor. Without a timestamp slot there is nothing to protect.
    if (!m_TimingController->timestampPlayoutActive() || !frame.timestampValid()) {
        VrrReceiveDeadline::clear();
        VrrReceiveDeadline::clearPresentWindow();
        return;
    }
    // A deliberate decode hold is spare time, not cost: counting it would
    // move every deadline earlier and cut more detail for nothing. A frame
    // whose own decode took longer than a source period is left out too:
    // releasing later frames earlier cannot relieve an overloaded decoder, and
    // counting it moved every deadline before the frames finished arriving.
    const uint64_t decodeStartUs = frame.reassembledUs() + frame.decodeHoldUs();
    if (frame.reassembledUs() && frame.decodeCompleteUs() >= decodeStartUs &&
            frame.decodeCompleteUs() - decodeStartUs <= decision.sourcePeriodUs) {
        m_RecentDuration.observeGpuCompletion(
            decodeStartUs, frame.decoderOutputUs(), frame.decodeCompleteUs(),
            frame.decoderOutputComplete());
    }
    m_DecodeGpuCost.observeGpuCompletion(
        frame.decodeSubmitUs(), frame.decoderOutputUs(), frame.decodeCompleteUs(),
        frame.decoderOutputComplete());
    if (m_DecodeGpuCost.ready() && m_PresentCallCost.ready()) {
        // A decode submitted from here on would still be running when this
        // frame is presented. The window closes when the Present call returns;
        // the expected close only decides whether a hold is affordable.
        const uint64_t startUs = decision.targetUs -
            std::min(decision.targetUs, m_DecodeGpuCost.percentileUs());
        VrrReceiveDeadline::publishPresentWindow(
            startUs, decision.targetUs + m_PresentCallCost.percentileUs());
    }
    if (!m_RecentDuration.ready()) {
        return;
    }
    if (m_PreparationCost.ready()) {
        const uint64_t holdLeadUs = m_RecentDuration.percentileUs() +
            m_PreparationCost.percentileUs() + kReceiveDeadlineMarginUs;
        if (decision.targetUs > holdLeadUs) {
            VrrReceiveDeadline::publishHold(frame.rtpTimestamp(), decision.targetUs - holdLeadUs);
        }
    }
    const uint64_t onTimeUs = decision.sourceTimeUs + decision.playoutDelayUs;
    const uint64_t leadUs = m_RecentDuration.percentileUs() + kReceiveDeadlineMarginUs;
    if (onTimeUs <= leadUs) {
        return;
    }
    VrrReceiveDeadline::publish(frame.rtpTimestamp(), onTimeUs - leadUs);
}

void VrrPacingWorker::recordFrameCompletion(const QueuedFrame& queuedFrame,
                                 const VrrTimingDecision& decision,
                                 const VrrPresentFeedback& feedback,
                                 const FrameTelemetry& telemetry,
                                 TraceDisposition disposition,
                                 bool decisionValid)
{
    // Publish every playback outcome once, even when disk tracing is disabled
    // or drops a row. Intentional window/shutdown discards are not playback.
    const bool playbackOutcome = disposition == TraceDisposition::Presented ||
        disposition == TraceDisposition::OutputDropped ||
        disposition == TraceDisposition::QueueCapacity ||
        disposition == TraceDisposition::QueueStale ||
        disposition == TraceDisposition::Stale ||
        disposition == TraceDisposition::PreparationFailed;
    if (m_Telemetry && playbackOutcome) {
        const auto& parameters = m_TimingController->parameters();
        const uint64_t deadlineUs = 12 >= 3 ?
            decision.originalTargetUs : decision.targetUs;
        const bool dropped = disposition != TraceDisposition::Presented;
        // Controller state belongs to the worker. Producer-side drop rows
        // preserve the last published interval result instead of reading it.
        const bool intervalValid = decisionValid && 12 >= 6;
        const auto intervalStats = intervalValid ? m_TimingController->intervalStats() : Vrr13::IntervalBuffer::Stats{};
        m_Telemetry->recordVrrReadiness(LiGetMicroseconds(),
            decisionValid ? positiveDifference(telemetry.preparationEndUs, deadlineUs) : 0,
            dropped, parameters.playoutOnTimeTargetPerMillion,
            decision.playoutCapacityLimited, decisionValid,
            12 >= 4,
            12 >= 5,
            12 >= 6, intervalValid ? &intervalStats : nullptr);
    }
    if (queuedFrame.trace.arrivalSequence == 0) {
        return;
    }
    struct ProducerGuard {
        std::atomic_uint& active;
        explicit ProducerGuard(std::atomic_uint& value) : active(value) { ++active; }
        ~ProducerGuard() { --active; }
    } producer(m_TraceProducersActive);
    if (!m_TraceAcceptingRows.load()) {
        m_TraceDroppedRows.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const PacedFrame& frame = queuedFrame.frame;
    TraceRow row;
    row.frameNumber = frame.frameNumber();
    row.rtpTimestamp = frame.rtpTimestamp();
    row.lostPackets = frame.lostPackets();
    row.timestampValid = frame.timestampValid();
    row.decodeCompleteUs = frame.decodeCompleteUs();
    row.decoderOutputUs = frame.decoderOutputUs();
    row.latencyMode = m_Config.latencyMode;
    row.receiveUs = frame.receiveUs();
    row.reassembledUs = frame.reassembledUs();
    row.decodeSubmitUs = frame.decodeSubmitUs();
    row.decodeHoldUs = frame.decodeHoldUs();
    row.input = queuedFrame.trace;
    row.decision = decision;
    // submit() and window notifications may emit terminal rows from threads
    // other than the pacing worker. The controller's learned-state containers
    // are worker-owned and are not safe to inspect concurrently. A valid
    // decision proves this row is on the worker path; producer-side terminal
    // rows deliberately carry zero/unavailable controller diagnostics.
    row.diagnostics = decisionValid ?
        m_TimingController->diagnostics() : VrrTimingDiagnostics {};
    if (decisionValid) {
        // Only scalar reads on the controller-owning thread. Do not calculate
        // histogram quantiles or serialize calibration on the delivery path.
        const auto& history = m_TimingController->playoutHistory();
        row.historySamples = history.evidence();
        row.historyMisses = history.misses();
        row.historyDurationUs = static_cast<uint64_t>(history.duration() / 1000);
        row.historyCanRelease = history.canRelease();
        row.bufferStats = m_TimingController->intervalStats();
    }
    row.feedback = feedback;
    row.telemetry = telemetry;
    row.completionQueueDepth =
        m_FrameQueueDepth.load(std::memory_order_relaxed);
    row.disposition = disposition;
    row.decisionValid = decisionValid;
    row.terminalTimeUs = LiGetMicroseconds();

    if (m_TraceQueue->push(std::move(row)))
        m_TraceRowsEnqueued.fetch_add(1, std::memory_order_relaxed);
    else
        m_TraceDroppedRows.fetch_add(1, std::memory_order_relaxed);
}
