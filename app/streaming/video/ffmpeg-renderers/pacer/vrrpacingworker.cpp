#include "vrrpacingworker.h"

#include "vrr/vrrtimingcontroller.h"

#include <Limelight.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace {

// Keep enough decoded successors to absorb the short gap-then-burst delivery
// pattern seen near the panel ceiling. Capacity remains bounded and evicts the
// oldest queued successor under sustained pressure, so it cannot accumulate
// an unbounded latency backlog.
constexpr size_t kMaximumQueuedFrames = 3;

// After an interrupted BFI pair, keep a run of complete source frames at
// normal luminance before resuming the black/boosted cadence. Three frames
// proved short enough that marginal-fps play toggled regimes more than once
// per second, which itself reads as brightness flicker; 12 frames (200 ms at
// 60 FPS) bounds the toggle rate while the 50%-duty pair stretch keeps the
// average luminance of both regimes identical across the transition.
constexpr unsigned int kBfiRecoveryFrameCount = 12;

constexpr uint64_t kBfiDiagnosticSummaryIntervalUs = 30ULL * 1000 * 1000;
constexpr uint64_t kBfiDiagnosticWarningCooldownUs = 60ULL * 1000 * 1000;

// A frame that arrives too late to reach its black transition on time is a
// slowdown signal: present it promptly at normal luminance instead of
// discarding it for a recycled dim image. The tolerance absorbs scheduler
// jitter without letting a genuinely late pair start.
constexpr uint64_t kBfiMissedBlackDeadlineMinimumUs = 1000;
constexpr uint64_t kBfiMissedBlackDeadlineLeadDivisor = 8;

// Boosted pairs assume the fixed black interval is half the real source
// period. While the learned cadence runs materially slower than the
// negotiated rate, that duty-cycle assumption is wrong and the 600-nit image
// would be held too long, so stay on normal-luminance recovery frames.
constexpr uint64_t kBfiSlowCadenceToleranceDivisor = 8;

// Grid-locked BFI. Submissions land this far ahead of their predicted
// refresh slot; the grid is considered stale without an exact latch sample
// for this long; the learned panel period uses a 1/8 EWMA and rejects
// samples further than ~2% from the nominal display period.
constexpr uint64_t kBfiGridSubmitLeadUs = 2500;
constexpr uint64_t kBfiGridStaleUs = 1000000;
constexpr unsigned int kBfiGridEwmaShift = 3;
constexpr uint64_t kBfiGridSanityDivisor = 50;

// Adaptive-refresh headroom below the nominal ceiling, matching the plain
// VRR practice of streaming at 116 FPS on a 120 Hz panel.
constexpr int kBfiAdaptiveHeadroomHz = 4;

constexpr uint32_t kVrrWindowStateMask =
    WINDOW_STATE_CHANGE_SIZE |
    WINDOW_STATE_CHANGE_DISPLAY |
    WINDOW_STATE_CHANGE_MINIMIZED |
    WINDOW_STATE_CHANGE_RESTORED;
constexpr uint32_t kVrrDisplayEpochStateMask =
    WINDOW_STATE_CHANGE_SIZE |
    WINDOW_STATE_CHANGE_DISPLAY;

uint64_t submissionBoundaryUs(const VrrPresentFeedback& feedback,
                              uint64_t operationStartUs,
                              uint64_t operationEndUs)
{
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
        return feedback.submissionTimeUs;
    }

    // Presenters without native timing are required to be thin. Anchoring at
    // entry prevents an unrelated blocking return from adding a display period
    // to every subsequent frame.
    return operationStartUs;
}

uint64_t saturatingAdd(uint64_t left, uint64_t right)
{
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return left > maximum - right ? maximum : left + right;
}

enum class BfiDiagnosticWarning : size_t {
    QueueEviction,
    SourceGapRecovery,
    IdleRecovery,
    RecoveryComplete,
    PrePresentFailure,
    PairTiming,
    VideoPresentFailure,
    StaleFrame,
    DecodeLatency,
    CancelAfterBlack,
    SpacingCorrection,
    MissedBlackDeadline,
    CadenceHold,
    PanelRepeat,
    GridPairRepeat,
    CeilingGovernor,
    Count,
};

enum class BfiPresentPhase : uint8_t {
    Black,
    Video,
    Recovery,
    CancelRestore,
};

struct TimingSamples {
    static constexpr std::array<uint64_t, 12> kUpperBoundsUs {
        250, 500, 1000, 2000, 4000, 8000,
        16000, 32000, 64000, 128000, 256000,
        std::numeric_limits<uint64_t>::max(),
    };

    void add(uint64_t valueUs)
    {
        count++;
        totalUs += valueUs;
        maximumUs = std::max(maximumUs, valueUs);
        for (size_t i = 0; i < kUpperBoundsUs.size(); i++) {
            if (valueUs <= kUpperBoundsUs[i]) {
                buckets[i]++;
                break;
            }
        }
    }

    uint64_t meanUs() const
    {
        return count == 0 ? 0 : totalUs / count;
    }

    uint64_t p95UpperBoundUs() const
    {
        if (count == 0) {
            return 0;
        }

        const uint64_t target = (count * 95 + 99) / 100;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < buckets.size(); i++) {
            cumulative += buckets[i];
            if (cumulative >= target) {
                return kUpperBoundsUs[i] ==
                        std::numeric_limits<uint64_t>::max() ?
                    maximumUs : kUpperBoundsUs[i];
            }
        }
        return maximumUs;
    }

    uint64_t count = 0;
    uint64_t totalUs = 0;
    uint64_t maximumUs = 0;
    std::array<uint64_t, kUpperBoundsUs.size()> buckets {};
};

struct BfiIntervalDiagnostics {
    uint64_t frames = 0;
    uint64_t pairAttempts = 0;
    uint64_t pairCompleted = 0;
    uint64_t prePresentFailures = 0;
    uint64_t videoPresentFailures = 0;
    uint64_t sourceGaps = 0;
    uint64_t idleRecoveryAttempts = 0;
    uint64_t idleRecoverySuccesses = 0;
    uint64_t idleRecoveryFailures = 0;
    uint64_t recoveryFrames = 0;
    uint64_t recoveryCompletions = 0;
    uint64_t queueEvictions = 0;
    uint64_t queueDiscards = 0;
    uint64_t queueHighWater = 0;
    uint64_t staleQueueDrops = 0;
    uint64_t staleRenderDrops = 0;
    uint64_t cancellationsAfterBlack = 0;
    uint64_t spacingCorrections = 0;
    uint64_t latencyAnomalies = 0;
    uint64_t pairTimingAnomalies = 0;
    uint64_t latchedFrames = 0;
    uint64_t missedBlackDeadlines = 0;
    uint64_t cadenceHoldFrames = 0;
    uint64_t gridLockedFrames = 0;
    uint64_t gridPairRepeats = 0;
    uint64_t ceilingGovernorSkips = 0;
    uint64_t blackPresents = 0;
    uint64_t blackTearingPresents = 0;
    uint64_t videoPresents = 0;
    uint64_t videoTearingPresents = 0;
    uint64_t recoveryPresents = 0;
    uint64_t recoveryTearingPresents = 0;
    uint64_t cancelRestorePresents = 0;
    uint64_t cancelRestoreTearingPresents = 0;
    uint64_t dxgiLatchSamples = 0;
    uint64_t dxgiCorrelatedLatches = 0;
    uint64_t dxgiBlackLatches = 0;
    uint64_t dxgiVideoLatches = 0;
    uint64_t dxgiRecoveryLatches = 0;
    uint64_t dxgiCancelRestoreLatches = 0;
    uint64_t dxgiExactLatchTimes = 0;
    uint64_t dxgiCorrelationMisses = 0;
    uint64_t dxgiExcessRefreshes = 0;
    uint64_t dxgiRepeatEvents = 0;
    uint64_t dxgiBlackRepeatRefreshes = 0;
    uint64_t dxgiVideoRepeatRefreshes = 0;
    uint64_t dxgiOtherRepeatRefreshes = 0;
    uint64_t dxgiSequenceAnomalies = 0;

    TimingSamples decodeToSubmission;
    TimingSamples pairGap;
    TimingSamples brightGap;
    TimingSamples blackTargetLateness;
    TimingSamples preparation;
    TimingSamples presentCall;
    TimingSamples renderSchedulerDelay;
    TimingSamples targetSchedulerDelay;
    TimingSamples spacingDeficit;
    TimingSamples idleRecoveryLateness;
    TimingSamples exactLatchDelay;
};

} // namespace

struct VrrPacingWorker::Diagnostics {
    explicit Diagnostics(const VrrSessionConfig& config) :
        displayRefreshHz(config.displayRefreshHz),
        streamRateHz(config.streamRateHz)
    {
    }

