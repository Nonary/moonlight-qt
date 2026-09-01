#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.h"
#include "vrrtestfakes.h"

#include <SDL.h>

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
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

QByteArray readExpandedTrace(const QString& tracePath)
{
    QFile traceFile(tracePath);
    if (!traceFile.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray encoded = traceFile.readAll();
    constexpr int magicLength = 7;
    if (!encoded.startsWith("MLVRR1\n")) {
        return encoded;
    }

    QByteArray expanded;
    int offset = magicLength;
    while (offset < encoded.size()) {
        if (encoded.size() - offset < 4) {
            return {};
        }
        const unsigned char* length =
            reinterpret_cast<const unsigned char*>(encoded.constData() + offset);
        const uint32_t compressedBytes =
            static_cast<uint32_t>(length[0]) |
            (static_cast<uint32_t>(length[1]) << 8) |
            (static_cast<uint32_t>(length[2]) << 16) |
            (static_cast<uint32_t>(length[3]) << 24);
        offset += 4;
        if (compressedBytes > static_cast<uint32_t>(encoded.size() - offset)) {
            return {};
        }
        const QByteArray chunk = qUncompress(
            reinterpret_cast<const uchar*>(encoded.constData() + offset),
            static_cast<int>(compressedBytes));
        if (chunk.isEmpty()) {
            return {};
        }
        expanded.append(chunk);
        offset += static_cast<int>(compressedBytes);
    }
    return expanded;
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

void testQueuedStaleFrameYieldsToFreshSuccessor()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.blockPreparation();
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime stale;
    TrackedFrameLifetime fresh;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for queued-stale recovery");
        worker.submit(frame(1, first));
        expect(backend.waitForPrepareCount(1),
               "active frame must enter preparation before queueing successors");

        worker.submit(frame(2, stale));
        worker.submit(frame(3, fresh));
        // Frame 2 is now older than its 60 FPS source period while frame 3 is
        // available. It is stale content, not a pacing deadline to honor.
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        backend.releasePreparation();

        expect(backend.waitForPresentCount(2),
               "the active frame and freshest successor must present");
        expect(waitFor([&telemetry] {
                   return telemetryStats(telemetry).vrrPacingDroppedFrames >= 1;
               }),
               "an obsolete queued frame must count as a pacing drop");

        const std::vector<int> presentedFrames = backend.presentedFrames();
        expect(presentedFrames.size() >= 2 && presentedFrames[0] == 1 &&
                   presentedFrames[1] == 3,
               "a stale queued frame must yield to its fresher successor");
    }
}

void testSinglePeriodQueueDelayPreservesFluidity()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.blockPreparation();
    PacerTelemetry telemetry;
    TrackedFrameLifetime first;
    TrackedFrameLifetime retained;
    TrackedFrameLifetime fresh;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(),
               "worker must start for single-period queue recovery");
        worker.submit(frame(1, first));
        expect(backend.waitForPrepareCount(1),
               "active frame must enter preparation before queueing successors");

        worker.submit(frame(2, retained));
        worker.submit(frame(3, fresh));
        // This crosses the 60 FPS source period but remains below the bounded
        // two-period backlog threshold. The captured production bug dropped
        // content at this boundary despite a valid future target.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        backend.releasePreparation();

        expect(backend.waitForPresentCount(3),
               "a one-period queue delay must retain every frame");
        const std::vector<int> presentedFrames = backend.presentedFrames();
        expect(presentedFrames.size() >= 3 &&
                   presentedFrames[0] == 1 &&
                   presentedFrames[1] == 2 &&
                   presentedFrames[2] == 3,
               "ordinary pipeline occupancy must preserve frame order");
        expect(telemetryStats(telemetry).vrrPacingDroppedFrames == 0,
               "ordinary pipeline occupancy must not manufacture a pacing drop");
    }
}

