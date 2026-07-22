#pragma once

#include "../../decoder.h"
#include "../ivrrframepresenter.h"
#include "pacertelemetry.h"
#include "vrr/vrrtypes.h"
#include "vrr/vrrtimingcontroller.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>

#include <QByteArray>
#include <QMutex>
#include <QWaitCondition>

class VrrTargetWaiter;
class VrrTimingController;

// The complete greenfield VRR execution path lives in this one worker.  It
// owns a bounded queue and the renderer context from preparation through
// presentation; fixed-VSync and unpaced Pacer behavior never enter it.
class VrrPacingWorker {
public:
    VrrPacingWorker(IVrrFramePresenter* presenter,
                    const VrrSessionConfig& config,
                    PacerTelemetry* telemetry);
    ~VrrPacingWorker();

    VrrPacingWorker(const VrrPacingWorker&) = delete;
    VrrPacingWorker& operator=(const VrrPacingWorker&) = delete;

    bool start();

    void submit(PacedFrame&& frame);

    // Calls from the UI/main thread only manipulate worker-owned state. All
    // backend notifications are delivered by the worker itself.
    void notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info);

private:
    enum class TraceDisposition : uint8_t {
        Presented,
        OutputDropped,
        QueueCapacity,
        ArrivalRejected,
        SuspensionDiscard,
        ShutdownDiscard,
        Interrupted,
        Stale,
        PreparationFailed,
    };

    struct FrameTraceContext {
        uint64_t arrivalSequence = 0;
        uint64_t arrivalUs = 0;
        uint64_t dequeueUs = 0;
        size_t queueDepthBefore = 0;
        size_t queueDepthAfter = 0;
        bool enabled = false;
        bool queueAccepted = false;
        bool queueDiscontinuity = false;
    };

    struct QueuedFrame {
        PacedFrame frame;
        FrameTraceContext trace;

        explicit operator bool() const
        {
            return static_cast<bool>(frame);
        }
    };

    struct FrameTelemetry {
        uint64_t decisionTimeUs = 0;
        uint64_t decisionEndUs = 0;
        uint64_t staleCheckUs = 0;
        uint64_t staleAgeUs = 0;
        uint64_t renderWaitEntryUs = 0;
        uint64_t renderWaitFinalUs = 0;
        uint64_t renderWaitOvershootUs = 0;
        uint64_t renderSchedulerDelayUs = 0;
        bool renderSchedulerDelayValid = false;
        bool renderDeadlineAlreadyElapsed = false;
        uint64_t preparationStartUs = 0;
        uint64_t preparationEndUs = 0;
        uint64_t preparationDurationUs = 0;
        uint64_t targetWaitEntryUs = 0;
        uint64_t targetWaitOvershootUs = 0;
        uint64_t targetWaitFinalUs = 0;
        uint64_t targetSchedulerDelayUs = 0;
        uint64_t presentStartUs = 0;
        uint64_t submissionBoundaryUs = 0;
        uint64_t presentEndUs = 0;
        uint64_t presentDurationUs = 0;
        uint64_t presentSpacingUs = 0;
        int64_t submitErrorUs = 0;
        int64_t spacingMarginUs = 0;
        uint64_t spacingDeficitUs = 0;
        uint64_t spacingGuardFeedbackUs = 0;
        uint64_t spacingCheckUs = 0;
        uint64_t presentationFloorUs = 0;
        uint64_t correctionWaitStartUs = 0;
        uint64_t correctionWaitEndUs = 0;
        uint64_t submissionId = 0;
        uint64_t latchSubmissionId = 0;
        uint64_t latchTimeUs = 0;
        uint64_t latchPresentRefreshSequence = 0;
        uint64_t latchRefreshSequence = 0;
        bool targetSchedulerDelayValid = false;
        bool targetDeadlineAlreadyElapsed = false;
        bool hadPriorSubmission = false;
        bool usedPresenterSubmissionTime = false;
        bool spacingCorrected = false;
        bool submissionIdValid = false;
        bool latchSampleValid = false;
    };

    // Diagnostics must not perturb the measurement. The pacing thread only
    // copies a row into a bounded queue; a separate writer thread owns all
    // formatting and file I/O, so a stdio flush can never stall a present.
    struct TraceRow {
        int frameNumber = 0;
        uint32_t rtpTimestamp = 0;
        bool timestampValid = false;
        uint64_t decodeCompleteUs = 0;
        FrameTraceContext input;
        VrrTimingDecision decision;
        VrrTimingDiagnostics diagnostics;
        VrrPresentFeedback feedback;
        FrameTelemetry telemetry;
        size_t completionQueueDepth = 0;
        TraceDisposition disposition = TraceDisposition::OutputDropped;
        bool decisionValid = false;
        bool dropped = false;
        uint64_t terminalTimeUs = 0;
    };

    enum class TraceFormat : uint8_t {
        Csv,
        ChunkedCompressed,
    };

    static int threadProc(void* context);
    static int traceThreadProc(void* context);

    int run();
    bool dequeueFrame(QueuedFrame& frame, bool& queueDiscontinuity);
    bool hasQueuedFrame();
    size_t queuedFrameCount();
    void discardQueuedFrames(bool countDrops,
                             TraceDisposition disposition);
    void consumeWindowStateNotifications();
    bool presentationSuspended() const;
    bool isStopping() const;
    uint64_t staleToleranceUs(const VrrTimingDecision& decision) const;
    void waitForSubmissionFloor(const VrrTimingDecision& decision,
                                FrameTelemetry& telemetry);
    void recordSubmission(const VrrTimingDecision& decision,
                          const VrrPresentFeedback& feedback,
                          uint64_t operationStartUs,
                          uint64_t operationEndUs,
                          FrameTelemetry& telemetry);
    void deferFrame(PacedFrame&& frame);
    void noteDrop();
    void writeTrace(const QueuedFrame& frame,
                    const VrrTimingDecision& decision,
                    const VrrPresentFeedback& feedback,
                    const FrameTelemetry& telemetry,
                    size_t queueDepth,
                    TraceDisposition disposition,
                    bool decisionValid = true);
    void openTraceIfRequested();
    void closeTrace();
    int traceRun();
    void writeTraceRow(const TraceRow& row);
    void flushTraceChunk();
    bool minimumTraceDurationCaptured() const;
    static const char* traceDispositionName(TraceDisposition disposition);
    const char* tearClassification(const TraceRow& row) const;

    IVrrFramePresenter* m_Presenter;
    PacerTelemetry* m_Telemetry;
    VrrSessionConfig m_Config;
    bool m_CanLatchPresentation = false;

    std::unique_ptr<VrrTimingController> m_TimingController;
    std::unique_ptr<VrrTargetWaiter> m_TargetWaiter;

    QMutex m_FrameQueueLock;
    QWaitCondition m_FrameQueueNotEmpty;
    std::deque<QueuedFrame> m_FrameQueue;
    PacedFrame m_DeferredFrame;
    SDL_Thread* m_WorkerThread = nullptr;
    std::atomic_bool m_Stopping { false };
    std::atomic_bool m_Suspended { false };
    // Capacity eviction marks a local discontinuity while retaining source
    // timestamps so the cadence model can advance across omitted frames.
    std::atomic_bool m_QueueDiscontinuity { false };
    std::atomic_uint32_t m_PendingWindowStateFlags { 0 };
    bool m_PresenterSuspended = false;
    bool m_RebaseOnNextFrame = false;
    bool m_DeepTraceEnabled = false;
    std::FILE* m_TraceFile = nullptr;
    SDL_Thread* m_TraceThread = nullptr;
    QMutex m_TraceLock;
    QWaitCondition m_TraceQueueNotEmpty;
    std::deque<TraceRow> m_TraceQueue;
    std::atomic_bool m_TraceStopping { false };
    std::atomic_bool m_TraceAcceptingRows { false };
    std::atomic_uint64_t m_TraceArrivalSequence { 0 };
    size_t m_TraceDroppedRows = 0;
    uint64_t m_TraceBytesWritten = 0;
    uint64_t m_TraceStartUs = 0;
    uint64_t m_TraceLatestArrivalUs = 0;
    bool m_TraceSizeCapped = false;
    TraceFormat m_TraceFormat = TraceFormat::Csv;
    QByteArray m_TraceChunk;
};