    void enable(uint64_t nowUs, uint64_t expectedPairGapUs)
    {
        if (enabled) {
            return;
        }

        enabled = true;
        intervalStartUs = nowUs;
        lastSummaryUs = nowUs;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR-BFI diagnostics enabled: display=%dHz stream=%dHz "
                    "pair_target=%lluus summary=30s warning_cooldown=60s",
                    displayRefreshHz, streamRateHz,
                    static_cast<unsigned long long>(expectedPairGapUs));
    }

    void noteQueueDepth(size_t depth)
    {
        uint64_t previous = pendingQueueHighWater.load();
        while (depth > previous &&
               !pendingQueueHighWater.compare_exchange_weak(previous, depth)) {
        }
    }

    void noteQueueEviction()
    {
        pendingQueueEvictions.fetch_add(1);
    }

    void noteQueueDiscard(uint64_t count)
    {
        pendingQueueDiscards.fetch_add(count);
    }

    uint64_t collectQueueEvictions()
    {
        const uint64_t totalQueueEvictions =
            pendingQueueEvictions.load();
        const uint64_t queueEvictions =
            totalQueueEvictions - collectedQueueEvictions;
        collectedQueueEvictions = totalQueueEvictions;
        interval.queueEvictions += queueEvictions;
        return queueEvictions;
    }

    void collectProducerCounters()
    {
        collectQueueEvictions();
        const uint64_t totalQueueDiscards =
            pendingQueueDiscards.load();
        interval.queueDiscards +=
            totalQueueDiscards - collectedQueueDiscards;
        collectedQueueDiscards = totalQueueDiscards;
        interval.queueHighWater = std::max(
            interval.queueHighWater,
            pendingQueueHighWater.load());
    }

    uint64_t currentQueueHighWater() const
    {
        return std::max(interval.queueHighWater,
                        pendingQueueHighWater.load());
    }

    void observePresent(BfiPresentPhase phase,
                        const VrrPresentFeedback& feedback,
                        uint64_t submissionUs)
    {
        if (feedback.presented && feedback.presentModeValid) {
            uint64_t* total = nullptr;
            uint64_t* tearing = nullptr;
            switch (phase) {
            case BfiPresentPhase::Black:
                total = &interval.blackPresents;
                tearing = &interval.blackTearingPresents;
                break;
            case BfiPresentPhase::Video:
                total = &interval.videoPresents;
                tearing = &interval.videoTearingPresents;
                break;
            case BfiPresentPhase::Recovery:
                total = &interval.recoveryPresents;
                tearing = &interval.recoveryTearingPresents;
                break;
            case BfiPresentPhase::CancelRestore:
                total = &interval.cancelRestorePresents;
                tearing = &interval.cancelRestoreTearingPresents;
                break;
            }
            (*total)++;
            if (feedback.tearingAllowed) {
                (*tearing)++;
            }
        }

        if (feedback.presented && feedback.submissionIdValid) {
            if (!pendingDisplaySubmissions.empty() &&
                    feedback.submissionId <
                        pendingDisplaySubmissions.back().submissionId) {
                pendingDisplaySubmissions.clear();
                haveLastLatchSample = false;
                haveLastLatchPhase = false;
                interval.dxgiSequenceAnomalies++;
            }
            if (pendingDisplaySubmissions.empty() ||
                    feedback.submissionId >
                        pendingDisplaySubmissions.back().submissionId) {
                pendingDisplaySubmissions.push_back(
                    {feedback.submissionId, phase, submissionUs});
            }
            while (pendingDisplaySubmissions.size() >
                    kMaximumPendingDisplaySubmissions) {
                pendingDisplaySubmissions.pop_front();
            }
        }

        if (!feedback.latchSampleValid) {
            return;
        }

        interval.dxgiLatchSamples++;
        if (haveLastLatchSample &&
                feedback.latchSubmissionId == lastLatchSubmissionId &&
                feedback.latchPresentRefreshSequence ==
                    lastLatchPresentRefreshSequence) {
            return;
        }

        bool repeatedSameSubmission = false;
        if (haveLastLatchSample) {
            if (feedback.latchSubmissionId < lastLatchSubmissionId ||
                    feedback.latchPresentRefreshSequence <
                        lastLatchPresentRefreshSequence) {
                interval.dxgiSequenceAnomalies++;
                pendingDisplaySubmissions.clear();
                haveLastLatchSample = false;
                haveLastLatchPhase = false;
            }
            else {
                const uint64_t submissionDelta =
                    feedback.latchSubmissionId - lastLatchSubmissionId;
                const uint64_t refreshDelta =
                    feedback.latchPresentRefreshSequence -
                        lastLatchPresentRefreshSequence;
                if (refreshDelta > submissionDelta) {
                    const uint64_t excess = refreshDelta - submissionDelta;
                    interval.dxgiRepeatEvents++;
                    interval.dxgiExcessRefreshes += excess;

                    // The content shown during the excess refreshes is
                    // whatever latched last. Attributing repeats to a phase
                    // distinguishes a repeated black (a 2-refresh full-field
                    // black event) from a repeated boosted frame (a bright
                    // pulse) when correlating visible artifacts.
                    if (haveLastLatchPhase) {
                        switch (lastLatchPhase) {
                        case BfiPresentPhase::Black:
                            interval.dxgiBlackRepeatRefreshes += excess;
                            break;
                        case BfiPresentPhase::Video:
                            interval.dxgiVideoRepeatRefreshes += excess;
                            break;
                        default:
                            interval.dxgiOtherRepeatRefreshes += excess;
                            break;
                        }
                        uint64_t suppressed = 0;
                        if (shouldLog(BfiDiagnosticWarning::PanelRepeat,
                                      submissionUs, suppressed)) {
                            SDL_LogWarn(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "VRR-BFI panel-repeat: phase=%s "
                                "excess_refreshes=%llu suppressed=%llu",
                                lastLatchPhase == BfiPresentPhase::Black ?
                                    "black" :
                                    lastLatchPhase ==
                                            BfiPresentPhase::Video ?
                                        "video" : "recovery",
                                static_cast<unsigned long long>(excess),
                                static_cast<unsigned long long>(
                                    suppressed));
                        }
                    }
                }
                repeatedSameSubmission = submissionDelta == 0;
            }
        }

        if (repeatedSameSubmission) {
            lastLatchPresentRefreshSequence =
                feedback.latchPresentRefreshSequence;
            return;
        }

        auto matched = std::find_if(
            pendingDisplaySubmissions.begin(),
            pendingDisplaySubmissions.end(),
            [&feedback](const PendingDisplaySubmission& submission) {
                return submission.submissionId ==
                    feedback.latchSubmissionId;
            });
        if (matched != pendingDisplaySubmissions.end()) {
            lastLatchPhase = matched->phase;
            haveLastLatchPhase = true;
            interval.dxgiCorrelatedLatches++;
            switch (matched->phase) {
            case BfiPresentPhase::Black:
                interval.dxgiBlackLatches++;
                break;
            case BfiPresentPhase::Video:
                interval.dxgiVideoLatches++;
                break;
            case BfiPresentPhase::Recovery:
                interval.dxgiRecoveryLatches++;
                break;
            case BfiPresentPhase::CancelRestore:
                interval.dxgiCancelRestoreLatches++;
                break;
            }

            if (feedback.latchTimeValid &&
                    feedback.latchPresentRefreshSequence ==
                        feedback.latchRefreshSequence &&
                    feedback.latchTimeUs >= matched->submissionUs) {
                interval.dxgiExactLatchTimes++;
                interval.exactLatchDelay.add(
                    feedback.latchTimeUs - matched->submissionUs);
            }
        }
        else {
            interval.dxgiCorrelationMisses++;
        }

        while (!pendingDisplaySubmissions.empty() &&
                pendingDisplaySubmissions.front().submissionId <=
                    feedback.latchSubmissionId) {
            pendingDisplaySubmissions.pop_front();
        }
        lastLatchSubmissionId = feedback.latchSubmissionId;
        lastLatchPresentRefreshSequence =
            feedback.latchPresentRefreshSequence;
        haveLastLatchSample = true;
    }

    void resetDisplayEpoch()
    {
        pendingDisplaySubmissions.clear();
        haveLastLatchSample = false;
        haveLastLatchPhase = false;
        lastLatchSubmissionId = 0;
        lastLatchPresentRefreshSequence = 0;
    }

    bool shouldLog(BfiDiagnosticWarning warning,
                   uint64_t nowUs,
                   uint64_t& suppressed)
    {
        WarningState& state =
            warnings[static_cast<size_t>(warning)];
        if (state.lastLogUs != 0 &&
                nowUs - state.lastLogUs <
                    kBfiDiagnosticWarningCooldownUs) {
            state.suppressed++;
            return false;
        }

        suppressed = state.suppressed;
        state.suppressed = 0;
        state.lastLogUs = nowUs;
        return true;
    }

    void maybeLogSummary(uint64_t nowUs)
    {
        if (enabled &&
                nowUs - lastSummaryUs >=
                    kBfiDiagnosticSummaryIntervalUs) {
            logSummary(nowUs, false);
        }
    }

    void logSummary(uint64_t nowUs, bool final)
    {
        if (!enabled) {
            return;
        }

        collectProducerCounters();
        const uint64_t windowMs =
            nowUs >= intervalStartUs ?
                (nowUs - intervalStartUs) / 1000 : 0;
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "VRR-BFI %s: window=%llums frames=%llu pairs=%llu/%llu "
            "recovery={gaps:%llu idle:%llu/%llu fail:%llu frames:%llu "
            "complete:%llu} drops={queue:%llu discard:%llu stale_queue:%llu "
            "stale_render:%llu} queue_max=%llu failures={pre:%llu video:%llu "
            "cancel_black:%llu} anomalies={latency:%llu pair:%llu spacing:%llu} "
            "miss={deadline:%llu cadence_hold:%llu} "
            "grid={locked:%llu repeats:%llu governor:%llu} "
            "timing_us={decode:%llu/%llu/%llu pair:%llu/%llu/%llu "
            "bright:%llu/%llu/%llu black_late:%llu/%llu/%llu "
            "prepare_p95/max:%llu/%llu present_p95/max:%llu/%llu "
            "wake_render_p95:%llu wake_target_p95:%llu spacing_max:%llu "
            "idle_late_max:%llu} latched=%llu "
            "mode={black_tearing:%llu/%llu video_tearing:%llu/%llu "
            "recovery_tearing:%llu/%llu restore_tearing:%llu/%llu} "
            "dxgi={samples:%llu correlated:%llu miss:%llu "
            "phase:%llu/%llu/%llu/%llu exact:%llu "
            "excess_refresh:%llu repeat_events:%llu "
            "repeat_phase:%llu/%llu/%llu seq_error:%llu "
            "latch_us:%llu/%llu/%llu}",
            final ? "final" : "summary",
            static_cast<unsigned long long>(windowMs),
            static_cast<unsigned long long>(interval.frames),
            static_cast<unsigned long long>(interval.pairCompleted),
            static_cast<unsigned long long>(interval.pairAttempts),
            static_cast<unsigned long long>(interval.sourceGaps),
            static_cast<unsigned long long>(interval.idleRecoverySuccesses),
            static_cast<unsigned long long>(interval.idleRecoveryAttempts),
            static_cast<unsigned long long>(interval.idleRecoveryFailures),
            static_cast<unsigned long long>(interval.recoveryFrames),
            static_cast<unsigned long long>(interval.recoveryCompletions),
            static_cast<unsigned long long>(interval.queueEvictions),
            static_cast<unsigned long long>(interval.queueDiscards),
            static_cast<unsigned long long>(interval.staleQueueDrops),
            static_cast<unsigned long long>(interval.staleRenderDrops),
            static_cast<unsigned long long>(interval.queueHighWater),
            static_cast<unsigned long long>(interval.prePresentFailures),
            static_cast<unsigned long long>(interval.videoPresentFailures),
            static_cast<unsigned long long>(interval.cancellationsAfterBlack),
            static_cast<unsigned long long>(interval.latencyAnomalies),
            static_cast<unsigned long long>(interval.pairTimingAnomalies),
            static_cast<unsigned long long>(interval.spacingCorrections),
            static_cast<unsigned long long>(interval.missedBlackDeadlines),
            static_cast<unsigned long long>(
                interval.cadenceHoldFrames),
            static_cast<unsigned long long>(
                interval.gridLockedFrames),
            static_cast<unsigned long long>(
                interval.gridPairRepeats),
            static_cast<unsigned long long>(
                interval.ceilingGovernorSkips),
            static_cast<unsigned long long>(
                interval.decodeToSubmission.meanUs()),
            static_cast<unsigned long long>(
                interval.decodeToSubmission.p95UpperBoundUs()),
            static_cast<unsigned long long>(
                interval.decodeToSubmission.maximumUs),
            static_cast<unsigned long long>(interval.pairGap.meanUs()),
            static_cast<unsigned long long>(
                interval.pairGap.p95UpperBoundUs()),
            static_cast<unsigned long long>(interval.pairGap.maximumUs),
            static_cast<unsigned long long>(interval.brightGap.meanUs()),
            static_cast<unsigned long long>(
                interval.brightGap.p95UpperBoundUs()),
            static_cast<unsigned long long>(
                interval.brightGap.maximumUs),
            static_cast<unsigned long long>(
                interval.blackTargetLateness.meanUs()),
            static_cast<unsigned long long>(
                interval.blackTargetLateness.p95UpperBoundUs()),
            static_cast<unsigned long long>(
                interval.blackTargetLateness.maximumUs),
            static_cast<unsigned long long>(
                interval.preparation.p95UpperBoundUs()),
            static_cast<unsigned long long>(interval.preparation.maximumUs),
            static_cast<unsigned long long>(
                interval.presentCall.p95UpperBoundUs()),
            static_cast<unsigned long long>(interval.presentCall.maximumUs),
            static_cast<unsigned long long>(
                interval.renderSchedulerDelay.p95UpperBoundUs()),
            static_cast<unsigned long long>(
                interval.targetSchedulerDelay.p95UpperBoundUs()),
            static_cast<unsigned long long>(
                interval.spacingDeficit.maximumUs),
            static_cast<unsigned long long>(
                interval.idleRecoveryLateness.maximumUs),
            static_cast<unsigned long long>(interval.latchedFrames),
            static_cast<unsigned long long>(
                interval.blackTearingPresents),
            static_cast<unsigned long long>(interval.blackPresents),
            static_cast<unsigned long long>(
                interval.videoTearingPresents),
            static_cast<unsigned long long>(interval.videoPresents),
            static_cast<unsigned long long>(
                interval.recoveryTearingPresents),
            static_cast<unsigned long long>(interval.recoveryPresents),
            static_cast<unsigned long long>(
                interval.cancelRestoreTearingPresents),
            static_cast<unsigned long long>(
                interval.cancelRestorePresents),
            static_cast<unsigned long long>(interval.dxgiLatchSamples),
            static_cast<unsigned long long>(
                interval.dxgiCorrelatedLatches),
            static_cast<unsigned long long>(
                interval.dxgiCorrelationMisses),
            static_cast<unsigned long long>(interval.dxgiBlackLatches),
            static_cast<unsigned long long>(interval.dxgiVideoLatches),
            static_cast<unsigned long long>(
                interval.dxgiRecoveryLatches),
            static_cast<unsigned long long>(
                interval.dxgiCancelRestoreLatches),
            static_cast<unsigned long long>(
                interval.dxgiExactLatchTimes),
            static_cast<unsigned long long>(
                interval.dxgiExcessRefreshes),
            static_cast<unsigned long long>(
                interval.dxgiRepeatEvents),
            static_cast<unsigned long long>(
                interval.dxgiBlackRepeatRefreshes),
            static_cast<unsigned long long>(
                interval.dxgiVideoRepeatRefreshes),
            static_cast<unsigned long long>(
                interval.dxgiOtherRepeatRefreshes),
            static_cast<unsigned long long>(
                interval.dxgiSequenceAnomalies),
            static_cast<unsigned long long>(
                interval.exactLatchDelay.meanUs()),
            static_cast<unsigned long long>(
                interval.exactLatchDelay.p95UpperBoundUs()),
            static_cast<unsigned long long>(
                interval.exactLatchDelay.maximumUs));

        interval = {};
        intervalStartUs = nowUs;
        lastSummaryUs = nowUs;
    }

    struct WarningState {
        uint64_t lastLogUs = 0;
        uint64_t suppressed = 0;
    };

    struct PendingDisplaySubmission {
        uint64_t submissionId;
        BfiPresentPhase phase;
        uint64_t submissionUs;
    };

    static constexpr size_t kMaximumPendingDisplaySubmissions = 256;

    const int displayRefreshHz;
    const int streamRateHz;
    bool enabled = false;
    uint64_t intervalStartUs = 0;
    uint64_t lastSummaryUs = 0;
    BfiIntervalDiagnostics interval;
    std::array<WarningState,
               static_cast<size_t>(BfiDiagnosticWarning::Count)> warnings {};
    std::atomic_uint64_t pendingQueueEvictions { 0 };
    std::atomic_uint64_t pendingQueueDiscards { 0 };
    std::atomic_uint64_t pendingQueueHighWater { 0 };
    uint64_t collectedQueueEvictions = 0;
    uint64_t collectedQueueDiscards = 0;
    std::deque<PendingDisplaySubmission> pendingDisplaySubmissions;
    bool haveLastLatchSample = false;
    bool haveLastLatchPhase = false;
    BfiPresentPhase lastLatchPhase = BfiPresentPhase::Video;
    uint64_t lastLatchSubmissionId = 0;
    uint64_t lastLatchPresentRefreshSequence = 0;
};