void testTelemetrySnapshotsRemainCumulative()
{
    PacerTelemetry telemetry;
    telemetry.beginVrrSession();

    constexpr uint64_t frameCount = 512;
    std::atomic_bool producerDone { false };
    std::thread producer([&telemetry, &producerDone, frameCount] {
        for (uint64_t i = 1; i <= frameCount; ++i) {
            VrrTelemetrySample sample;
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

void testReusableSurfaceReleasedWithoutSuccessor()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.setSourceFrameReusable(true);
    PacerTelemetry telemetry;
    TrackedFrameLifetime reusable;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for reusable lifetime testing");
        worker.submit(frame(1, reusable));
        expect(backend.waitForPresentCount(1), "reusable frame must present");
        expect(waitFor([&reusable] { return reusable.releases.load() == 1; }),
               "a GPU-complete decoder surface must release without a successor");
        const std::vector<int> presented = backend.presentedFrames();
        expect(presented.size() == 1 && presented.front() == 1,
               "presentation must not depend on the released source frame");
    }

    expect(reusable.releases.load() == 1,
           "worker destruction must not release a reusable surface twice");
}

void testDecodeBoundaryCapturedBeforeQueueAndPreparedExactly()
{
    resetFakeClock();
    FakeVrrFramePresenter backend;
    backend.setDecodeBoundary(73);
    PacerTelemetry telemetry;
    TrackedFrameLifetime lifetime;

    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for decode-boundary testing");
        worker.submit(frame(1, lifetime));
        expect(backend.waitForPresentCount(1),
               "decode-boundary frame must present");
        const std::vector<uint64_t> boundaries =
            backend.preparedDecodeBoundaries();
        expect(backend.decodeBoundaryCaptureCount() == 1,
               "each submitted frame must capture one decoder boundary");
        expect(boundaries.size() == 1 && boundaries.front() == 73,
               "preparation must receive the submitted frame's exact boundary");
    }
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

void testTraceCapturesEveryDeliveredFrame()
{
    resetFakeClock();
    QTemporaryDir traceDirectory;
    expect(traceDirectory.isValid(),
           "replay trace test must create a temporary directory");
    const QString tracePath = traceDirectory.filePath("vrr-replay.csv");
    const QByteArray tracePathBytes = QFile::encodeName(tracePath);
    SDL_setenv("MOONLIGHT_VRR_TRACE", tracePathBytes.constData(), 1);
    SDL_setenv("MOONLIGHT_VRR_DEEP_TRACE", "0", 1);

    FakeVrrFramePresenter backend;
    backend.blockPreparation();
    PacerTelemetry telemetry;
    TrackedFrameLifetime lifetimes[10];
    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(), "worker must start for replay tracing");
        worker.submit(frame(1, lifetimes[0]));
        expect(backend.waitForPrepareCount(1),
               "replay trace must hold one active frame");
        for (int frameNumber = 2; frameNumber <= 5; ++frameNumber) {
            worker.submit(frame(frameNumber, lifetimes[frameNumber - 1]));
        }
        backend.releasePreparation();
        expect(backend.waitForPresentCount(4),
               "replay trace test must drain retained frames");

        WINDOW_STATE_CHANGE_INFO minimized {};
        minimized.stateChangeFlags = WINDOW_STATE_CHANGE_MINIMIZED;
        worker.notifyWindowChanged(&minimized);
        worker.submit(frame(6, lifetimes[5]));

        WINDOW_STATE_CHANGE_INFO restored {};
        restored.stateChangeFlags = WINDOW_STATE_CHANGE_RESTORED;
        worker.notifyWindowChanged(&restored);
        worker.submit(frame(7, lifetimes[6]));
        expect(backend.waitForPresentCount(5),
               "restored trace frame must present after an explicit rebase");

        WINDOW_STATE_CHANGE_INFO resized {};
        resized.stateChangeFlags = WINDOW_STATE_CHANGE_SIZE;
        resized.width = 1920;
        resized.height = 1080;
        worker.notifyWindowChanged(&resized);
        worker.submit(frame(8, lifetimes[7]));
        expect(backend.waitForPresentCount(6),
               "first frame after a geometry refresh must use a new trace epoch");

        backend.blockPreparation();
        worker.submit(frame(9, lifetimes[8]));
        expect(backend.waitForPrepareCount(7),
               "display-epoch race test must hold an in-flight frame");
        WINDOW_STATE_CHANGE_INFO displayChanged {};
        displayChanged.stateChangeFlags = WINDOW_STATE_CHANGE_DISPLAY;
        displayChanged.displayIndex = 0;
        worker.notifyWindowChanged(&displayChanged);
        backend.releasePreparation();
        expect(backend.waitForCancelCount(1),
               "an in-flight frame must be cancelled after display state changes");
        expect(backend.presentCount() == 6,
               "a decision from the prior display epoch must not be presented");

        worker.submit(frame(10, lifetimes[9]));
        expect(backend.waitForPresentCount(7),
               "the post-interrupt frame must start the refreshed display epoch");
    }

    const QByteArray expandedTrace = readExpandedTrace(tracePath);
    const QList<QByteArray> lines = expandedTrace.split('\n');
    const QList<QByteArray> columns = lines.value(0).split(',');
    const int frameColumn = columns.indexOf("frame");
    const int rtpColumn = columns.indexOf("rtp_timestamp");
    const int arrivalColumn = columns.indexOf("pacer_arrival_us");
    const int decisionValidColumn = columns.indexOf("decision_valid");
    const int dispositionColumn = columns.indexOf("disposition");
    const int additionalQueuedFrameColumn =
        columns.indexOf("additional_queued_frame");
    const int externalRebaseColumn =
        columns.indexOf("external_rebase_applied");
    const int externalRebaseFlagsColumn =
        columns.indexOf("external_rebase_flags");
    const int midframeWindowStateFlagsColumn =
        columns.indexOf("midframe_window_state_flags");
    const QList<QByteArray> controllerDiagnosticNames {
        "readiness_phase_us",
        "readiness_demand_us",
        "applied_readiness_reserve_us",
        "render_baseline_us",
        "render_insurance_us",
        "pacing_latency_budget_us",
        "cadence_sample_count",
        "rate_candidate_sample_count",
        "readiness_sample_count",
        "preparation_sample_count",
        "render_scheduler_sample_count",
        "target_scheduler_sample_count",
        "clean_spacing_frames",
        "phase_error_frames",
        "readiness_model_valid",
        "render_wait_initial_us",
        "render_wait_active_budget_us",
        "render_wait_coarse_sleep_count",
        "render_wait_coarse_requested_total_us",
        "render_wait_coarse_requested_wake_us",
        "render_wait_coarse_return_us",
        "render_wait_coarse_clock_stalled",
        "render_wait_active_entered",
        "render_wait_active_start_us",
        "render_wait_active_limit_us",
        "render_wait_active_yield_count",
        "render_wait_active_clock_stalled",
        "render_wait_active_yield_limit_reached",
        "target_wait_initial_us",
        "target_wait_active_budget_us",
        "target_wait_coarse_sleep_count",
        "target_wait_coarse_requested_total_us",
        "target_wait_coarse_requested_wake_us",
        "target_wait_coarse_return_us",
        "target_wait_coarse_clock_stalled",
        "target_wait_active_entered",
        "target_wait_active_start_us",
        "target_wait_active_limit_us",
        "target_wait_active_yield_count",
        "target_wait_active_clock_stalled",
        "target_wait_active_yield_limit_reached",
    };
    QList<int> controllerDiagnosticColumns;
    for (const QByteArray& name : controllerDiagnosticNames) {
        controllerDiagnosticColumns.append(columns.indexOf(name));
    }
    const QList<QByteArray> nativeEvidenceNames {
        "native_backend_valid",
        "native_backend",
        "native_present_result_valid",
        "native_present_result",
        "native_present_parameters_valid",
        "native_present_sync_interval",
        "native_present_flags",
        "native_vrr_state_valid",
        "native_tearing_supported",
        "native_borderless_flip_model",
        "native_same_gpu_output",
        "native_render_adapter_luid_valid",
        "native_render_adapter_luid",
        "native_swap_chain_allows_tearing",
        "native_tearing_feature_query_result_valid",
        "native_tearing_feature_query_result",
        "native_tearing_feature_allows_tearing",
        "native_swap_chain_desc_query_result_valid",
        "native_swap_chain_desc_query_result",
        "native_swap_chain_flags",
        "native_swap_chain_swap_effect",
        "native_fullscreen_state_query_result_valid",
        "native_fullscreen_state_query_result",
        "native_fullscreen_exclusive",
        "native_window_flags",
        "native_present_ready_available",
        "native_foreground_window",
        "native_vrr_fallback_reason",
        "native_desktop_monitor_count",
        "native_vblank_virtualization_probe_complete",
        "native_vblank_virtualization_call_available",
        "native_vblank_virtualization_result_valid",
        "native_vblank_virtualization_result",
        "native_vblank_virtualization_disabled",
        "native_display_config_query_result_valid",
        "native_display_config_query_result",
        "native_display_path_valid",
        "native_display_path_flags",
        "native_display_target_available",
        "native_display_source_adapter_luid",
        "native_display_source_id",
        "native_display_target_adapter_luid",
        "native_display_target_id",
        "native_display_output_technology",
        "native_display_rotation",
        "native_display_scaling",
        "native_display_path_refresh_numerator",
        "native_display_path_refresh_denominator",
        "native_display_signal_valid",
        "native_display_signal_pixel_rate_hz",
        "native_display_signal_hsync_numerator",
        "native_display_signal_hsync_denominator",
        "native_display_signal_vsync_numerator",
        "native_display_signal_vsync_denominator",
        "native_display_signal_active_width",
        "native_display_signal_active_height",
        "native_display_signal_total_width",
        "native_display_signal_total_height",
        "native_display_signal_additional_info_raw",
        "native_display_signal_scanline_ordering",
        "gpu_ready_attempted",
        "gpu_ready_signal_result_valid",
        "gpu_ready_signal_result",
        "gpu_ready_set_event_result_valid",
        "gpu_ready_set_event_result",
        "gpu_ready_wait_result_valid",
        "gpu_ready_wait_result",
        "gpu_ready_signal_start_us",
        "gpu_ready_signal_end_us",
        "gpu_ready_flush_start_us",
        "gpu_ready_flush_end_us",
        "gpu_ready_set_event_start_us",
        "gpu_ready_set_event_end_us",
        "native_raster_sampling_requested",
        "native_raster_open_result_valid",
        "native_raster_open_result",
        "native_raster_source_valid",
        "native_raster_vidpn_source_id",
        "native_raster_before_query_result_valid",
        "native_raster_before_query_result",
        "native_raster_before_query_start_us",
        "native_raster_before_query_end_us",
        "native_raster_before_in_vertical_blank",
        "native_raster_before_scanline",
        "native_raster_after_query_result_valid",
        "native_raster_after_query_result",
        "native_raster_after_query_start_us",
        "native_raster_after_query_end_us",
        "native_raster_after_in_vertical_blank",
        "native_raster_after_scanline",
        "submission_id_query_result_valid",
        "submission_id_query_result",
        "submission_id_query_start_us",
        "submission_id_query_end_us",
        "frame_stats_query_result_valid",
        "frame_stats_query_result",
        "frame_stats_query_start_us",
        "frame_stats_query_end_us",
        "latch_raw_sync_qpc_valid",
        "latch_raw_sync_qpc_ticks",
        "latch_raw_sync_qpc_frequency_hz",
        "latch_qpc_correlation_valid",
        "latch_qpc_correlation_reference_ticks",
        "latch_qpc_correlation_reference_time_us",
        "latch_qpc_correlation_span_ticks",
    };
    QList<int> nativeEvidenceColumns;
    for (const QByteArray& name : nativeEvidenceNames) {
        nativeEvidenceColumns.append(columns.indexOf(name));
    }
    expect(frameColumn >= 0 && rtpColumn >= 0 && arrivalColumn >= 0 &&
               decisionValidColumn >= 0 && dispositionColumn >= 0 &&
               additionalQueuedFrameColumn >= 0 &&
               externalRebaseColumn >= 0 &&
               externalRebaseFlagsColumn >= 0 &&
               midframeWindowStateFlagsColumn >= 0 &&
               std::all_of(
                   controllerDiagnosticColumns.cbegin(),
                   controllerDiagnosticColumns.cend(),
                   [](int column) { return column >= 0; }) &&
               std::all_of(
                   nativeEvidenceColumns.cbegin(),
                   nativeEvidenceColumns.cend(),
                   [](int column) { return column >= 0; }),
           "replay schema must expose raw arrivals and terminal disposition");

    bool observedFrames[11] = {};
    bool observedCapacityDrop = false;
    bool observedRejectedArrival = false;
    bool observedRestoreRebase = false;
    bool observedGeometryRebase = false;
    bool observedMidframeDisplayInterrupt = false;
    bool observedPostInterruptDisplayRebase = false;
    bool observedCleanFooter = false;
    QByteArray observedFooterHash;
    int rowCount = 0;
    for (int i = 1; i < lines.size(); ++i) {
        if (lines[i].isEmpty()) {
            continue;
        }
        if (lines[i].startsWith("#vrr_trace_footer,")) {
            const QByteArray hashMarker("decoded_sha256=");
            const int hashOffset = lines[i].indexOf(hashMarker);
            const QByteArray footerHash = hashOffset >= 0 ?
                lines[i].mid(hashOffset + hashMarker.size()) :
                QByteArray();
            observedFooterHash = footerHash;
            observedCleanFooter =
                lines[i].contains("format_version=2") &&
                lines[i].contains("clean_shutdown=1") &&
                lines[i].contains("arrival_sequence_allocated=10") &&
                lines[i].contains("rows_enqueued=10") &&
                lines[i].contains("rows_dropped=0") &&
                lines[i].contains("size_capped=0") &&
                lines[i].contains("write_failed=0") &&
                footerHash.size() == 64 &&
                std::all_of(
                    footerHash.cbegin(), footerHash.cend(),
                    [](char value) {
                        return (value >= '0' && value <= '9') ||
                            (value >= 'a' && value <= 'f');
                    });
            continue;
        }
        const QList<QByteArray> fields = lines[i].split(',');
        expect(fields.size() == columns.size(),
               "every replay row must match the declared schema");
        if (fields.size() != columns.size()) {
            continue;
        }
        ++rowCount;
        const int frameNumber = fields[frameColumn].toInt();
        if (frameNumber >= 1 && frameNumber <= 10) {
            observedFrames[frameNumber] = true;
        }
        expect(fields[rtpColumn].toULongLong() ==
                   static_cast<uint64_t>((frameNumber - 1) * 1500),
               "replay trace must preserve each raw RTP timestamp");
        expect(fields[arrivalColumn].toULongLong() != 0,
               "replay trace must capture the pacer arrival instant");
        expect(fields[additionalQueuedFrameColumn] == "0",
               "default trace rows must record low-latency queue policy");
        if (fields[decisionValidColumn] == "0") {
            for (int diagnosticColumn : controllerDiagnosticColumns) {
                expect(fields[diagnosticColumn] == "0",
                       "producer-side terminal rows must not inspect worker-owned controller state");
            }
            for (int evidenceColumn : nativeEvidenceColumns) {
                expect(fields[evidenceColumn] == "0",
                       "nondecision rows must not invent native outcome or QPC evidence");
            }
        }
        if (frameNumber == 2 &&
            fields[dispositionColumn] == "queue_capacity") {
            observedCapacityDrop = true;
            expect(fields[decisionValidColumn] == "0",
                   "pre-schedule eviction must not invent a timing decision");
        }
        if (frameNumber == 6 &&
            fields[dispositionColumn] == "arrival_rejected") {
            observedRejectedArrival = true;
            expect(fields[decisionValidColumn] == "0",
                    "suspended arrival must not invent a timing decision");
        }
        if (frameNumber == 7 &&
                fields[dispositionColumn] == "presented") {
            observedRestoreRebase =
                fields[decisionValidColumn] == "1" &&
                fields[externalRebaseColumn] == "1" &&
                fields[externalRebaseFlagsColumn] ==
                    QByteArray::number(
                        WINDOW_STATE_CHANGE_MINIMIZED |
                        WINDOW_STATE_CHANGE_RESTORED) &&
                fields[midframeWindowStateFlagsColumn] == "0";
        }
        if (frameNumber == 8 &&
                fields[dispositionColumn] == "presented") {
            observedGeometryRebase =
                fields[decisionValidColumn] == "1" &&
                fields[externalRebaseColumn] == "1" &&
                fields[externalRebaseFlagsColumn] ==
                    QByteArray::number(
                        WINDOW_STATE_CHANGE_SIZE) &&
                fields[midframeWindowStateFlagsColumn] == "0";
        }
        if (frameNumber == 9 &&
                fields[dispositionColumn] == "interrupted") {
            observedMidframeDisplayInterrupt =
                fields[decisionValidColumn] == "1" &&
                fields[externalRebaseColumn] == "0" &&
                fields[externalRebaseFlagsColumn] == "0" &&
                fields[midframeWindowStateFlagsColumn] ==
                    QByteArray::number(
                        WINDOW_STATE_CHANGE_DISPLAY);
        }
        if (frameNumber == 10 &&
                fields[dispositionColumn] == "presented") {
            observedPostInterruptDisplayRebase =
                fields[decisionValidColumn] == "1" &&
                fields[externalRebaseColumn] == "1" &&
                fields[externalRebaseFlagsColumn] ==
                    QByteArray::number(
                        WINDOW_STATE_CHANGE_DISPLAY) &&
                fields[midframeWindowStateFlagsColumn] == "0";
        }
    }
    expect(rowCount == 10,
           "trace must contain exactly one terminal row per delivered frame");
    expect(observedFrames[1] && observedFrames[2] && observedFrames[3] &&
               observedFrames[4] && observedFrames[5] && observedFrames[6] &&
               observedFrames[7] && observedFrames[8] &&
               observedFrames[9] && observedFrames[10],
           "trace must not omit evicted or presented deliveries");
    expect(observedCapacityDrop,
           "trace must identify the frame evicted by queue capacity");
    expect(observedRejectedArrival,
           "trace must retain frames rejected before queue admission");
    expect(observedRestoreRebase,
           "trace must distinguish an explicit window-state rebase from an internal controller reset");
    expect(observedGeometryRebase,
           "trace must begin a new epoch after renderer geometry/display state refresh");
    expect(observedMidframeDisplayInterrupt,
           "trace must identify a display change that interrupts an in-flight frame");
    expect(observedPostInterruptDisplayRebase,
           "the first frame after a mid-frame display change must carry its exact rebase cause");
    expect(observedCleanFooter,
           "trace must end with exact clean-close row accounting");
    const int footerOffset = expandedTrace.indexOf("#vrr_trace_footer,");
    const QByteArray expectedFooterHash = footerOffset >= 0 ?
        QCryptographicHash::hash(
            expandedTrace.left(footerOffset),
            QCryptographicHash::Sha256).toHex() :
        QByteArray();
    expect(!expectedFooterHash.isEmpty() &&
               observedFooterHash == expectedFooterHash,
           "trace footer must authenticate the decoded header and rows");

    SDL_setenv("MOONLIGHT_VRR_TRACE", "", 1);
}

