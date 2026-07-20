#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.h"
#include "vrrtestfakes.h"

#include <SDL.h>

#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

namespace {

std::chrono::steady_clock::time_point g_TestClockOrigin =
    std::chrono::steady_clock::now();
int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void resetFakeClock()
{
    g_TestClockOrigin = std::chrono::steady_clock::now();
}

bool waitFor(const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

VrrSessionConfig enabledConfig()
{
    VrrSessionConfig config;
    config.displayRefreshHz = 120;
    config.streamRateHz = 60;
    return config;
}

PacerTelemetrySnapshot telemetryStats(const PacerTelemetry& telemetry)
{
    return telemetry.snapshot();
}

PacedFrame frame(int number, TrackedFrameLifetime& lifetime)
{
    return makeTrackedPacedFrame(number,
                                 static_cast<uint32_t>((number - 1) * 1500),
                                 LiGetMicroseconds(),
                                 lifetime);
}

void testCapabilityRejection()
{
    FakeVrrFramePresenter backend;
    PacerTelemetry telemetry;

    backend.setSupport(
        VrrFallbackReason::AdaptivePresentationUnavailable);
    VrrPacingWorker unsupportedWorker(&backend, enabledConfig(), &telemetry);
    expect(!unsupportedWorker.start(),
           "worker must reject an unsupported VRR presentation backend");
    expect(backend.checkSupport() ==
               VrrFallbackReason::AdaptivePresentationUnavailable,
           "presenter must retain a concrete rejection reason");
}

void testQueueCapacityAndDrops()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.blockPreparation();
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime second;
    TrackedFrameLifetime third;
    TrackedFrameLifetime fourth;
    TrackedFrameLifetime freshest;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for a capable backend");
        worker.submit(frame(1, first));
        expect(backend.waitForPrepareCount(1),
               "first worker frame must enter the preparation gate");

        // The active frame and three successors absorb a short decoder burst.
        // A fourth successor evicts only the oldest queued frame.
        worker.submit(frame(2, second));
        worker.submit(frame(3, third));
        worker.submit(frame(4, fourth));
        worker.submit(frame(50, freshest));
        const PacerTelemetrySnapshot stats = telemetryStats(telemetry);
        expect(stats.vrrPacingDroppedFrames == 1 &&
                   stats.pacerDroppedFrames == 1,
               "a short successor burst must be buffered with only capacity overflow coalesced");

        const uint64_t releaseUs = LiGetMicroseconds();
        backend.releasePreparation();
        expect(backend.waitForPresentCount(1),
               "releasing preparation must present the active frame");
        expect(backend.waitForPresentCount(4),
               "overflow recovery must drain every retained successor");

        const std::vector<int> presentedFrames = backend.presentedFrames();
        expect(presentedFrames.size() >= 4 && presentedFrames[0] == 1 &&
                   presentedFrames[1] == 3 && presentedFrames[2] == 4 &&
                   presentedFrames[3] == 50,
               "queue overflow must retain the three freshest successors in cadence order");
        const std::vector<uint64_t> calls = backend.presentCallTimesUs();
        expect(calls.size() >= 2 && calls[1] >= releaseUs &&
                   calls[1] - releaseUs < 100000,
               "overflow recovery must promptly rebase to the freshest frame");
    }

    expect(backend.cancelCount() == 1,
           "worker shutdown must release the presenter exactly once");
}

void testLatePreparedFramePresentsImmediately()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.blockPreparation();
    PacerTelemetry telemetry;
    TrackedFrameLifetime lifetime;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for late-presentation recovery");
        worker.submit(frame(1, lifetime));
        expect(backend.waitForPrepareCount(1),
               "frame must enter preparation before simulating a stall");

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        backend.releasePreparation();
        expect(backend.waitForPresentCount(1),
               "a late prepared frame must present instead of being cancelled");
        expect(waitFor([&telemetry] {
                   return telemetryStats(telemetry).vrrEligibleFrames >= 1;
               }),
               "late preparation telemetry must publish with the presentation");
        const PacerTelemetrySnapshot stats = telemetryStats(telemetry);
        expect(stats.vrrPacingDroppedFrames == 0,
               "a late observation must not manufacture a pacing drop");
        expect(stats.vrrPrepareLateFrames >= 1,
               "late preparation must remain visible in timing telemetry");
    }
}