VrrPacingWorker::VrrPacingWorker(IVrrFramePresenter* presenter,
                                 const VrrSessionConfig& config,
                                 PVIDEO_STATS videoStats) :
    m_Presenter(presenter),
    m_VideoStats(videoStats),
    m_TimingController(std::make_unique<VrrTimingController>(
        config, presenter != nullptr &&
                presenter->canLatchAdaptivePresent())),
    m_Diagnostics(std::make_unique<Diagnostics>(config))
{
    m_BfiGridLockEnabled =
        qEnvironmentVariableIntValue("MOONLIGHT_BFI_GRID_LOCK") != 0;
    if (m_BfiGridLockEnabled) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "BFI grid-locked scheduling enabled by "
                    "MOONLIGHT_BFI_GRID_LOCK: assumes a fixed refresh grid");
    }

    // Same headroom rule as the plain VRR path: total presents per second
    // stop a few Hz below the nominal refresh instead of scaling to the
    // ceiling. A stream at or below (refresh - headroom) / 2 FPS gives
    // every frame a full pair with the governor permanently silent.
    if (config.displayRefreshHz > kBfiAdaptiveHeadroomHz) {
        const uint64_t safeRateHz = static_cast<uint64_t>(
            config.displayRefreshHz - kBfiAdaptiveHeadroomHz);
        m_BfiSafePresentPeriodQ16 =
            ((1000000ULL << 16) + safeRateHz / 2) / safeRateHz;
    }
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
}