void testSmoothnessTraceCapturesReadinessPolicy()
{
    resetFakeClock();
    QTemporaryDir traceDirectory;
    expect(traceDirectory.isValid(),
           "smoothness policy trace test must create a temporary directory");
    const QString tracePath = traceDirectory.filePath("vrr-smoothness-policy.csv");
    const QByteArray tracePathBytes = QFile::encodeName(tracePath);
    SDL_setenv("MOONLIGHT_VRR_TRACE", tracePathBytes.constData(), 1);
    SDL_setenv("MOONLIGHT_VRR_DEEP_TRACE", "0", 1);

    FakeVrrFramePresenter backend;
    PacerTelemetry telemetry;
    TrackedFrameLifetime lifetime;
    VrrSessionConfig smoothnessConfig = enabledConfig();
    {
        VrrPacingWorker worker(&backend, smoothnessConfig, &telemetry);
        expect(worker.start(),
               "worker must start for playout policy tracing");
        worker.submit(frame(1, lifetime));
        expect(backend.waitForPresentCount(1),
               "smoothness policy trace must present one frame");
    }

    const QList<QByteArray> lines = readExpandedTrace(tracePath).split('\n');
    const QList<QByteArray> columns = lines.value(0).split(',');
    const int dispositionColumn = columns.indexOf("disposition");
    const int additionalQueueColumn =
        columns.indexOf("additional_queued_frame");
    const int lowPercentileColumn =
        columns.indexOf("param_readiness_low_percentile");
    const int loosePercentileColumn =
        columns.indexOf("param_readiness_loose_percentile");
    const int sourceDelayColumn =
        columns.indexOf("param_source_playout_delay_us");
    const int timestampPlayoutColumn =
        columns.indexOf("param_timestamp_playout_enabled");
    const int retainReserveColumn =
        columns.indexOf("param_retain_readiness_on_phase_reset");
    const int adaptiveColumn =
        columns.indexOf("param_playout_delay_adaptive");
    const int playoutDelayColumn = columns.indexOf("playout_delay_us");
    expect(dispositionColumn >= 0 && additionalQueueColumn >= 0 &&
               lowPercentileColumn >= 0 && loosePercentileColumn >= 0 &&
               sourceDelayColumn >= 0 && timestampPlayoutColumn >= 0 &&
               retainReserveColumn >= 0 && adaptiveColumn >= 0 &&
               playoutDelayColumn >= 0,
           "smoothness trace must expose its resolved playout policy and the applied delay");

    bool foundPresentedRow = false;
    for (int i = 1; i < lines.size(); ++i) {
        if (lines[i].isEmpty() || lines[i].startsWith("#vrr_trace_footer,")) {
            continue;
        }
        const QList<QByteArray> fields = lines[i].split(',');
        if (fields.value(dispositionColumn) != "presented") {
            continue;
        }
        foundPresentedRow = true;
        expect(fields.value(additionalQueueColumn) == "0" &&
                   fields.value(lowPercentileColumn) == "0" &&
                   fields.value(loosePercentileColumn) == "80" &&
                   fields.value(sourceDelayColumn) == "3000" &&
                   fields.value(timestampPlayoutColumn) == "1" &&
                   fields.value(adaptiveColumn) == "1" &&
                   fields.value(retainReserveColumn) == "0" &&
                   fields.value(playoutDelayColumn).toULongLong() >= 1000,
                "the session must record the single adaptive timestamp playout policy");
    }
    expect(foundPresentedRow,
           "smoothness policy trace must contain a presented row");

    SDL_setenv("MOONLIGHT_VRR_TRACE", "", 1);
    SDL_setenv("MOONLIGHT_VRR_DEEP_TRACE", "0", 1);
}

