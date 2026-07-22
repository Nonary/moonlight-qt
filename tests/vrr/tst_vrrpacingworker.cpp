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
    TrackedFrameLifetime lifetimes[6];
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
    }

    const QList<QByteArray> lines = readExpandedTrace(tracePath).split('\n');
    const QList<QByteArray> columns = lines.value(0).split(',');
    const int frameColumn = columns.indexOf("frame");
    const int rtpColumn = columns.indexOf("rtp_timestamp");
    const int arrivalColumn = columns.indexOf("pacer_arrival_us");
    const int decisionValidColumn = columns.indexOf("decision_valid");
    const int dispositionColumn = columns.indexOf("disposition");
    expect(frameColumn >= 0 && rtpColumn >= 0 && arrivalColumn >= 0 &&
               decisionValidColumn >= 0 && dispositionColumn >= 0,
           "replay schema must expose raw arrivals and terminal disposition");

    bool observedFrames[6] = {};
    bool observedCapacityDrop = false;
    bool observedRejectedArrival = false;
    int rowCount = 0;
    for (int i = 1; i < lines.size(); ++i) {
        if (lines[i].isEmpty()) {
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
        if (frameNumber >= 1 && frameNumber <= 5) {
            observedFrames[frameNumber] = true;
        }
        expect(fields[rtpColumn].toULongLong() ==
                   static_cast<uint64_t>((frameNumber - 1) * 1500),
               "replay trace must preserve each raw RTP timestamp");
        expect(fields[arrivalColumn].toULongLong() != 0,
               "replay trace must capture the pacer arrival instant");
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
    }
    expect(rowCount == 6,
           "trace must contain exactly one terminal row per delivered frame");
    expect(observedFrames[1] && observedFrames[2] && observedFrames[3] &&
               observedFrames[4] && observedFrames[5],
           "trace must not omit evicted or presented deliveries");
    expect(observedCapacityDrop,
           "trace must identify the frame evicted by queue capacity");
    expect(observedRejectedArrival,
           "trace must retain frames rejected before queue admission");

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
    expect(header.contains("native_present_call_us") &&
               header.contains("gpu_ready_wait_us") &&
               header.contains("tear_classification") &&
               header.contains("spacing_guard_feedback_us") &&
               header.contains("latch_present_refresh_seq"),
           "deep trace must identify native, tear, guard, and renderer-readiness timing");
    expect(row.startsWith("5,"),
           "new captures must use parameterized trace schema 5");
    expect(!row.isEmpty() && header.count(',') == row.count(','),
           "deep trace rows must match the CSV schema");
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
    testTraceCapturesEveryDeliveredFrame();
    testDeepTraceRequestsNativeObservationsWithoutChangingMode();

    SDL_Quit();
    return failures == 0 ? 0 : 1;
}