bool VrrPacingWorker::start()
{
    if (m_Presenter == nullptr ||
        m_Presenter->checkSupport() != VrrFallbackReason::NoFallback) {
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

    PacedFrame dropped;
    bool queued = false;
    bool queueEvicted = false;
    size_t queuedDepth = 0;
    {
        QMutexLocker lock(&m_FrameQueueLock);
        // Close the race where suspension can begin after the optimistic
        // check above but before this producer acquires the queue lock.
        if (m_Stopping.load() || m_Suspended.load()) {
            dropped = std::move(frame);
        }
        else {
            if (m_FrameQueue.size() >= kMaximumQueuedFrames) {
                dropped = std::move(m_FrameQueue.front());
                m_FrameQueue.pop_front();
                queueEvicted = true;
            }
            m_FrameQueue.emplace_back(std::move(frame));
            queued = true;
            queuedDepth = m_FrameQueue.size();
        }
    }

    if (queueEvicted) {
        m_Diagnostics->noteQueueEviction();
    }
    if (queued) {
        m_Diagnostics->noteQueueDepth(queuedDepth);
    }

    // The evicted frame is released here rather than under the queue lock, so
    // returning a decoder surface cannot stall the time-critical worker.
    if (dropped) {
        m_VideoStats->pacerDroppedFrames++;
    }
    if (queued) {
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

    if (flags & WINDOW_STATE_CHANGE_MINIMIZED) {
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

    // Abandons the prepared image and counts the frame as dropped. The worker
    // owns the target and display-spacing policy, so the timing controller is
    // always told how the abandoned submission ended.
    auto cancelPresentation = [this]() {
        const uint64_t startUs = LiGetMicroseconds();
        VrrPresentFeedback feedback = m_Presenter->cancelFrame();
        const uint64_t endUs = LiGetMicroseconds();
        feedback.cancelled = true;
        const uint64_t submissionUs =
            submissionBoundaryUs(feedback, startUs, endUs);
        recordSubmission(feedback, startUs, endUs);
        m_Diagnostics->observePresent(
            BfiPresentPhase::CancelRestore, feedback, submissionUs);
        m_BfiHaveLastVideoSlot = false;
        if (feedback.presented &&
                m_Presenter->prePresentLeadTimeUs() != 0) {
            // D3D restores the cached lit image when a pair is cancelled
            // after its black transition. Arm the same idle deadline used
            // after an ordinary boosted video present.
            m_BfiBrightFrameHeld = true;
            m_LastBfiBrightSubmissionUs = submissionUs;
        }
        m_VideoStats->pacerDroppedFrames++;
    };

    auto reportCancelAfterBlack = [this](const char* phase) {
        m_Diagnostics->interval.cancellationsAfterBlack++;
        uint64_t suppressed = 0;
        const uint64_t nowUs = LiGetMicroseconds();
        if (m_Diagnostics->shouldLog(
                BfiDiagnosticWarning::CancelAfterBlack,
                nowUs, suppressed)) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "VRR-BFI anomaly cancel-after-black: phase=%s "
                "bright_restore_expected=1 suppressed=%llu",
                phase,
                static_cast<unsigned long long>(suppressed));
        }
    };

    while (!isStopping()) {
        consumeWindowStateNotifications();

        const uint64_t nominalPrePresentLeadUs =
            m_Presenter->prePresentLeadTimeUs();
        const bool bfiDiagnosticsEnabled =
            nominalPrePresentLeadUs != 0;
        const uint64_t loopNowUs = LiGetMicroseconds();
        if (bfiDiagnosticsEnabled) {
            m_Diagnostics->enable(loopNowUs, nominalPrePresentLeadUs);
        }
        const uint64_t newQueueEvictions =
            m_Diagnostics->collectQueueEvictions();
        if (bfiDiagnosticsEnabled && newQueueEvictions != 0) {
            uint64_t suppressed = 0;
            if (m_Diagnostics->shouldLog(
                    BfiDiagnosticWarning::QueueEviction,
                    loopNowUs, suppressed)) {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "VRR-BFI anomaly queue-eviction: count=%llu "
                    "queue_max=%llu bright_held=%d recovery=%d "
                    "suppressed=%llu",
                    static_cast<unsigned long long>(newQueueEvictions),
                    static_cast<unsigned long long>(
                        m_Diagnostics->currentQueueHighWater()),
                    m_BfiBrightFrameHeld ? 1 : 0,
                    m_BfiRecoveryActive ? 1 : 0,
                    static_cast<unsigned long long>(suppressed));
            }
        }
        m_Diagnostics->maybeLogSummary(loopNowUs);

        uint64_t idleRecoveryDeadlineUs = 0;
        if (m_BfiBrightFrameHeld &&
                nominalPrePresentLeadUs != 0 &&
                m_TimingController->hasLastSubmission()) {
            // This is when the next pair's black transition would have
            // replaced the boosted image. Never submit sooner than one
            // physical display period on higher-refresh panels.
            idleRecoveryDeadlineUs = saturatingAdd(
                m_TimingController->lastSubmissionUs(),
                std::max(nominalPrePresentLeadUs,
                         m_TimingController->displayPeriodUs()));
        }

        PacedFrame frame;
        bool idleRecoveryDue = false;
        if (!dequeueFrame(frame, idleRecoveryDeadlineUs,
                          idleRecoveryDue)) {
            break;
        }

        consumeWindowStateNotifications();
        if (isStopping()) {
            if (frame) {
                m_VideoStats->pacerDroppedFrames++;
            }
            continue;
        }
        if (presentationSuspended()) {
            if (frame) {
                m_VideoStats->pacerDroppedFrames++;
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
        if (idleRecoveryDue) {
            VrrPresentRequest recoveryRequest;
            const uint64_t recoveryStartUs = LiGetMicroseconds();
            const uint64_t recoveryLatenessUs =
                recoveryStartUs > idleRecoveryDeadlineUs ?
                    recoveryStartUs - idleRecoveryDeadlineUs : 0;
            const VrrPresentFeedback recoveryFeedback =
                m_Presenter->presentIdleFrameRecovery(recoveryRequest);
            const uint64_t recoveryEndUs = LiGetMicroseconds();
            const uint64_t recoveryDurationUs =
                recoveryEndUs >= recoveryStartUs ?
                    recoveryEndUs - recoveryStartUs : 0;
            const uint64_t recoverySubmissionUs = submissionBoundaryUs(
                recoveryFeedback, recoveryStartUs, recoveryEndUs);
            m_Diagnostics->observePresent(
                BfiPresentPhase::Recovery,
                recoveryFeedback,
                recoverySubmissionUs);
            observeGridSample(recoveryFeedback);
            m_BfiHaveLastVideoSlot = false;
            const bool recoverySucceeded =
                recoveryFeedback.presented &&
                    !recoveryFeedback.cancelled &&
                    recoverySubmissionUs != 0;
            m_Diagnostics->interval.idleRecoveryAttempts++;
            m_Diagnostics->interval.idleRecoveryLateness.add(
                recoveryLatenessUs);
            m_Diagnostics->interval.presentCall.add(recoveryDurationUs);
            if (recoverySucceeded) {
                m_Diagnostics->interval.idleRecoverySuccesses++;
                m_TimingController->noteAuxiliarySubmission(
                    recoverySubmissionUs);
                if (!m_BfiRecoveryActive) {
                    m_BfiRecoveryStartUs = recoverySubmissionUs;
                }
                m_BfiRecoveryActive = true;
                m_BfiRecoveryFrames = 0;
            }
            else {
                m_Diagnostics->interval.idleRecoveryFailures++;
            }

            uint64_t suppressed = 0;
            if (m_Diagnostics->shouldLog(
                    BfiDiagnosticWarning::IdleRecovery,
                    recoveryEndUs, suppressed)) {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "VRR-BFI recovery idle-dim: success=%d presented=%d "
                    "cancelled=%d deadline_late=%lluus call=%lluus "
                    "pair_target=%lluus suppressed=%llu",
                    recoverySucceeded ? 1 : 0,
                    recoveryFeedback.presented ? 1 : 0,
                    recoveryFeedback.cancelled ? 1 : 0,
                    static_cast<unsigned long long>(recoveryLatenessUs),
                    static_cast<unsigned long long>(recoveryDurationUs),
                    static_cast<unsigned long long>(
                        nominalPrePresentLeadUs),
                    static_cast<unsigned long long>(suppressed));
            }

            // Do not repeatedly retry a failed auxiliary Present against the
            // same cached frame. The renderer queues a device reset on native
            // failure, while a successful recovery stays dim until a new
            // decoded image is available.
            m_BfiBrightFrameHeld = false;
            m_LastBfiBrightSubmissionUs = 0;
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

        const VrrTimingDecision decision = m_TimingController->schedule(
            frame, LiGetMicroseconds());
        const bool sourceGapDetected =
            decision.sourceFrameDelta > 1 &&
            nominalPrePresentLeadUs != 0;
        const bool brightHeldAtSourceGap = m_BfiBrightFrameHeld;
        const bool recoveryWasActive = m_BfiRecoveryActive;
        if (sourceGapDetected) {
            if (!m_BfiRecoveryActive) {
                m_BfiRecoveryStartUs = LiGetMicroseconds();
            }
            m_BfiRecoveryActive = true;
            m_BfiRecoveryFrames = 0;
            m_Diagnostics->interval.sourceGaps +=
                decision.sourceFrameDelta - 1;
        }
        const uint64_t scheduleNowUs = LiGetMicroseconds();
        const uint64_t scheduleAgeUs = scheduleNowUs >=
                frame.decodeCompleteUs() ?
            scheduleNowUs - frame.decodeCompleteUs() : 0;

        // A frame that can no longer reach its black transition on time is
        // itself the slowdown signal, not merely a skipped frame number.
        // Enter normal-luminance recovery and present this fresh frame
        // without a black transition, instead of the former bailout that
        // discarded it in favor of a recycled dim image and another
        // display-spacing wait.
        //
        // No predictive missed-black-deadline entry: every prediction tried
        // charged the preparation budget against the black deadline, but the
        // pair's floor chain rebuilds its standing pipeline offset within a
        // few frames of any tightening, so a transiently late black self-
        // heals with the 50% duty preserved. Predicting from a collapsed
        // offset (as after a recovery exit) misclassified healthy frames and
        // recycled the session through recovery indefinitely. Genuine
        // slowdown signals are owned by the cadence hold (sustained slow
        // period), source-gap detection (skipped frames), the idle dim
        // (stall with a boosted image held), and the ceiling governor
        // (present rate above the panel's adaptive headroom).

        // The black interval is fixed at half the negotiated frame period.
        // While the learned cadence runs materially slower, a boosted pair
        // would hold the 600-nit image well past a 50% duty cycle, so stay
        // on normal-luminance frames until the source returns to nominal
        // cadence; the recovery counter restarts every held frame.
        const uint64_t nominalSourcePeriodUs = nominalPrePresentLeadUs * 2;
        if (nominalPrePresentLeadUs != 0 &&
                decision.sourcePeriodUs >
                    saturatingAdd(nominalSourcePeriodUs,
                                  nominalSourcePeriodUs /
                                      kBfiSlowCadenceToleranceDivisor)) {
            if (!m_BfiRecoveryActive) {
                m_BfiRecoveryStartUs = scheduleNowUs;
                m_BfiRecoveryActive = true;
            }
            m_BfiRecoveryFrames = 0;
            m_Diagnostics->interval.cadenceHoldFrames++;
            uint64_t suppressed = 0;
            if (m_Diagnostics->shouldLog(
                    BfiDiagnosticWarning::CadenceHold,
                    scheduleNowUs, suppressed)) {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "VRR-BFI recovery cadence-hold: source_period=%lluus "
                    "nominal=%lluus boosted_pairs_suspended=1 "
                    "suppressed=%llu",
                    static_cast<unsigned long long>(
                        decision.sourcePeriodUs),
                    static_cast<unsigned long long>(nominalSourcePeriodUs),
                    static_cast<unsigned long long>(suppressed));
            }
        }

        // VRR ceiling governor. Two presents per source frame can exceed
        // the panel's true minimum refresh period (a nominal 120 Hz panel
        // may only sustain ~118 Hz), and the resulting queue pressure has
        // to discharge somewhere - visibly, as an uncontrolled repeat or a
        // walking beat. When the measured overdraft reaches one refresh,
        // repay it deliberately: present this one frame without its black
        // transition at normal luminance, which is luminance-neutral and
        // relieves a full refresh of pressure.
        const uint64_t presentPeriodFloorQ16 = std::max(
            m_BfiGridValid ? m_BfiGridPeriodQ16 : 0,
            m_BfiSafePresentPeriodQ16);
        bool governorSkip = false;
        if (nominalPrePresentLeadUs != 0 && !m_BfiRecoveryActive &&
                presentPeriodFloorQ16 != 0 &&
                m_BfiCeilingDebtQ16 >= presentPeriodFloorQ16) {
            governorSkip = true;
            m_BfiCeilingDebtQ16 -= presentPeriodFloorQ16;
            m_Diagnostics->interval.ceilingGovernorSkips++;
            uint64_t suppressed = 0;
            if (m_Diagnostics->shouldLog(
                    BfiDiagnosticWarning::CeilingGovernor,
                    scheduleNowUs, suppressed)) {
                SDL_LogInfo(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "VRR-BFI ceiling-governor: black skipped to repay "
                    "refresh overdraft, present_floor=%llu.%02lluus "
                    "source_period=%lluus stream_at_or_below_half_floor_"
                    "silences_this suppressed=%llu",
                    static_cast<unsigned long long>(
                        presentPeriodFloorQ16 >> 16),
                    static_cast<unsigned long long>(
                        ((presentPeriodFloorQ16 & 0xffff) * 100) >> 16),
                    static_cast<unsigned long long>(
                        decision.sourcePeriodUs),
                    static_cast<unsigned long long>(suppressed));
            }
        }

        const bool frameDropRecovery =
            (m_BfiRecoveryActive || governorSkip) &&
            nominalPrePresentLeadUs != 0;
        const uint64_t prePresentLeadUs =
            frameDropRecovery ? 0 : nominalPrePresentLeadUs;
        if (nominalPrePresentLeadUs != 0) {
            m_Diagnostics->interval.frames++;
            if (decision.latchedPresentation) {
                m_Diagnostics->interval.latchedFrames++;
            }
        }

        if (sourceGapDetected) {
            uint64_t suppressed = 0;
            if (m_Diagnostics->shouldLog(
                    BfiDiagnosticWarning::SourceGapRecovery,
                    scheduleNowUs, suppressed)) {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "VRR-BFI recovery source-gap: missing=%llu "
                    "bright_held=%d already_recovering=%d "
                    "decode_age=%lluus source_period=%lluus "
                    "queue_max=%llu suppressed=%llu",
                    static_cast<unsigned long long>(
                        decision.sourceFrameDelta - 1),
                    brightHeldAtSourceGap ? 1 : 0,
                    recoveryWasActive ? 1 : 0,
                    static_cast<unsigned long long>(scheduleAgeUs),
                    static_cast<unsigned long long>(
                        decision.sourcePeriodUs),
                    static_cast<unsigned long long>(
                        m_Diagnostics->currentQueueHighWater()),
                    static_cast<unsigned long long>(suppressed));
            }
        }
        // schedule() deliberately clamps an overdue target to the current
        // one-slot deadline. That is right for the newest frame, but it makes
        // the later target-relative stale check unable to see time already
        // spent waiting in this worker's queue. If a fresher successor exists,
        // skip a frame that is already more than one source interval old before
        // rendering it; its RTP/frame delta remains in the controller, so the
        // successor preserves cadence without re-anchoring the whole model.
        if (decision.sourcePeriodUs != 0 &&
            scheduleAgeUs > decision.sourcePeriodUs && hasQueuedFrame()) {
            if (bfiDiagnosticsEnabled) {
                m_Diagnostics->interval.staleQueueDrops++;
                uint64_t suppressed = 0;
                if (m_Diagnostics->shouldLog(
                        BfiDiagnosticWarning::StaleFrame,
                        scheduleNowUs, suppressed)) {
                    SDL_LogWarn(
                        SDL_LOG_CATEGORY_APPLICATION,
                        "VRR-BFI anomaly stale-frame: phase=queue "
                        "decode_age=%lluus source_period=%lluus "
                        "bright_held=%d recovery=%d suppressed=%llu",
                        static_cast<unsigned long long>(scheduleAgeUs),
                        static_cast<unsigned long long>(
                            decision.sourcePeriodUs),
                        m_BfiBrightFrameHeld ? 1 : 0,
                        frameDropRecovery ? 1 : 0,
                        static_cast<unsigned long long>(suppressed));
                }
            }
            m_VideoStats->pacerDroppedFrames++;
            m_TimingController->noteSubmission(false, false, 0);
            continue;
        }

        // --- Grid-locked pair planning ---------------------------------
        // At the panel ceiling the source and display clocks beat against
        // each other; free-running pair submissions drift through the
        // refresh grid and the panel absorbs the drift as uncontrolled
        // single-refresh repeats: a repeated black is a full-field black
        // event, a repeated video a bright pulse. Snap pair submissions
        // into predicted refresh slots and absorb the drift explicitly as
        // a luminance-neutral repeat of the previous black/video pair.
        // Planned before preparation so an inserted repeat can still use
        // the previous frame's cached image.
        bool gridLocked = false;
        bool insertPairRepeat = false;
        bool pairRepeatInterrupted = false;
        uint64_t plannedBlackSubmitUs = 0;
        uint64_t plannedVideoSubmitUs = 0;
        uint64_t plannedVideoSlotSeq = 0;
        uint64_t preparationBudgetUs =
            decision.targetUs > decision.renderStartUs ?
                decision.targetUs - decision.renderStartUs : 0;
        if (m_BfiGridLockEnabled && prePresentLeadUs != 0) {
            const uint64_t planNowUs = LiGetMicroseconds();
            const uint64_t minimumPreSubmitUs =
                m_TimingController->hasLastSubmission() ?
                    saturatingAdd(m_TimingController->lastSubmissionUs(),
                                  m_TimingController->displayPeriodUs()) : 0;
            const uint64_t feasibleSlotFloorUs = std::max(
                saturatingAdd(saturatingAdd(planNowUs, preparationBudgetUs),
                              kBfiGridSubmitLeadUs),
                saturatingAdd(minimumPreSubmitUs, kBfiGridSubmitLeadUs));
            uint64_t floorSlotUs = 0;
            uint64_t floorSlotSeq = 0;
            if (gridSlotAtOrAfter(feasibleSlotFloorUs,
                                  floorSlotUs, floorSlotSeq)) {
                const uint64_t requestedPreTargetUs =
                    decision.targetUs > nominalPrePresentLeadUs ?
                        decision.targetUs - nominalPrePresentLeadUs : 0;
                uint64_t naturalSlotUs = 0;
                uint64_t naturalSlotSeq = 0;
                gridSlotAtOrAfter(
                    saturatingAdd(requestedPreTargetUs,
                                  kBfiGridSubmitLeadUs),
                    naturalSlotUs, naturalSlotSeq);

                uint64_t blackSlotSeq;
                if (!m_BfiHaveLastVideoSlot) {
                    blackSlotSeq = std::max(floorSlotSeq, naturalSlotSeq);
                }
                else {
                    const uint64_t gapNatural =
                        naturalSlotSeq > m_BfiLastVideoSlotSeq ?
                            naturalSlotSeq - m_BfiLastVideoSlotSeq : 1;
                    if (gapNatural <= 1) {
                        blackSlotSeq = std::max(
                            floorSlotSeq, m_BfiLastVideoSlotSeq + 1);
                    }
                    else if (gapNatural == 2) {
                        if (floorSlotSeq <= m_BfiLastVideoSlotSeq + 1) {
                            // The frame is ready early enough to run one
                            // slot ahead of its source-natural position and
                            // keep every refresh filled.
                            blackSlotSeq = m_BfiLastVideoSlotSeq + 1;
                        }
                        else {
                            // Accumulated drift has consumed the pull-early
                            // margin. Fill the two open slots with a repeat
                            // of the previous pair, then place this frame.
                            insertPairRepeat = true;
                            blackSlotSeq = m_BfiLastVideoSlotSeq + 3;
                        }
                    }
                    else {
                        // A transition (recovery exit, stall) rather than
                        // steady drift; schedule naturally without filling.
                        blackSlotSeq = std::max(floorSlotSeq,
                                                naturalSlotSeq);
                    }
                }

                const auto slotTimeFor = [this](uint64_t seq) {
                    return seq <= m_BfiGridAnchorSeq ?
                        m_BfiGridAnchorUs :
                        m_BfiGridAnchorUs +
                            (((seq - m_BfiGridAnchorSeq) *
                              m_BfiGridPeriodQ16) >> 16);
                };
                const uint64_t blackSlotUs = slotTimeFor(blackSlotSeq);
                plannedBlackSubmitUs =
                    blackSlotUs > kBfiGridSubmitLeadUs ?
                        blackSlotUs - kBfiGridSubmitLeadUs : 0;
                const uint64_t videoSlotUs = slotTimeFor(blackSlotSeq + 1);
                plannedVideoSubmitUs =
                    videoSlotUs > kBfiGridSubmitLeadUs ?
                        videoSlotUs - kBfiGridSubmitLeadUs : 0;
                plannedVideoSlotSeq = blackSlotSeq + 1;
                gridLocked = true;
                m_Diagnostics->interval.gridLockedFrames++;
            }
        }

        if (gridLocked && insertPairRepeat) {
            const auto slotTimeFor = [this](uint64_t seq) {
                return seq <= m_BfiGridAnchorSeq ?
                    m_BfiGridAnchorUs :
                    m_BfiGridAnchorUs +
                        (((seq - m_BfiGridAnchorSeq) *
                          m_BfiGridPeriodQ16) >> 16);
            };
            const uint64_t repeatBlackSlotUs =
                slotTimeFor(m_BfiLastVideoSlotSeq + 1);
            const uint64_t repeatVideoSlotUs =
                slotTimeFor(m_BfiLastVideoSlotSeq + 2);
            const uint64_t repeatBlackSubmitUs =
                repeatBlackSlotUs > kBfiGridSubmitLeadUs ?
                    repeatBlackSlotUs - kBfiGridSubmitLeadUs : 0;
            const uint64_t repeatVideoSubmitUs =
                repeatVideoSlotUs > kBfiGridSubmitLeadUs ?
                    repeatVideoSlotUs - kBfiGridSubmitLeadUs : 0;

            m_TargetWaiter.waitUntil(repeatBlackSubmitUs);
            while (LiGetMicroseconds() < repeatBlackSubmitUs) {
                m_TargetWaiter.waitUntil(repeatBlackSubmitUs);
            }
            pairRepeatInterrupted =
                (consumeWindowStateNotifications() &
                    kVrrDisplayEpochStateMask) != 0;
            if (!pairRepeatInterrupted && !presentationSuspended() &&
                    !isStopping()) {
                VrrPresentRequest repeatRequest;
                const uint64_t rbStartUs = LiGetMicroseconds();
                const VrrPresentFeedback repeatBlack =
                    m_Presenter->presentPairRepeatBlack(repeatRequest);
                const uint64_t rbEndUs = LiGetMicroseconds();
                const uint64_t rbSubmissionUs = submissionBoundaryUs(
                    repeatBlack, rbStartUs, rbEndUs);
                m_Diagnostics->observePresent(
                    BfiPresentPhase::Black, repeatBlack, rbSubmissionUs);
                observeGridSample(repeatBlack);
                if (repeatBlack.presented && !repeatBlack.cancelled &&
                        rbSubmissionUs != 0) {
                    m_TimingController->noteAuxiliarySubmission(
                        rbSubmissionUs);
                    if (m_LastBfiBrightSubmissionUs != 0 &&
                            rbSubmissionUs >=
                                m_LastBfiBrightSubmissionUs) {
                        m_Diagnostics->interval.brightGap.add(
                            rbSubmissionUs -
                                m_LastBfiBrightSubmissionUs);
                    }
                    m_BfiBrightFrameHeld = false;
                    m_LastBfiBrightSubmissionUs = 0;

                    m_TargetWaiter.waitUntil(repeatVideoSubmitUs);
                    while (LiGetMicroseconds() < repeatVideoSubmitUs) {
                        m_TargetWaiter.waitUntil(repeatVideoSubmitUs);
                    }
                    const uint64_t rvStartUs = LiGetMicroseconds();
                    const VrrPresentFeedback repeatVideo =
                        m_Presenter->presentPairRepeatVideo(repeatRequest);
                    const uint64_t rvEndUs = LiGetMicroseconds();
                    const uint64_t rvSubmissionUs = submissionBoundaryUs(
                        repeatVideo, rvStartUs, rvEndUs);
                    m_Diagnostics->observePresent(
                        BfiPresentPhase::Video, repeatVideo,
                        rvSubmissionUs);
                    observeGridSample(repeatVideo);
                    if (repeatVideo.presented && !repeatVideo.cancelled &&
                            rvSubmissionUs != 0) {
                        m_TimingController->noteAuxiliarySubmission(
                            rvSubmissionUs);
                        m_BfiBrightFrameHeld = true;
                        m_LastBfiBrightSubmissionUs = rvSubmissionUs;
                        m_BfiLastVideoSlotSeq += 2;
                        m_Diagnostics->interval.gridPairRepeats++;
                        uint64_t suppressed = 0;
                        if (m_Diagnostics->shouldLog(
                                BfiDiagnosticWarning::GridPairRepeat,
                                rvEndUs, suppressed)) {
                            SDL_LogInfo(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "VRR-BFI grid pair-repeat: drift absorbed "
                                "as one repeated pair, grid_period=%llu.%02lluus "
                                "suppressed=%llu",
                                static_cast<unsigned long long>(
                                    m_BfiGridPeriodQ16 >> 16),
                                static_cast<unsigned long long>(
                                    ((m_BfiGridPeriodQ16 & 0xffff) * 100) >>
                                        16),
                                static_cast<unsigned long long>(
                                    suppressed));
                        }
                    }
                }
            }
        }

        // A BFI frame must be fully prepared for the black transition, not
        // merely for the later lit-video target. Starting at the ordinary
        // render deadline made preparation consume the pre-present interval,
        // so black and video both slipped by one half-cycle.
        const uint64_t renderStartUs = gridLocked ?
            (plannedBlackSubmitUs > preparationBudgetUs ?
                plannedBlackSubmitUs - preparationBudgetUs : 0) :
            (decision.renderStartUs > prePresentLeadUs ?
                decision.renderStartUs - prePresentLeadUs : 0);
        const VrrTargetWaitResult renderWait =
            m_TargetWaiter.waitUntil(renderStartUs);
        // A deadline that was already in the past is not an OS wake delay.
        // This matters when a decoded frame arrives after its projected render
        // start: readiness/render learning owns that lateness instead.
        const uint64_t renderWaitOvershootUs =
            renderWait.deadlineAlreadyElapsed ||
                    renderWait.finalNowUs <= renderStartUs ?
                0 : renderWait.finalNowUs - renderStartUs;
        if (bfiDiagnosticsEnabled) {
            m_Diagnostics->interval.renderSchedulerDelay.add(
                renderWaitOvershootUs);
        }

        bool displayEpochInterrupted =
            (consumeWindowStateNotifications() &
                kVrrDisplayEpochStateMask) != 0 || pairRepeatInterrupted;
        if (displayEpochInterrupted ||
                presentationSuspended() || isStopping()) {
            cancelPresentation();
            continue;
        }

        // A frame can become stale while the worker waits for its render
        // start. Leave the surface unprepared and let the next iteration start
        // fresh rather than rendering an avoidably old image.
        if (decision.sourcePeriodUs != 0 &&
            LiGetMicroseconds() > decision.targetUs + decision.sourcePeriodUs &&
            hasQueuedFrame()) {
            const uint64_t staleNowUs = LiGetMicroseconds();
            const uint64_t targetLatenessUs =
                staleNowUs > decision.targetUs ?
                    staleNowUs - decision.targetUs : 0;
            if (bfiDiagnosticsEnabled) {
                m_Diagnostics->interval.staleRenderDrops++;
                uint64_t suppressed = 0;
                if (m_Diagnostics->shouldLog(
                        BfiDiagnosticWarning::StaleFrame,
                        staleNowUs, suppressed)) {
                    SDL_LogWarn(
                        SDL_LOG_CATEGORY_APPLICATION,
                        "VRR-BFI anomaly stale-frame: phase=render-wait "
                        "target_late=%lluus source_period=%lluus "
                        "wake_late=%lluus recovery=%d suppressed=%llu",
                        static_cast<unsigned long long>(targetLatenessUs),
                        static_cast<unsigned long long>(
                            decision.sourcePeriodUs),
                        static_cast<unsigned long long>(
                            renderWaitOvershootUs),
                        frameDropRecovery ? 1 : 0,
                        static_cast<unsigned long long>(suppressed));
                }
            }
            m_VideoStats->pacerDroppedFrames++;
            m_TimingController->rebase();
            continue;
        }

        // Recovery occupies the existing video slot at half luminance and
        // omits its black pre-transition. It adds neither a Present nor a
        // full-screen correction pass to the queue.
        m_Presenter->setFrameDropRecovery(frameDropRecovery);
        const uint64_t preparationStartUs = LiGetMicroseconds();
        const VrrPrepareResult preparation =
            m_Presenter->prepareFrame(frame.frame());
        const uint64_t preparationEndUs = LiGetMicroseconds();
        const uint64_t preparationDurationUs =
            preparationEndUs >= preparationStartUs ?
                preparationEndUs - preparationStartUs : 0;
        if (bfiDiagnosticsEnabled) {
            m_Diagnostics->interval.preparation.add(
                preparationDurationUs);
        }
        m_TimingController->notePreparationDuration(preparationDurationUs);

        // Always drain, even when already interrupted: the drain is what
        // reconciles the presenter's suspension state and arms the rebase.
        displayEpochInterrupted =
            (consumeWindowStateNotifications() &
                kVrrDisplayEpochStateMask) != 0 || displayEpochInterrupted;
        if (!preparation.prepared || displayEpochInterrupted ||
                presentationSuspended() || isStopping()) {
            VrrPresentFeedback feedback = preparation.feedback;
            uint64_t operationStartUs = preparationStartUs;
            uint64_t operationEndUs = preparationEndUs;
            const bool mustCancel = !feedback.presented &&
                (preparation.prepared || preparation.cancellationMaySubmit ||
                 !feedback.cancelled);
            if (mustCancel) {
                // A presenter may need to submit an acquired image in order
                // to abandon it. It reports only that neutral fact; the worker
                // owns the target and display-spacing policy.
                if (preparation.cancellationMaySubmit) {
                    waitForSubmissionFloor(decision);
                }
                operationStartUs = LiGetMicroseconds();
                feedback = m_Presenter->cancelFrame();
                operationEndUs = LiGetMicroseconds();
            }
            feedback.cancelled = true;
            recordSubmission(feedback, operationStartUs, operationEndUs);
            m_VideoStats->pacerDroppedFrames++;
            m_DeferredFrame = std::move(frame);
            continue;
        }

        VrrPresentRequest presentRequest;
        presentRequest.latchedPresentation = decision.latchedPresentation;

        uint64_t videoTargetUs = gridLocked ?
            plannedVideoSubmitUs : decision.targetUs;
        uint64_t preSubmissionUs = 0;

        // A 50% duty cycle belongs to the actual source cadence, not the
        // negotiated rate: with the black interval fixed at the nominal lead,
        // a source running slightly slow stretches only the bright phase and
        // the average luminance climbs. Stretch the black-to-video interval
        // to half the learned period so black and bright stay equal. The
        // cadence hold bounds the learned period at 9/8 nominal for pair
        // frames, and the same bound is applied here defensively.
        const uint64_t pairIntervalUs = std::min(
            std::max(nominalPrePresentLeadUs, decision.sourcePeriodUs / 2),
            saturatingAdd(nominalPrePresentLeadUs,
                          nominalPrePresentLeadUs /
                              kBfiSlowCadenceToleranceDivisor));
        if (prePresentLeadUs != 0) {
            m_Diagnostics->interval.pairAttempts++;
            const uint64_t requestedPreTargetUs =
                decision.targetUs > prePresentLeadUs ?
                    decision.targetUs - prePresentLeadUs : 0;

            // The timing controller's normal submission floor includes an
            // adaptive safety guard. A 50/50 BFI pair at exactly twice the
            // stream rate has no room for that guard between the previous lit
            // frame and this black transition. Use the physical display-period
            // floor here and enforce it exactly below; the final lit present
            // remains protected by the full worker floor.
            const uint64_t minimumPreTargetUs =
                m_TimingController->hasLastSubmission() ?
                    saturatingAdd(
                        m_TimingController->lastSubmissionUs(),
                        m_TimingController->displayPeriodUs()) : 0;
            const uint64_t preTargetUs = gridLocked ?
                std::max(plannedBlackSubmitUs, minimumPreTargetUs) :
                std::max(requestedPreTargetUs, minimumPreTargetUs);
            m_TargetWaiter.waitUntil(
                preTargetUs, decision.targetWakeLeadUs);
            while (LiGetMicroseconds() < preTargetUs) {
                m_TargetWaiter.waitUntil(preTargetUs);
            }

            displayEpochInterrupted =
                (consumeWindowStateNotifications() &
                    kVrrDisplayEpochStateMask) != 0 ||
                displayEpochInterrupted;
            if (displayEpochInterrupted ||
                    presentationSuspended() || isStopping()) {
                cancelPresentation();
                m_DeferredFrame = std::move(frame);
                continue;
            }

            const uint64_t prePresentStartUs = LiGetMicroseconds();
            const uint64_t blackTargetLatenessUs =
                prePresentStartUs > preTargetUs ?
                    prePresentStartUs - preTargetUs : 0;
            m_Diagnostics->interval.blackTargetLateness.add(
                blackTargetLatenessUs);

            // Residual lateness here is prepare/wake overrun beyond the
            // learned budget; a genuinely late frame already entered
            // normal-luminance recovery at schedule time. Presenting the
            // pair slightly late preserves fresh content and the 50% duty
            // cycle, which beats the former bailout's discarded frame,
            // recycled dim image, and extra display-spacing wait.
            const VrrPresentFeedback prePresentFeedback =
                m_Presenter->presentPreFrame(presentRequest);
            const uint64_t prePresentEndUs = LiGetMicroseconds();
            const uint64_t prePresentDurationUs =
                prePresentEndUs >= prePresentStartUs ?
                    prePresentEndUs - prePresentStartUs : 0;
            m_Diagnostics->interval.presentCall.add(
                prePresentDurationUs);
            if (!prePresentFeedback.presented ||
                    prePresentFeedback.cancelled) {
                m_Diagnostics->interval.prePresentFailures++;
                uint64_t suppressed = 0;
                if (m_Diagnostics->shouldLog(
                        BfiDiagnosticWarning::PrePresentFailure,
                        prePresentEndUs, suppressed)) {
                    SDL_LogWarn(
                        SDL_LOG_CATEGORY_APPLICATION,
                        "VRR-BFI anomaly black-present-failed: "
                        "presented=%d cancelled=%d call=%lluus "
                        "latched=%d bright_held=%d suppressed=%llu",
                        prePresentFeedback.presented ? 1 : 0,
                        prePresentFeedback.cancelled ? 1 : 0,
                        static_cast<unsigned long long>(
                            prePresentDurationUs),
                        decision.latchedPresentation ? 1 : 0,
                        m_BfiBrightFrameHeld ? 1 : 0,
                        static_cast<unsigned long long>(suppressed));
                }
                cancelPresentation();
                m_DeferredFrame = std::move(frame);
                continue;
            }
            m_BfiBrightFrameHeld = false;

            preSubmissionUs = submissionBoundaryUs(
                prePresentFeedback,
                prePresentStartUs,
                prePresentEndUs);
            m_Diagnostics->observePresent(
                BfiPresentPhase::Black,
                prePresentFeedback,
                preSubmissionUs);
            observeGridSample(prePresentFeedback);
            if (m_LastBfiBrightSubmissionUs != 0 &&
                    preSubmissionUs >=
                        m_LastBfiBrightSubmissionUs) {
                m_Diagnostics->interval.brightGap.add(
                    preSubmissionUs -
                        m_LastBfiBrightSubmissionUs);
            }
            m_LastBfiBrightSubmissionUs = 0;
            // Grid-locked pairs take their spacing from the absolute slot
            // times; the source-cadence stretch below applies only to the
            // free-running path.
            if (!gridLocked && preSubmissionUs != 0 &&
                    preSubmissionUs <=
                        std::numeric_limits<uint64_t>::max() -
                            pairIntervalUs) {
                videoTargetUs = std::max(
                    videoTargetUs,
                    preSubmissionUs + pairIntervalUs);
            }
        }

        const VrrTargetWaitResult targetWait =
            m_TargetWaiter.waitUntil(videoTargetUs,
                                      decision.targetWakeLeadUs);
        if (bfiDiagnosticsEnabled &&
                targetWait.schedulerDelayValid) {
            m_Diagnostics->interval.targetSchedulerDelay.add(
                targetWait.schedulerDelayUs);
        }
        // Always drain, even when already interrupted: the drain is what
        // reconciles the presenter's suspension state and arms the rebase.
        displayEpochInterrupted =
            (consumeWindowStateNotifications() &
                kVrrDisplayEpochStateMask) != 0 || displayEpochInterrupted;
        if (displayEpochInterrupted ||
                presentationSuspended() || isStopping()) {
            if (preparation.cancellationMaySubmit) {
                waitForSubmissionFloor(decision);
            }
            if (preSubmissionUs != 0) {
                reportCancelAfterBlack("target-wait");
            }
            cancelPresentation();
            m_DeferredFrame = std::move(frame);
            continue;
        }

        // Recheck both mathematical floors immediately before Present. The
        // waiter deliberately has a bounded active phase, so a pathological
        // clock must not turn an early return into an early submission.
        // This frame's floor is fixed before the clean-spacing feedback, so a
        // guard decay can only take effect from the next frame onward.
        const uint64_t presentationFloorUs = std::max(
            videoTargetUs, m_TimingController->earliestSubmissionUs());
        m_TimingController->noteSpacingDeficit(0);
        while (LiGetMicroseconds() < presentationFloorUs) {
            m_TargetWaiter.waitUntil(presentationFloorUs);
        }

        uint64_t spacingDeficitUs = 0;
        if (m_TimingController->hasLastSubmission()) {
            const uint64_t minimumUntornUs =
                m_TimingController->lastSubmissionUs() +
                m_TimingController->displayPeriodUs();

            // A second check protects against a clock anomaly between the
            // first check and the actual call boundary.
            const uint64_t nowUs = LiGetMicroseconds();
            if (nowUs < minimumUntornUs) {
                spacingDeficitUs = minimumUntornUs - nowUs;
                m_TimingController->noteSpacingDeficit(spacingDeficitUs);
                if (bfiDiagnosticsEnabled) {
                    m_Diagnostics->interval.spacingCorrections++;
                    m_Diagnostics->interval.spacingDeficit.add(
                        spacingDeficitUs);
                    const uint64_t warningThresholdUs =
                        std::max<uint64_t>(
                            1000,
                            m_TimingController->displayPeriodUs() / 4);
                    if (spacingDeficitUs >= warningThresholdUs) {
                        uint64_t suppressed = 0;
                        if (m_Diagnostics->shouldLog(
                                BfiDiagnosticWarning::SpacingCorrection,
                                nowUs, suppressed)) {
                            SDL_LogWarn(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "VRR-BFI anomaly spacing-correction: "
                                "deficit=%lluus display_period=%lluus "
                                "pair_target=%lluus recovery=%d "
                                "suppressed=%llu",
                                static_cast<unsigned long long>(
                                    spacingDeficitUs),
                                static_cast<unsigned long long>(
                                    m_TimingController->displayPeriodUs()),
                                static_cast<unsigned long long>(
                                    nominalPrePresentLeadUs),
                                frameDropRecovery ? 1 : 0,
                                static_cast<unsigned long long>(
                                    suppressed));
                        }
                    }
                }
                const uint64_t correctedFloorUs =
                    m_TimingController->earliestSubmissionUs();
                while (LiGetMicroseconds() < correctedFloorUs) {
                    m_TargetWaiter.waitUntil(correctedFloorUs);
                }
            }
        }

        // The mathematical floor and its correction can add another
        // display-period wait after the primary target checkpoint. Drain
        // notifications once more at the actual submission boundary so a
        // minimize or a renderer-accepted display epoch cannot slip through
        // that final wait and be presented under the old decision.
        // Always drain, even when already interrupted: the drain is what
        // reconciles the presenter's suspension state and arms the rebase.
        displayEpochInterrupted =
            (consumeWindowStateNotifications() &
                kVrrDisplayEpochStateMask) != 0 || displayEpochInterrupted;
        if (displayEpochInterrupted ||
                presentationSuspended() || isStopping()) {
            if (preSubmissionUs != 0) {
                reportCancelAfterBlack("submission-floor");
            }
            cancelPresentation();
            m_DeferredFrame = std::move(frame);
            continue;
        }

        m_TimingController->noteSchedulerDelays(
            renderWaitOvershootUs,
            targetWait.schedulerDelayUs,
            targetWait.schedulerDelayValid);

        const uint64_t presentStartUs = LiGetMicroseconds();
        const VrrPresentFeedback feedback =
            m_Presenter->presentAdaptive(presentRequest);
        const uint64_t presentEndUs = LiGetMicroseconds();
        const uint64_t presentDurationUs =
            presentEndUs >= presentStartUs ?
                presentEndUs - presentStartUs : 0;
        if (bfiDiagnosticsEnabled) {
            m_Diagnostics->interval.presentCall.add(
                presentDurationUs);
        }
        const uint64_t videoSubmissionUs = submissionBoundaryUs(
            feedback, presentStartUs, presentEndUs);
        recordSubmission(feedback, presentStartUs, presentEndUs);
        m_Diagnostics->observePresent(
            frameDropRecovery ?
                BfiPresentPhase::Recovery :
                BfiPresentPhase::Video,
            feedback,
            videoSubmissionUs);
        observeGridSample(feedback);

        if (feedback.presented && !feedback.cancelled) {
            m_VideoStats->totalPacerTimeUs +=
                preparationStartUs >= frame.decodeCompleteUs() ?
                    preparationStartUs - frame.decodeCompleteUs() : 0;
            m_VideoStats->totalRenderTimeUs += preparationDurationUs +
                (presentEndUs >= presentStartUs ?
                    presentEndUs - presentStartUs : 0);
            m_VideoStats->renderedFrames++;

            if (bfiDiagnosticsEnabled &&
                    videoSubmissionUs >= frame.decodeCompleteUs()) {
                const uint64_t decodeToSubmissionUs =
                    videoSubmissionUs - frame.decodeCompleteUs();
                m_Diagnostics->interval.decodeToSubmission.add(
                    decodeToSubmissionUs);
                if (decision.sourcePeriodUs != 0 &&
                        decodeToSubmissionUs > decision.sourcePeriodUs) {
                    m_Diagnostics->interval.latencyAnomalies++;
                    uint64_t suppressed = 0;
                    if (m_Diagnostics->shouldLog(
                            BfiDiagnosticWarning::DecodeLatency,
                            presentEndUs, suppressed)) {
                        SDL_LogWarn(
                            SDL_LOG_CATEGORY_APPLICATION,
                            "VRR-BFI anomaly latency: decode_to_submit=%lluus "
                            "source_period=%lluus schedule_age=%lluus "
                            "prepare=%lluus present=%lluus recovery=%d "
                            "queue_max=%llu suppressed=%llu",
                            static_cast<unsigned long long>(
                                decodeToSubmissionUs),
                            static_cast<unsigned long long>(
                                decision.sourcePeriodUs),
                            static_cast<unsigned long long>(scheduleAgeUs),
                            static_cast<unsigned long long>(
                                preparationDurationUs),
                            static_cast<unsigned long long>(
                                presentDurationUs),
                            frameDropRecovery ? 1 : 0,
                            static_cast<unsigned long long>(
                                m_Diagnostics->currentQueueHighWater()),
                            static_cast<unsigned long long>(suppressed));
                    }
                }
            }

            if (preSubmissionUs != 0 &&
                    videoSubmissionUs >= preSubmissionUs) {
                const uint64_t pairGapUs =
                    videoSubmissionUs - preSubmissionUs;
                m_Diagnostics->interval.pairCompleted++;
                m_Diagnostics->interval.pairGap.add(pairGapUs);
                const uint64_t pairErrorUs =
                    pairGapUs >= nominalPrePresentLeadUs ?
                        pairGapUs - nominalPrePresentLeadUs :
                        nominalPrePresentLeadUs - pairGapUs;
                const uint64_t pairWarningThresholdUs =
                    std::max<uint64_t>(
                        1000, nominalPrePresentLeadUs / 4);
                if (pairErrorUs >= pairWarningThresholdUs) {
                    m_Diagnostics->interval.pairTimingAnomalies++;
                    uint64_t suppressed = 0;
                    if (m_Diagnostics->shouldLog(
                            BfiDiagnosticWarning::PairTiming,
                            presentEndUs, suppressed)) {
                        SDL_LogWarn(
                            SDL_LOG_CATEGORY_APPLICATION,
                            "VRR-BFI anomaly pair-timing: gap=%lluus "
                            "target=%lluus error=%c%lluus "
                            "wake_late=%lluus prepare=%lluus present=%lluus "
                            "latched=%d suppressed=%llu",
                            static_cast<unsigned long long>(pairGapUs),
                            static_cast<unsigned long long>(
                                nominalPrePresentLeadUs),
                            pairGapUs >= nominalPrePresentLeadUs ? '+' : '-',
                            static_cast<unsigned long long>(pairErrorUs),
                            static_cast<unsigned long long>(
                                targetWait.schedulerDelayValid ?
                                    targetWait.schedulerDelayUs : 0),
                            static_cast<unsigned long long>(
                                preparationDurationUs),
                            static_cast<unsigned long long>(
                                presentDurationUs),
                            decision.latchedPresentation ? 1 : 0,
                            static_cast<unsigned long long>(suppressed));
                    }
                }
            }

            m_BfiBrightFrameHeld =
                nominalPrePresentLeadUs != 0 && !frameDropRecovery;
            m_LastBfiBrightSubmissionUs =
                m_BfiBrightFrameHeld ? videoSubmissionUs : 0;
            m_BfiHaveLastVideoSlot = gridLocked;
            if (gridLocked) {
                m_BfiLastVideoSlotSeq = plannedVideoSlotSeq;
            }

            // Ceiling-overdraft accounting: a completed pair consumed two
            // refreshes of the governed present period against one source
            // period of real time. A single-present frame (recovery or a
            // governor skip) relieves all standing pressure.
            if (preSubmissionUs != 0 && presentPeriodFloorQ16 != 0 &&
                    decision.sourcePeriodUs <=
                        std::numeric_limits<uint64_t>::max() >> 16) {
                const uint64_t pairCostQ16 = 2 * presentPeriodFloorQ16;
                const uint64_t sourceQ16 = decision.sourcePeriodUs << 16;
                if (pairCostQ16 > sourceQ16) {
                    m_BfiCeilingDebtQ16 = saturatingAdd(
                        m_BfiCeilingDebtQ16, pairCostQ16 - sourceQ16);
                }
                else {
                    m_BfiCeilingDebtQ16 = 0;
                }
            }
            else if (preSubmissionUs == 0) {
                m_BfiCeilingDebtQ16 = 0;
            }

            if (frameDropRecovery && m_BfiRecoveryActive &&
                    ++m_BfiRecoveryFrames >=
                        kBfiRecoveryFrameCount) {
                const uint64_t recoveryDurationUs =
                    m_BfiRecoveryStartUs != 0 &&
                            presentEndUs >= m_BfiRecoveryStartUs ?
                        presentEndUs - m_BfiRecoveryStartUs : 0;
                m_Diagnostics->interval.recoveryCompletions++;
                m_BfiRecoveryActive = false;
                m_BfiRecoveryFrames = 0;
                m_BfiRecoveryStartUs = 0;
                uint64_t suppressed = 0;
                if (m_Diagnostics->shouldLog(
                        BfiDiagnosticWarning::RecoveryComplete,
                        presentEndUs, suppressed)) {
                    SDL_LogInfo(
                        SDL_LOG_CATEGORY_APPLICATION,
                        "VRR-BFI recovery complete: normal_frames=%u "
                        "duration=%lluus boosted_pairs_resume=1 "
                        "suppressed=%llu",
                        kBfiRecoveryFrameCount,
                        static_cast<unsigned long long>(
                            recoveryDurationUs),
                        static_cast<unsigned long long>(suppressed));
                }
            }
            if (frameDropRecovery) {
                m_Diagnostics->interval.recoveryFrames++;
            }
        }
        else {
            if (nominalPrePresentLeadUs != 0) {
                m_Diagnostics->interval.videoPresentFailures++;
                uint64_t suppressed = 0;
                if (m_Diagnostics->shouldLog(
                        BfiDiagnosticWarning::VideoPresentFailure,
                        presentEndUs, suppressed)) {
                    SDL_LogWarn(
                        SDL_LOG_CATEGORY_APPLICATION,
                        "VRR-BFI anomaly video-present-failed: "
                        "presented=%d cancelled=%d after_black=%d "
                        "recovery=%d call=%lluus bright_held=%d "
                        "suppressed=%llu",
                        feedback.presented ? 1 : 0,
                        feedback.cancelled ? 1 : 0,
                        preSubmissionUs != 0 ? 1 : 0,
                        frameDropRecovery ? 1 : 0,
                        static_cast<unsigned long long>(
                            presentDurationUs),
                        m_BfiBrightFrameHeld ? 1 : 0,
                        static_cast<unsigned long long>(suppressed));
                }
            }
            m_VideoStats->pacerDroppedFrames++;
            m_BfiHaveLastVideoSlot = false;
        }

        m_DeferredFrame = std::move(frame);
    }

    // Release any native state retained between preparation and presentation.
    m_Presenter->cancelFrame();
    m_Diagnostics->logSummary(LiGetMicroseconds(), true);
    return 0;
}