void testFailedCancellationNativeEvidenceIsTraced()
{
    resetFakeClock();
    QTemporaryDir traceDirectory;
    expect(traceDirectory.isValid(),
           "failed cancellation trace test must create a temporary directory");
    const QString tracePath =
        traceDirectory.filePath("vrr-cancellation-failure.csv");
    const QByteArray tracePathBytes = QFile::encodeName(tracePath);
    SDL_setenv("MOONLIGHT_VRR_TRACE", tracePathBytes.constData(), 1);
    SDL_setenv("MOONLIGHT_VRR_DEEP_TRACE", "0", 1);

    FakeVrrFramePresenter backend;
    backend.setPreparationSucceeds(false);
    backend.setCancellationMaySubmit(true);
    backend.setCancelSubmits(false);
    PacerTelemetry telemetry;
    TrackedFrameLifetime failed;
    {
        VrrPacingWorker worker(&backend, enabledConfig(), &telemetry);
        expect(worker.start(),
               "worker must start for failed cancellation tracing");
        worker.submit(frame(1, failed));
        expect(backend.waitForCancelCount(1),
               "failed preparation must attempt native cancellation");
    }

    const QByteArray expandedTrace = readExpandedTrace(tracePath);
    const QList<QByteArray> lines = expandedTrace.split('\n');
    const QList<QByteArray> columns = lines.value(0).split(',');
    const QList<QByteArray> fields = lines.value(1).split(',');
    const int dispositionColumn = columns.indexOf("disposition");
    const int backendValidColumn = columns.indexOf("native_backend_valid");
    const int backendColumn = columns.indexOf("native_backend");
    const int resultValidColumn =
        columns.indexOf("native_present_result_valid");
    const int resultColumn = columns.indexOf("native_present_result");
    expect(fields.size() == columns.size() &&
               dispositionColumn >= 0 &&
               backendValidColumn >= 0 &&
               backendColumn >= 0 &&
               resultValidColumn >= 0 &&
               resultColumn >= 0,
           "failed cancellation trace must retain a complete row");
    if (fields.size() == columns.size() &&
            dispositionColumn >= 0 &&
            backendValidColumn >= 0 &&
            backendColumn >= 0 &&
            resultValidColumn >= 0 &&
            resultColumn >= 0) {
        expect(fields[dispositionColumn] == "preparation_failed" &&
                   fields[backendValidColumn] == "1" &&
                   fields[backendColumn] == "2" &&
                   fields[resultValidColumn] == "1" &&
                   fields[resultColumn] == "-1",
               "failed native cancellation must not be collapsed into generic cancellation");
    }

    SDL_setenv("MOONLIGHT_VRR_TRACE", "", 1);
}

