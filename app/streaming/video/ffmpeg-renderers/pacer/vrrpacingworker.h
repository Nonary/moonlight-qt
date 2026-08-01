#pragma once

#include "../../decoder.h"
#include "../ivrrframepresenter.h"
#include "vrr/vrrtargetwaiter.h"
#include "vrr/vrrtypes.h"

#include <atomic>
#include <deque>
#include <memory>

#include <QMutex>
#include <QWaitCondition>

class VrrTimingController;
struct VrrTimingDecision;

// The complete VRR execution path lives in this one worker.  It owns a bounded
// queue and the renderer context from preparation through presentation;
// fixed-VSync and unpaced Pacer behavior never enter it.
class VrrPacingWorker {
public:
    VrrPacingWorker(IVrrFramePresenter* presenter,
                    const VrrSessionConfig& config,
                    PVIDEO_STATS videoStats);
    ~VrrPacingWorker();

    VrrPacingWorker(const VrrPacingWorker&) = delete;
    VrrPacingWorker& operator=(const VrrPacingWorker&) = delete;

    bool start();

    void submit(PacedFrame&& frame);

    // Calls from the UI/main thread only manipulate worker-owned state. All
    // backend notifications are delivered by the worker itself.
    void notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info);

private:
    struct Diagnostics;

    static int threadProc(void* context);

    int run();
    bool dequeueFrame(PacedFrame& frame,
                      uint64_t idleTransitionDeadlineUs,
                      bool& idleTransitionDue);
    bool hasQueuedFrame();
    void discardQueuedFrames(bool countDrops);
    uint32_t consumeWindowStateNotifications();
    bool presentationSuspended() const;
    bool isStopping() const;
    void waitForSubmissionFloor(const VrrTimingDecision& decision);
    void recordSubmission(const VrrPresentFeedback& feedback,
                          uint64_t operationStartUs,
                          uint64_t operationEndUs);
    void observeGridSample(const VrrPresentFeedback& feedback);
    bool gridSlotAtOrAfter(uint64_t timeUs,
                           uint64_t& slotTimeUs,
                           uint64_t& slotSeq);
    void resetGrid();

    IVrrFramePresenter* m_Presenter;
    PVIDEO_STATS m_VideoStats;

    std::unique_ptr<VrrTimingController> m_TimingController;
    std::unique_ptr<Diagnostics> m_Diagnostics;
    VrrTargetWaiter m_TargetWaiter;

    QMutex m_FrameQueueLock;
    QWaitCondition m_FrameQueueNotEmpty;
    std::deque<PacedFrame> m_FrameQueue;
    // Keeps the most recently presented frame alive until the next result so a
    // decoder-owned surface cannot be recycled while the GPU still reads it.
    PacedFrame m_DeferredFrame;
    SDL_Thread* m_WorkerThread = nullptr;
    std::atomic_bool m_Stopping { false };
    std::atomic_bool m_Suspended { false };
    std::atomic_uint32_t m_PendingWindowStateFlags { 0 };
    bool m_PresenterSuspended = false;
    bool m_RebaseOnNextFrame = false;
    bool m_BfiBrightFrameHeld = false;
    uint64_t m_LastBfiBrightSubmissionUs = 0;
    bool m_BfiIdleBlackPending = false;
    uint64_t m_BfiIdleBlackSubmissionUs = 0;

    // Display-refresh grid model learned from exact DXGI latch feedback.
    // Grid-locked BFI snaps pair submissions into refresh slots so
    // source/display clock drift is absorbed as explicit pair repeats
    // instead of uncontrolled single-refresh black or bright repeats.
    bool m_BfiGridValid = false;
    uint64_t m_BfiGridAnchorUs = 0;
    uint64_t m_BfiGridAnchorSeq = 0;
    uint64_t m_BfiGridPeriodQ16 = 0;
    uint64_t m_BfiGridLastSampleUs = 0;
    bool m_BfiHaveLastVideoSlot = false;
    uint64_t m_BfiLastVideoSlotSeq = 0;

    // Grid-locked scheduling assumes a fixed refresh grid, which is wrong on
    // a display whose VRR genuinely follows our presents. Opt-in only.
    bool m_BfiGridLockEnabled = false;

    // A pair costs two refreshes of the panel's true minimum period. When
    // that exceeds the source period (stream rate * 2 above the real VRR
    // ceiling), the overdraft accumulates here and is repaid by presenting
    // one frame without its black transition at normal luminance.
    uint64_t m_BfiCeilingDebtQ16 = 0;

    // The same adaptive-refresh headroom rule the plain VRR path uses:
    // total presents per second stop a few Hz below the nominal refresh
    // (116/s on a 120 Hz panel), never scale up to the nominal ceiling.
    uint64_t m_BfiSafePresentPeriodQ16 = 0;
};