bool VrrPacingWorker::dequeueFrame(PacedFrame& frame,
                                   uint64_t idleRecoveryDeadlineUs,
                                   bool& idleRecoveryDue)
{
    idleRecoveryDue = false;
    QMutexLocker lock(&m_FrameQueueLock);
    while (!isStopping() && !m_Suspended.load() &&
            m_PendingWindowStateFlags.load() == 0 &&
            m_FrameQueue.empty()) {
        if (idleRecoveryDeadlineUs == 0) {
            m_FrameQueueNotEmpty.wait(&m_FrameQueueLock);
            continue;
        }

        const uint64_t nowUs = LiGetMicroseconds();
        if (nowUs >= idleRecoveryDeadlineUs) {
            idleRecoveryDue = true;
            return true;
        }

        const uint64_t remainingUs = idleRecoveryDeadlineUs - nowUs;
        const uint64_t remainingMs = (remainingUs + 999) / 1000;
        m_FrameQueueNotEmpty.wait(
            &m_FrameQueueLock,
            static_cast<unsigned long>(std::min<uint64_t>(
                remainingMs,
                std::numeric_limits<unsigned long>::max())));
    }

    if (isStopping()) {
        return false;
    }
    if (m_Suspended.load() || m_FrameQueue.empty()) {
        return true;
    }

    frame = std::move(m_FrameQueue.front());
    m_FrameQueue.pop_front();
    return true;
}