void testTelemetrySnapshotsRemainCumulative()
{
    PacerTelemetry telemetry;
    telemetry.beginVrrSession(1);

    constexpr uint64_t frameCount = 512;
    std::atomic_bool producerDone { false };
    std::thread producer([&telemetry, &producerDone, frameCount] {
        for (uint64_t i = 1; i <= frameCount; ++i) {
            VrrTelemetrySample sample;
            sample.publicationTimeUs = i + 1;
            sample.decisionTimeUs = i;
            sample.pacerTimeUs = i;
            sample.renderTimeUs = i * 2;
            sample.prepareLate = (i % 2) == 0;
            sample.preparationLatenessUs = i;
            sample.submitErrorUs = static_cast<int64_t>(i) - 450;
            sample.spacingCorrected = (i % 8) == 0;
            sample.presented = true;
            sample.readinessBudgetUs = static_cast<int64_t>(i);
            sample.timingBudgetUs = i * 3;
            sample.renderLeadUs = i * 4;
            sample.renderWakeLeadUs = i * 5;
            sample.targetWakeLeadUs = i * 6;
            sample.guardUs = i * 7;
            sample.sourcePeriodUs = i * 8;
            telemetry.recordVrrFrame(sample);
        }
        producerDone.store(true);
    });

    uint64_t previousSequence = 0;
    uint64_t previousRenderedFrames = 0;
    bool monotonic = true;
    while (!producerDone.load()) {
        const PacerTelemetrySnapshot snapshot = telemetryStats(telemetry);
        monotonic = monotonic && snapshot.sequence >= previousSequence &&
            snapshot.renderedFrames >= previousRenderedFrames;
        previousSequence = snapshot.sequence;
        previousRenderedFrames = snapshot.renderedFrames;
    }
    producer.join();

    VrrTelemetrySample delayedSubmission;
    delayedSubmission.publicationTimeUs = frameCount + 2;
    delayedSubmission.decisionTimeUs = frameCount + 1;
    delayedSubmission.targetWaitEntryLate = true;
    telemetry.recordVrrFrame(delayedSubmission);

    const PacerTelemetrySnapshot finalSnapshot = telemetryStats(telemetry);
    expect(monotonic,
           "telemetry snapshots must not regress while another thread publishes");
    expect(finalSnapshot.vrrActive &&
               finalSnapshot.renderedFrames == frameCount &&
               finalSnapshot.vrrEligibleFrames == frameCount + 1,
           "cumulative telemetry must retain every published frame");
    expect(finalSnapshot.vrrPrepareLateFrames == frameCount / 2 &&
               finalSnapshot.vrrPrepareLatenessP50Us == 384 &&
               finalSnapshot.vrrPrepareLatenessP95Us == 500 &&
               finalSnapshot.vrrPrepareLatenessP99Us == 510 &&
               finalSnapshot.vrrTargetWaitEntryLateFrames == 1 &&
               finalSnapshot.vrrSubmitErrorP50Us == -2 &&
               finalSnapshot.vrrSubmitErrorP95Us == 56 &&
               finalSnapshot.vrrSubmitErrorP99Us == 61 &&
               finalSnapshot.vrrSubmitErrorMaxUs == 62 &&
               finalSnapshot.vrrPresentFailedFrames == 1 &&
               finalSnapshot.vrrStateSequence == finalSnapshot.sequence,
           "telemetry must keep bounded timing distributions and output outcomes separate");
}

void testSuspendDiscardAndFreshFrame()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.blockPreparation();
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime queuedOne;
    TrackedFrameLifetime queuedTwo;
    TrackedFrameLifetime fresh;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start before exercising suspension");
        worker.submit(frame(1, first));
        expect(backend.waitForPrepareCount(1),
               "active frame must be preparing before suspension");
        worker.submit(frame(2, queuedOne));
        worker.submit(frame(3, queuedTwo));

        WINDOW_STATE_CHANGE_INFO minimized {};
        minimized.stateChangeFlags = WINDOW_STATE_CHANGE_MINIMIZED;
        worker.notifyWindowChanged(&minimized);
        expect(telemetryStats(telemetry).vrrPacingDroppedFrames >= 2,
               "minimize must synchronously discard queued VRR frames");

        backend.releasePreparation();
        expect(backend.waitForCancelCount(1),
               "suspension while preparing must cancel the acquired image");
        expect(waitFor([&backend] { return backend.suspendedCount() == 1; }),
               "worker must suspend the presenter on its own thread");
        expect(backend.presentCount() == 0,
               "a suspended active frame must not be presented");

        WINDOW_STATE_CHANGE_INFO restored {};
        restored.stateChangeFlags = WINDOW_STATE_CHANGE_RESTORED;
        worker.notifyWindowChanged(&restored);
        worker.submit(frame(4, fresh));
        expect(backend.waitForPresentCount(1),
               "a fresh frame after restore must use a rebased timeline");
        expect(waitFor([&backend] { return backend.resumedCount() == 1; }),
               "resume must reach the presenter before fresh presentation");
        expect(backend.presentedFrames().front() == 4,
               "pre-suspend frames must not survive restoration");
    }
}