void testDeepTraceRequestsNativeObservationsWithoutChangingMode()
{
    resetFakeClock();
    QTemporaryDir traceDirectory;
    expect(traceDirectory.isValid(),
           "deep diagnostics test must create a temporary directory");
    const QString tracePath = traceDirectory.filePath("vrr-deep-trace.vrrtrace");
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

    const QByteArray expandedTrace = readExpandedTrace(tracePath);
    const QList<QByteArray> lines = expandedTrace.split('\n');
    const QByteArray header = lines.value(0);
    const QByteArray row = lines.value(1);
    const QList<QByteArray> columns = header.split(',');
    const QList<QByteArray> fields = row.split(',');
    expect(header.contains("native_present_call_us") &&
               header.contains("presenter_submission_time_valid") &&
               header.contains("presenter_submission_time_us") &&
               header.contains("presenter_submission_time_used") &&
               header.contains("gpu_ready_attempted") &&
               header.contains("gpu_ready_signal_result_valid") &&
               header.contains("gpu_ready_signal_result") &&
               header.contains("gpu_ready_set_event_result_valid") &&
               header.contains("gpu_ready_set_event_result") &&
               header.contains("gpu_ready_wait_result_valid") &&
               header.contains("gpu_ready_wait_result") &&
               header.contains("gpu_ready_signal_start_us") &&
               header.contains("gpu_ready_signal_end_us") &&
               header.contains("gpu_ready_flush_start_us") &&
               header.contains("gpu_ready_flush_end_us") &&
               header.contains("gpu_ready_set_event_start_us") &&
               header.contains("gpu_ready_set_event_end_us") &&
               header.contains("gpu_ready_poll_start_us") &&
               header.contains("gpu_ready_poll_end_us") &&
               header.contains("gpu_ready_fence_value") &&
               header.contains("gpu_ready_poll_completed_value") &&
               header.contains("gpu_ready_completed_before_wait") &&
               header.contains("gpu_ready_completion_lower_bound_us") &&
               header.contains("gpu_ready_completion_upper_bound_us") &&
               header.contains("gpu_ready_completion_uncertainty_us") &&
               header.contains("gpu_ready_wait_us") &&
               header.contains(
                   "native_display_signal_additional_info_raw") &&
               header.contains("tear_classification") &&
               header.contains("spacing_deficit_us") &&
               header.contains("spacing_guard_feedback_us") &&
               header.contains("spacing_corrected") &&
               header.contains("spacing_recheck_us") &&
               header.contains("spacing_corrected_floor_us") &&
               header.contains("submission_id_query_start_us") &&
               header.contains("submission_id_query_end_us") &&
               header.contains("frame_stats_query_start_us") &&
               header.contains("frame_stats_query_end_us") &&
               header.contains("completion_queue_depth") &&
               header.contains("latch_present_refresh_seq"),
           "deep trace must identify presenter, native, tear, spacing, queue, and renderer-readiness timing");
    expect(row.startsWith("5,"),
           "new captures must use parameterized trace schema 5");
    expect(!row.isEmpty() && header.count(',') == row.count(','),
           "deep trace rows must match the CSV schema");
    const int submissionBoundaryColumn =
        columns.indexOf("submission_boundary_us");
    const int presenterSubmissionValidColumn =
        columns.indexOf("presenter_submission_time_valid");
    const int presenterSubmissionTimeColumn =
        columns.indexOf("presenter_submission_time_us");
    const int presenterSubmissionUsedColumn =
        columns.indexOf("presenter_submission_time_used");
    expect(fields.size() == columns.size() &&
               submissionBoundaryColumn >= 0 &&
               presenterSubmissionValidColumn >= 0 &&
               presenterSubmissionTimeColumn >= 0 &&
               presenterSubmissionUsedColumn >= 0,
           "deep trace must retain a complete presenter submission-timing row");
    if (fields.size() == columns.size() &&
            submissionBoundaryColumn >= 0 &&
            presenterSubmissionValidColumn >= 0 &&
            presenterSubmissionTimeColumn >= 0 &&
            presenterSubmissionUsedColumn >= 0) {
        expect(fields[presenterSubmissionValidColumn] == "1" &&
                   fields[presenterSubmissionUsedColumn] == "1" &&
                   fields[presenterSubmissionTimeColumn] ==
                       fields[submissionBoundaryColumn],
               "worker submission boundary must preserve the presenter's exact native-call timestamp");
    }
    expect(expandedTrace.contains(
               "#vrr_trace_footer,format_version=2,clean_shutdown=1,"
               "arrival_sequence_allocated=1,rows_enqueued=1,"
               "rows_dropped=0,size_capped=0,write_failed=0,"
               "decoded_sha256="),
           "compressed traces must retain the clean-close accounting footer");
    QFile traceFile(tracePath);
    expect(traceFile.size() > 7 && traceFile.size() < expandedTrace.size(),
           "recommended traces must be chunk-compressed on disk");
    expect(traceFile.size() < 16384,
           "one deep trace row must remain compact");

    const char* exportPath = SDL_getenv("MOONLIGHT_VRR_TEST_EXPORT_TRACE");
    if (exportPath != nullptr && exportPath[0] != '\0') {
        QFile::remove(QString::fromLocal8Bit(exportPath));
        expect(QFile::copy(tracePath, QString::fromLocal8Bit(exportPath)),
               "deep trace test must export its replay fixture when requested");
    }

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
    testQueuedStaleFrameYieldsToFreshSuccessor();
    testSinglePeriodQueueDelayPreservesFluidity();
    testTelemetrySnapshotsRemainCumulative();
    testSuspendDiscardAndFreshFrame();
    testDeferredSurfaceLifetime();
    testReusableSurfaceReleasedWithoutSuccessor();
    testDecodeBoundaryCapturedBeforeQueueAndPreparedExactly();
    testCancelledPresentationCountsAsDroppedOutput();
    testPresentCallSpacingSetsDisplayFloor();
    testBlockingPresentUsesWorkerCallBoundary();
    testSubmissionErrorCapturesPresentOverhead();
    testFailedPreparationCancellationHonorsDisplayFloor();
    testSuspendedPreparedCancellationHonorsDisplayFloor();
    testImmutablePresentationContract();
    testTraceCapturesEveryDeliveredFrame();
    testSmoothnessTraceCapturesReadinessPolicy();
    testFailedCancellationNativeEvidenceIsTraced();
    testDeepTraceRequestsNativeObservationsWithoutChangingMode();

    SDL_Quit();
    return failures == 0 ? 0 : 1;
}