bool VrrPacingWorker::hasQueuedFrame()
{
    QMutexLocker lock(&m_FrameQueueLock);
    return !m_FrameQueue.empty();
}

void VrrPacingWorker::discardQueuedFrames(bool countDrops)
{
    std::deque<PacedFrame> discardedFrames;
    {
        QMutexLocker lock(&m_FrameQueueLock);
        discardedFrames.swap(m_FrameQueue);
    }

    if (countDrops) {
        m_VideoStats->pacerDroppedFrames +=
            static_cast<uint32_t>(discardedFrames.size());
        m_Diagnostics->noteQueueDiscard(discardedFrames.size());
    }
}

uint32_t VrrPacingWorker::consumeWindowStateNotifications()
{
    uint32_t consumedFlags = m_PendingWindowStateFlags.exchange(0);
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
    m_BfiBrightFrameHeld = false;
    m_BfiRecoveryActive = false;
    m_BfiRecoveryFrames = 0;
    m_BfiRecoveryStartUs = 0;
    m_LastBfiBrightSubmissionUs = 0;
    m_Diagnostics->resetDisplayEpoch();
    resetGrid();
    m_RebaseOnNextFrame = true;
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

void VrrPacingWorker::waitForSubmissionFloor(const VrrTimingDecision& decision)
{
    const uint64_t submissionFloorUs = std::max(
        decision.targetUs, m_TimingController->earliestSubmissionUs());
    if (LiGetMicroseconds() >= submissionFloorUs) {
        return;
    }

    m_TargetWaiter.waitUntil(submissionFloorUs, decision.targetWakeLeadUs);

    // The waiter deliberately bounds each active phase. Re-enter it until the
    // shared monotonic clock confirms the floor; an incomplete wait must never
    // become permission to submit.
    while (LiGetMicroseconds() < submissionFloorUs) {
        m_TargetWaiter.waitUntil(submissionFloorUs);
    }
}

void VrrPacingWorker::recordSubmission(const VrrPresentFeedback& feedback,
                                       uint64_t operationStartUs,
                                       uint64_t operationEndUs)
{
    m_TimingController->noteSubmission(
        feedback.presented, feedback.cancelled,
        submissionBoundaryUs(feedback, operationStartUs, operationEndUs));
}

void VrrPacingWorker::observeGridSample(const VrrPresentFeedback& feedback)
{
    // Only samples where the reported latch refresh is the same refresh the
    // statistics describe give an exact (vblank time, refresh ordinal) pair.
    if (!feedback.latchSampleValid || !feedback.latchTimeValid ||
            feedback.latchPresentRefreshSequence !=
                feedback.latchRefreshSequence) {
        return;
    }

    const uint64_t seq = feedback.latchRefreshSequence;
    const uint64_t timeUs = feedback.latchTimeUs;
    const uint64_t nominalQ16 =
        m_TimingController->displayPeriodUs() << 16;

    if (!m_BfiGridValid) {
        m_BfiGridValid = true;
        m_BfiGridAnchorUs = timeUs;
        m_BfiGridAnchorSeq = seq;
        m_BfiGridPeriodQ16 = nominalQ16;
        m_BfiGridLastSampleUs = timeUs;
        return;
    }

    if (seq < m_BfiGridAnchorSeq || timeUs < m_BfiGridAnchorUs) {
        // Sequence or clock went backwards: a display epoch changed under
        // the model. Relearn from scratch.
        resetGrid();
        return;
    }
    if (seq == m_BfiGridAnchorSeq) {
        return;
    }

    const uint64_t deltaSeq = seq - m_BfiGridAnchorSeq;
    const uint64_t deltaUs = timeUs - m_BfiGridAnchorUs;
    if (deltaUs <= std::numeric_limits<uint64_t>::max() >> 16) {
        const uint64_t measuredQ16 = (deltaUs << 16) / deltaSeq;
        const uint64_t sanityUs = nominalQ16 / kBfiGridSanityDivisor;
        if (measuredQ16 + sanityUs >= nominalQ16 &&
                measuredQ16 <= nominalQ16 + sanityUs) {
            const int64_t errorQ16 =
                static_cast<int64_t>(measuredQ16) -
                static_cast<int64_t>(m_BfiGridPeriodQ16);
            m_BfiGridPeriodQ16 = static_cast<uint64_t>(
                static_cast<int64_t>(m_BfiGridPeriodQ16) +
                (errorQ16 >> kBfiGridEwmaShift));
        }
    }
    m_BfiGridAnchorUs = timeUs;
    m_BfiGridAnchorSeq = seq;
    m_BfiGridLastSampleUs = timeUs;
}

bool VrrPacingWorker::gridSlotAtOrAfter(uint64_t timeUs,
                                        uint64_t& slotTimeUs,
                                        uint64_t& slotSeq)
{
    if (!m_BfiGridValid || m_BfiGridPeriodQ16 == 0) {
        return false;
    }
    if (timeUs > saturatingAdd(m_BfiGridLastSampleUs, kBfiGridStaleUs)) {
        resetGrid();
        return false;
    }
    if (timeUs <= m_BfiGridAnchorUs) {
        slotTimeUs = m_BfiGridAnchorUs;
        slotSeq = m_BfiGridAnchorSeq;
        return true;
    }

    const uint64_t elapsedUs = timeUs - m_BfiGridAnchorUs;
    if (elapsedUs > std::numeric_limits<uint64_t>::max() >> 16) {
        return false;
    }
    const uint64_t elapsedQ16 = elapsedUs << 16;
    const uint64_t steps =
        (elapsedQ16 + m_BfiGridPeriodQ16 - 1) / m_BfiGridPeriodQ16;
    slotSeq = m_BfiGridAnchorSeq + steps;
    slotTimeUs = m_BfiGridAnchorUs + ((steps * m_BfiGridPeriodQ16) >> 16);
    return true;
}

void VrrPacingWorker::resetGrid()
{
    m_BfiGridValid = false;
    m_BfiGridAnchorUs = 0;
    m_BfiGridAnchorSeq = 0;
    m_BfiGridPeriodQ16 = 0;
    m_BfiGridLastSampleUs = 0;
    m_BfiHaveLastVideoSlot = false;
    m_BfiLastVideoSlotSeq = 0;
}