void testDeferredSurfaceLifetime()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime second;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for deferred lifetime testing");
        worker.submit(frame(1, first));
        expect(backend.waitForPresentCount(1), "first frame must present");
        expect(first.releases.load() == 0,
               "a presented decoder surface must remain deferred");

        worker.submit(frame(2, second));
        expect(backend.waitForPresentCount(2), "second frame must present");
        expect(waitFor([&first] { return first.releases.load() == 1; }),
               "the next result must release the prior deferred surface");
        expect(second.releases.load() == 0,
               "the current surface must remain deferred");
    }

    expect(second.releases.load() == 1,
           "worker destruction must release the final deferred surface");
}

void testCancelledPresentationCountsAsDroppedOutput()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.setPresentCancelled(true);
    PacerTelemetry telemetry;
    TrackedFrameLifetime cancelled;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for cancellation classification");
        worker.submit(frame(1, cancelled));
        expect(backend.waitForPresentCount(1),
               "cancelled presentation fixture must still submit its frame");
        expect(waitFor([&telemetry] {
                   const PacerTelemetrySnapshot stats = telemetryStats(telemetry);
                   return stats.pacerDroppedFrames == 1 &&
                       stats.vrrPacingDroppedFrames == 1 &&
                       stats.vrrPresentCancelledFrames == 1;
               }),
               "a cancelled presentation must increment both pacing drop counters");
        const PacerTelemetrySnapshot stats = telemetryStats(telemetry);
        expect(stats.renderedFrames == 0 && stats.vrrEligibleFrames == 1 &&
                   stats.vrrPresentFailedFrames == 0,
               "a cancelled presentation must not count as rendered output");
    }
}

void testPresentCallSpacingSetsDisplayFloor()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime second;
    VrrSessionConfig atRefresh = enabledConfig();
    atRefresh.streamRateHz = 120;

    {
        VrrPacingWorker worker(&backend, atRefresh, &telemetry);
        expect(worker.start(), "worker must start for presentation spacing");
        worker.submit(makeTrackedPacedFrame(1, 0, LiGetMicroseconds(), first));
        expect(backend.waitForPresentCount(1), "first spacing frame must present");
        worker.submit(makeTrackedPacedFrame(2, 750, LiGetMicroseconds(), second));
        expect(backend.waitForPresentCount(2), "second spacing frame must present");

        const std::vector<uint64_t> calls = backend.presentCallTimesUs();
        expect(calls.size() >= 2 && calls[1] >= calls[0] + 8333,
               "present calls must remain at least one display period apart");
    }
}

void testBlockingPresentUsesWorkerCallBoundary()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.setPresentDelayUs(20000);
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime second;
    VrrSessionConfig config = enabledConfig();
    config.displayRefreshHz = 60;

    {
        VrrPacingWorker worker(&backend, config, &telemetry);
        expect(worker.start(), "worker must start for blocking presentation testing");
        worker.submit(frame(1, first));
        expect(backend.waitForPresentCount(1), "blocking first present must return");

        backend.setPresentDelayUs(0);
        worker.submit(frame(2, second));
        expect(backend.waitForPresentCount(2), "second frame must present");

        const std::vector<uint64_t> calls = backend.presentCallTimesUs();
        const std::vector<uint64_t> returns = backend.presentReturnTimesUs();
        constexpr uint64_t displayPeriodUs = 16666;
        expect(calls.size() >= 2 &&
                   calls[1] >= calls[0] + displayPeriodUs,
               "the worker-owned call boundary must enforce display spacing");
        expect(calls.size() >= 2 && returns.size() >= 1 &&
                   calls[1] < returns[0] + displayPeriodUs,
               "a blocking presenter must not add a second display period");
    }
}

void testSubmissionErrorCapturesPresentOverhead()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.setPreSubmissionDelayUs(1000);
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for submission-error testing");
        worker.submit(frame(1, first));
        expect(backend.waitForPresentCount(1),
               "submission-error fixture must present its frame");
        expect(waitFor([&telemetry] {
                   return telemetryStats(telemetry).vrrEligibleFrames >= 1;
               }),
               "submission-error telemetry must publish with the presentation");

        const PacerTelemetrySnapshot stats = telemetryStats(telemetry);
        expect(stats.vrrSubmitErrorP50Us > 0 &&
                   stats.vrrSubmitErrorMaxUs > 0 &&
                   stats.vrrPresentFailedFrames == 0,
               "post-target present overhead must be reported as submission error, not output failure");
    }
}

void testFailedPreparationCancellationHonorsDisplayFloor()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime failed;
    VrrSessionConfig config = enabledConfig();
    config.displayRefreshHz = 20;

    {
        VrrPacingWorker worker(&backend, config, &telemetry);
        expect(worker.start(), "worker must start for cancellation timing");

        backend.blockPreparation();
        worker.submit(frame(1, first));
        expect(backend.waitForPrepareCount(1), "priming frame must prepare");
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        backend.releasePreparation();
        expect(backend.waitForPresentCount(1),
               "priming frame must establish the display floor");

        backend.setPreparationSucceeds(false);
        backend.setCancellationMaySubmit(true);
        backend.setCancelSubmits(true);
        worker.submit(frame(2, failed));
        expect(backend.waitForPresentCount(2),
               "failed preparation must release its image by cancellation");

        const std::vector<uint64_t> calls = backend.presentCallTimesUs();
        expect(calls.size() >= 2 && calls[1] >= calls[0] + 50000,
               "a cancellation that may submit must honor the display floor");
        expect(backend.cancelCount() >= 1,
               "failed preparation must invoke the cancellation boundary");
    }
}

void testSuspendedPreparedCancellationHonorsDisplayFloor()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime suspended;
    VrrSessionConfig config = enabledConfig();
    config.displayRefreshHz = 20;

    {
        VrrPacingWorker worker(&backend, config, &telemetry);
        expect(worker.start(), "worker must start for suspended cancellation timing");

        backend.blockPreparation();
        worker.submit(frame(1, first));
        expect(backend.waitForPrepareCount(1), "priming frame must prepare");
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        backend.releasePreparation();
        expect(backend.waitForPresentCount(1),
               "priming frame must establish the display floor");

        backend.setCancellationMaySubmit(true);
        backend.setCancelSubmits(true);
        backend.blockPreparation();
        worker.submit(frame(2, suspended));
        expect(backend.waitForPrepareCount(2),
               "second frame must acquire its image before suspension");

        WINDOW_STATE_CHANGE_INFO minimized {};
        minimized.stateChangeFlags = WINDOW_STATE_CHANGE_MINIMIZED;
        worker.notifyWindowChanged(&minimized);
        backend.releasePreparation();
        expect(backend.waitForPresentCount(2),
               "suspension must cancel an acquired image even if release submits");

        const std::vector<uint64_t> calls = backend.presentCallTimesUs();
        expect(calls.size() >= 2 && calls[1] >= calls[0] + 50000,
               "suspending cancellation must honor the display floor");
        expect(backend.cancelCount() >= 1,
               "suspension must invoke the cancellation boundary");
    }
}

void testImmutablePresentationContract()
{
    expect(expectedDxgiVrrPresentFlags() == kFakeDxgiPresentAllowTearing,
           "D3D11 adaptive presentation must not depend on phase telemetry availability");
    expect(expectedLinuxPresentationMode(FakeLinuxPresentationBackend::Wayland, true) ==
               FakeLinuxPresentationMode::Mailbox,
           "Wayland selects Mailbox when available");
    expect(expectedLinuxPresentationMode(FakeLinuxPresentationBackend::X11, true) ==
               FakeLinuxPresentationMode::Immediate,
           "X11 selects Immediate when available");
    expect(expectedLinuxPresentationMode(FakeLinuxPresentationBackend::Gamescope, true) ==
               FakeLinuxPresentationMode::Immediate,
           "Gamescope selects Immediate when available");
    expect(expectedLinuxPresentationMode(FakeLinuxPresentationBackend::KmsDrm, true) ==
               FakeLinuxPresentationMode::Immediate,
           "KMSDRM selects Immediate when available");
    expect(expectedLinuxPresentationMode(FakeLinuxPresentationBackend::Other, true) ==
               FakeLinuxPresentationMode::Fifo,
           "unknown Linux backends fall back to FIFO");
    expect(expectedLinuxPresentationMode(FakeLinuxPresentationBackend::Wayland, false) ==
               FakeLinuxPresentationMode::Fifo,
           "unsupported adaptive modes fall back to FIFO");

    resetFakeClock();
    FakeVrrFramePresenter backend;
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime second;
    VrrSessionConfig immutableConfig = enabledConfig();
    immutableConfig.streamRateHz = 116;

    {
        VrrPacingWorker worker(&backend, immutableConfig, &telemetry);
        expect(worker.start(), "worker must start for presentation contract testing");
        worker.submit(frame(1, first));
        expect(backend.waitForPresentCount(1), "first contract frame must present");
        worker.submit(frame(2, second));
        expect(backend.waitForPresentCount(2), "second contract frame must present");
    }

    const std::vector<int> presentedFrames = backend.presentedFrames();
    expect(presentedFrames.size() == 2 && presentedFrames[0] == 1 &&
               presentedFrames[1] == 2,
           "the minimal presenter contract must preserve frame order");
    const std::vector<VrrPresentRequest> requests = backend.presentRequests();
    expect(requests.size() == 2 && !requests[0].latchedPresentation &&
               !requests[1].latchedPresentation,
           "an immutable presenter must never receive a per-present latch request");
}

void testDeepTraceRequestsNativeObservationsWithoutChangingMode()
{
    resetFakeClock();
    QTemporaryDir traceDirectory;
    expect(traceDirectory.isValid(),
           "deep diagnostics test must create a temporary directory");
    const QString tracePath = traceDirectory.filePath("vrr-deep-trace.csv");
    const QByteArray tracePathBytes = QFile::encodeName(tracePath);
    SDL_setenv("MOONLIGHT_VRR_TRACE", tracePathBytes.constData(), 1);
    SDL_setenv("MOONLIGHT_VRR_DEEP_TRACE", "1", 1);
    FakeVrrFramePresenter backend;
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for deep diagnostics testing");
        worker.submit(frame(1, first));
        expect(backend.waitForPresentCount(1),
               "deep diagnostics must not suppress presentation");
    }

    const std::vector<VrrPresentRequest> requests = backend.presentRequests();
    expect(requests.size() == 1 && requests[0].collectDiagnostics,
           "deep trace must request adjacent native observations");
    expect(requests.size() == 1 && !requests[0].latchedPresentation,
           "deep trace must not change the controller presentation mode");

    QFile traceFile(tracePath);
    expect(traceFile.open(QIODevice::ReadOnly | QIODevice::Text),
           "deep diagnostics must produce a readable trace");
    const QByteArray header = traceFile.readLine();
    const QByteArray row = traceFile.readLine();
    expect(header.contains("native_present_call_us") &&
               header.contains("gpu_ready_wait_us"),
           "deep trace must identify native and renderer-readiness timing");
    expect(!row.isEmpty() && header.count(',') == row.count(','),
           "deep trace rows must match the CSV schema");
    expect(traceFile.size() < 16384,
           "one deep trace row must remain compact");
    traceFile.close();

    SDL_setenv("MOONLIGHT_VRR_TRACE", "", 1);
    SDL_setenv("MOONLIGHT_VRR_DEEP_TRACE", "0", 1);
}

} // namespace

// VrrPacingWorker uses the common monotonic clock. The isolated test owns an
// equivalent steady-clock epoch so it needs no network or streaming runtime.
extern "C" uint64_t LiGetMicroseconds(void)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - g_TestClockOrigin).count());
}

int main()
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    testCapabilityRejection();
    testQueueCapacityAndDrops();
    testLatePreparedFramePresentsImmediately();
    testTelemetrySnapshotsRemainCumulative();
    testSuspendDiscardAndFreshFrame();
    testDeferredSurfaceLifetime();
    testCancelledPresentationCountsAsDroppedOutput();
    testPresentCallSpacingSetsDisplayFloor();
    testBlockingPresentUsesWorkerCallBoundary();
    testSubmissionErrorCapturesPresentOverhead();
    testFailedPreparationCancellationHonorsDisplayFloor();
    testSuspendedPreparedCancellationHonorsDisplayFloor();
    testImmutablePresentationContract();
    testDeepTraceRequestsNativeObservationsWithoutChangingMode();

    SDL_Quit();
    return failures == 0 ? 0 : 1;
}
