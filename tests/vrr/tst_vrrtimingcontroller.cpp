// Standalone deterministic coverage for the platform-neutral VRR core.
// This file is intentionally not wired into the application build; tests/vrr
// owns the optional qmake harness.

#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include "../../app/streaming/vrrratepolicy.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrcatchup.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrframedroppolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

VrrSessionConfig config(int streamRateHz = 60, int displayRefreshHz = 120)
{
    VrrSessionConfig value;
    value.streamRateHz = streamRateHz;
    value.displayRefreshHz = displayRefreshHz;
    return value;
}

PacedFrame frame(int number, uint32_t timestamp, bool timestampValid,
                  uint64_t decodedUs)
{
    return PacedFrame(nullptr, number, timestamp, timestampValid, decodedUs);
}

uint32_t quantizedRtpTimestamp(int frameNumber, int sourceRateHz,
                               int captureRateHz = 120)
{
    const uint64_t captureFrame =
        (static_cast<uint64_t>(frameNumber) * captureRateHz +
         static_cast<uint64_t>(sourceRateHz) / 2) /
        static_cast<uint64_t>(sourceRateHz);
    return static_cast<uint32_t>(
        (captureFrame * 90000ULL +
         static_cast<uint64_t>(captureRateHz) / 2) /
        static_cast<uint64_t>(captureRateHz));
}

uint64_t decodedTimeForRtp(uint64_t epochUs, uint32_t timestamp)
{
    return epochUs + static_cast<uint64_t>(timestamp) * 1000000ULL / 90000ULL;
}

uint64_t idealDecodedTime(uint64_t epochUs, int frameNumber, int rateHz)
{
    return epochUs + (static_cast<uint64_t>(frameNumber) * 1000000ULL +
                      static_cast<uint64_t>(rateHz) / 2) /
        static_cast<uint64_t>(rateHz);
}

void testRtpWrapResetAndFallback()
{
    VrrTimingController controller(config());
    const uint32_t wrappedStart = 0xfffffe00U;
    controller.schedule(frame(1, wrappedStart, true, 100000), 100000);
    VrrTimingDecision wrapped = controller.schedule(
        frame(2, wrappedStart + 1500U, true, 116666), 116666);
    expect(!wrapped.rebased && wrapped.usedRtpTimestamp,
           "RTP wrap must be a normal valid interval");
    expect(wrapped.sourceIntervalUs == 16666,
           "wrapped RTP delta must convert at 90 kHz");

    VrrTimingDecision reset = controller.schedule(
        frame(3, 100U, true, 130000), 130000);
    expect(reset.rebased,
           "backward RTP movement must rebase rather than unwrap forward");

    VrrTimingController largeForwardController(config());
    largeForwardController.schedule(frame(1, 0, true, 100000), 100000);
    VrrTimingDecision largeForward = largeForwardController.schedule(
        frame(2, 90001, true, 1100011), 1100011);
    expect(largeForward.rebased,
           "valid RTP movement over one second must rebase");

    VrrTimingController fallbackController(config());
    fallbackController.schedule(frame(10, 0, false, 500000), 500000);
    VrrTimingDecision fallback = fallbackController.schedule(
        frame(12, 0, false, 533334), 533334);
    expect(!fallback.usedRtpTimestamp && fallback.sourceIntervalUs == 33333,
           "invalid timestamps must use rational frame-number cadence");

    VrrTimingDecision forwardReset = fallbackController.schedule(
        frame(1000, 0, false, 2000000), 2000000);
    expect(forwardReset.rebased,
           "fallback movement beyond one second must rebase");
}

void testTimingFormulaeAndReserveCap()
{
    VrrTimingController controller(config(60, 120));
    VrrTimingDecision first = controller.schedule(
        frame(1, 0, true, 100000), 100000);
    const VrrTimingParameters& parameters = controller.parameters();
    const uint64_t expectedGuardUs = std::clamp(
        controller.displayPeriodUs() / parameters.baseGuardDivisor,
        parameters.minimumGuardUs, parameters.maximumBaseGuardUs);
    expect(first.guardUs == expectedGuardUs,
           "display guard must honor the configured divisor and bounds");
    expect(first.headroomUs ==
               controller.sourcePeriodUs() - controller.displayPeriodUs() -
                   expectedGuardUs,
           "headroom must subtract one display period and the guard");
    expect(first.targetUs ==
               100000 + first.renderLeadUs +
                   parameters.presentationSafetyUs +
                   parameters.sourcePlayoutDelayUs &&
               first.renderStartUs ==
                   first.targetUs - first.renderLeadUs - first.renderWakeLeadUs,
           "target must include render lead and presentation safety");

    controller.noteSubmission(true, false, first.targetUs);
    VrrTimingDecision second = controller.schedule(
        frame(2, 1500, true, 116666), 116666);
    expect(second.targetUs >=
               first.targetUs + controller.displayPeriodUs() + expectedGuardUs,
           "target must honor the prior presentation floor and guard");

    VrrTimingController capped(config(360, 120));
    VrrTimingDecision cappedDecision = capped.schedule(
        frame(1, 0, true, 100000), 100000);
    capped.notePreparationDuration(10000);
    capped.noteSubmission(true, false, cappedDecision.targetUs);
    expect(capped.renderLeadUs() <= capped.sourcePeriodUs(),
           "render lead must never exceed the source period");
}

// Isolate the mapper from adaptive delay, render training and smoothing. The
// public schedule path still exercises RTP conversion and cadence eligibility.
VrrTimingParameters offsetTestPolicy()
{
    VrrTimingParameters policy;
    policy.timestampPlayoutEnabled = 1;
    policy.sourcePlayoutDelayUs = 6000;
    policy.playoutOffsetWarmupSamples = 0;
    policy.playoutOffsetCadenceGate = 1;
    policy.playoutOffsetSlewUsPerSecond = 2400;
    policy.playoutOffsetSourceClock = 1;
    policy.playoutOffsetMaximumStepUs = 100;
    return policy;
}

void testOffsetSlewUsesElapsedTime()
{
    for (int rateHz : {30, 60, 120, 240}) {
        for (int direction : {-1, 1}) {
            for (uint64_t slewRate : {uint64_t(7), uint64_t(2400)}) {
                auto policy = offsetTestPolicy();
                policy.playoutOffsetWindowUs = 1;
                policy.playoutOffsetSlewUsPerSecond = slewRate;
                VrrTimingController controller(config(rateHz, 240), true, policy);
                constexpr uint64_t epochUs = 1000000;
                controller.schedule(frame(1, 0, true, epochUs), epochUs);
                int64_t previousOffset = controller.playoutOffsetUs();
                uint64_t lastSourceClockUs = 0;
                // Keep a downward phase step shorter than one source interval
                // so even the 240 FPS fixture has a monotonic observation clock.
                const int64_t phaseUs = static_cast<int64_t>(
                    std::min<uint64_t>(6000, 750000 / uint64_t(rateHz)));
                for (int i = 1; i <= rateHz; ++i) {
                    const auto timestamp = static_cast<uint32_t>(
                        uint64_t(i) * 90000 / uint64_t(rateHz));
                    const uint64_t decodedUs = static_cast<uint64_t>(
                        int64_t(decodedTimeForRtp(epochUs, timestamp)) + direction * phaseUs);
                    const auto decision = controller.schedule(
                        frame(i + 1, timestamp, true, decodedUs), decodedUs);
                    const int64_t offset = controller.playoutOffsetUs();
                    expect(std::abs(offset - previousOffset) <= 100,
                           "elapsed-time offset slew must respect its per-observation cap");
                    expect(decision.playoutDelayUs == 6000,
                           "offset correction must not acquire extra playout buffering");
                    previousOffset = offset;
                    lastSourceClockUs =
                        uint64_t(timestamp) * 1000000 / 90000;
                }
                const int64_t expectedUs = static_cast<int64_t>(
                    lastSourceClockUs * slewRate / 1000000);
                expect(std::abs((controller.playoutOffsetUs() - int64_t(epochUs)) -
                                direction * expectedUs) <= 1,
                       "equal elapsed time must buy equal correction across FPS, including fractional rates");
            }
        }
    }
}

void testOffsetSlewIgnoresWorkerBacklogAndPreservesHistoricalClock()
{
    auto policy = offsetTestPolicy();
    policy.playoutOffsetWindowUs = 1;
    auto historicalPolicy = policy;
    historicalPolicy.playoutOffsetSourceClock = 0;
    VrrTimingController controller(config(60, 120), true, policy);
    VrrTimingController delayed(config(60, 120), true, policy);
    VrrTimingController historical(config(60, 120), true, historicalPolicy);
    constexpr uint64_t epochUs = 1000000;
    controller.schedule(frame(1, 0, true, epochUs), epochUs);
    delayed.schedule(frame(1, 0, true, epochUs), epochUs);
    historical.schedule(frame(1, 0, true, epochUs), epochUs);
    // A two-second worker/GPU stall must not age the source-clock mapping.
    // Historical captures explicitly retain the worker-clock behavior.
    uint64_t nowUs = epochUs + 2000000;
    uint32_t timestamp = 1500;
    uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 6000;
    controller.schedule(frame(2, timestamp, true, decodedUs), decodedUs);
    delayed.schedule(frame(2, timestamp, true, decodedUs), nowUs);
    historical.schedule(frame(2, timestamp, true, decodedUs), nowUs);
    expect(delayed.playoutOffsetUs() == controller.playoutOffsetUs(),
           "local GPU/worker backlog must not buy source-offset correction");
    expect(historical.playoutOffsetUs() == int64_t(epochUs + 100),
           "captured worker-clock policy must retain its bounded gap step");
    for (int i = 3; i <= 8; ++i) {
        timestamp += 1500;
        decodedUs = decodedTimeForRtp(epochUs, timestamp) + 6000;
        nowUs += 10000;
        controller.schedule(frame(i, timestamp, true, decodedUs), decodedUs);
        delayed.schedule(frame(i, timestamp, true, decodedUs), nowUs);
        expect(delayed.playoutOffsetUs() == controller.playoutOffsetUs(),
               "continued render contention must not perturb mapping age or slew");
    }
    controller.rebase();
    controller.schedule(frame(1, 0, true, epochUs), epochUs);
    expect(controller.playoutOffsetUs() == int64_t(epochUs),
           "a genuine epoch rebase must discard old offset state and credit");
}

void testSourceMappingIgnoresDecodePollTiming()
{
    auto policy = offsetTestPolicy();
    policy.playoutSourceMappingDecoderOutput = 1;
    policy.playoutOffsetWindowUs = 1;
    VrrTimingController early(config(120, 120), true, policy);
    VrrTimingController late(config(120, 120), true, policy);
    VrrTimingController alreadyComplete(config(120, 120), true, policy);

    auto historicalPolicy = policy;
    historicalPolicy.playoutSourceMappingDecoderOutput = 0;
    VrrTimingController historicalEarly(config(120, 120), true,
                                         historicalPolicy);
    VrrTimingController historicalLate(config(120, 120), true,
                                        historicalPolicy);
    bool historicalDiverged = false;

    constexpr uint64_t epochUs = 1000000;
    for (int i = 0; i < 240; ++i) {
        const uint32_t timestamp = static_cast<uint32_t>(i * 750);
        const uint64_t outputUs = decodedTimeForRtp(epochUs, timestamp);
        auto makeObserved = [&](uint64_t syntheticCompletionUs,
                                uint64_t residualWaitUs) {
            auto value = frame(i + 1, timestamp, true, outputUs);
            value.noteGpuReadyUs(syntheticCompletionUs);
            value.noteDecodeSyncWaitUs(residualWaitUs);
            return value;
        };

        const auto earlyDecision = early.schedule(
            makeObserved(outputUs + 6000, 6000), outputUs + 6000);
        const auto lateDecision = late.schedule(
            makeObserved(outputUs + 2000, 2000), outputUs + 4000);
        const auto completeDecision = alreadyComplete.schedule(
            makeObserved(outputUs, 0), outputUs + 7000);

        const auto sameMapping = [&](const VrrTimingDecision& decision,
                                     const VrrTimingDecision& reference,
                                     const VrrTimingController& controller,
                                     const VrrTimingController& referenceController) {
            return decision.sourceTimeUs == reference.sourceTimeUs &&
                decision.sourcePeriodUs == reference.sourcePeriodUs &&
                decision.originalTargetUs == reference.originalTargetUs &&
                decision.cadenceSmoothingUs == reference.cadenceSmoothingUs &&
                controller.playoutOffsetUs() == referenceController.playoutOffsetUs() &&
                decision.cadenceEligible == reference.cadenceEligible &&
                decision.sourceRateChanged == reference.sourceRateChanged &&
                decision.phaseDiscontinuity == reference.phaseDiscontinuity &&
                decision.rebased == reference.rebased;
        };
        expect(sameMapping(lateDecision, earlyDecision, late, early) &&
                   sameMapping(completeDecision, earlyDecision,
                               alreadyComplete, early),
               "source mapping must be invariant to worker poll timing and residual decode wait");

        const auto oldEarly = historicalEarly.schedule(
            makeObserved(outputUs + 6000, 6000), outputUs + 6000);
        const auto oldLate = historicalLate.schedule(
            makeObserved(outputUs + 2000, 2000), outputUs + 4000);
        historicalDiverged |=
            oldEarly.sourceTimeUs != oldLate.sourceTimeUs ||
            oldEarly.originalTargetUs != oldLate.originalTargetUs ||
            historicalEarly.playoutOffsetUs() !=
                historicalLate.playoutOffsetUs();
    }
    expect(historicalDiverged,
           "the compatibility policy must retain decode-completion-based mapping for old captures");
}

void testOffsetRecoveryRejectsTransitionMinimum()
{
    // A source discontinuity can contain an unusually early readiness
    // observation. It must not pull every later target earlier for three
    // seconds, particularly when the following source phase is later.
    for (bool canLatch : {false, true}) {
        auto policy = offsetTestPolicy();
        auto historical = policy;
        historical.playoutOffsetCadenceGate = 0;
        historical.playoutOffsetSlewUsPerSecond = 0;
        VrrTimingController recovered(config(60, 120), canLatch, policy);
        VrrTimingController legacy(config(60, 120), canLatch, historical);
        constexpr uint64_t epochUs = 1000000;
        uint32_t timestamp = 0;
        int number = 0;
        for (int i = 0; i < 180; ++i) {
            timestamp = static_cast<uint32_t>(i * 1500);
            const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
            recovered.schedule(frame(++number, timestamp, true, decodedUs), decodedUs);
            legacy.schedule(frame(number, timestamp, true, decodedUs), decodedUs);
        }
        timestamp += 6000; // 66.7 ms source gap, below the full-rebase bound.
        const uint64_t earlyUs = decodedTimeForRtp(epochUs, timestamp) - 6000;
        const auto transition = recovered.schedule(
            frame(++number, timestamp, true, earlyUs), earlyUs);
        legacy.schedule(frame(number, timestamp, true, earlyUs), earlyUs);
        expect(transition.phaseDiscontinuity && !transition.cadenceEligible && !transition.rebased,
               "the regression fixture must exercise an ineligible phase change, not a full rebase");
        expect(recovered.playoutOffsetUs() == int64_t(epochUs),
               "an ineligible transition minimum must not move the applied offset");
        for (int i = 0; i < 60; ++i) {
            timestamp += 1500;
            const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 4000;
            const int64_t before = recovered.playoutOffsetUs();
            const auto decision = recovered.schedule(
                frame(++number, timestamp, true, decodedUs), decodedUs);
            legacy.schedule(frame(number, timestamp, true, decodedUs), decodedUs);
            const int64_t after = recovered.playoutOffsetUs();
            expect(after >= before && after - before <= 100,
                   "post-transition offset recovery must be gradual and use the new phase");
            expect(decision.playoutDelayUs == 6000,
                   "transition recovery must not reseed the retained delay");
        }
        expect(recovered.playoutOffsetUs() >= int64_t(epochUs + 2000),
               "fresh stable cadence must recover without waiting for an old minimum to expire");
        expect(legacy.playoutOffsetUs() < int64_t(epochUs - 1000),
               "the historical policy must still reproduce the stale-minimum regression");
    }
}

void testOffsetRecoveryKeepsStartupPaddingAndNativePolicy()
{
    // canLatch=true covers DXGI and persistent Vulkan Mailbox. false covers
    // persistent Vulkan Immediate/FIFO: retain their software safety floor.
    for (bool canLatch : {false, true}) {
        for (int mode : {0, 1, 2}) {
            for (int rateHz : {30, 60, 116, 120}) {
                auto session = config(rateHz, 120);
                session.latencyMode = mode;
                auto policy = vrrTimingParametersForSession(session);
                auto historical = policy;
                historical.playoutOffsetCadenceGate = 0;
                historical.playoutOffsetSlewUsPerSecond = 0;
                VrrTimingController current(session, canLatch, policy);
                VrrTimingController previous(session, canLatch, historical);
                expect(policy.playoutOffsetCadenceGate == 1 &&
                           policy.playoutOffsetSlewUsPerSecond == 2400 &&
                           policy.playoutOffsetMaximumStepUs == 100,
                       "all live presets must select the same bounded mapping policy");
                for (int i = 0; i < 240; ++i) {
                    const auto timestamp = static_cast<uint32_t>(
                        uint64_t(i) * 90000 / uint64_t(rateHz));
                    const uint64_t decodedUs = decodedTimeForRtp(1000000, timestamp);
                    const auto a = current.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
                    const auto b = previous.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
                    expect(a.playoutDelayUs == b.playoutDelayUs && a.targetUs == b.targetUs &&
                               a.latchedPresentation == b.latchedPresentation,
                           "steady startup padding and native protection must not change at any source rate");
                    current.noteSubmission(true, false, a.targetUs);
                    previous.noteSubmission(true, false, b.targetUs);
                    expect(current.earliestSubmissionUs() == previous.earliestSubmissionUs(),
                           "Linux's unprotected-present safety floor must remain intact");
                }
            }
        }
    }
}

void testProductionPreparationUsesAvailableSlack()
{
    // Production spends the existing playout interval on preparation
    // while retaining the dynamic queue and its independent GPU lead.
    for (int mode : {0, 1, 2}) {
        auto session = config(120, 120);
        session.latencyMode = mode;
        const auto policy = vrrTimingParametersForSession(session);
        expect(policy.playoutPrepareOnArrival == 1 &&
                   policy.renderStartAfterSubmissionUs == 0 &&
                   policy.renderStartPreserveLearnedLead == 1,
               "production must spend the playout cushion on preparation without squeezing learned lead");
        expect(policy.playoutSourceMappingDecoderOutput == 0 && policy.playoutSmoothingGainPerMille == 150,
               "preparation retains immutable mapping and current smoothing");
    }
    // An asynchronous renderer cannot learn a render-ahead budget from a CPU
    // completion wait that it deliberately avoids. Give it the existing
    // playout interval from the first frame, including after a decode stall,
    // without moving the presentation target or increasing standing delay.
    for (int mode : {0, 1, 2}) {
        auto session = config(120, 120);
        session.latencyMode = mode;
        auto policy = vrrTimingParametersForSession(session);
        policy.playoutPrepareOnArrival = 1;
        policy.renderStartAfterSubmissionUs = 0;
        policy.sourcePlayoutDelayUs = 10000;
        policy.playoutDelayAdaptive = 0;
        auto delayedPolicy = policy;
        delayedPolicy.playoutPrepareOnArrival = 0;
        delayedPolicy.renderStartAfterSubmissionUs = 6000;
        VrrTimingController early(session, true, policy);
        VrrTimingController delayed(session, true, delayedPolicy);
        bool gainedSlack = false;
        uint64_t previousSubmissionUs = 0;
        for (int i = 0; i < 240; ++i) {
            const uint32_t rtp = uint32_t(i * 750);
            const uint64_t outputUs = decodedTimeForRtp(1000000, rtp);
            const uint64_t decodeWaitUs = i % 23 == 0 ? 6000 : 1000;
            const uint64_t nowUs = std::max(outputUs, previousSubmissionUs) + decodeWaitUs;
            const auto a = early.schedule(frame(i + 1, rtp, true, outputUs), nowUs);
            const auto b = delayed.schedule(frame(i + 1, rtp, true, outputUs), nowUs);
            expect(a.targetUs == b.targetUs &&
                       a.playoutDelayUs == b.playoutDelayUs &&
                       a.latchedPresentation == b.latchedPresentation,
                   "early preparation must preserve target, buffer and presentation protection");
            expect(a.renderStartUs <= nowUs,
                   "production must prepare a ready source immediately, including after a decode stall");
            gainedSlack |= std::max(nowUs, a.renderStartUs) < b.renderStartUs;
            early.notePreparationDuration(750);
            delayed.notePreparationDuration(750);
            previousSubmissionUs = std::max(nowUs + 750, a.targetUs);
            early.noteSubmission(true, false, previousSubmissionUs);
            delayed.noteSubmission(true, false, previousSubmissionUs);
        }
        expect(gainedSlack, "early preparation must recover usable GPU execution time");
    }
}

void testSmoothnessLearningWindowTracksCadence()
{
    // The quarter-second learning window remains a replay-selectable policy
    // even though the production smoothness session no longer uses it.
    const auto learnedSampleCount = [](int sourceRateHz) {
        VrrSessionConfig session = config(sourceRateHz, 240);
        VrrTimingParameters parameters;
        parameters.readinessLearningWindowUs = 250000;
        parameters.readinessLearningSamples = 120;
        VrrTimingController controller(session, true, parameters);
        const uint64_t epochUs = 100000;
        for (int i = 0; i < 140; ++i) {
            const uint32_t timestamp = static_cast<uint32_t>(
                static_cast<uint64_t>(i) * 90000ULL /
                static_cast<uint64_t>(sourceRateHz));
            const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
            const VrrTimingDecision decision = controller.schedule(
                frame(i + 1, timestamp, true, decodedUs), decodedUs);
            controller.noteSubmission(true, false, decision.targetUs);
        }
        return controller.diagnostics().readinessSamples;
    };

    const size_t samples60 = learnedSampleCount(60);
    const size_t samples120 = learnedSampleCount(120);
    expect(samples60 == 16,
           "the quarter-second readiness window must retain its robust 16-sample floor at 60 FPS");
    expect(samples120 >= 29 && samples120 <= 31,
           "the quarter-second readiness window must contain about 30 samples at 120 FPS");
}

void testLongRunNearRefreshRtpCadence()
{
    constexpr int streamRateHz = 116;
    constexpr uint64_t rtpClockHz = 90000;
    constexpr uint64_t microsecondsPerSecond = 1000000;
    constexpr uint64_t initialUs = 1000000;
    VrrTimingController controller(config(streamRateHz, 120));
    controller.schedule(frame(0, 0, true, initialUs), initialUs);

    uint64_t maximumSourceErrorUs = 0;
    bool sawRebase = false;
    for (int i = 1; i <= 2000; ++i) {
        const uint32_t timestamp = static_cast<uint32_t>(
            (static_cast<uint64_t>(i) * rtpClockHz) / streamRateHz);
        const uint64_t expectedSourceUs = initialUs +
            (static_cast<uint64_t>(i) * microsecondsPerSecond) / streamRateHz;
        VrrTimingDecision decision = controller.schedule(
            frame(i, timestamp, true, expectedSourceUs), expectedSourceUs);
        sawRebase = sawRebase || decision.rebased;
        const uint64_t sourceErrorUs = decision.sourceTimeUs > expectedSourceUs ?
            decision.sourceTimeUs - expectedSourceUs :
            expectedSourceUs - decision.sourceTimeUs;
        maximumSourceErrorUs = std::max(maximumSourceErrorUs, sourceErrorUs);
    }

    expect(!sawRebase,
           "steady near-refresh RTP cadence must not rebase");
    expect(maximumSourceErrorUs <= 12,
           "90 kHz timestamp quantization must not accumulate source-clock drift");
}

void testQuantizedCadenceDoesNotOscillate()
{
    constexpr uint64_t epochUs = 1000000;
    struct CadenceCase {
        int streamRateHz;
        int captureRateHz;
        int displayRefreshHz;
    };
    const CadenceCase cases[] = {
        {59, 60, 60},
        {116, 120, 120},
        {138, 144, 144},
        {480, 480, 960},
    };

    for (const CadenceCase& cadence : cases) {
        VrrTimingController controller(config(cadence.streamRateHz,
                                              cadence.displayRefreshHz));
        VrrTimingDecision decision = controller.schedule(
            frame(0, 0, true, epochUs), epochUs);
        controller.noteSubmission(true, false, decision.targetUs);

        const uint64_t expectedPeriodUs =
            (1000000ULL + static_cast<uint64_t>(cadence.streamRateHz) / 2) /
            static_cast<uint64_t>(cadence.streamRateHz);
        uint64_t minimumPeriodUs = std::numeric_limits<uint64_t>::max();
        uint64_t maximumPeriodUs = 0;
        int64_t minimumReadyOffsetUs = std::numeric_limits<int64_t>::max();
        int64_t maximumReadyOffsetUs = std::numeric_limits<int64_t>::min();
        uint64_t maximumTimingBudgetUs = 0;
        bool sawRebase = false;
        const int warmupFrames = cadence.streamRateHz * 4;
        const int frameCount = cadence.streamRateHz * 12;

        for (int i = 1; i <= frameCount; ++i) {
            const uint32_t timestamp = quantizedRtpTimestamp(
                i, cadence.streamRateHz, cadence.captureRateHz);
            const uint64_t decodedUs = idealDecodedTime(
                epochUs, i, cadence.streamRateHz);
            decision = controller.schedule(
                frame(i, timestamp, true, decodedUs), decodedUs);
            controller.noteSubmission(true, false, decision.targetUs);
            sawRebase = sawRebase || decision.rebased;

            if (i > warmupFrames) {
                minimumPeriodUs = std::min(minimumPeriodUs,
                                            decision.sourcePeriodUs);
                maximumPeriodUs = std::max(maximumPeriodUs,
                                            decision.sourcePeriodUs);
                minimumReadyOffsetUs = std::min(minimumReadyOffsetUs,
                                                decision.readyOffsetUs);
                maximumReadyOffsetUs = std::max(maximumReadyOffsetUs,
                                                decision.readyOffsetUs);
                maximumTimingBudgetUs = std::max(
                    maximumTimingBudgetUs, controller.timingBudgetUs());
            }
        }

        const uint64_t readyOffsetSpanUs =
            maximumReadyOffsetUs > minimumReadyOffsetUs ?
                static_cast<uint64_t>(maximumReadyOffsetUs -
                                      minimumReadyOffsetUs) : 0;
        if (minimumPeriodUs != expectedPeriodUs ||
                maximumPeriodUs != expectedPeriodUs ||
                readyOffsetSpanUs > 2 ||
                maximumTimingBudgetUs > 2000) {
            std::fprintf(stderr,
                         "quantized cadence %d/%d: period=%llu..%llu "
                         "expected=%llu phase-span=%llu budget=%llu\n",
                         cadence.streamRateHz, cadence.captureRateHz,
                         static_cast<unsigned long long>(minimumPeriodUs),
                         static_cast<unsigned long long>(maximumPeriodUs),
                         static_cast<unsigned long long>(expectedPeriodUs),
                         static_cast<unsigned long long>(readyOffsetSpanUs),
                         static_cast<unsigned long long>(maximumTimingBudgetUs));
        }
        expect(!sawRebase,
               "steady host-quantized cadence must not rebase");
        expect(minimumPeriodUs == expectedPeriodUs &&
                   maximumPeriodUs == expectedPeriodUs,
               "host-quantized cadence must retain one exact learned period");
        expect(readyOffsetSpanUs <= 2,
               "host-quantized cadence must not create millisecond source phase motion");
        expect(maximumTimingBudgetUs <= 2000,
               "host-quantized cadence must not create a multi-millisecond readiness reserve");
    }
}

void testNegotiatedRateCeiling()
{
    constexpr uint64_t epochUs = 1000000;

    VrrTimingController candidateController(config(60, 120));
    candidateController.schedule(frame(0, 0, true, epochUs), epochUs);
    candidateController.schedule(
        frame(1, 1500, true, idealDecodedTime(epochUs, 1, 60)),
        idealDecodedTime(epochUs, 1, 60));
    VrrTimingDecision provisional = candidateController.schedule(
        frame(2, 1875, true, idealDecodedTime(epochUs, 2, 240)),
        idealDecodedTime(epochUs, 2, 240));
    VrrTimingDecision boundedCandidate = candidateController.schedule(
        frame(3, 2250, true, idealDecodedTime(epochUs, 3, 240)),
        idealDecodedTime(epochUs, 3, 240));
    const uint64_t sixtyFpsPeriodUs = (1000000ULL + 30) / 60;
    expect(provisional.phaseDiscontinuity &&
               !boundedCandidate.sourceRateChanged &&
               candidateController.sourcePeriodUs() == sixtyFpsPeriodUs,
           "a faster provisional candidate must remain at the negotiated rate");

    VrrTimingController fittedController(config(116, 120));
    fittedController.schedule(frame(0, 0, true, epochUs), epochUs);
    uint64_t minimumPeriodUs = std::numeric_limits<uint64_t>::max();
    for (int i = 1; i <= 128; ++i) {
        const uint32_t timestamp = static_cast<uint32_t>(i * 750);
        const uint64_t decodedUs = idealDecodedTime(epochUs, i, 120);
        const VrrTimingDecision decision = fittedController.schedule(
            frame(i, timestamp, true, decodedUs), decodedUs);
        if (i >= 16) {
            minimumPeriodUs = std::min(minimumPeriodUs,
                                        decision.sourcePeriodUs);
        }
    }
    const uint64_t negotiatedPeriodUs = (1000000ULL + 58) / 116;
    expect(minimumPeriodUs == negotiatedPeriodUs,
           "a fitted cadence must never imply a rate above the negotiated stream FPS");
}

void testSpacingGuardFeedback()
{
    VrrTimingController controller(config(60, 120));
    VrrTimingDecision first = controller.schedule(
        frame(1, 0, true, 100000), 100000);
    controller.noteSubmission(true, false, first.targetUs);
    const VrrTimingParameters& parameters = controller.parameters();
    const uint64_t raisedGuardUs = std::min(
        parameters.maximumAdaptiveGuardUs,
        first.guardUs + std::max<uint64_t>(parameters.guardStepUs, 300));
    controller.noteSpacingDeficit(300);
    expect(controller.guardUs() == raisedGuardUs,
           "a spacing deficit must raise the bounded guard directly");

    VrrTimingDecision second = controller.schedule(
        frame(2, 1500, true, 116666), 116666);
    expect(second.targetUs >=
               first.targetUs + controller.displayPeriodUs() + raisedGuardUs,
           "the raised guard must affect the next display-spacing floor");

    for (size_t i = 0; i < parameters.guardDecayFrames; ++i) {
        controller.noteSpacingDeficit(0);
    }
    expect(controller.guardUs() ==
               raisedGuardUs - std::min(parameters.guardStepUs,
                                         raisedGuardUs - first.guardUs),
           "a clean run must decay the guard by one small step");
}

void testNearRefreshRequestsLatchedPresentation()
{
    VrrTimingParameters headroomOnlyParameters;
    headroomOnlyParameters.cadenceStabilityLatchFrames = 0;
    for (int streamRateHz = 30; streamRateHz <= 115; ++streamRateHz) {
        VrrTimingController withHeadroom(
            config(streamRateHz, 120), true, headroomOnlyParameters);
        const VrrTimingDecision decision = withHeadroom.schedule(
            frame(1, 0, true, 100000), 100000);
        expect(!decision.latchedPresentation,
               "useful in-range cadences at 120 Hz must retain adaptive presentation");
    }

    VrrTimingController nearRefresh(
        config(116, 120), true, headroomOnlyParameters);
    VrrTimingDecision decision = nearRefresh.schedule(
        frame(1, 0, true, 100000), 100000);
    expect(decision.headroomUs == 188 && decision.latchedPresentation,
           "116 FPS must retain its 188 us near-refresh latched path");

    VrrTimingController boundary(
        config(115, 120), true, headroomOnlyParameters);
    decision = boundary.schedule(frame(1, 0, true, 100000), 100000);
    expect(decision.headroomUs == 263 && !decision.latchedPresentation,
           "115 FPS must remain adaptive with 263 us of headroom");

    VrrTimingController immutableMailbox(config(116, 120), false);
    decision = immutableMailbox.schedule(
        frame(1, 0, true, 100000), 100000);
    expect(!decision.latchedPresentation,
           "an immutable cadence-following backend must not be classified as fixed-vsync latched");
}

void testLatchedPresentationUsesFullHysteresis()
{
    const auto decayGuardToBase = [](VrrTimingController& controller,
                                     uint64_t baseGuardUs) {
        const VrrTimingParameters& parameters = controller.parameters();
        const size_t decayCycles = static_cast<size_t>(
            (controller.guardUs() - baseGuardUs +
             parameters.guardStepUs - 1) /
            parameters.guardStepUs);
        for (size_t i = 0;
             i < decayCycles * parameters.guardDecayFrames; ++i) {
            controller.noteSpacingDeficit(0);
        }
    };

    // 115 FPS begins between the entry and exit thresholds. A small guard
    // excursion should latch it, and returning to the base guard must not
    // immediately undo that decision while it remains inside the band.
    VrrTimingParameters hysteresisParameters;
    hysteresisParameters.cadenceStabilityLatchFrames = 0;
    VrrTimingController borderline(
        config(115, 120), true, hysteresisParameters);
    VrrTimingDecision decision = borderline.schedule(
        frame(1, 0, true, 100000), 100000);
    expect(!decision.latchedPresentation,
           "115 FPS must begin adaptive above the latch-entry threshold");

    const uint64_t baseGuardUs = decision.guardUs;
    const VrrTimingParameters& parameters = borderline.parameters();
    expect(decision.headroomUs >
               parameters.latchedPresentationHeadroomUs &&
           decision.headroomUs <
               parameters.latchedPresentationExitHeadroomUs,
           "115 FPS must begin inside the configured hysteresis band");
    const uint64_t latchDeficitUs =
        decision.headroomUs - parameters.latchedPresentationHeadroomUs + 1;
    borderline.noteSpacingDeficit(latchDeficitUs);
    decision = borderline.schedule(
        frame(2, 783, true, 108696), 108696);
    expect(decision.latchedPresentation,
           "a transient guard increase must select the safe latched path");

    decayGuardToBase(borderline, baseGuardUs);
    decision = borderline.schedule(
        frame(3, 1565, true, 117392), 117392);
    expect(decision.latchedPresentation,
           "base-guard recovery inside the hysteresis band must stay latched");

    // 113 FPS has enough base headroom to cross the exit threshold after the
    // same temporary guard protection decays.
    VrrTimingController recoverable(
        config(113, 120), true, hysteresisParameters);
    decision = recoverable.schedule(
        frame(1, 0, true, 100000), 100000);
    const uint64_t recoverableBaseGuardUs = decision.guardUs;
    expect(!decision.latchedPresentation &&
               decision.headroomUs >=
                   parameters.latchedPresentationExitHeadroomUs,
           "113 FPS must have enough base headroom to exit latching");
    recoverable.noteSpacingDeficit(
        decision.headroomUs -
            parameters.latchedPresentationHeadroomUs + 1);
    decision = recoverable.schedule(
        frame(2, 796, true, 108850), 108850);
    expect(decision.latchedPresentation,
           "a large guard excursion must latch 113 FPS temporarily");
    decayGuardToBase(recoverable, recoverableBaseGuardUs);
    decision = recoverable.schedule(
        frame(3, 1593, true, 117700), 117700);
    expect(!decision.latchedPresentation,
           "crossing the full exit threshold must restore adaptive presentation");


}

void testOptionalDisplayScaledLatchedPresentationBoundary()
{
    VrrTimingParameters parameters;
    parameters.cadenceStabilityLatchFrames = 0;
    parameters.latchedPresentationHeadroomPeriodNumerator = 3;
    parameters.latchedPresentationHeadroomPeriodDenominator = 1;
    parameters.latchedPresentationExitHeadroomPeriodNumerator = 13;
    parameters.latchedPresentationExitHeadroomPeriodDenominator = 4;

    const struct {
        int displayHz;
        int protectedRateHz;
        int adaptiveRateHz;
    } cases[] = {
        {60, 15, 14},
        {120, 30, 29},
        {144, 36, 35},
        {165, 42, 41},
    };
    for (const auto& value : cases) {
        VrrTimingController protectedController(
            config(value.protectedRateHz, value.displayHz), true,
            parameters);
        const VrrTimingDecision protectedDecision =
            protectedController.schedule(
                frame(1, 0, true, 100000), 100000);
        expect(protectedDecision.latchedPresentation,
               "three-period latch protection must scale with display refresh");

        VrrTimingController adaptiveController(
            config(value.adaptiveRateHz, value.displayHz), true,
            parameters);
        const VrrTimingDecision adaptiveDecision =
            adaptiveController.schedule(
                frame(1, 0, true, 100000), 100000);
        expect(!adaptiveDecision.latchedPresentation,
               "cadence beyond the three-period window must stay adaptive at every display rate");
    }
}

void testCadenceInstabilityUsesLatchedRecovery()
{
    constexpr int sourceRateHz = 90;
    constexpr uint64_t epochUs = 100000;
    VrrTimingController controller(config(sourceRateHz, 120));
    const VrrTimingParameters& parameters = controller.parameters();

    VrrTimingDecision decision = controller.schedule(
        frame(0, 0, true, epochUs), epochUs);
    expect(decision.latchedPresentation,
           "an uninitialized source phase must begin on the safe latched path");

    for (size_t i = 1;
         i <= parameters.cadenceStabilityLatchFrames; ++i) {
        const uint32_t timestamp = static_cast<uint32_t>(
            i * 90000ULL / sourceRateHz);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
        decision = controller.schedule(
            frame(static_cast<int>(i), timestamp, true, decodedUs),
            decodedUs);
        expect(decision.latchedPresentation,
               "cadence recovery must remain latched for the configured clean window");
    }

    const size_t adaptiveFrame = parameters.cadenceStabilityLatchFrames + 1;
    uint32_t timestamp = static_cast<uint32_t>(
        adaptiveFrame * 90000ULL / sourceRateHz);
    uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
    decision = controller.schedule(
        frame(static_cast<int>(adaptiveFrame), timestamp, true, decodedUs),
        decodedUs);
    expect(!decision.latchedPresentation,
           "a stable source with ample display headroom must recover adaptive VRR");

    const size_t hitchFrame = adaptiveFrame + 1;
    timestamp += 9000;
    decodedUs += 100000;
    decision = controller.schedule(
        frame(static_cast<int>(hitchFrame), timestamp, true, decodedUs),
        decodedUs);
    expect(decision.phaseDiscontinuity && decision.latchedPresentation,
           "a source hitch must immediately restore latched protection");
}

void testHeadroomAwareReadinessReserve()
{
    constexpr uint64_t epochUs = 1000000;
    VrrTimingController wideHeadroom(config(60, 120), false);
    VrrTimingController nearCeiling(config(116, 120), false);

    const auto train = [](VrrTimingController& controller, int rateHz) {
        constexpr uint64_t startUs = epochUs;
        VrrTimingDecision decision = controller.schedule(
            frame(0, 0, true, startUs), startUs);
        controller.noteSubmission(true, false, decision.targetUs);
        for (int i = 1; i <= 96; ++i) {
            const uint32_t timestamp = static_cast<uint32_t>(
                static_cast<uint64_t>(i) * 90000ULL /
                static_cast<uint64_t>(rateHz));
            const uint64_t sourceUs = decodedTimeForRtp(startUs, timestamp);
            const uint64_t tailUs = i % 4 == 0 ? 3000 : 0;
            decision = controller.schedule(
                frame(i, timestamp, true, sourceUs + tailUs),
                sourceUs + tailUs);
            controller.noteSubmission(true, false, decision.targetUs);
        }
    };

    train(wideHeadroom, 60);
    train(nearCeiling, 116);

    if (wideHeadroom.timingBudgetUs() + 500 >=
            nearCeiling.timingBudgetUs()) {
        std::fprintf(stderr,
                     "headroom budgets: wide=%llu us near=%llu us\n",
                     static_cast<unsigned long long>(wideHeadroom.timingBudgetUs()),
                     static_cast<unsigned long long>(nearCeiling.timingBudgetUs()));
    }
    expect(wideHeadroom.timingBudgetUs() + 500 <
               nearCeiling.timingBudgetUs(),
           "cadence headroom must absorb arrival spread without carrying the near-ceiling reserve at lower rates");
    expect(nearCeiling.timingBudgetUs() >= 3000,
           "near-ceiling cadence-following presentation must retain a real burst cushion");
}

void testCadenceGapAndRateChange()
{
    VrrTimingController controller(config(120, 120));
    uint32_t timestamp = 0;
    uint64_t decodedUs = 100000;
    controller.schedule(frame(0, timestamp, true, decodedUs), decodedUs);
    for (int i = 1; i <= 8; ++i) {
        timestamp += 1500;
        decodedUs += 16666;
        controller.schedule(frame(i, timestamp, true, decodedUs), decodedUs);
    }
    expect(controller.sourcePeriodUs() == 16667,
           "stable raw cadence must retain rational RTP conversion carry");

    timestamp += 6000;
    decodedUs += 66666;
    VrrTimingDecision gap = controller.schedule(
        frame(9, timestamp, true, decodedUs), decodedUs);
    expect(!gap.cadenceEligible,
           "one large interval must be isolated from cadence adaptation");
    expect(controller.sourcePeriodUs() == 16667,
           "an isolated gap must not retune the source period");

    timestamp += 1500;
    decodedUs += 16666;
    controller.schedule(frame(10, timestamp, true, decodedUs), decodedUs);

    VrrTimingDecision accepted;
    for (int i = 0; i < 16; ++i) {
        timestamp += 1000;
        decodedUs += 11111;
        accepted = controller.schedule(
            frame(11 + i, timestamp, true, decodedUs), decodedUs);
    }
    const double learnedRateHz = 1000000.0 /
        static_cast<double>(controller.sourcePeriodUs());
    expect(std::abs(learnedRateHz - 90.0) < 1.0,
           "a cumulative segment must converge on a non-atomic new rate");
}

void testFutureSourceProjectionReseedsPhase()
{
    VrrTimingController controller(config(116, 120));
    constexpr uint64_t initialUs = 1000000;
    controller.schedule(frame(0, 0, true, initialUs), initialUs);

    constexpr uint64_t nowUs = initialUs + 8621;
    VrrTimingDecision recovered = controller.schedule(
        frame(50, 38024, true, nowUs), nowUs);

    expect(!recovered.rebased && recovered.sourceIntervalUs != 0 &&
               recovered.targetUs < nowUs + 2 * 8621,
           "a source projection ahead of decoded local time must reseed phase without discarding cadence");
}

void testDecodeTailAdaptation()
{
    VrrTimingController controller(config());
    uint32_t timestamp = 0;
    uint64_t sourceUs = 1000000;
    controller.schedule(frame(0, timestamp, true, sourceUs), sourceUs);

    for (int i = 1; i <= 16; ++i) {
        timestamp += 1500;
        sourceUs += 16666;
        const uint64_t tailUs = i % 4 == 0 ? 5000 : 0;
        VrrTimingDecision decision = controller.schedule(
            frame(i, timestamp, true, sourceUs + tailUs), sourceUs + tailUs);
        controller.notePreparationDuration(1000);
        controller.noteSubmission(true, false, decision.targetUs);
    }

    expect(controller.renderLeadUs() >=
               1000 + controller.parameters().renderLeadSlackUs,
           "preparation duration must include render slack");
    expect(controller.diagnostics().readinessDemandUs >
               controller.parameters().minimumReadinessReserveUs,
           "positive readiness tail must grow learned readiness demand");
}

void testRateChangeReseedsReadinessBudget()
{
    VrrTimingController controller(config(116, 120));
    controller.schedule(frame(0, 0, true, 100000), 100000);
    VrrTimingDecision provisional = controller.schedule(
        frame(1, 3000, true, 133333), 133333);
    VrrTimingDecision accepted = controller.schedule(
        frame(2, 6000, true, 166666), 166666);

    expect(provisional.phaseDiscontinuity &&
               accepted.sourceRateChanged &&
               accepted.readinessBudgetUs == accepted.readyOffsetUs &&
               std::abs(1000000.0 /
                   static_cast<double>(accepted.sourcePeriodUs) - 30.0) < 0.1,
            "a confirmed major slowdown must reseed phase and readiness after two intervals");
}

void testFractionalQuantizedCadenceLearning()
{
    constexpr uint64_t epochUs = 1000000;

    for (int rateHz = 30; rateHz <= 116; ++rateHz) {
        VrrTimingController controller(config(116, 120));
        controller.schedule(frame(0, 0, true, epochUs), epochUs);

        const int sampleCount = std::max(160, rateHz * 2);
        bool rebased = false;
        for (int i = 1; i <= sampleCount; ++i) {
            const uint32_t timestamp = quantizedRtpTimestamp(i, rateHz);
            const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
            const VrrTimingDecision decision = controller.schedule(
                frame(i, timestamp, true, decodedUs), decodedUs);
            rebased = rebased || decision.rebased;
        }

        const double learnedRateHz = 1000000.0 /
            static_cast<double>(controller.sourcePeriodUs());
        expect(!rebased,
               "fractional capture-clock cadence must not rebase");
        if (std::abs(learnedRateHz - rateHz) >= 0.75) {
            std::fprintf(stderr,
                         "cadence mismatch: requested=%d learned=%.3f\n",
                         rateHz, learnedRateHz);
        }
        expect(std::abs(learnedRateHz - rateHz) < 0.75,
               "cumulative cadence learning must represent arbitrary rates continuously");
    }
}

void testCutsceneRecoveryAndHitchIsolation()
{
    constexpr uint64_t epochUs = 1000000;
    VrrTimingController controller(config(116, 120));
    controller.schedule(frame(0, 0, true, epochUs), epochUs);

    int frameNumber = 0;
    uint32_t timestamp = 0;
    for (int i = 1; i <= 140; ++i) {
        frameNumber = i;
        timestamp = quantizedRtpTimestamp(i, 116);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
        controller.schedule(frame(frameNumber, timestamp, true, decodedUs),
                            decodedUs);
    }
    const uint64_t stablePeriodUs = controller.sourcePeriodUs();

    timestamp += 3750;
    ++frameNumber;
    uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
    VrrTimingDecision hitch = controller.schedule(
        frame(frameNumber, timestamp, true, decodedUs), decodedUs);
    expect(hitch.phaseDiscontinuity && !hitch.sourceRateChanged,
           "one large hitch must start only a provisional cadence segment");

    timestamp += 750;
    ++frameNumber;
    decodedUs = decodedTimeForRtp(epochUs, timestamp);
    VrrTimingDecision recovered = controller.schedule(
        frame(frameNumber, timestamp, true, decodedUs), decodedUs);
    expect(recovered.phaseDiscontinuity && !recovered.sourceRateChanged &&
               std::abs(static_cast<int64_t>(controller.sourcePeriodUs()) -
                        static_cast<int64_t>(stablePeriodUs)) < 100,
           "a normal successor must abandon a hitch without poisoning the stable rate");

    VrrTimingDecision cutscene;
    for (int i = 0; i < 2; ++i) {
        timestamp += 3000;
        ++frameNumber;
        decodedUs = decodedTimeForRtp(epochUs, timestamp);
        cutscene = controller.schedule(
            frame(frameNumber, timestamp, true, decodedUs), decodedUs);
    }
    expect(cutscene.sourceRateChanged &&
               std::abs(1000000.0 /
                   static_cast<double>(controller.sourcePeriodUs()) - 30.0) < 0.1,
           "a 30 FPS cutscene must be accepted after two confirming intervals");

    VrrTimingDecision acceleration;
    for (int i = 0; i < 2; ++i) {
        timestamp += 750;
        ++frameNumber;
        decodedUs = decodedTimeForRtp(epochUs, timestamp);
        acceleration = controller.schedule(
            frame(frameNumber, timestamp, true, decodedUs), decodedUs);
    }
    expect(acceleration.sourceRateChanged && acceleration.latchedPresentation,
           "returning to the tight high-rate range must recover provisionally in latched mode");
}

void testModerateSlowdownSelfHealsPhase()
{
    constexpr uint64_t epochUs = 1000000;
    VrrTimingController controller(config(60, 120));
    controller.schedule(frame(0, 0, true, epochUs), epochUs);

    int frameNumber = 0;
    uint32_t timestamp = 0;
    for (int i = 1; i <= 80; ++i) {
        frameNumber = i;
        timestamp += 1500;
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
        controller.schedule(frame(frameNumber, timestamp, true, decodedUs),
                            decodedUs);
    }

    bool healedPhase = false;
    for (int i = 0; i < 40; ++i) {
        ++frameNumber;
        timestamp += 3000;
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
        const VrrTimingDecision decision = controller.schedule(
            frame(frameNumber, timestamp, true, decodedUs), decodedUs);
        healedPhase = healedPhase || decision.phaseDiscontinuity;
        if (i == 3) {
            expect(healedPhase,
                   "a moderate slowdown must heal phase within four frames even without a major-rate candidate");
        }
    }

    const double learnedRateHz = 1000000.0 /
        static_cast<double>(controller.sourcePeriodUs());
    expect(std::abs(learnedRateHz - 30.0) < 0.75,
           "a moderate slowdown must converge to its cumulative cadence after phase recovery");

    ++frameNumber;
    timestamp += 1500;
    const uint64_t acceleratedDecodeUs = decodedTimeForRtp(epochUs,
                                                            timestamp);
    const VrrTimingDecision accelerated = controller.schedule(
        frame(frameNumber, timestamp, true, acceleratedDecodeUs),
        acceleratedDecodeUs);
    expect(accelerated.phaseDiscontinuity &&
               accelerated.targetUs < acceleratedDecodeUs + 5000,
           "a frame arriving ahead of a slower cutscene clock must bypass stale latency immediately");
}

void testContinuousCadenceSweep()
{
    constexpr uint64_t epochUs = 1000000;
    VrrTimingController controller(config(116, 120));
    controller.schedule(frame(0, 0, true, epochUs), epochUs);

    int frameNumber = 0;
    long double idealRtpTicks = 0.0L;
    uint32_t timestamp = 0;
    double maximumRateErrorHz = 0.0;
    bool rebased = false;

    const auto runRate = [&](int rateHz) {
        const uint32_t startTimestamp = timestamp;
        const int startFrame = frameNumber;
        for (int i = 0; i < rateHz; ++i) {
            idealRtpTicks += 90000.0L /
                static_cast<long double>(rateHz);
            const uint64_t captureTick = static_cast<uint64_t>(
                std::llround(idealRtpTicks / 750.0L));
            timestamp = static_cast<uint32_t>(captureTick * 750ULL);
            ++frameNumber;
            const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
            const VrrTimingDecision decision = controller.schedule(
                frame(frameNumber, timestamp, true, decodedUs), decodedUs);
            rebased = rebased || decision.rebased;
        }

        const uint32_t elapsedTicks = timestamp - startTimestamp;
        const int elapsedFrames = frameNumber - startFrame;
        const double measuredRateHz = elapsedTicks == 0 ? 0.0 :
            static_cast<double>(elapsedFrames) * 90000.0 /
                static_cast<double>(elapsedTicks);
        const double learnedRateHz = 1000000.0 /
            static_cast<double>(controller.sourcePeriodUs());
        maximumRateErrorHz = std::max(
            maximumRateErrorHz,
            std::abs(learnedRateHz - measuredRateHz));
    };

    for (int rateHz = 116; rateHz >= 30; --rateHz) {
        runRate(rateHz);
    }
    for (int rateHz = 31; rateHz <= 116; ++rateHz) {
        runRate(rateHz);
    }

    if (maximumRateErrorHz >= 2.0) {
        std::fprintf(stderr, "sweep maximum rate error: %.3f Hz\n",
                     maximumRateErrorHz);
    }
    expect(!rebased,
           "a continuous cadence sweep must not reset the source epoch");
    expect(maximumRateErrorHz < 2.0,
           "a one FPS-per-second sweep must remain within two FPS of measured cadence");
}

void testQuantizedCadenceProjectsSmoothTargets()
{
    constexpr uint64_t epochUs = 1000000;
    constexpr uint64_t expectedPeriodUs = 10000;
    uint64_t intervalError[2] = {};
    for (int smoothing : {0, 1}) {
        auto session = config(116, 120);
        session.smoothFrameTiming = smoothing != 0;
        session = vrrProductionSessionConfig(session);
        VrrTimingController controller(session, true, vrrTimingParametersForSession(session));
        uint64_t previousTargetUs = 0;
        for (int i = 0; i <= 300; ++i) {
            const uint32_t timestamp = quantizedRtpTimestamp(i, 100);
            const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
            const auto decision = controller.schedule(
                frame(i, timestamp, true, decodedUs), decodedUs);
            expect(decision.targetUs >= decodedUs &&
                       decision.targetUs >= controller.earliestSubmissionUs(),
                   "quantized source output must retain readiness and display spacing");
            controller.noteSubmission(true, false, decision.targetUs);
            if (i > 180) {
                const uint64_t spacingUs = decision.targetUs - previousTargetUs;
                intervalError[smoothing] += std::max(spacingUs, expectedPeriodUs) -
                    std::min(spacingUs, expectedPeriodUs);
            }
            previousTargetUs = decision.targetUs;
        }
    }
    // Compare the actual production policies; the retired extra-frame budget
    // no longer defines a sub-500-us metronomic target for this quantized input.
    expect(intervalError[1] * 2 < intervalError[0],
           "production smoothing must reduce quantized-source interval error without an extra-frame budget");
}

void testSkippedLocalFramePreservesCadence()
{
    constexpr uint64_t epochUs = 1000000;
    VrrTimingController controller(config(100, 120));
    VrrTimingDecision decision = controller.schedule(
        frame(0, 0, true, epochUs), epochUs);

    for (int i = 1; i <= 180; ++i) {
        const uint32_t timestamp = quantizedRtpTimestamp(i, 100);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
        decision = controller.schedule(
            frame(i, timestamp, true, decodedUs), decodedUs);
    }
    const uint64_t stablePeriodUs = controller.sourcePeriodUs();

    const int successor = 183;
    const uint32_t timestamp = quantizedRtpTimestamp(successor, 100);
    const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
    decision = controller.schedule(
        frame(successor, timestamp, true, decodedUs), decodedUs);

    expect(!decision.rebased && decision.cadenceEligible &&
               std::abs(static_cast<int64_t>(controller.sourcePeriodUs()) -
                        static_cast<int64_t>(stablePeriodUs)) < 100,
           "a latest-frame queue replacement must advance by frame delta without resetting cadence");
}

void testSchedulerDelayFeedback()
{
    VrrTimingController controller(config());
    VrrTimingDecision first = controller.schedule(
        frame(1, 0, true, 100000), 100000);
    controller.noteSchedulerDelays(600, 300, true);
    controller.noteSubmission(true, false, first.targetUs);

    VrrTimingDecision second = controller.schedule(
        frame(2, 1500, true, 116666), 116666);
    expect(second.renderWakeLeadUs == 600 &&
               second.targetWakeLeadUs == 300 &&
               second.renderStartUs + second.renderLeadUs +
                   second.renderWakeLeadUs == second.targetUs,
           "render and final-target wake delays must learn independently");

    for (int i = 0; i < 40; ++i) {
        controller.noteSchedulerDelays(0, 0, false);
    }
    expect(controller.targetWakeLeadUs() == 300,
           "frames without a coarse target sleep must retain learned delay");
}

void trainRenderDurations(VrrTimingController& controller,
                          uint64_t baselineUs, uint64_t tailUs)
{
    constexpr uint64_t epochUs = 1000000;
    for (int i = 0; i < 96; ++i) {
        const uint64_t decodedUs = epochUs +
            static_cast<uint64_t>(i) * 10000ULL;
        VrrTimingDecision decision = controller.schedule(
            frame(i, static_cast<uint32_t>(i * 900), true, decodedUs),
            decodedUs);
        controller.notePreparationDuration(i == 95 ? tailUs : baselineUs);
        controller.noteSubmission(true, false, decision.targetUs);
    }
}

void testRenderBaselineDoesNotConsumePacingBudget()
{
    VrrTimingController controller(config(100, 120));
    trainRenderDurations(controller, 9000, 9000);

    const VrrTimingDiagnostics diagnostics = controller.diagnostics();
    expect(diagnostics.renderBaselineUs == 9000 &&
               controller.renderLeadUs() == 9000,
           "stable render work above the old ceiling must be learned in full");
    expect(diagnostics.renderInsuranceUs == 0,
           "stable render work must not be counted as pacing insurance");
    expect(controller.timingBudgetUs() <=
               diagnostics.pacingLatencyBudgetUs,
           "unavoidable render baseline must not consume the half-scanout budget");
}

void testRenderTailSharesHalfScanoutBudget()
{
    VrrTimingController controller(config(100, 120));
    trainRenderDurations(controller, 4000, 10000);

    const VrrTimingDiagnostics diagnostics = controller.diagnostics();
    expect(diagnostics.renderBaselineUs == 4000,
           "render baseline must use the configured median percentile");
    expect(controller.renderLeadUs() >= 7600 &&
               controller.renderLeadUs() <= 7700,
           "render p99 must be capped to baseline plus available tail insurance");
    expect(diagnostics.renderInsuranceUs >= 3600 &&
               diagnostics.renderInsuranceUs <= 3700,
           "only the p99-minus-median render tail must consume pacing budget");
    expect(diagnostics.appliedReadinessReserveUs == 500,
           "render tail must leave the minimum readiness reserve intact");
    expect(controller.timingBudgetUs() <=
               diagnostics.pacingLatencyBudgetUs,
           "readiness and render-tail insurance must not exceed half a scanout");
}

void testPacingBudgetAtExtremeRefreshRates()
{
    for (int displayRefreshHz = 60; displayRefreshHz <= 2000;
            displayRefreshHz += 37) {
        VrrTimingController controller(config(100, displayRefreshHz));
        trainRenderDurations(controller, 400, 2400);
        const VrrTimingDiagnostics diagnostics = controller.diagnostics();
        if (controller.timingBudgetUs() >
                diagnostics.pacingLatencyBudgetUs) {
            std::fprintf(stderr,
                         "refresh %d: pacing=%llu budget=%llu baseline=%llu insurance=%llu readiness=%llu\n",
                         displayRefreshHz,
                         static_cast<unsigned long long>(
                             controller.timingBudgetUs()),
                         static_cast<unsigned long long>(
                             diagnostics.pacingLatencyBudgetUs),
                         static_cast<unsigned long long>(
                             diagnostics.renderBaselineUs),
                         static_cast<unsigned long long>(
                             diagnostics.renderInsuranceUs),
                         static_cast<unsigned long long>(
                             diagnostics.appliedReadinessReserveUs));
        }
        expect(controller.timingBudgetUs() <=
                   diagnostics.pacingLatencyBudgetUs,
               "pacing reserve must remain bounded from 60 through 2000 Hz");
    }
}





void testTargetWaiterBoundaries()
{
    uint64_t nowUs = 0;
    uint64_t requestedCoarseSleepUs = 0;
    VrrTargetWaiterHooks hooks;
    hooks.nowUs = [&nowUs]() { return nowUs; };
    hooks.sleepForUs = [&nowUs, &requestedCoarseSleepUs](uint64_t durationUs) {
        requestedCoarseSleepUs += durationUs;
        nowUs += durationUs;
    };
    hooks.yield = [&nowUs]() { nowUs += 25; };
    VrrTargetWaiter waiter(hooks);

    VrrTargetWaitResult result = waiter.waitUntil(1000);
    expect(requestedCoarseSleepUs == 500 && result.finalNowUs >= 1000,
           "waiter must sleep to the active-wait boundary");
    expect(result.initialNowUs == 0 && result.activeWaitUs == 500 &&
               result.coarseSleepCount == 1 &&
               result.coarseSleepRequestedUs == 500 &&
               result.coarseSleepRequestedWakeUs == 500 &&
               result.coarseSleepReturnUs == 500 &&
               result.activeWaitEntered &&
               result.activeWaitStartUs == 500 &&
               result.activeWaitLimitUs == 1000 &&
               result.activeWaitYieldCount == 20,
           "waiter must expose the exact coarse and active lifecycle");

    nowUs = 0;
    requestedCoarseSleepUs = 0;
    result = waiter.waitUntil(1000, 400);
    expect(requestedCoarseSleepUs == 100 && result.finalNowUs >= 1000,
           "learned scheduler delay must wake the final wait earlier");
    expect(result.activeWaitUs == 900 &&
               result.coarseSleepRequestedWakeUs == 100 &&
               result.coarseSleepReturnUs == 100,
           "waiter lifecycle must include the bounded learned wake lead");

    uint64_t delayedNowUs = 0;
    VrrTargetWaiterHooks delayedHooks;
    delayedHooks.nowUs = [&delayedNowUs]() { return delayedNowUs; };
    delayedHooks.sleepForUs = [&delayedNowUs](uint64_t durationUs) {
        delayedNowUs += durationUs + 900;
    };
    delayedHooks.yield = [&delayedNowUs]() { delayedNowUs += 25; };
    VrrTargetWaiter delayedWaiter(delayedHooks);
    result = delayedWaiter.waitUntil(5000);
    expect(result.schedulerDelayValid && result.schedulerDelayUs == 400 &&
               result.finalNowUs == 5400 &&
               result.coarseSleepRequestedWakeUs == 4500 &&
               result.coarseSleepReturnUs == 5400 &&
               !result.activeWaitEntered,
           "coarse wake feedback must measure overshoot beyond the active margin");

    delayedNowUs = 0;
    result = delayedWaiter.waitUntil(5000, 400);
    expect(result.schedulerDelayValid && result.schedulerDelayUs == 400 &&
               result.finalNowUs == 5000 &&
               result.coarseSleepRequestedWakeUs == 4100 &&
               result.coarseSleepReturnUs == 5000 &&
               !result.activeWaitEntered,
           "learned wake delay must correct final-target overshoot");

    nowUs = 100;
    result = waiter.waitUntil(100);
    expect(result.deadlineAlreadyElapsed && result.finalNowUs == 100,
           "an elapsed deadline must return without waiting");
    expect(result.initialNowUs == 100 && result.activeWaitUs == 500 &&
               result.coarseSleepCount == 0 &&
               !result.activeWaitEntered,
           "elapsed waiter lifecycle must remain explicit and empty");

    unsigned int stalledSleepCalls = 0;
    unsigned int stalledYieldCalls = 0;
    VrrTargetWaiterHooks stalledHooks;
    stalledHooks.nowUs = []() { return 0ULL; };
    stalledHooks.sleepForUs = [&stalledSleepCalls](uint64_t) {
        ++stalledSleepCalls;
    };
    stalledHooks.yield = [&stalledYieldCalls]() { ++stalledYieldCalls; };
    VrrTargetWaiter stalled(stalledHooks);
    result = stalled.waitUntil(1000);
    expect(result.finalNowUs == 0 && stalledSleepCalls == 2 &&
               stalledYieldCalls == 64 &&
               result.coarseSleepClockStalled &&
               result.activeWaitClockStalled &&
               result.coarseSleepCount == 2 &&
               result.activeWaitYieldCount == 64,
           "a non-advancing clock must not create unbounded active spinning");

    uint64_t slowNowUs = 0;
    uint64_t slowYieldCalls = 0;
    VrrTargetWaiterHooks slowHooks;
    slowHooks.nowUs = [&slowNowUs]() { return slowNowUs; };
    slowHooks.sleepForUs = [&slowNowUs](uint64_t durationUs) {
        slowNowUs += durationUs;
    };
    slowHooks.yield = [&slowNowUs, &slowYieldCalls]() {
        ++slowYieldCalls;
        if (slowYieldCalls % 16 == 0) {
            ++slowNowUs;
        }
    };
    VrrTargetWaiter slowWaiter(slowHooks);
    result = slowWaiter.waitUntil(1000);
    expect(result.finalNowUs == 1000 &&
               result.activeWaitYieldCount == 8000 &&
               !result.activeWaitClockStalled &&
               !result.activeWaitYieldLimitReached,
           "an advancing clock must reach the deadline even when a fast CPU yields more than 4096 times");
}

void testNativeSynchronizedPresentation()
{
    for (int rate : {30, 60, 116, 120}) {
        auto session = config(rate, 120);
        auto policy = vrrTimingParametersForSession(session, true);
        expect(policy.nativeSynchronizedPresentation == 1 &&
                   vrrTimingParametersForSession(session).nativeSynchronizedPresentation == 0,
               "constant synchronization must be selected from the actual presenter");
        // An exploratory adaptive-only request cannot turn a synchronized
        // native presenter into a tearing presenter.
        policy.playoutAdaptiveOnly = 1;
        VrrTimingController controller(session, true, policy);
        for (int i = 0; i < 80; ++i) {
            const uint32_t rtp = uint32_t(uint64_t(i) * 90000 / rate);
            const uint64_t at = 1000000 + uint64_t(rtp) * 1000 / 90;
            const auto d = controller.schedule(frame(i + 1, rtp, true, at), at);
            expect(d.latchedPresentation,
                   "native synchronized mode must remain latched at startup and every source rate");
            controller.noteSubmission(true, false, d.targetUs);
            expect(controller.earliestSubmissionUs() == 0,
                   "native synchronization must not add a software display-period wait");
        }
        // The capability cannot bypass spacing on an incapable backend.
        VrrTimingController incapable(session, false, policy);
        const auto d = incapable.schedule(frame(1, 0, true, 1000000), 1000000);
        incapable.noteSubmission(true, false, d.targetUs);
        expect(!d.latchedPresentation && incapable.earliestSubmissionUs() != 0,
               "an incapable presenter must retain its software floor");
    }
}

VrrTimingParameters nativeDeadlineTestParameters(const VrrSessionConfig& session)
{
    auto policy = vrrTimingParametersForSession(session, true, true);
    // Isolate native deadline admission from adaptive padding and smoothing.
    // Readiness still goes through schedule(), and production's nonzero guard
    // and disabled historical latched floor remain selected.
    policy.playoutDelayStartUs = 2000;
    policy.playoutDelayMinimumUs = 2000;
    policy.playoutDelayMaximumUs = 2000;
    policy.playoutDelayStartSeedUs = 2000;
    policy.playoutDelayStartPeriodPerMille = 0;
    policy.playoutDelayMaximumPeriodPerMille = 0;
    policy.playoutDelayCapSourcePeriodPerMille = 0;
    policy.playoutSmoothingGainPerMille = 0;
    policy.playoutSmoothingReserveMaxUs = 0;
    policy.playoutCatchupPerMille = 0;
    return policy;
}

void testNativeScheduledRetimingBounds()
{
    for (int rate : {60, 90, 120, 144, 240}) for (int mode : {0, 1, 2}) {
        auto session = config(rate, std::max(120, rate));
        session.latencyMode = mode;
        const auto scheduled = vrrTimingParametersForSession(session, true, true);
        const auto paced = vrrTimingParametersForSession(session, true, false);
        const auto tolerance = uint64_t(session.timingOptions.resolved(mode).toleranceUs);
        expect(scheduled.smoothingMode == 4 &&
                   scheduled.playoutSmoothingMaxLagUs == tolerance &&
                   scheduled.playoutSmoothingReserveMaxUs == 0 &&
                   paced.smoothingMode == 2 &&
                   paced.playoutSmoothingMaxLagUs == 6000 && paced.playoutSmoothingReserveMaxUs == 3000 &&
                   paced.playoutSmoothingOwnMovement == 1 && scheduled.playoutSmoothingOwnMovement == 0,
               "native deadlines retain their bound while worker-paced smoothing owns its bounded retiming");
        VrrTimingController controller(session, true, scheduled);
        uint32_t ticks = 0;
        uint64_t previousTarget = 0;
        for (int i = 0; i < 300; ++i) {
            ticks += i % 71 == 70 ? 4590U : uint32_t(90000 / rate);
            const uint64_t at = decodedTimeForRtp(1000000, ticks) + uint64_t(i % 5) * 100;
            const auto d = controller.schedule(frame(i + 1, ticks, true, at), at);
            expect(std::abs(d.cadenceSmoothingUs) <= int64_t(tolerance) && controller.smoothingReserveUs() == 0,
                   "rate changes and irregular readiness must retain the shared tolerance bound without a reserve");
            expect(!previousTarget || d.targetUs >= previousTarget + controller.displayPeriodUs(),
                   "bounded native retiming must retain the accepted display deadline floor");
            controller.notePreparationDuration(1000, 0, at + 1000);
            controller.noteSubmission(true, false, std::max(d.targetUs, at + 1000), at + 1000);
            previousTarget = d.targetUs;
        }
        session.smoothFrameTiming = false;
        const auto disabled = vrrTimingParametersForSession(session, true, true);
        expect(disabled.playoutSmoothingGainPerMille == 0 && disabled.playoutSmoothingReserveMaxUs == 0,
               "the independent smoothing preference must still disable native retiming");
    }
}

void testNativeScheduledDeadlinesSurviveCompressedCadence()
{
    const auto session = config(120, 120);
    const auto policy = nativeDeadlineTestParameters(session);
    expect(policy.nativeScheduledPresentation == 1 &&
               policy.nativeSynchronizedPresentation == 1 &&
               policy.latchedFloorDisabled == 1,
           "native scheduling must have a separate capability from historical synchronization");
    VrrTimingController controller(session, true, policy);
    uint32_t timestamp = 0;
    uint64_t priorTargetUs = 0;
    unsigned compressedTargets = 0;
    for (int i = 0; i < 400; ++i) {
        // Alternate 3.33/13.33 ms source stamps while retaining 120 FPS on
        // average. Two such accepted presents must not target one refresh.
        if (i) timestamp += i % 2 ? 300 : 1200;
        const uint64_t at = decodedTimeForRtp(1000000, timestamp);
        const auto d = controller.schedule(frame(i + 1, timestamp, true, at), at);
        expect(d.latchedPresentation,
               "native scheduled frames must retain synchronized presentation");
        if (priorTargetUs) {
            expect(d.targetUs >= priorTargetUs + controller.displayPeriodUs(),
                   "compressed source timestamps must not compress native display deadlines");
            if (d.originalTargetUs < priorTargetUs + controller.displayPeriodUs()) {
                ++compressedTargets;
                expect(d.presentationFloorPushUs != 0,
                       "native deadline admission must expose its spacing correction");
            }
        }
        controller.noteSubmission(true, false, d.targetUs);
        expect(controller.earliestSubmissionUs() == d.targetUs + controller.displayPeriodUs(),
               "native scheduled admission must reserve one display period without a guard");
        priorTargetUs = d.targetUs;
    }
    expect(compressedTargets > 100,
           "compressed-cadence regression must exercise the native deadline floor");
}

void testNativeScheduledLogicalAnchorDoesNotAccumulateCpuOverhead()
{
    const auto session = config(120, 120);
    const auto policy = nativeDeadlineTestParameters(session);
    VrrTimingController controller(session, true, policy);
    uint64_t workerAvailableUs = 0;
    uint64_t initialPhaseUs = 0;
    uint64_t firstTargetUs = 0;
    uint64_t finalTargetUs = 0;
    constexpr uint64_t holdOvershootUs = 75;
    constexpr int frames = 7200;
    for (int i = 0; i < frames; ++i) {
        const uint32_t timestamp = uint32_t(i * 750);
        const uint64_t readyUs = decodedTimeForRtp(1000000, timestamp);
        const uint64_t nowUs = std::max(readyUs, workerAvailableUs);
        const auto d = controller.schedule(frame(i + 1, timestamp, true, readyUs), nowUs);
        // Native enqueue finishes before the accepted display deadline. The
        // subsequent CPU hold overshoots by 75 us, but that is not a later
        // display request and must not shift the next deadline.
        expect(d.targetUs >= nowUs + 100,
               "exact-period fixture must leave time for native enqueue before its deadline");
        const uint64_t nativeEnqueueUs = d.targetUs - 100;
        const uint64_t logicalSubmissionUs = std::max(nativeEnqueueUs, d.targetUs);
        controller.noteSubmission(true, false, logicalSubmissionUs);
        workerAvailableUs = d.targetUs + holdOvershootUs;
        if (i == 0) {
            initialPhaseUs = d.targetUs - readyUs;
            firstTargetUs = d.targetUs;
        }
        expect(d.targetUs - readyUs == initialPhaseUs,
               "CPU hold overshoot must not accumulate into native deadline phase");
        expect(controller.earliestSubmissionUs() == d.targetUs + controller.displayPeriodUs(),
               "the native deadline floor must exclude both CPU hold overshoot and tearing guard");
        finalTargetUs = d.targetUs;
    }
    expect(finalTargetUs - firstTargetUs == uint64_t(frames - 1) * 1000000 / 120,
           "one minute of exact source cadence must retain its rational source phase");
}

void testNativeScheduledFailureAndCancellationDoNotConsumeSlots()
{
    const auto session = config(120, 120);
    VrrTimingController controller(session, true, nativeDeadlineTestParameters(session));
    auto d = controller.schedule(frame(1, 0, true, 1000000), 1000000);
    controller.noteSubmission(false, true, d.targetUs);
    expect(controller.lastSubmissionUs() == 0 && controller.earliestSubmissionUs() == 0,
           "cancellation before native enqueue must not create a display anchor");

    d = controller.schedule(frame(2, 750, true, 1008333), 1008333);
    controller.noteSubmission(true, false, d.targetUs);
    const uint64_t acceptedUs = d.targetUs;
    for (int i = 3; i <= 4; ++i) {
        const uint64_t at = decodedTimeForRtp(1000000, uint32_t((i - 1) * 750));
        d = controller.schedule(frame(i, uint32_t((i - 1) * 750), true, at), at);
        controller.noteSubmission(false, i == 4, d.targetUs + 100000);
        expect(controller.lastSubmissionUs() == acceptedUs &&
                   controller.earliestSubmissionUs() == acceptedUs + controller.displayPeriodUs(),
               "failed or unsubmitted canceled frames must not reserve future native slots");
    }
    // A canceled frame which actually reached a native queue is different:
    // other backends can submit an abandoned image and must retain its slot.
    d = controller.schedule(frame(5, 3000, true, 1033333), 1033333);
    controller.noteSubmission(true, true, d.targetUs);
    expect(controller.lastSubmissionUs() == d.targetUs &&
               controller.earliestSubmissionUs() == d.targetUs + controller.displayPeriodUs(),
           "native acceptance must retain spacing even when the lifecycle result is canceled");
    const uint64_t acceptedCancelledUs = d.targetUs;
    controller.rebase();
    expect(controller.earliestSubmissionUs() == acceptedCancelledUs + controller.displayPeriodUs(),
           "source requalification must retain an accepted native predecessor");
    controller.reset();
    expect(controller.lastSubmissionUs() == 0 && controller.earliestSubmissionUs() == 0,
           "native lifecycle reset must discard the old deadline anchor");
}

void testNativeScheduledSlowerCadenceAndOverloadAdmission()
{
    for (int rate : {30, 60, 116, 120}) {
        const auto session = config(rate, 120);
        VrrTimingController controller(session, true, nativeDeadlineTestParameters(session));
        uint64_t priorTargetUs = 0;
        for (int i = 0; i < 600; ++i) {
            const uint32_t timestamp = uint32_t(uint64_t(i) * 90000 / rate);
            const uint64_t at = decodedTimeForRtp(1000000, timestamp);
            const auto d = controller.schedule(frame(i + 1, timestamp, true, at), at);
            expect(!priorTargetUs || d.targetUs >= priorTargetUs + controller.displayPeriodUs(),
                   "every supported slower source must retain admissible native deadline spacing");
            expect(d.targetUs >= at && d.targetUs - at < 10000,
                   "slower source cadence must not accumulate display-floor latency");
            controller.noteSubmission(true, false, d.targetUs);
            priorTargetUs = d.targetUs;
        }
    }

    // Sustained 240 FPS cannot be displayed at 120 Hz. Exercise the actual
    // worker's before-render age policy rather than demanding an impossible
    // controller-only guarantee that every admitted source frame is shown.
    const auto session = config(240, 120);
    const auto policy = nativeDeadlineTestParameters(session);
    VrrTimingController controller(session, true, policy);
    uint64_t workerAvailableUs = 0;
    uint64_t priorTargetUs = 0;
    unsigned dropped = 0;
    unsigned accepted = 0;
    for (int i = 0; i < 1200; ++i) {
        const uint32_t timestamp = uint32_t(uint64_t(i) * 90000 / 240);
        const uint64_t at = decodedTimeForRtp(1000000, timestamp);
        const uint64_t nowUs = std::max(at, workerAvailableUs);
        const auto d = controller.schedule(frame(i + 1, timestamp, true, at), nowUs);
        const bool successorQueued = decodedTimeForRtp(
            1000000, uint32_t(uint64_t(i + 1) * 90000 / 240)) <= nowUs;
        if (successorQueued && VrrFrameDropPolicy::beforeRender(d, controller.displayPeriodUs(),
                nowUs - at, true, d.playoutDelayUs, policy.playoutLateRecovery != 0)) {
            controller.noteSubmission(false, false, 0);
            ++dropped;
            continue;
        }
        expect(!priorTargetUs || d.targetUs >= priorTargetUs + controller.displayPeriodUs(),
               "overload recovery must not compress accepted native display deadlines");
        expect(d.targetUs >= at && d.targetUs - at < 30000,
               "shared stale-frame admission must bound overload latency instead of accumulating slots");
        controller.noteSubmission(true, false, d.targetUs);
        priorTargetUs = d.targetUs;
        workerAvailableUs = d.targetUs + 75;
        ++accepted;
    }
    expect(dropped > 400 && accepted > 400,
           "an above-ceiling source must shed replaceable frames while continuing native presentation");
}

void testRuntimeParametersChangePolicy()
{
    VrrTimingController defaults(config(60, 120));
    VrrTimingParameters parameters;
    parameters.guardStepUs = 500;
    VrrTimingController candidate(config(60, 120), true, parameters);
    defaults.schedule(frame(1, 0, true, 100000), 100000);
    candidate.schedule(frame(1, 0, true, 100000), 100000);
    defaults.noteSpacingDeficit(1);
    candidate.noteSpacingDeficit(1);
    expect(candidate.guardUs() > defaults.guardUs(),
           "runtime parameters must change controller policy without recompilation");
    expect(candidate.parameters().guardStepUs == 500,
           "controller must retain its resolved parameter set");
}

void testTearingPresentClearsLatchedFlip()
{
    // A latched present flips no sooner than one display period after the
    // previous flip. A tearing present anchored only to that latched call
    // could flip inside the panel's minimum period after a late frame.
    const auto run = [](uint64_t anchor, uint64_t perFrameLatch) {
        auto session = config(116, 120);
        auto policy = vrrTimingParametersForSession(session);
        policy.latchedFlipAnchor = anchor;
        // Isolate the historical flip-anchor defect from newer late recovery.
        policy.playoutLateRecovery = 0;
        policy.playoutPerFrameLatch = perFrameLatch;
        VrrTimingController controller(session, true, policy);
        uint64_t flipUs = 0, violations = 0, adaptive = 0;
        bool haveFlip = false;
        for (int i = 0; i < 2000; ++i) {
            const uint32_t rtp = uint32_t(uint64_t(i) * 90000 / 116);
            const uint64_t at = 1000000 + uint64_t(rtp) * 1000 / 90;
            const auto d = controller.schedule(frame(i, rtp, true, at), at);
            // Every 7th frame's Present is late by 0.6-2.4 ms.
            const uint64_t call = d.targetUs + (i % 7 == 3 ? 600 + (i % 4) * 600 : 0);
            if (!d.latchedPresentation) {
                ++adaptive;
                if (haveFlip && call < flipUs + controller.displayPeriodUs()) ++violations;
                flipUs = call;
            }
            else flipUs = haveFlip ? std::max(call, flipUs + controller.displayPeriodUs()) : call;
            haveFlip = true;
            controller.noteSubmission(true, false, call);
        }
        return std::array<uint64_t, 2>{violations, adaptive};
    };
    expect(run(0, 1)[0] > 0,
           "call-anchored spacing must reproduce tearing presents after late latched frames");
    expect(run(1, 1)[0] == 0 && run(1, 2)[0] == 0,
           "tearing presents must clear the predicted flip of a latched predecessor");
    const auto production = vrrTimingParametersForSession(config(116, 120));
    expect(production.latchedFlipAnchor == 1 && production.vrrFloorLatchGapUs == 20000,
           "production must anchor spacing to latched flips and latch after VRR-floor gaps");
}

void testNativeFlipProtectionPreventsScanoutTears()
{
    // DXGI can flip a tearing present milliseconds after its call, so the
    // call-anchored model underestimates when the predecessor's scanout
    // ends. The presenter's frame-statistics check latches instead; this
    // models it: pending predecessor, or a refresh begun under one period ago.
    const auto run = [](uint64_t protection) {
        auto session = config(105, 120);
        auto policy = vrrTimingParametersForSession(session);
        policy.nativeFlipProtection = protection;
        VrrTimingController controller(session, true, policy);
        const uint64_t periodUs = controller.displayPeriodUs();
        uint64_t flipUs = 0, tears = 0, nativeLatches = 0, adaptive = 0;
        uint64_t displayLatencyUs = 0;
        bool haveFlip = false;
        for (int i = 0; i < 3000; ++i) {
            const uint32_t rtp = uint32_t(uint64_t(i) * 90000 / 105);
            const uint64_t at = 1000000 + uint64_t(rtp) * 1000 / 90 + (i % 5) * 400;
            const auto d = controller.schedule(frame(i, rtp, true, at), at);
            const uint64_t call = d.targetUs;
            bool latched = d.latchedPresentation;
            if (protection != 0 && !latched && haveFlip &&
                    (flipUs > call || call - flipUs < periodUs)) {
                latched = true;
                ++nativeLatches;
                controller.noteNativeFlipProtection(flipUs > call ? 0 : flipUs);
            }
            if (latched) {
                flipUs = haveFlip ? std::max(call + 500, flipUs + periodUs) : call + 500;
            }
            else {
                // Tearing-present flip latency of 0.5-4.5 ms.
                const uint64_t lagUs = 500 +
                    uint64_t(((uint32_t(i) * 2654435761u) >> 16) % 9) * 500;
                ++adaptive;
                if (haveFlip && call + lagUs < flipUs + periodUs) ++tears;
                flipUs = call + lagUs;
            }
            displayLatencyUs += flipUs - call;
            haveFlip = true;
            controller.noteSubmission(true, false, call);
        }
        return std::array<uint64_t, 4>{tears, nativeLatches, adaptive, displayLatencyUs / 3000};
    };
    const auto unprotected = run(0);
    const auto protectedRun = run(1);
    std::printf("native flip protection tears %llu -> %llu, native latches %llu, adaptive %llu -> %llu, "
                "mean call-to-flip %llu -> %llu us\n",
                (unsigned long long) unprotected[0], (unsigned long long) protectedRun[0],
                (unsigned long long) protectedRun[1], (unsigned long long) unprotected[2],
                (unsigned long long) protectedRun[2], (unsigned long long) unprotected[3],
                (unsigned long long) protectedRun[3]);
    expect(unprotected[0] > 0,
           "call-anchored spacing must reproduce tears from late tearing flips");
    expect(protectedRun[0] == 0 && protectedRun[1] > 0 && protectedRun[2] > 0,
           "native flip protection must latch every present that would tear");
    expect(vrrTimingParametersForSession(config(116, 120)).nativeFlipProtection == 1,
           "production must enable native flip protection");
}

void testBalancedReadinessFloorFollowsLoad()
{
    // Captures that recorded a readiness floor replay it: the delay is
    // floored at the recent p90 ready offset. Slow decode on most frames must
    // raise the delay; once decode recovers the floor must fall away within
    // its window rather than pin the buffer. No profile uses it any more.
    auto session = config(116, 120);
    session.latencyMode = 1;
    const auto production = vrrTimingParametersForSession(session);
    constexpr uint64_t kRecordedFloorPerMille = 900;
    for (int mode : {0, 1, 2}) {
        auto other = config(116, 120);
        other.latencyMode = mode;
        expect(vrrTimingParametersForSession(other).playoutReadinessFloorPerMille == 0,
               "no profile may floor its delay at a ready-offset percentile");
    }
    const auto run = [&](uint64_t perMille, bool tailOnly = false) {
        auto policy = production;
        // This fixture isolates the recorded revision-7 readiness floor.
        // Production may raise delay without that historical floor.

        policy.playoutReadinessFloorPerMille = perMille;
        VrrTimingController controller(session, true, policy);
        uint64_t loadedUs = 0, relievedUs = 0;
        constexpr int kFrames = 116 * 70;
        for (int i = 0; i < kFrames; ++i) {
            const uint32_t rtp = uint32_t(uint64_t(i) * 90000 / 116);
            // For 30 s, three of five frames finish decoding 6 ms late, or
            // for the tail case one of eight frames finishes 6 ms late.
            const bool slow = i < 116 * 30 && (tailOnly ? i % 8 == 0 : i % 5 < 3);
            const uint64_t at = decodedTimeForRtp(1000000, rtp) + (slow ? 6000 : 0);
            const auto d = controller.schedule(frame(i, rtp, true, at), at);
            controller.notePreparationDuration(1000);
            controller.noteSubmission(true, false, std::max(d.targetUs, at + 1000));
            if (i == 116 * 30 - 1) loadedUs = d.playoutDelayUs;
            relievedUs = d.playoutDelayUs;
        }
        return std::array<uint64_t, 2>{loadedUs, relievedUs};
    };
    const auto without = run(0);
    const auto with = run(kRecordedFloorPerMille);
    // A late tail that a median cannot see must still raise the p90 floor.
    const auto medianTail = run(500, true);
    const auto tail = run(kRecordedFloorPerMille, true);
    std::printf("readiness floor with a 12.5%% late tail: median %llu us, p90 %llu us\n",
                (unsigned long long) medianTail[0], (unsigned long long) tail[0]);
    expect(tail[0] > medianTail[0] + 1000,
           "the p90 floor must cover a late tail that the median ignores");
    expect(tail[1] + 1000 < tail[0],
           "the tail floor must release once the late frames stop");
    std::printf("readiness floor delay loaded %llu -> %llu us, relieved %llu -> %llu us\n",
                (unsigned long long) without[0], (unsigned long long) with[0],
                (unsigned long long) without[1], (unsigned long long) with[1]);
    expect(with[0] >= 5500 && with[0] > without[0] + 1000,
           "the floor must follow a slow median ready offset up");
    expect(with[1] + 1000 < with[0] && with[1] <= without[1] + 500,
           "the floor must release once decode recovers");
}

void testProfilesOrderEveryTrade()
{
    // The profiles are one latency/smoothness dial. Every trade moves the
    // same way from Low Latency through Balanced to Smooth, and they share
    // the queue, so capacity never clips one profile's delay before its own
    // ceiling does.
    const auto parameters = [](int mode) {
        auto session = config(116, 120);
        session.latencyMode = mode;
        return vrrTimingParametersForSession(session);
    };
    const auto low = parameters(2);
    const auto balanced = parameters(1);
    const auto smooth = parameters(0);
    expect(low.playoutOnTimeTargetPerMillion < balanced.playoutOnTimeTargetPerMillion &&
               balanced.playoutOnTimeTargetPerMillion < smooth.playoutOnTimeTargetPerMillion,
           "on-time targets must rise from Low Latency to Smooth");
    expect(low.playoutReadinessWindowUs <= balanced.playoutReadinessWindowUs &&
               balanced.playoutReadinessWindowUs <= smooth.playoutReadinessWindowUs,
           "readiness memory must lengthen from Low Latency to Smooth");
    expect(low.playoutDelayCapSourcePeriodPerMille < balanced.playoutDelayCapSourcePeriodPerMille &&
               balanced.playoutDelayCapSourcePeriodPerMille < smooth.playoutDelayCapSourcePeriodPerMille,
           "delay caps must rise from Low Latency to Smooth");
    expect(low.playoutDelayMaximumUs <= balanced.playoutDelayMaximumUs &&
               balanced.playoutDelayMaximumUs <= smooth.playoutDelayMaximumUs,
           "absolute delay ceilings must not fall from Low Latency to Smooth");
    expect(low.playoutMeanMissHoldUs <= balanced.playoutMeanMissHoldUs &&
               balanced.playoutMeanMissHoldUs <= smooth.playoutMeanMissHoldUs,
           "earned protection must be held longer from Low Latency to Smooth");
    expect(low.playoutMeanMissReleaseUsPerSecond >= balanced.playoutMeanMissReleaseUsPerSecond &&
               balanced.playoutMeanMissReleaseUsPerSecond >= smooth.playoutMeanMissReleaseUsPerSecond,
           "protection must release no slower from Smooth to Low Latency");
    for (const auto* profile : {&low, &balanced, &smooth}) {
        expect(profile->playoutQueueFrames == VrrLargestQueuedFrames,
               "every profile must wait in the same full queue");
        expect(profile->playoutReadinessFloorPerMille == 0,
               "no profile may floor its delay at a ready-offset percentile");
    }
}

void testFirstPresentAfterVrrFloorGapLatches()
{
    auto session = config(116, 120);
    auto policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session, true, policy);
    uint64_t at = 1000000;
    uint32_t rtp = 0;
    for (int i = 0; i < 300; ++i) {
        rtp += 90000 / 116;
        at += 1000000 / 116;
        const auto d = controller.schedule(frame(i, rtp, true, at), at);
        controller.noteSubmission(true, false, d.targetUs);
    }
    // A 60 ms host stall leaves the panel below its VRR range.
    rtp += 90000 * 60 / 1000;
    at += 60000;
    const auto d = controller.schedule(frame(300, rtp, true, at), at);
    expect(d.latchedPresentation,
           "the first present after a gap beyond the VRR floor must use the flip queue");
    controller.noteSubmission(true, false, d.targetUs);
}

void testExplicitAdaptiveOnlyPolicy()
{
    for (int refresh : {60, 120, 144, 240}) {
        const auto session = config(refresh, refresh);
        auto policy = vrrTimingParametersForSession(session);
        policy.playoutAdaptiveOnly = 1;
        VrrTimingController controller(session, true, policy);
        uint64_t ticks = 0;
        uint64_t prior = 0;
        for (int i = 0; i < 900; ++i) {
            const int rate = i < 300 ? refresh : i < 600 ? refresh - 5 : refresh;
            ticks += 90000 / rate;
            const auto arrival = 1000000 + ticks * 1000 / 90;
            const auto d = controller.schedule(frame(i, uint32_t(ticks), true, arrival),
                                               std::max(arrival, prior));
            expect(!d.latchedPresentation, "rate changes and late work must not enable V-Sync");
            if (prior) expect(d.targetUs >= prior + controller.displayPeriodUs(),
                              "adaptive production must retain the software spacing floor");
            prior = d.targetUs + (i % 31 == 0 ? 4000 : 0);
            controller.noteSubmission(true, false, prior);
            if (i % 47 == 0) controller.noteSpacingDeficit(200);
        }
    }
}

void testProductionAdaptiveProtectionRecoversWithoutDrift()
{
    const auto session = config(120, 120);
    const auto policy = vrrTimingParametersForSession(session);
    expect(policy.playoutAdaptiveOnly == 0 && policy.playoutPerFrameLatch == 1 &&
           policy.playoutRateProtectionEnabled == 0,
           "production must choose protection for each slot, not force a rate band");
    VrrTimingController controller(session, true, policy);
    uint64_t ticks = 0, prior = 0;
    unsigned protectedAtCeiling = 0, adaptiveWithHeadroom = 0;
    for (int i = 0; i < 1800; ++i) {
        const int rate = i < 600 || i >= 1200 ? 120 : 100;
        ticks += 90000 / rate;
        const uint64_t arrival = 1000000 + ticks * 1000 / 90;
        const auto decision = controller.schedule(frame(i, uint32_t(ticks), true, arrival),
                                                  std::max(arrival, prior));
        if (decision.latchedPresentation) {
            expect(controller.earliestSubmissionUs() == 0,
                   "native protection must remove the unsustainable software floor");
            if (i > 1300) ++protectedAtCeiling;
        }
        else {
            if (prior) expect(decision.targetUs >= controller.earliestSubmissionUs(),
                              "tearing submissions must retain their spacing protection");
            if (i > 700 && i < 1100) ++adaptiveWithHeadroom;
        }
        expect(decision.targetUs < arrival + 25000,
               "120 FPS and temporary execution stalls must not accumulate pacing latency");
        // A late submission must recover on subsequent source slots rather
        // than pushing every later frame by a display period plus guard.
        prior = decision.targetUs + (i % 113 == 0 ? 4000 : 0);
        controller.noteSubmission(true, false, prior);
    }
    expect(protectedAtCeiling > 400, "native-rate slots must use protection");
    expect(adaptiveWithHeadroom > 300, "source slowdown must return to adaptive presentation");
}

void testPerFrameLatchIncludesSafetyHeadroom()
{
    for (int mode : {0, 1, 2}) {
        for (int refresh : {60, 120, 144, 165, 240, 360}) {
            auto session = config(refresh * 3 / 4, refresh);
            session.latencyMode = mode;
            session.smoothFrameTiming = false;
            auto policy = vrrTimingParametersForSession(session);
            policy.playoutPerFrameLatch = 2; // Preserve historical vrr17 replay.
            policy.playoutLateRecovery = 0;
            policy.playoutCatchupPerMille = 0;
            // A shadow schedule supplies the uncompressed source slots. No
            // preparation observations means both controllers retain the same
            // render budget and buffer; only submitted spacing differs.
            for (uint64_t slack : {224ULL, 225ULL, 399ULL, 400ULL}) {
                VrrTimingController reference(session, true, policy);
                VrrTimingController controller(session, true, policy);
                const auto period = 1000000ULL / session.streamRateHz;
                const auto a = frame(0, 0, true, 1000000);
                reference.schedule(a, 1000000);
                controller.schedule(a, 1000000);
                const auto b = frame(1, uint32_t(90000 / session.streamRateHz), true, 1000000 + period);
                const auto rawB = reference.schedule(b, b.decodeCompleteUs());
                const auto floorInterval = controller.displayPeriodUs() + controller.guardUs();
                controller.noteSubmission(true, false, rawB.targetUs - floorInterval - slack);
                auto d = controller.schedule(b, b.decodeCompleteUs());
                expect(d.latchedPresentation == (slack < policy.latchedPresentationHeadroomUs),
                       "entry must include the VRR12 safety headroom in every preset and refresh rate");

                // Force entry, then verify exit uses the larger hysteresis
                // threshold on the planned slot rather than fitted source FPS.
                const auto c = frame(2, uint32_t(180000 / session.streamRateHz), true, 1000000 + 2 * period);
                const auto rawC = reference.schedule(c, c.decodeCompleteUs());
                controller.noteSubmission(true, false, rawC.targetUs - floorInterval - 224);
                d = controller.schedule(c, c.decodeCompleteUs());
                expect(d.latchedPresentation, "a tight slot must enter native protection");
                const auto e = frame(3, uint32_t(270000 / session.streamRateHz), true, 1000000 + 3 * period);
                const auto rawE = reference.schedule(e, e.decodeCompleteUs());
                controller.noteSubmission(true, false, rawE.targetUs - floorInterval - slack);
                d = controller.schedule(e, e.decodeCompleteUs());
                expect(d.latchedPresentation == (slack < policy.latchedPresentationExitHeadroomUs),
                       "exit must retain the full VRR12 safety headroom hysteresis");
                expect(d.targetUs == rawE.targetUs,
                       "native protection must not add padding to the source deadline");
            }
        }
    }
}

void testSmoothQueueReachesItsCeilingNearRefresh()
{
    // At 116/120 with Reduce judder, the shared four-frame queue must still
    // leave Smooth its full two-frame ceiling after the render lead and the
    // retiming budget.
    for (int mode : {0, 1, 2}) {
        auto session = config(116, 120);
        session.latencyMode = mode;
        VrrTimingController controller(session, true, vrrTimingParametersForSession(session));
        VrrTimingDecision d;
        for (int i = 0; i < 240; ++i) {
            const uint32_t rtp = uint32_t(uint64_t(i) * 90000 / 116);
            const uint64_t at = 1000000 + uint64_t(rtp) * 1000 / 90;
            d = controller.schedule(frame(i, rtp, true, at), at);
            controller.noteSubmission(true, false, d.targetUs);
        }
        expect(controller.queuedFrameCapacity() == 4u,
               "every profile must wait in the same four-frame queue");
        if (mode == 0) {
            expect(d.playoutDelayMaximumUs >= 16500,
                   "Smooth must reach its two-frame ceiling at 116 FPS with Reduce judder enabled");
        }
        expect(d.playoutDelayMaximumUs <= controller.playoutQueueLimitUs(),
               "the delay ceiling must stay inside the queue budget");
    }
    VrrTimingParameters historical;
    VrrTimingController replayed(config(116, 120), true, historical);
    expect(replayed.queuedFrameCapacity() == VrrMaximumQueuedFrames,
           "captures without the parameter keep the historical three waiting frames");
}

void testProductionMatchesVrr14NearRefresh()
{
    for (int mode : {0, 1, 2}) {
        auto session = config(116, 120);
        session.latencyMode = mode;
        session.smoothFrameTiming = false;
        const auto policy = vrrTimingParametersForSession(session);
        auto historical = policy;
        historical.playoutPerFrameLatch = 2;
        VrrTimingController controller(session, true, policy);
        VrrTimingController oldController(session, true, historical);
        unsigned protectedFrames = 0, oldProtectedFrames = 0;
        for (int i = 0; i < 600; ++i) {
            const auto ticks = uint32_t(uint64_t(i) * 90000 / 116);
            const auto at = 1000000 + uint64_t(ticks) * 1000 / 90;
            const auto f = frame(i, ticks, true, at);
            const auto d = controller.schedule(f, at);
            const auto old = oldController.schedule(f, at);
            if (i > 100) {
                protectedFrames += d.latchedPresentation;
                oldProtectedFrames += old.latchedPresentation;
            }
            expect(d.targetUs == old.targetUs && d.playoutDelayUs == old.playoutDelayUs,
                   "vrr14-style protection must not add buffer or retime source slots");
            controller.noteSubmission(true, false, d.targetUs);
            oldController.noteSubmission(true, false, old.targetUs);
        }
        expect(protectedFrames == 0 && oldProtectedFrames == 499,
               "116/120 must match vrr14 while explicit vrr17 replay retains safety-headroom protection");
    }
}

void testPersistentImmediateUsesSafetyFloor()
{
    for (int mode : {0, 1, 2}) {
        for (int refresh : {60, 120, 144, 165, 240, 360}) {
            auto session = config(refresh, refresh);
            session.latencyMode = mode;
            auto policy = vrrTimingParametersForSession(session);
            VrrTimingController controller(session, false, policy);
            uint64_t prior = 0;
            for (int i = 0; i < 200; ++i) {
                const auto ticks = uint32_t(uint64_t(i) * 90000 / refresh);
                const auto at = 1000000 + uint64_t(ticks) * 1000 / 90;
                const auto d = controller.schedule(frame(i, ticks, true, at), std::max(at, prior));
                expect(!d.latchedPresentation,
                       "persistent Immediate/FIFO must never request unsupported native protection");
                if (prior) {
                    const auto safe = prior + controller.displayPeriodUs() + d.guardUs;
                    expect(controller.earliestSubmissionUs() == safe && d.targetUs >= safe,
                           "production Immediate/FIFO must retain vrr14's period-plus-guard software floor");
                }
                prior = d.targetUs + (i % 37 == 0 ? 3000 : 0);
                controller.noteSubmission(true, false, prior);
            }
        }
    }
}

void testSourceRateProtection()
{
    struct Display { int refreshHz; int cutoffHz; };
    for (const auto display : {Display{60, 59}, Display{120, 116},
                              Display{144, 138}, Display{165, 157},
                              Display{240, 224}, Display{360, 324}}) {
        for (double sourceRate : {display.cutoffHz - 0.25, double(display.cutoffHz),
                                  display.cutoffHz + 0.25, double(display.refreshHz)}) {
            for (bool canLatch : {false, true}) {
                // Negotiate native maximum but learn the actual game rate.
                const auto session = config(display.refreshHz, display.refreshHz);
                auto policy = vrrTimingParametersForSession(session);
                policy.playoutAdaptiveOnly = 0;
                policy.playoutRateProtectionEnabled = 1;
                expect(policy.playoutRateProtectionEnabled == 1,
                       "historical source-rate protection remains replayable");
                VrrTimingController controller(session, canLatch, policy);
                for (int i = 0; i < 600; ++i) {
                    const auto rtp = uint32_t(std::llround(i * 90000.0 / sourceRate));
                    const auto arrival = 1000000 + uint64_t(rtp) * 1000 / 90 + (i % 9) * 30;
                    const auto prior = controller.lastSubmissionUs();
                    const auto now = std::max(arrival, prior);
                    const auto d = controller.schedule(frame(i, rtp, true, arrival), now);
                    if (i == 0) {
                        expect(d.latchedPresentation == canLatch,
                               "native-rate startup must protect the first frame");
                    }
                    if (i > 200) {
                        expect(d.latchedPresentation == (canLatch && sourceRate >= display.cutoffHz),
                               "the fitted source rate must select protection inclusively at the old suggested cutoff");
                    }
                    if (prior && !d.latchedPresentation) {
                        expect(d.targetUs >= prior + controller.displayPeriodUs() + d.guardUs,
                               "adaptive presents must keep the software spacing floor");
                    }
                    // Late CPU/GPU completion and guard changes cannot change
                    // the selected mode while the source stays in this range.
                    controller.noteSubmission(true, false, d.targetUs + (i % 31 == 0 ? 2000 : 0));
                    if (i % 47 == 0) controller.noteSpacingDeficit(200);
                }
            }
        }
    }

    const auto session = config(120, 120);
    auto policy = vrrTimingParametersForSession(session);
    policy.playoutAdaptiveOnly = 0;
    policy.playoutRateProtectionEnabled = 1;
    VrrTimingController controller(session, true, policy);
    double ticks = 0;
    int number = 0;
    for (int rate : {110, 119, 110}) {
        for (int i = 0; i < 600; ++i, ++number) {
            ticks += 90000.0 / rate;
            const auto rtp = uint32_t(std::llround(ticks));
            const auto at = 1000000 + uint64_t(rtp) * 1000 / 90;
            const auto d = controller.schedule(frame(number, rtp, true, at), at);
            if (i > 300) {
                expect(d.latchedPresentation == (rate >= 116),
                       "source rate changes must enter protection and return to adaptive presentation");
            }
            controller.noteSubmission(true, false, d.targetUs);
        }
    }
}

void testProcessingEpisodeClassification()
{
    using R = Vrr13::Reserve;
    R history;
    Vrr13::WorkloadEpisode episode;
    episode.observe(history, 8000000, 10000000, R::Second, 30000, 16667);
    expect(history.evidence() == 0, "a work episode must remain provisional until recovery");
    episode.observe(history, 1000000, 10000000, R::Second + 16667000, 0, 16667);
    expect(history.evidence() == 2 && history.common() == 8000000,
           "a transient decoder backlog must teach its tail after draining");
    const auto before = history.evidence();
    for (int i = 0; i < 240; ++i)
        episode.observe(history, 90000000, 10000000,
                        2 * R::Second + int64_t(i) * 16667000, 100000, 16667);
    episode.observe(history, 1000000, 10000000, 7 * R::Second, 0, 16667);
    expect(history.evidence() == before + 1 && history.common() == 8000000,
           "sustained processing overload must not inflate the receiver jitter buffer");
}

void testRollingPlayoutHistory()
{
    using R = Vrr13::Reserve;
    R memory;
    for (int i = 0; i <= 600; ++i)
        memory.observe(i % 100 == 0 ? 8000000 : 1000000, 9000000,
                       R::Second + int64_t(i) * R::Second / 120);
    expect(memory.common() == 8000000, "five-minute history must retain recurring receiver tails");
    const auto checkpoint = memory.checkpoint();
    R restored;
    expect(restored.restore(checkpoint), "a checkpoint must restore the complete histogram and expiration clock");
    for (int i = 1; i <= 300; ++i) {
        const auto at = 6 * R::Second + int64_t(i) * R::Second / 30;
        memory.observe(1000000, 9000000, at);
        restored.observe(1000000, 9000000, at);
    }
    expect(memory.checkpoint() == restored.checkpoint(), "checkpoint continuation must be exact across an FPS change");
    expect(memory.common() == 8000000, "30 FPS must not discard the jitter learned at 120 FPS");
    memory.observe(1000000, 9000000, 307 * R::Second);
    expect(memory.common() == 1000000, "expired evidence must not pin latency forever");
    R prior;
    expect(prior.loadProfile(restored.profile()), "compatible jitter profiles must load");
    expect(prior.evidence() == 0 && !prior.canRelease(), "cached history is not fresh validation");
    prior.age(14);
    expect(prior.cachedEvidence() < restored.evidence(), "offline aging must reduce prior evidence");
    auto invalid = restored.profile(); invalid[0] = 12;
    expect(!prior.loadProfile(invalid), "VRR14 recovery-budget profiles must not be accepted by VRR13");
}

} // namespace

void testRefreshReferencesAreNotDisplayEvents()
{
    using namespace Vrr13;
    PresentationPrediction prediction;
    PresentationObservation first;
    first.submitted = first.idValid = first.sampleValid = first.dxgi = true;
    first.timeKind = PresentationTimeKind::RefreshReference;
    first.id = first.sampleId = 29;
    first.submission = first.ready = 895965;
    first.sampleTime = 902785;
    first.observed = 903325;
    first.presentRefresh = first.syncRefresh = 260933;
    first.deadline = 902785;
    prediction.observe(first);
    expect(prediction.measured() == 0 && prediction.lead(first.observed) == 0,
           "even a causal equal-refresh reference cannot certify a display event");

    auto second = first;
    second.id = second.sampleId = 30;
    second.submission = second.ready = 903178;
    second.observed = 910213;
    prediction.observe(second);
    expect(prediction.measured() == 0,
           "a newer present sharing the old refresh reference must remain unmeasured");

    // Delayed exact events remain usable for both DXGI and other backends.
    second.submitted = false;
    second.timeKind = PresentationTimeKind::DisplayEvent;
    second.sampleTime = 905178;
    prediction.observe(second);
    expect(prediction.measured() == 1 && prediction.lead(second.observed) == 2000,
           "an explicit display event matches the retained submission directly");
    prediction.observe(second);
    expect(prediction.measured() == 1, "duplicate display events cannot inflate coverage");

    const auto session = config(100, 120);
    const auto policy = vrrTimingParametersForSession(session);
    expect(policy.playoutRequireDisplayEvents == 1,
           "optional display cadence diagnostics must require real display events");
    VrrTimingController control(session, true, policy);
    VrrTimingController referenceOnly(session, true, policy);
    for (int i = 1; i <= 120; ++i) {
        const auto at = uint64_t(1000000 + i * 10000);
        const auto a = control.schedule(frame(i, i * 900, true, at), at);
        const auto b = referenceOnly.schedule(frame(i, i * 900, true, at), at);
        expect(a.targetUs == b.targetUs && a.playoutDelayUs == b.playoutDelayUs &&
                   b.compositorLeadUs == 0 && b.nativeSmoothnessSamples == 0 &&
                   referenceOnly.nativeCadenceIntervals() == 0 && referenceOnly.nativeCadenceHitches() == 0,
               "refresh references must not change latency learning or buffer adaptation");
        control.noteSubmission(true, false, a.targetUs);
        referenceOnly.noteSubmission(true, false, b.targetUs);
        PresentationObservation observation;
        observation.timeKind = PresentationTimeKind::RefreshReference;
        observation.dxgi = observation.submitted = observation.idValid = observation.sampleValid = true;
        observation.smoothness = referenceOnly.smoothnessSample(b);
        observation.id = observation.sampleId = uint64_t(i);
        observation.submission = observation.ready = b.targetUs;
        observation.sampleTime = b.targetUs + (i % 2 ? 1000 : 7000);
        observation.observed = observation.sampleTime;
        observation.presentRefresh = observation.syncRefresh = uint64_t(i);
        observation.deadline = b.originalScanoutUs;
        referenceOnly.notePresentation(observation);
    }
}

void testPreparationKeepsLearnedLead()
{
    const auto session = config(120, 120);
    const auto policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session, true, policy);
    uint64_t previousSubmission = 0;
    bool sawLargerLead = false;
    for (int i = 0; i < 300; ++i) {
        const auto decoded = decodedTimeForRtp(1000000, uint32_t(i * 750));
        const auto now = std::max(decoded, previousSubmission + 100);
        const auto d = controller.schedule(frame(i, uint32_t(i * 750), true, decoded), now);
        if (i > 20) {
            expect(d.targetUs - d.renderStartUs >= d.renderLeadUs + d.renderWakeLeadUs,
                   "post-present spacing must not truncate learned preparation lead");
            if (i > 160 && d.renderLeadUs >= 5000) sawLargerLead = true;
        }
        const uint64_t work = i < 150 ? 3500 : 5000;
        const auto ready = std::max(now, d.renderStartUs) + work;
        controller.notePreparationDuration(work);
        previousSubmission = std::max(ready, d.targetUs);
        controller.noteSubmission(true, false, previousSubmission);
    }
    expect(sawLargerLead, "longer preparation must earn rendering time, not only a later target");
}

void testGpuReadinessLeadIsSeparateFromTarget()
{
    const auto session = config(60, 120);
    const auto production = vrrTimingParametersForSession(session);
    auto noGpuAdaptation = production;
    noGpuAdaptation.playoutGpuReadinessAdaptation = 0;
    VrrTimingController adapted(session, true, production);
    VrrTimingController baseline(session, true, noGpuAdaptation);

    // Feed the same completed GPU wait to the adapted controller and the
    // equivalent non-GPU preparation cost to the baseline. The learned term
    // must buy render-start time without moving the source presentation slot.
    for (int i = 0; i < 80; ++i) {
        const uint32_t rtp = static_cast<uint32_t>(i * 1500);
        const uint64_t decoded = decodedTimeForRtp(1000000, rtp);
        const auto a = adapted.schedule(frame(i, rtp, true, decoded), decoded);
        const auto b = baseline.schedule(frame(i, rtp, true, decoded), decoded);
        expect(a.targetUs == b.targetUs,
               "GPU readiness adaptation must not move the presentation target");
        adapted.notePreparationDuration(7000, 0, decoded + 7000, 6000);
        baseline.notePreparationDuration(1000, 0, decoded + 1000, 0);
        adapted.noteGpuReadyWait(6000, true, decoded + 7000);
        adapted.noteSubmission(true, false, a.targetUs);
        baseline.noteSubmission(true, false, b.targetUs);
    }

    const uint32_t rtp = 80U * 1500U;
    const uint64_t decoded = decodedTimeForRtp(1000000, rtp);
    const auto adaptedDecision = adapted.schedule(
        frame(80, rtp, true, decoded), decoded);
    const auto baselineDecision = baseline.schedule(
        frame(80, rtp, true, decoded), decoded);
    expect(adaptedDecision.gpuReadinessLeadUs > 0,
           "completed GPU waits must earn a readiness lead");
    expect(adaptedDecision.gpuReadinessLeadUs <=
               std::min<uint64_t>(production.playoutGpuReadinessMaximumUs,
                                  adaptedDecision.sourcePeriodUs),
           "GPU readiness lead must remain within its source-period ceiling");
    expect(adaptedDecision.targetUs == baselineDecision.targetUs,
           "learned GPU readiness must leave the source target unchanged");
    expect(adaptedDecision.renderStartUs < baselineDecision.renderStartUs,
           "learned GPU readiness must advance the render-start deadline");

    VrrTimingController failed(session, true, production);
    failed.schedule(frame(0, 0, true, 1000000), 1000000);
    failed.noteGpuReadyWait(12000, false, 1007000);
    expect(failed.gpuReadinessLeadUs() == 0,
           "failed GPU waits must not train readiness head start");
}

void testGpuReadinessPercentileExpiresDuplicateSamples()
{
    const auto session = config(60, 120);
    auto policy = vrrTimingParametersForSession(session);
    policy.playoutGpuReadinessWindowUs = 100000;
    policy.playoutGpuReadinessPercentile = 50;
    policy.playoutGpuReadinessMarginUs = 0;
    policy.playoutGpuReadinessAttackUs = 20000;
    policy.playoutGpuReadinessReleaseUsPerSecond = 1000000;
    VrrTimingController controller(session, true, policy);
    controller.schedule(frame(0, 0, true, 1000000), 1000000);

    controller.noteGpuReadyWait(1000, true, 1000000);
    controller.noteGpuReadyWait(4000, true, 1010000);
    controller.noteGpuReadyWait(4000, true, 1020000);
    controller.noteGpuReadyWait(8000, true, 1030000);
    expect(controller.gpuReadinessLeadUs() == 4000,
           "GPU readiness median must include duplicate waits");

    controller.noteGpuReadyWait(1000, true, 1200000);
    expect(controller.gpuReadinessLeadUs() == 1000,
           "expired duplicate waits must leave the readiness window");
    controller.noteGpuReadyWait(6000, true, 1210000);
    expect(controller.gpuReadinessLeadUs() == 1000,
           "readiness percentile must use the same lower-rank rule");
    controller.noteGpuReadyWait(7000, true, 1220000);
    expect(controller.gpuReadinessLeadUs() == 6000,
           "readiness percentile must update after window rollover");
}

void testDeferredGpuObservationDoesNotCreateBufferPressure()
{
    auto session = config(120, 240);
    session.latencyMode = 1;
    auto policy = vrrTimingParametersForSession(session);
    policy.playoutDelayStartUs = 1000;
    policy.playoutDelayStartPeriodPerMille = 0;
    policy.playoutDelayMinimumUs = 1000;
    policy.playoutDelayMaximumUs = 6000;
    policy.playoutDelayMaximumPeriodPerMille = 0;
    policy.playoutDelayCapSourcePeriodPerMille = 0;
    VrrTimingController preparationBound(session, true, policy);
    VrrTimingController lateObservation(session, true, policy);
    bool sawCurrentPressure = false;
    bool sawPreparationBoundGrowth = false;
    bool sawLateObservationGrowth = false;

    for (int i = 0; i < 600; ++i) {
        const uint32_t rtp = static_cast<uint32_t>(i * 750);
        const uint64_t decoded = decodedTimeForRtp(1000000, rtp);
        const auto boundedDecision = preparationBound.schedule(
            frame(i, rtp, true, decoded), decoded);
        const auto observedDecision = lateObservation.schedule(
            frame(i, rtp, true, decoded), decoded);
        expect(boundedDecision.targetUs == observedDecision.targetUs &&
                   boundedDecision.playoutDelayUs ==
                       observedDecision.playoutDelayUs,
               "late fence observation must not change the current or future presentation target");

        const uint64_t preparationStartUs = std::max(
            decoded, boundedDecision.renderStartUs);
        const uint64_t preparationCompleteUs = std::min(
            boundedDecision.originalTargetUs,
            preparationStartUs + 500);
        preparationBound.notePreparationDuration(
            500, 0, preparationCompleteUs);
        lateObservation.notePreparationDuration(
            500, 0, preparationCompleteUs);
        preparationBound.noteSchedulerDelays(0, 0, true);
        lateObservation.noteSchedulerDelays(0, 0, true);

        // Both fences have zero residual wait and the same absorbable service
        // bound. The second is merely first observed after its target wake.
        preparationBound.noteDeferredGpuReady(
            0, true, preparationCompleteUs, 6000);
        lateObservation.noteDeferredGpuReady(
            0, true, observedDecision.originalTargetUs + 1000, 6000);
        const uint64_t submittedUs = boundedDecision.targetUs +
            (i % 2 ? 3000 : 0);
        preparationBound.noteSubmission(true, false, submittedUs);
        lateObservation.noteSubmission(true, false, submittedUs);

        const auto boundedUpdate = preparationBound.intervalStats().update;
        const auto observedUpdate = lateObservation.intervalStats().update;
        sawCurrentPressure |=
            observedUpdate.action ==
                Vrr13::IntervalBuffer::Action::CurrentPressure ||
            observedUpdate.action ==
                Vrr13::IntervalBuffer::Action::NoFreshMiss;
        sawPreparationBoundGrowth |=
            boundedUpdate.action == Vrr13::IntervalBuffer::Action::Grow;
        sawLateObservationGrowth |=
            observedUpdate.action == Vrr13::IntervalBuffer::Action::Grow;
    }

    expect(sawCurrentPressure,
           "the deferred-observation fixture must exercise sustained interval pressure");
    expect(!sawPreparationBoundGrowth && !sawLateObservationGrowth,
           "observing an already-complete fence after cadence hold must not manufacture readiness growth");
    expect(preparationBound.intervalStats().update.requestedUs ==
               lateObservation.intervalStats().update.requestedUs,
           "fence observation time must leave the interval-buffer request unchanged");

    // Actual unfinished GPU work at the target is different from observing
    // an already-complete fence late. Revision 1 silently discarded this
    // readiness miss; revision 2 must learn without counting the cadence hold.
    VrrTimingController residualWait(session, true, policy);
    bool residualGrew = false;
    for (int i = 0; i < 600; ++i) {
        const uint32_t rtp = static_cast<uint32_t>(i * 750);
        const uint64_t decoded = decodedTimeForRtp(1000000, rtp);
        for (auto* controller : {&residualWait}) {
            const auto decision = controller->schedule(frame(i, rtp, true, decoded), decoded);
            controller->notePreparationDuration(500, 0, decoded + 500);
            controller->noteSchedulerDelays(0, 0, true);
            const uint64_t wait = i % 2 ? 3000 : 0;
            controller->noteDeferredGpuReady(wait, true, decision.targetUs + wait,
                decision.targetUs + wait - decoded, wait != 0);
            controller->noteSubmission(true, false, decision.targetUs + wait);
        }
        residualGrew |= residualWait.intervalStats().update.action == Vrr13::IntervalBuffer::Action::Grow;
    }
    expect(residualGrew,
           "only verified residual GPU waits must supply readiness pressure to the new policy");
}

void testDelayedDisplayEventsAgreeAcrossBackends()
{
    const auto session = config(60, 120);
    const auto policy = vrrTimingParametersForSession(session);
    VrrTimingController linuxController(session, true, policy);
    VrrTimingController windowsController(session, true, policy);
    std::array<Vrr13::PresentationObservation, 3> delayed{};
    uint64_t beforeHitch = 0, maximumAfterHitch = 0;
    for (uint64_t i = 1; i < 700; ++i) {
        const uint64_t source = decodedTimeForRtp(1000000, uint32_t(i * 1500));
        const auto linuxDecision = linuxController.schedule(frame(i, uint32_t(i * 1500), true, source), source);
        const auto windowsDecision = windowsController.schedule(frame(i, uint32_t(i * 1500), true, source), source);
        expect(linuxDecision.playoutDelayUs == windowsDecision.playoutDelayUs &&
               linuxDecision.nativeSmoothnessSamples == windowsDecision.nativeSmoothnessSamples,
               "verified delayed display events must drive the same padding across backends");
        linuxController.notePreparationDuration(1000);
        windowsController.notePreparationDuration(1000);
        linuxController.noteSubmission(true, false, linuxDecision.targetUs);
        windowsController.noteSubmission(true, false, windowsDecision.targetUs);
        Vrr13::PresentationObservation current;
        current.timeKind = Vrr13::PresentationTimeKind::DisplayEvent;
        current.smoothness = linuxController.smoothnessSample(linuxDecision);
        current.submitted = current.idValid = true;
        current.id = i;
        current.submission = linuxDecision.targetUs;
        current.ready = source + 1000;
        current.deadline = linuxDecision.originalScanoutUs;
        current.observed = linuxDecision.targetUs + 100;
        current.uncertainty = 10;
        // Real feedback arrives three submissions later, with IDs belonging to
        // that older image, not to the frame currently being submitted.
        const auto older = delayed[i % delayed.size()];
        if (older.id) {
            current.sampleValid = true;
            current.sampleId = older.id;
            current.sampleTime = older.submission + 2000 + (older.id == 300 ? 6000 : 0);
            current.presentRefresh = current.syncRefresh = older.id;
        }
        delayed[i % delayed.size()] = current;
        linuxController.notePresentation(current);
        current.dxgi = true;
        windowsController.notePresentation(current);
        if (i == 299) beforeHitch = linuxDecision.playoutDelayUs;
        if (i > 303) maximumAfterHitch = std::max(maximumAfterHitch, linuxDecision.playoutDelayUs);
    }
    expect(maximumAfterHitch <= beforeHitch && linuxController.nativeCadenceHitches() > 0,
           "delayed compositor hitches must remain diagnostic without growing predictive padding");
}

// A PyroWave frame delivered with zero-filled detail can still be late (its
// lost tail waits out a pacing gap), but lost detail is neither delivery jitter
// nor slow work. Presenting such frames late must not teach any adaptive
// buffer to add standing latency. The same faults without loss still do.
void testLostPacketFramesDoNotGrowBuffer()
{
    struct Variant { const char* name; bool readinessHitch; bool legacyPredictor; bool controlGrows; };
    const Variant variants[] = {
        {"production", false, false, false},
        {"prediction-only", false, true, true},
    };
    for (const auto& variant : variants) {
        auto session = config(60, 120);
        auto policy = vrrTimingParametersForSession(session);
        if (variant.legacyPredictor) {

            policy.playoutDelayMarginUs = 3000;
        }
        uint64_t clean[2] = {}, peak[2] = {};
        for (int lossy = 0; lossy < 2; ++lossy) {
            VrrTimingController controller(session, true, policy);
            uint64_t last = 0;
            for (int i = 0; i < 3000; ++i) {
                const auto source = decodedTimeForRtp(1000000, uint32_t(i * 1500));
                const bool fault = i >= 600 && i < 1200 && i % 10 == 9;
                const auto decoded = source + (fault ? 9000 : 0);
                const auto now = std::max(decoded, last);
                auto paced = frame(i, uint32_t(i * 1500), true, decoded);
                if (lossy && fault) paced.setLostPackets(12);
                const auto d = controller.schedule(paced, now);
                controller.notePreparationDuration(1000);
                last = std::max(d.targetUs, std::max(now, d.renderStartUs) + 1000);
                controller.noteSubmission(true, false, last);
                if (i == 599) clean[lossy] = d.playoutDelayUs;
                if (i >= 600) peak[lossy] = std::max(peak[lossy], d.playoutDelayUs);
            }
        }
        std::printf("lost-packet buffer (%s): clean %llu peak %llu; with loss clean %llu peak %llu\n",
                    variant.name, (unsigned long long)clean[0], (unsigned long long)peak[0],
                    (unsigned long long)clean[1], (unsigned long long)peak[1]);
        if (variant.controlGrows) {
            expect(peak[0] > clean[0], "late frames without loss must still earn buffering");
        }
        expect(peak[1] <= clean[1], "late frames with lost packets must not grow the playout buffer");
        expect(peak[1] <= peak[0], "loss must never earn more buffering than the same lateness without it");
    }
}

void testHistoricalSourceGapSmoothFrameTiming()
{
    // Approx. 77 FPS with alternating 11/15 ms source intervals. Exercise
    // the actual session resolver and adaptive buffer at every latency preset.
    for (int mode : {0, 1, 2}) {
        struct Result {
            uint64_t jerk = 0;
            uint64_t latency = 0;
            unsigned samples = 0;
        } results[2];
        for (int enabled : {0, 1}) {
            auto session = config(120, 120);
            session.latencyMode = mode;
            session.smoothFrameTiming = enabled != 0;
            auto policy = vrrTimingParametersForSession(session);
            policy.playoutSmoothingOwnMovement = 0;
            policy.playoutSmoothingReadinessBound = enabled ? 1 : 0;
            policy.smoothingMode = enabled ? 4 : 2;
            policy.playoutSmoothingMaxLagUs = enabled ? session.timingOptions.resolved(mode).toleranceUs : 6000;
            policy.playoutSmoothingReserveMaxUs = 0;
            policy.playoutSmoothingReserveToleranceUs = 0;
            policy.playoutSmoothingReservePercentilePerMille = 0;
            policy.playoutSmoothingReserveReleaseUsPerSecond = 0;
            policy.playoutSmoothingPeriodFeedbackPerMillion = 0;
            policy.playoutSmoothingResetSlewUs = 0;
            const auto tolerance = uint64_t(session.timingOptions.resolved(mode).toleranceUs);
            expect(policy.playoutSmoothingGainPerMille == (enabled ? 150 : 0) &&
                       policy.playoutSmoothingPeriodAlphaPerMille == 25 &&
                       policy.playoutSmoothingRecoveryUs == 0 &&
                       policy.playoutSmoothingMaxLagUs == (enabled ? tolerance : 6000) &&
                       policy.smoothingMode == (enabled ? 4U : 2U) &&
                       policy.playoutSmoothingAbsorbReserveUs == 0 &&
                       policy.playoutSmoothingPeriodFeedbackPerMillion == 0 &&
                       policy.playoutSmoothingReserveMaxUs == 0 &&
                       policy.playoutSmoothingReserveToleranceUs == 0 &&
                       policy.playoutSmoothingReservePercentilePerMille == 0 &&
                       policy.playoutSmoothingReserveReleaseUsPerSecond == 0 &&
                       true,
                   "the preference must select bounded smoothing independently of the timing preset");
            VrrTimingController controller(session, true, policy);
            uint32_t ticks = 0;
            uint64_t lastSubmission = 0, previousInterval = 0;
            int64_t previousSmoothing = 0;
            for (int i = 0; i < 1800; ++i) {
                // A host stall, then a sustained change to 60 FPS, must not
                // trap the smoother at the original rate or grow phase debt.
                ticks += i == 600 ? 4500 : i >= 900 ? 1500 :
                    (i % 2 ? 1350 : 990);
                const auto decoded = decodedTimeForRtp(1000000, ticks);
                const auto now = std::max(decoded, lastSubmission);
                const auto d = controller.schedule(frame(i, ticks, true, decoded), now);
                const uint64_t lateWake = i == 750 ? 4000 : 0;
                const auto ready = std::max(now, d.renderStartUs) + 1000 + lateWake;
                const auto submitted = std::max(d.targetUs, ready);
                controller.notePreparationDuration(1000);
                controller.noteSchedulerDelays(lateWake, 0, true);
                controller.noteSubmission(true, false, submitted);
                expect(std::abs(d.cadenceSmoothingUs) <= int64_t(tolerance) &&
                           std::abs(d.cadenceSmoothingUs - previousSmoothing) <= int64_t(tolerance) &&
                           (enabled || d.cadenceSmoothingUs == 0),
                       "production smoothing must respect both total/per-gap tolerance bounds and the off switch");
                expect(submitted >= decoded && submitted - decoded <= 30000 &&
                           d.playoutDelayUs <= controller.playoutQueueLimitUs(),
                       "host jitter, stalls and rate changes must retain bounded latency and queue capacity");
                if (i == 600) {
                    expect(d.cadenceSmoothingUs == 0,
                           "a host stall must restart smoothing on the raw source slot");
                }
                if (i > 1200) {
                    expect(std::abs(int64_t(controller.sourcePeriodUs()) - 16667) <= 2 &&
                               std::abs(d.cadenceSmoothingUs) <= int64_t(tolerance) &&
                               d.cadenceSmoothingUs == previousSmoothing,
                           "the smoother must follow a new source rate without changing clean intervals to repay phase");
                }
                const uint64_t interval = submitted - lastSubmission;
                if (i >= 200 && i < 600) {
                    results[enabled].jerk += interval > previousInterval ?
                        interval - previousInterval : previousInterval - interval;
                    results[enabled].latency += submitted - decoded;
                    ++results[enabled].samples;
                }
                previousSmoothing = d.cadenceSmoothingUs;
                previousInterval = interval;
                lastSubmission = submitted;
            }
        }
        expect(results[1].jerk < results[0].jerk,
               "one-tolerance smoothing must reduce the excess wobble without trying to flatten the entire source variation");
        std::fprintf(stderr, "production smoothing mode %d: mean jerk %llu -> %llu us, latency %llu -> %llu us\n",
                     mode, static_cast<unsigned long long>(results[0].jerk / results[0].samples),
                     static_cast<unsigned long long>(results[1].jerk / results[1].samples),
                     static_cast<unsigned long long>(results[0].latency / results[0].samples),
                     static_cast<unsigned long long>(results[1].latency / results[1].samples));
        // This clean-delivery fixture must not buy the old multi-ms reserve.
        const auto tolerance = uint64_t(VrrTimingOptions{}.resolved(mode).toleranceUs);
        expect(results[1].latency <= results[0].latency + results[1].samples * tolerance,
               "source-gap smoothing must add at most one tolerance average latency under alternating host jitter");
    }
}

void testWithinToleranceCannotCreateQueuedRecovery()
{
    auto session = config(120, 120);
    session.smoothFrameTiming = false;
    session.experimentalBoundedCatchup = true;
    session.experimentalInputPercentilePpm = 990000;
    session.latencyMode = 2;
    const auto policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session, true, policy);
    uint64_t previous = 0, previousNominal = 0;
    uint64_t maximumCarry = 0, maximumIntervalError = 0;
    for (int i = 0; i < 6000; ++i) {
        const auto decoded = decodedTimeForRtp(1000000, uint32_t(i * 750));
        const auto now = std::max(decoded, previous);
        const auto d = controller.schedule(frame(i + 1, uint32_t(i * 750), true, decoded), now);
        const uint64_t overshoot = i % 2 ? 300 : 100;
        const auto submitted = std::max(d.targetUs, now + 1000) + overshoot;
        if (i > 0) {
            const auto actual = submitted - previous;
            const auto intended = d.originalTargetUs - previousNominal;
            maximumIntervalError = std::max(maximumIntervalError,
                std::max(actual, intended) - std::min(actual, intended));
        }
        if (i > 300) {
            maximumCarry = std::max(maximumCarry, d.trajectory.carryUs);
            expect(d.latchedPresentation, "within-tolerance fixture must retain native queue protection");
        }
        controller.notePreparationDuration(1000);
        controller.noteSubmission(true, false, submitted);
        previous = submitted;
        previousNominal = d.originalTargetUs;
    }
    std::fprintf(stderr, "within-tolerance interval error=%llu us, recovery carry=%llu us\n",
        static_cast<unsigned long long>(maximumIntervalError),
        static_cast<unsigned long long>(maximumCarry));
    expect(maximumIntervalError <= policy.playoutIntervalToleranceUs,
        "every measured interval in the regression must remain within the selected tolerance");
    expect(maximumCarry == 0,
        "within-tolerance timing must not manufacture queued recovery delay");

    // Retain the recovery trigger: exactly T is accepted, while a real
    // beyond-T event still creates bounded carry on the existing trajectory.
    for (uint64_t lateness : {499, 500, 501}) {
        Vrr13::PhaseTrajectory trajectory;
        uint64_t carry = 0;
        for (uint64_t i = 0; i < 100; ++i) {
            const uint64_t slot = 1000000 + i * 8333;
            const auto planned = trajectory.plan(i + 1, slot, slot, slot,
                slot, 8333, 0, 500, 8333, true, 4000);
            carry = std::max(carry, planned.carryUs);
            const uint64_t submitted = planned.targetUs + (i == 50 ? lateness : 0);
            trajectory.commit(i + 1, slot, submitted, submitted, true, false, true);
        }
        expect(carry == (lateness > 500 ? lateness : 0),
            "recovery must accept exact tolerance and retain genuine above-tolerance carry");
    }
}

void testQueuedCeilingRecoveryCannotRatchet()
{
    uint64_t maximumCarry[2] = {};
    for (int corrected : {0, 1}) {
        auto session = config(120, 120);
        session.smoothFrameTiming = false;
        session.experimentalBoundedCatchup = true;
        session.experimentalInputPercentilePpm = 990000;
        session.latencyMode = 2;
        auto policy = vrrTimingParametersForSession(session);
        // Isolate the recorded floor variants from the newer ceiling drain.
        policy.playoutCeilingPhaseReplacement = 0;
        policy.latchedFloorDisabled = corrected ? 2 : 1;
        VrrTimingController controller(session, true, policy);
        uint64_t previous = 0;
        for (int i = 0; i < 18000; ++i) {
            const auto decoded = decodedTimeForRtp(1000000, uint32_t(i * 750));
            const auto now = std::max(decoded, previous);
            const auto d = controller.schedule(frame(i + 1, uint32_t(i * 750), true, decoded), now);
            // A stable 120 FPS source and ordinary variable wake-up overshoot.
            // The protected native queue spaces display; enqueue time is not
            // an additional physical display deadline.
            const uint64_t overshoot = 100 + uint64_t(i % 7) * 100;
            const auto submitted = std::max(d.targetUs, now + 1000) + overshoot;
            expect(i == 0 || d.latchedPresentation, "ceiling recovery must retain native queue protection");
            controller.notePreparationDuration(1000);
            controller.noteSubmission(true, false, submitted);
            previous = submitted;
            if (i > 300) maximumCarry[corrected] = std::max(maximumCarry[corrected], d.trajectory.carryUs);
        }
    }
    std::fprintf(stderr, "queued ceiling maximum recovery carry: %llu -> %llu us\n",
                 static_cast<unsigned long long>(maximumCarry[0]),
                 static_cast<unsigned long long>(maximumCarry[1]));
    expect(maximumCarry[0] > 4000 && maximumCarry[1] <= 4000,
           "protected queue must not reimpose CPU overshoot above intentional recovery cap");
    // Removing the duplicate floor is limited to queue-owned presentation.
    // Compare both revisions with immediate and explicit future deadlines.
    for (bool nativeDeadline : {false, true}) {
        auto session = config(120, 120);
        session.smoothFrameTiming = false;
        session.experimentalBoundedCatchup = true;
        auto oldPolicy = vrrTimingParametersForSession(session, nativeDeadline, nativeDeadline);
        oldPolicy.latchedFloorDisabled = 1;
        auto newPolicy = oldPolicy;
        newPolicy.latchedFloorDisabled = 3;
        VrrTimingController oldController(session, nativeDeadline, oldPolicy);
        VrrTimingController newController(session, nativeDeadline, newPolicy);
        for (int i = 0; i < 300; ++i) {
            const auto decoded = decodedTimeForRtp(1000000, uint32_t(i * 750));
            const auto input = frame(i + 1, uint32_t(i * 750), true, decoded);
            const auto a = oldController.schedule(input, decoded);
            const auto b = newController.schedule(input, decoded);
            expect(a.targetUs == b.targetUs && a.requestedPhaseTargetUs == b.requestedPhaseTargetUs,
                   "immediate and future-deadline physical floors must remain identical");
            const auto submitted = std::max(a.targetUs, decoded + 1000) + uint64_t(i % 7) * 100;
            oldController.noteSubmission(true, false, submitted);
            newController.noteSubmission(true, false, submitted);
        }
    }
}

void testProducerClockExcludesLateGpuCollection()
{
    uint64_t worstClockSkew[2] = {};
    for (int corrected : {0, 1}) {
        auto session = config(120, 120);
        session.smoothFrameTiming = false;
        session.experimentalBoundedCatchup = true;
        auto policy = vrrTimingParametersForSession(session);
        policy.normalizedSourceIntrinsicDecodeGpu = corrected;
        policy.playoutOffsetWindowUs = 250000;
        VrrTimingController controller(session, true, policy);
        for (int i = 0; i < 1200; ++i) {
            const uint64_t output = 1000000 + uint64_t(i) * 8333;
            const uint64_t completion = output + (i < 8 ? 500 : 15500);
            auto input = frame(i + 1, uint32_t(i * 750), true, output);
            input.setDeliveryTimeline(output - 900, output - 800, output - 500);
            input.noteGpuReadyUs(completion);
            input.noteDecodeSyncWaitUs(500);
            const auto d = controller.schedule(input, completion);
            const auto submitted = std::max(d.targetUs, completion + 1000);
            expect(submitted >= completion,
                   "producer clock correction must retain actual GPU readiness");
            if (i > 600 && d.sourceTimeUs > output)
                worstClockSkew[corrected] = std::max(worstClockSkew[corrected], d.sourceTimeUs - output);
            controller.notePreparationDuration(1000);
            controller.noteSubmission(true, false, submitted);
        }
    }
    std::fprintf(stderr, "late GPU collection clock skew: %llu -> %llu us\n",
                 static_cast<unsigned long long>(worstClockSkew[0]),
                 static_cast<unsigned long long>(worstClockSkew[1]));
    expect(worstClockSkew[0] < 2000 && worstClockSkew[1] < 2000,
           "both source-mapping paths must exclude our own queue residence from GPU service");

    auto session = config(120, 120);
    session.smoothFrameTiming = false;
    session.experimentalBoundedCatchup = true;
    VrrTimingController controller(session, true, vrrTimingParametersForSession(session));
    auto input = frame(1, 750, true, 1000000);
    input.setDeliveryTimeline(999000, 999200, 999500);
    input.noteGpuReadyUs(1015500);
    input.noteDecodeSyncWaitUs(15500);
    const auto d = controller.schedule(input, 1015500);
    expect(d.sourceTimeUs >= 1014000,
           "promptly observed genuine long GPU service must remain in the producer clock");
}

void testProductionWorkerSmoothingOwnsMovement()
{
    // Alternating 11/15 ms source intervals require sub-millisecond retiming
    // every frame. A T/second adaptation budget allows only a few us instead.
    for (bool absorb : {false, true}) {
        uint64_t jerk[2] = {};
        for (int own : {0, 1}) {
            auto session = config(120, 120);
            session.smoothFrameTiming = true;
            session.absorbMicrostutter = absorb;
            session = vrrProductionSessionConfig(session);
            auto policy = vrrTimingParametersForSession(session);
            expect(policy.playoutSmoothingOwnMovement == 1 &&
                       policy.smoothingMode == (absorb ? 7u : 2u) &&
                       policy.playoutSmoothingMaxLagUs == (absorb ? 1000u : 6000u) &&
                       policy.playoutSmoothingReserveMaxUs == (absorb ? 0u : 3000u),
                   "both worker smoothing paths must own their existing bounded retiming");
            policy.playoutSmoothingOwnMovement = own;
            VrrTimingController controller(session, true, policy);
            uint32_t ticks = 0;
            uint64_t previous = 0, previousInterval = 0;
            for (int i = 0; i < 1200; ++i) {
                ticks += i % 2 ? 1350 : 990;
                const auto decoded = decodedTimeForRtp(1000000, ticks);
                const auto now = std::max(decoded, previous);
                const auto d = controller.schedule(frame(i + 1, ticks, true, decoded), now);
                const auto ready = std::max(now, d.renderStartUs) + 1000;
                const auto submitted = std::max(d.targetUs, ready);
                controller.notePreparationDuration(1000);
                controller.noteSubmission(true, false, submitted);
                expect(submitted >= decoded && submitted - decoded <= 40000 &&
                           std::abs(d.cadenceSmoothingUs) <= 9000,
                       "restored smoothing must retain readiness, bounded phase and latency");
                const auto interval = submitted - previous;
                if (i > 300) jerk[own] += interval > previousInterval ?
                    interval - previousInterval : previousInterval - interval;
                previousInterval = interval;
                previous = submitted;
            }
        }
        std::fprintf(stderr, "worker smoothing movement (absorb=%d): jerk %llu -> %llu us\n", int(absorb),
                     static_cast<unsigned long long>(jerk[0]), static_cast<unsigned long long>(jerk[1]));
        expect(absorb ? jerk[1] * 4 < jerk[0] * 3 : jerk[1] * 2 < jerk[0],
               "both production smoothers must remove source wobble instead of spending T/second");
    }
}

void testProductionPreservesRelativeGameSpacing()
{
    auto session = config(120, 120);
    session.smoothFrameTiming = false;
    auto policy = vrrTimingParametersForSession(session);
    expect(policy.playoutDelayMaximumUs == policy.playoutDelayMinimumUs &&
               policy.playoutDelayMaximumPeriodPerMille == 2000 &&
               policy.playoutDelayCapSourcePeriodPerMille == 2000,
           "production Smooth must allow two fitted source periods");
    // Hold padding constant to isolate the spacing contract from adaptation.
    policy.playoutDelayMaximumPeriodPerMille = 0;
    policy.playoutDelayCapSourcePeriodPerMille = 0;
    policy.playoutDelayMinimumUs = policy.playoutDelayMaximumUs;
    VrrTimingController zeroEpoch(session, true, policy);
    VrrTimingController shiftedEpoch(session, true, policy);
    uint32_t ticks = 0;
    uint64_t previousTarget = 0, previousSource = 0;
    const uint32_t intervals[] = {750, 1500, 1000, 1250};
    for (int i = 0; i < 500; ++i) {
        ticks += intervals[i % 4];
        const uint64_t source = uint64_t(ticks) * 1000 / 90;
        const uint64_t decoded = 1000000 + source;
        const auto a = zeroEpoch.schedule(frame(i, ticks, true, decoded), decoded);
        const auto b = shiftedEpoch.schedule(frame(i, ticks + 0xffff0000U, true, decoded), decoded);
        expect(a.targetUs == b.targetUs && a.originalTargetUs == b.originalTargetUs,
               "an arbitrary RTP epoch or wrap must not alter the client deadline");
        if (i > 32) {
            const auto error = int64_t(a.originalTargetUs - previousTarget) -
                               int64_t(source - previousSource);
            expect(std::abs(error) <= 1 && a.cadenceSmoothingUs == 0,
                   "variable game intervals must survive unchanged through fixed playout padding");
        }
        previousTarget = a.originalTargetUs; previousSource = source;
        for (auto* controller : {&zeroEpoch, &shiftedEpoch}) {
            controller->notePreparationDuration(1000);
            controller->noteSubmission(true, false, a.targetUs);
        }
    }
}





void testLatencyFixBufferlessLateFrameSafety()
{
    auto session = config(120, 120);
    auto policy = vrrTimingParametersForSession(session);
    policy.playoutDelayCapSourcePeriodPerMille = 0;
    policy.latencyFixDelayPeriodPerMille = 0;
    for (bool canLatch : {false, true}) {
        VrrTimingController controller(session, canLatch, policy);
        for (int i = 0; i < 360; ++i) {
            const auto rtp = uint32_t(i * 750);
            const auto decoded = decodedTimeForRtp(1000000, rtp) + (i == 180 ? 18000 : 0);
            const auto now = std::max(decoded, controller.lastSubmissionUs());
            const auto decision = controller.schedule(frame(i, rtp, i != 181, decoded), now);
            expect(controller.latencyFixActive() && decision.playoutDelayUs == 0,
                   "zero delay must override startup, late-frame and invalid-RTP fallback padding");
            expect(controller.playoutDelayUs() == 0,
                   "delay telemetry must agree with the active fallback limit");
            expect(decision.targetUs >= now && decision.targetUs >= controller.earliestSubmissionUs(),
                   "a lone late frame must remain presentable without violating readiness or the active native spacing floor");
            if (i > 0 && !decision.latchedPresentation) {
                expect(decision.targetUs >= controller.lastSubmissionUs() +
                           controller.displayPeriodUs() + decision.guardUs,
                       "bufferless adaptive presentation must retain its scanout safety floor");
            }
            controller.notePreparationDuration(1000);
            controller.noteSubmission(true, false, decision.targetUs);
        }
    }
}

void testLatencyPresetsAcrossSourceAndDisplayRates()
{
    expect(VrrSessionConfig{}.latencyMode == 0 &&
               VrrTimingParameters{}.latencyFixAllRates == 0 &&
               VrrTimingParameters{}.playoutDelayCapSourcePeriodPerMille == 0,
           "historical session and replay defaults must retain Smooth and the legacy rate band");
    struct Rates { int source; int display; };
    for (const auto rates : {Rates{40, 120}, Rates{60, 120}, Rates{100, 120},
                             Rates{120, 120}, Rates{60, 60}, Rates{120, 240}}) {
        const auto ordinarySession = config(rates.source, rates.display);
        const auto ordinaryPolicy = vrrTimingParametersForSession(ordinarySession);
        expect(ordinaryPolicy.latencyFixEnabled == 1 &&
                   ordinaryPolicy.latencyFixAllRates == 1 &&
                   ordinaryPolicy.playoutDelayCapSourcePeriodPerMille == 2000 &&
                   ordinaryPolicy.playoutDelayMaximumUs == ordinaryPolicy.playoutDelayMinimumUs &&
                   ordinaryPolicy.playoutDelayMaximumPeriodPerMille == 2000,
               "Smooth must allow two fitted source frames within capacity limits");
        for (int mode : {1, 2}) {
            auto session = ordinarySession;
            session.latencyMode = mode;
            const auto policy = vrrTimingParametersForSession(session);
            const uint64_t capPerMille = mode == 2 ? 500 : 1000;
            expect(policy.latencyFixEnabled == 1 && policy.latencyFixAllRates == 1 &&
                       policy.latencyFixDelayPeriodPerMille == 500 &&
                       policy.playoutDelayCapSourcePeriodPerMille == capPerMille,
                   "Balanced Target and Low Latency must resolve to replayable source-frame buffer caps");
            for (bool canLatch : {false, true}) {
                VrrTimingController selected(session, canLatch, policy);
                VrrTimingController ordinary(ordinarySession, canLatch, ordinaryPolicy);
                // Replay parameters, including an old disabled snapshot, win
                // over the user's current preference.
                VrrTimingController recorded(session, canLatch, ordinaryPolicy);
                expect(selected.latencyFixActive() && recorded.latencyFixActive(),
                       "all presets must activate the shared all-rate policy at cold start");
                for (int i = 0; i < 240; ++i) {
                    const auto rtp = uint32_t(std::llround(i * 90000.0 / rates.source));
                    const auto decoded = decodedTimeForRtp(1000000, rtp) +
                        (i == 120 ? 18000 : (i % 7) * 50);
                    const bool validRtp = i != 121 && i != 122;
                    const auto a = ordinary.schedule(frame(i, rtp, validRtp, decoded),
                        std::max(decoded, ordinary.lastSubmissionUs()));
                    const auto b = recorded.schedule(frame(i, rtp, validRtp, decoded),
                        std::max(decoded, recorded.lastSubmissionUs()));
                    const uint64_t ordinaryLimit =
                        ordinary.sourcePeriodUs() * 4000 / 1000;
                    expect(a.targetUs == b.targetUs && a.originalTargetUs == b.originalTargetUs &&
                               a.renderStartUs == b.renderStartUs &&
                               a.playoutDelayUs == b.playoutDelayUs &&
                               a.requestedPlayoutDelayUs == b.requestedPlayoutDelayUs &&
                               a.latchedPresentation == b.latchedPresentation,
                           "Smooth and its recorded policy must retain identical decisions through late and invalid-RTP frames");
                    expect(a.playoutDelayUs <= ordinaryLimit &&
                               ordinary.playoutDelayUs() <= ordinaryLimit,
                           "Smooth padding must stay within four fitted source frames");
                    const auto now = std::max(decoded, selected.lastSubmissionUs());
                    const auto decision = selected.schedule(frame(i, rtp, validRtp, decoded), now);
                    const uint64_t limit =
                        selected.sourcePeriodUs() * capPerMille / 1000;
                    expect(selected.latencyFixActive() && decision.playoutDelayUs <= limit &&
                               selected.playoutDelayUs() <= limit,
                           "startup, late-frame and invalid-RTP padding must obey the source-frame cap at every rate");
                    expect(decision.targetUs >= now &&
                               decision.targetUs >= selected.earliestSubmissionUs(),
                           "reduced padding must retain readiness and native presentation deadlines");
                    if (i > 0 && !decision.latchedPresentation) {
                        expect(decision.targetUs >= selected.lastSubmissionUs() +
                                   selected.displayPeriodUs() + decision.guardUs,
                               "all-rate presets must preserve the adaptive scanout safety floor");
                    }
                    ordinary.notePreparationDuration(1000);
                    recorded.notePreparationDuration(1000);
                    selected.notePreparationDuration(1000);
                    ordinary.noteSubmission(true, false, a.targetUs);
                    recorded.noteSubmission(true, false, b.targetUs);
                    selected.noteSubmission(true, false, decision.targetUs);
                }
            }
        }
    }
}

void testLatencyPresetsBoundHitchesThroughCadenceChanges()
{
    for (int mode : {1, 2}) {
        const auto ordinarySession = config(100, 120);
        auto selectedSession = ordinarySession;
        selectedSession.latencyMode = mode;
        VrrTimingController ordinary(ordinarySession, true, vrrTimingParametersForSession(ordinarySession));
        VrrTimingController selected(selectedSession, true, vrrTimingParametersForSession(selectedSession));
        const uint64_t capPerMille = mode == 2 ? 500 : 1000;
        uint64_t ordinaryBeforeHitches = 0;
        uint64_t ordinaryMaximum = 0;
        double ticks = 0;
        int number = 0;
        for (int rate : {100, 120, 60, 40, 117, 113}) {
            for (int i = 0; i < 600; ++i, ++number) {
                ticks += 90000.0 / rate;
                const auto rtp = uint32_t(std::llround(ticks));
                const auto decoded = decodedTimeForRtp(1000000, rtp);
                for (auto* controller : {&ordinary, &selected}) {
                    const auto priorSubmission = controller->lastSubmissionUs();
                    const auto decision = controller->schedule(frame(number, rtp, true, decoded),
                        std::max(decoded, priorSubmission));
                    if (controller == &ordinary && number < 600) {
                        if (number == 199) ordinaryBeforeHitches = decision.playoutDelayUs;
                        if (number > 200)
                            ordinaryMaximum = std::max(ordinaryMaximum, decision.playoutDelayUs);
                    }
                    if (controller == &selected) {
                        const uint64_t limit =
                            controller->sourcePeriodUs() * capPerMille / 1000;
                        expect(controller->latencyFixActive() && decision.playoutDelayUs <= limit,
                               "native hitches and cadence steps must not revive the old rate band or exceed the preset cushion");
                        expect(controller->lastSubmissionUs() == priorSubmission,
                               "a source-rate step must retain the prior submission boundary");
                        if (number > 0) {
                            expect(!decision.rebased && !decision.phaseDiscontinuity,
                                   "ordinary source-rate changes must retain the timing epoch in reduced-delay presets");
                        }
                    }
                    controller->notePreparationDuration(1000);
                    controller->noteSubmission(true, false, decision.targetUs);
                    Vrr13::PresentationObservation observation;
                    observation.timeKind = Vrr13::PresentationTimeKind::DisplayEvent;
                    observation.smoothness = controller->smoothnessSample(decision);
                    observation.submitted = observation.idValid = observation.sampleValid = true;
                    observation.id = observation.sampleId = uint64_t(number + 1);
                    observation.submission = decision.targetUs;
                    observation.ready = decoded + 1000;
                    const uint64_t hitch = number >= 200 && number % 10 == 9 ? 6000 : 0;
                    observation.sampleTime = observation.smoothness.intended + 20000 + hitch;
                    observation.observed = observation.sampleTime;
                    observation.deadline = decision.originalScanoutUs;
                    controller->notePresentation(observation);
                }
            }
        }
        expect(ordinaryMaximum <= ordinaryBeforeHitches,
               "display-only hitches must not grow Smooth's predictive buffer");
        expect(selected.nativeCadenceHitches() > 20,
               "reduced-delay presets must continue reporting the hitches they choose not to buffer");
    }
}

// Reduce judder fixtures: host present stamps and client delivery, both in
// integer arithmetic so every compiler replays the same frames.


// Deterministic delivery: sub-millisecond network/decode variation with a
// 2.5 ms spike on every 97th frame.




// A game presenting at `gameFps` on a fixed-refresh host is captured on the
// next host vblank, so its RTP intervals alternate between whole vblanks.

// The production Reduce judder policy before its reserve, feedback and wider cap.


// 116 FPS near a 120 Hz ceiling, stamped on a ~2.15 ms host capture grid, with
// a four-frame game slowdown (12.9 ms frames) every 150 frames. Capture
// 20260922-193211 showed each slowdown resetting the smoother and stepping its
// accumulated retiming into one presented interval.


void testPresetReadinessTargets()
{
    for (int mode : {0, 1, 2}) {
        auto session = config(120, 116);
        session.latencyMode = mode;
        const auto policy = vrrTimingParametersForSession(session);
        const uint64_t target = mode == 2 ? 990000 : mode == 1 ? 995000 : 999500;
        const uint64_t window = mode == 2 ? 60000000 : mode == 1 ? 120000000 : 300000000;
        expect(policy.playoutOnTimeTargetPerMillion == target &&
                   policy.playoutReadinessWindowUs == window,
               "presets must resolve their exact reliability target and bounded history");
    }
}

void testCustomTimingOptions()
{
    const auto invalid = VrrTimingOptions{-1, 12000, 999, 9999}.resolved(1);
    expect(invalid.bufferPerMille == 250 && invalid.targetHundredths == 9999 &&
           invalid.historySeconds == 300 && invalid.toleranceUs == 2000,
           "custom settings must clamp corrupt or excessive persisted values");
    for (int mode : {0, 1, 2}) {
        const auto migrated = VrrTimingOptions{}.resolved(mode);
        const auto preset = VrrTimingOptions::preset(mode);
        expect(migrated.bufferPerMille == preset.bufferPerMille &&
               migrated.targetHundredths == preset.targetHundredths &&
               migrated.historySeconds == preset.historySeconds &&
               migrated.toleranceUs == preset.toleranceUs,
               "missing saved values must migrate from the selected preset");
    }
    for (int tolerance = 250; tolerance <= 2000; tolerance += 250) {
        auto session = config(120, 120);
        session.timingOptions = {750, 9500, 30, tolerance};
        const auto reference = vrrTimingParametersForSession(session);
        for (int mode : {0, 1, 2}) {
            session.latencyMode = mode;
            const auto policy = vrrTimingParametersForSession(session);
#define CHECK_SAME(type, json, member, defaultValue) \
            expect(policy.member == reference.member, "preset name must not affect custom policy: " #member);
            VRR_TIMING_PARAMETER_FIELDS(CHECK_SAME)
#undef CHECK_SAME
            expect(policy.playoutDelayMaximumPeriodPerMille == 750 &&
                   policy.playoutDelayCapSourcePeriodPerMille == 750 &&
                   policy.playoutOnTimeTargetPerMillion == 950000 &&
                   policy.playoutReadinessWindowUs == 30000000,
                   "all four custom values must reach the controller without preset overrides");
            VrrTimingController controller(session, true, policy);
            for (int i = 0; i < 240; ++i) {
                const auto rtp = uint32_t(i * 750);
                const auto decoded = decodedTimeForRtp(1000000, rtp) + (i % 23 == 0 ? 8000 : 0);
                const auto now = std::max(decoded, controller.lastSubmissionUs());
                const auto decision = controller.schedule(frame(i, rtp, true, decoded), now);
                expect(decision.playoutDelayUs <= controller.sourcePeriodUs() * 750 / 1000 &&
                       decision.targetUs >= now && decision.targetUs >= controller.earliestSubmissionUs(),
                       "custom low targets and tolerances must retain the buffer cap and presentation safety");
                controller.notePreparationDuration(1000);
                controller.noteSubmission(true, false, decision.targetUs);
                expect(controller.intervalStats().toleranceUs == uint64_t(tolerance),
                       "the selected interval tolerance must reach the measured quality score");
            }
        }
    }
    for (int rate : {30, 60, 120, 240, 360}) {
        auto session = config(rate, rate);
        session.timingOptions = {250, 9000, 10, 2000};
        VrrTimingController controller(session, true, vrrTimingParametersForSession(session));
        for (int i = 0; i < 240; ++i) {
            const auto rtp = uint32_t(std::llround(i * 90000.0 / rate));
            const auto decoded = decodedTimeForRtp(1000000, rtp) + (i % 19 == 0 ? 3000 : 0);
            const auto now = std::max(decoded, controller.lastSubmissionUs());
            const auto decision = controller.schedule(frame(i, rtp, true, decoded), now);
            expect(decision.playoutDelayUs <= controller.sourcePeriodUs() / 4 &&
                   decision.targetUs >= now && decision.targetUs >= controller.earliestSubmissionUs(),
                   "quarter-frame custom allowance must retain its cap and native spacing from 30 to 360 FPS");
            controller.notePreparationDuration(250);
            controller.noteSubmission(true, false, decision.targetUs);
        }
    }

}

void testPresetIntervalTolerances()
{
    for (int mode : {0, 1, 2}) {
        auto session = config(120, 116);
        session.latencyMode = mode;
        VrrTimingController controller(
            session, true, vrrTimingParametersForSession(session));
        const auto decision = controller.schedule(
            frame(1, 0, true, 100000), 100000);
        controller.notePreparationDuration(1000, 0, 101000);
        controller.noteSubmission(true, false, decision.targetUs);

        expect(controller.intervalStats().toleranceUs == 500,
               "every preset, including Smooth (revision 3), must use 0.5 ms interval tolerance");
    }
}

void testProductionIntervalParameters()
{
    for (int mode : {0, 1, 2}) {
        auto session = config(116, 120);
        session.latencyMode = mode;
        const auto policy = vrrTimingParametersForSession(session);
        expect(policy.playoutSourceMappingDecoderOutput == 0, "mapping excludes worker waits");
    }
}

void testIntervalQualityBuffer()
{
    // Constant lateness must cancel from spacing; alternating late readiness
    // must acquire bounded protection only when it explains the error.
    for (bool variable : {false, true}) {
        for (bool absorbable : {false, true}) {
            Vrr13::IntervalBuffer buffer;
            uint64_t applied = 1000;
            for (uint64_t i = 1; i <= 500; ++i) {
                const uint64_t intended = 1000000 + i * 10000;
                const uint64_t late = variable && i % 2 ? 3000 : 1000;
                const uint64_t previous = applied;
                buffer.observe({i, intended, intended + late, intended,
                    intended + late, applied, true, absorbable},
                    1000, 4000, 6000000, 125, true, 990000);
                applied = buffer.demand(applied);
                expect(applied >= 1000 && applied <= 4000 && applied <= previous + 250,
                       "interval buffer must respect its minimum, cap, and bounded attack");
            }
            expect(buffer.stats().averageValid, "continuous intervals must establish valid coverage");
            if (!variable) {
                expect(applied == 1000 && buffer.stats().qualityPercent() == 100.0,
                       "constant lateness must not reduce interval quality or grow buffering");
            } else {
                expect(buffer.stats().qualityPercent() < 99.0,
                       "sustained spacing variation must lower the severity-weighted score");
                expect(absorbable ? applied > 1000 : applied == 1000,
                       "only readiness-attributable spacing errors may grow protection");
            }
        }
    }
}

void testPerIntervalBufferPreventsRecoverableSpikes()
{
    Vrr13::IntervalBuffer buffer;
    uint64_t applied = 1000;
    uint64_t excess = 0, unprotectedExcess = 0, peak = applied;
    for (uint64_t i = 1; i <= 4000; ++i) {
        const uint64_t intended = 1000000 + i * 10000;
        const uint64_t ready = intended + (i % 25 == 0 ? 3000 : 1000);
        const uint64_t deadline = intended + applied;
        const uint64_t submitted = std::max(ready, deadline);
        excess += ready > deadline + 500 ? ready - deadline - 500 : 0;
        unprotectedExcess += ready > intended + 1500 ? ready - intended - 1500 : 0;
        buffer.observe({i, intended, submitted, deadline, ready, applied,
                        true, true, 1000, 0},
                       1000, 8000, 6000000, 250, true, 990000,
                       500, 60000000, 500000, 32);
        applied = buffer.demand(applied);
        peak = std::max(peak, applied);
    }
    expect(peak > 1000 && peak <= 4000 && excess < unprotectedExcess &&
               buffer.stats().qualityPercent() >= 99.0,
           "buffering must reduce modeled recoverable excess without chasing the delay cap");
}

void testProductionIntervalHistoryUsesCustomSeconds()
{
    const auto run = [](int historySeconds) {
        auto session = config(100, 120);
        session.smoothFrameTiming = false;
        session.timingOptions = {1000, 9900, historySeconds, 500};
        auto policy = vrrTimingParametersForSession(session);

        policy.playoutDelayAdaptive = 0;
        policy.sourcePlayoutDelayUs = 1000;
        VrrTimingController controller(session, true, policy);
        for (uint64_t i = 1; i <= 2000; ++i) {
            const uint64_t decoded = 1000000 + i * 10000;
            const auto decision = controller.schedule(
                frame(int(i), uint32_t(i * 900), true, decoded), decoded);
            const uint64_t late = i <= 500 && i % 25 == 0 ? 2000 : 0;
            controller.notePreparationDuration(500, 0, decision.targetUs + late);
            controller.noteSchedulerDelays(0, 0, true);
            controller.noteSubmission(true, false, decision.targetUs + late);
        }
        return controller.intervalStats();
    };
    const auto shortLive = run(10), longLive = run(45);
    expect(shortLive.qualityPercent() == 100.0 && longLive.qualityPercent() < 100.0 &&
               shortLive.evaluatedUs <= 10100000 && longLive.evaluatedUs > 18000000,
           "production must expire quality history at custom seconds rather than a preset tuple");
}

void testTwoMinuteScoreCoverage()
{
    Vrr13::IntervalBuffer continuous, interrupted;
    const auto observe = [](Vrr13::IntervalBuffer& buffer, uint64_t number,
                            uint64_t at, uint64_t lateness) {
        buffer.observe({number, at, at + lateness, at, at + lateness,
                        1000, true, false},
                       1000, 4000, 8000000, 250, true, 975000,
                       500, 120000000, 500000, 32);
    };
    // Both sessions run for 130 seconds. A dropped frame every 1.5 seconds
    // repeatedly restarts one-second control qualification. Measurement must
    // keep every valid adjacent interval until its actual two-minute expiry.
    for (uint64_t i = 1; i <= 13000; ++i) {
        const uint64_t at = 1000000 + i * 10000;
        observe(continuous, i, at, 0);
        observe(interrupted, i + i / 150, at, 0);
    }
    expect(continuous.stats().scoreWindowUs == 120000000 &&
               continuous.stats().evaluatedUs >= 119900000 &&
               continuous.stats().evaluatedUs <= 120000000,
           "continuous two-minute scoring must retain the full rolling denominator");
    expect(interrupted.stats().evaluatedUs > 119000000 &&
               interrupted.stats().evaluatedUs < 120000000 &&
               interrupted.stats().qualityPercent() == 100.0,
           "control qualification must not discard known timing or reproduce historical undercoverage");
    const auto coverage = continuous.stats().evaluatedUs;
    continuous.breakSequence();
    expect(continuous.stats().evaluatedUs == coverage,
           "a sequence break must not reset the long score denominator");
    // Requalify, then inject one second of severe alternating late/catch-up
    // intervals. A full history cannot lose several percentage points in that
    // second; a short observed history can move much faster.
    for (uint64_t i = 13001; i <= 13200; ++i)
        observe(continuous, i, 1000000 + i * 10000,
                i > 13100 && i % 2 ? 9000 : 0);
    Vrr13::IntervalBuffer starting;
    for (uint64_t i = 1; i <= 200; ++i)
        observe(starting, i, 1000000 + i * 10000, i > 100 && i % 2 ? 9000 : 0);
    expect(continuous.stats().qualityPercent() < 100.0 &&
               continuous.stats().qualityPercent() >= 99.0 &&
               starting.stats().qualityPercent() < 75.0,
           "the same one-second disturbance must respect actual scored coverage, not the history label");
}

void testIntervalBufferReleaseAcrossShortGaps()
{
    const auto run = [](bool longSilence,
                        bool invalidSample = false) {
        Vrr13::IntervalBuffer buffer;
        uint64_t frameNumber = 0;
        uint64_t atUs = 1000000;
        for (int span = 0; span < 6; ++span) {
            if (span != 0) {
                ++frameNumber;
                if (longSilence && span == 3 && invalidSample) {
                    buffer.observe({}, 1000, 4000, 6000000, 1000, true,
                                   990000, 500, 30000000, 500000, 32);
                }
                atUs += longSilence && span == 3 ? 10000000 : 10000;
            }
            for (int i = 0; i < 250; ++i) {
                ++frameNumber;
                atUs += 10000;
                buffer.observe({frameNumber, atUs, atUs, atUs, atUs, 3000,
                                true, true},
                               1000, 4000, 6000000, 1000, true, 990000,
                               500, 30000000, 500000, 32);
            }
        }
        return buffer.demand(3000);
    };
    expect(run(false) < 2500,
           "new release revision must recover after qualified clean spans separated by short gaps");
    expect(run(true) == 3000,
           "a long silence must not count as observed clean recovery");
    expect(run(true, true) == 3000,
           "an invalid observation must not hide a long unobserved gap");
}

void testAlternatingSlowCadenceLeavesFastRate()
{
    auto session = config(120, 120);
    session.smoothFrameTiming = true;
    VrrTimingController controller(session, true, vrrTimingParametersForSession(session));
    uint32_t ticks = 0;
    for (int i = 0; i < 140; ++i) {
        ticks += 750;
        const auto decoded = decodedTimeForRtp(1000000, ticks);
        controller.schedule(frame(i, ticks, true, decoded), decoded);
    }
    expect(controller.sourcePeriodUs() >= 8200 && controller.sourcePeriodUs() <= 8500,
           "fixture must establish the stable 120 FPS source period");
    for (int i = 140; i < 300; ++i) {
        ticks += i % 2 ? 3000 : 1500;
        const auto decoded = decodedTimeForRtp(1000000, ticks);
        controller.schedule(frame(i, ticks, true, decoded), decoded);
    }
    expect(controller.sourcePeriodUs() >= 24000 && controller.sourcePeriodUs() <= 26000,
           "alternating 60/30 FPS intervals must fit their 40 FPS average");

    VrrTimingController isolated(session, true, vrrTimingParametersForSession(session));
    ticks = 0;
    for (int i = 0; i < 140; ++i) {
        ticks += 750;
        const auto decoded = decodedTimeForRtp(1000000, ticks);
        isolated.schedule(frame(i, ticks, true, decoded), decoded);
    }
    ticks += 3000;
    isolated.schedule(frame(140, ticks, true, decodedTimeForRtp(1000000, ticks)),
                      decodedTimeForRtp(1000000, ticks));
    ticks += 750;
    isolated.schedule(frame(141, ticks, true, decodedTimeForRtp(1000000, ticks)),
                      decodedTimeForRtp(1000000, ticks));
    expect(isolated.sourcePeriodUs() >= 8200 && isolated.sourcePeriodUs() <= 8500,
           "one long source interval followed by normal cadence must remain provisional");
}

void testInitialPreparationIsNotTypicalRender()
{
    const auto run = [](uint64_t excluded) {
        auto session = config(120, 120);
        auto parameters = vrrTimingParametersForSession(session);
        parameters.preparationInitialSampleExcluded = excluded;
        VrrTimingController controller(session, true, parameters);
        uint32_t ticks = 0;
        for (int i = 0; i < 1; ++i) {
            ticks += 750;
            const auto decoded = decodedTimeForRtp(1000000, ticks);
            const auto decision = controller.schedule(frame(i, ticks, true, decoded), decoded);
            controller.notePreparationDuration(400000);
            controller.noteSubmission(true, false, decision.targetUs);
        }
        return controller.typicalRenderUs();
    };
    expect(run(0) == 100000,
           "recorded policy retains renderer setup as a clamped render sample");
    expect(run(1) <= 10000,
           "renderer setup in the first preparation must not become typical render cost");
    auto session = config(120, 120);
    auto parameters = vrrTimingParametersForSession(session);
    expect(parameters.preparationInitialSampleExcluded == 2,
           "live policy enables the preparation warm-up guard");
    VrrTimingController controller(session, true, parameters);
    for (int i = 0; i < 10; ++i) {
        const uint32_t ticks = uint32_t((i + 1) * 750);
        const auto decoded = decodedTimeForRtp(1000000, ticks);
        const auto decision = controller.schedule(frame(i + 1, ticks, true, decoded), decoded);
        controller.notePreparationDuration(27000);
        controller.noteSubmission(true, false, decision.targetUs);
        expect(controller.typicalRenderUs() <= 8334,
               "warm-up preparation cannot exceed the source/display period");
    }
}

// Exercise the production admission-age drop rule with a continuous 116 FPS
// source and one interval of ordinary worker queue residence. The recorded
// policy starves it through its own render-start wait, despite fresh admission.
void testPreparationPredictionCannotStarveAdmission()
{
    for (const uint64_t poisonUs : {29081u, 42248u, 45002u}) {
        for (const bool corrected : {false, true}) {
            auto session = config(116, 120);
            session.latencyMode = 1;
            auto parameters = vrrTimingParametersForSession(session);
            parameters.preparationPredictionLead = corrected;
            parameters.preparationInitialSampleExcluded = corrected ? 2 : 1;
            VrrTimingController controller(session, true, parameters);
            const uint64_t originUs = 1000000;
            uint64_t nowUs = originUs, lastPresentUs = 0, poisonedAtUs = 0;
            uint64_t recoveryUs = 0, maximumGapUs = 0, maximumLatencyUs = 0;
            unsigned preparations = 0, consecutiveStale = 0, longestStale = 0;
            std::vector<uint64_t> presentations;
            while (nowUs < originUs + 12000000) {
                const uint64_t newest = (nowUs - originUs) * 116 / 1000000;
                const uint64_t ordinal = newest > 0 ? newest - 1 : 0;
                const uint64_t arrivalUs = originUs + (ordinal * 1000000 + 115) / 116;
                auto input = frame(int(ordinal + 1), uint32_t(ordinal * 90000 / 116), true, arrivalUs);
                const auto decision = controller.schedule(input, nowUs);
                const auto protectionUs = decision.playoutDelayUs +
                    std::min(decision.trajectory.carryUs, decision.sourcePeriodUs);
                // The selected image was fresh before this deliberate wait.
                expect(!VrrFrameDropPolicy::beforeRender(decision, controller.displayPeriodUs(),
                           nowUs - arrivalUs, true, protectionUs, true),
                       "the continuous-source fixture must admit a fresh frame before waiting");
                nowUs = std::max(nowUs, decision.renderStartUs);
                const bool successor = nowUs >= originUs + ((ordinal + 1) * 1000000 + 115) / 116;
                if (successor && VrrFrameDropPolicy::afterRenderWait(
                        decision, arrivalUs, nowUs, true, 0, protectionUs)) {
                    longestStale = std::max(longestStale, ++consecutiveStale);
                    controller.noteSubmission(false, false, 0);
                }
                else {
                    consecutiveStale = 0;
                    ++preparations;
                    const uint64_t costUs = preparations == 1 ? 400000 :
                        preparations == 2 ? 400 : preparations == 3 ? poisonUs : 400;
                    nowUs += costUs;
                    controller.notePreparationDuration(costUs, 0, nowUs);
                    nowUs = std::max(nowUs, decision.targetUs);
                    const bool cancelled = preparations == 2;
                    controller.noteSubmission(true, cancelled, nowUs);
                    if (!cancelled) {
                        if (lastPresentUs) maximumGapUs = std::max(maximumGapUs, nowUs - lastPresentUs);
                        lastPresentUs = nowUs;
                        presentations.push_back(nowUs);
                        if (preparations == 3) poisonedAtUs = nowUs;
                        if (preparations == 4) recoveryUs = nowUs - poisonedAtUs;
                        if (preparations > 3) maximumLatencyUs = std::max(maximumLatencyUs, nowUs - arrivalUs);
                    }
                }
                // Advance to a new actual source arrival; never invent frames
                // during a source gap or repeatedly schedule the same identity.
                nowUs = std::max(nowUs, originUs + ((ordinal + 2) * 1000000 + 115) / 116);
            }
            if (!corrected) {
                std::printf("historical poison=%llu preparations=%u stale=%u typical=%llu recovery=%llu\n", (unsigned long long)poisonUs, preparations, longestStale, (unsigned long long)controller.typicalRenderUs(), (unsigned long long)recoveryUs);
                expect(longestStale >= 2,
                       "historical prediction must repeatedly discard fresh frames after its own wait");
                continue;
            }
            expect(recoveryUs > 0 && recoveryUs <= 250000 && maximumGapUs <= 250000,
                   "cold-render prediction must recover presentation within 250 ms without a rate change");
            expect(longestStale == 0 && controller.typicalRenderUs() <= 1000,
                   "fresh frames must keep preparing until fast samples replace the poisoned estimate");
            expect(maximumLatencyUs <= 100000,
                   "progress recovery must retain an explicit 100 ms startup latency bound");
            for (const uint64_t windowUs : {1000000u, 2000000u, 5000000u, 10000000u}) {
                for (uint64_t endUs = originUs + windowUs; endUs <= originUs + 12000000; endUs += 10000) {
                    const auto it = std::upper_bound(presentations.begin(), presentations.end(), endUs - windowUs);
                    expect(it != presentations.end() && *it <= endUs,
                           "every complete 1/2/5/10 second diagnostic window must contain presentation progress");
                }
            }
            std::printf("preparation recovery: poison=%llu gap=%llu latency=%llu stale_cluster=%u\n",
                        (unsigned long long)poisonUs, (unsigned long long)maximumGapUs,
                        (unsigned long long)maximumLatencyUs, longestStale);
        }
    }
}

void testStartDelaySeedReplacesGenericStart()
{
    auto session = config(120, 120);
    const auto parameters = vrrTimingParametersForSession(session);
    const auto startOf = [&](uint64_t seedUs) {
        VrrTimingController controller(session, true, parameters);
        controller.seedPlayoutDelayStart(seedUs);
        const auto decoded = decodedTimeForRtp(1000000, 750);
        return controller.schedule(frame(0, 750, true, decoded), decoded).playoutDelayUs;
    };
    const auto generic = startOf(0);
    expect(startOf(3000) == 3000 && generic != 3000,
           "a cached settled delay must replace the generic starting guess");
    expect(startOf(1) == parameters.playoutDelayMinimumUs,
           "a cached delay must stay within the policy minimum");
}

void testIntervalBufferRestoreHoldsRestoredTarget()
{
    Vrr13::IntervalBuffer buffer;
    uint64_t atUs = 1000000;
    uint64_t frameNumber = 0;
    const auto clean = [&](int count) {
        for (int i = 0; i < count; ++i) {
            ++frameNumber;
            atUs += 10000;
            buffer.observe({frameNumber, atUs, atUs, atUs, atUs, 3000, true, true},
                           1000, 24000, 6000000, 1000, true, 990000,
                           500, 30000000, 500000, 32);
        }
    };
    clean(10);
    buffer.restoreTarget(12000, atUs);
    expect(buffer.demand(0) == 12000, "an epoch restore replaces the standing target");
    clean(500);
    expect(buffer.demand(0) == 12000, "a restored target is protected for one hold");
    clean(700);
    expect(buffer.demand(0) < 12000, "a restored target releases after its hold");
}

void testIntervalBufferAboveTargetDipDoesNotEraseRecovery()
{
    Vrr13::IntervalBuffer buffer;
    uint64_t intendedUs = 1000000;
    uint64_t submittedUs = intendedUs;
    uint64_t frameNumber = 0;
    const auto observe = [&](uint64_t actualIntervalUs, bool late = false) {
        intendedUs += 10000;
        submittedUs += actualIntervalUs;
        ++frameNumber;
        buffer.observe({frameNumber, intendedUs, submittedUs, intendedUs,
                        intendedUs + (late ? 1000 : 0), 3000, true, true},
                       1000, 4000, 6000000, 1000, true, 990000,
                       500, 30000000, 500000, 32);
    };
    for (int i = 0; i < 600; ++i) observe(10000);
    expect(buffer.demand(3000) == 3000,
           "clean evidence must remain inside its six-second hold");
    for (int i = 0; i < 40; ++i) observe(i % 2 ? 12000 : 8000, true);
    expect(buffer.stats().qualityPercent() >= 99.0,
           "short timing dip fixture must remain above the long-window quality target");
    for (int i = 0; i < 180; ++i) observe(10000);
    expect(buffer.demand(3000) < 3000,
           "a short above-target timing dip must pause rather than restart earned recovery");
}

void testIntervalBufferThresholdControlsShrinkage()
{
    for (const uint64_t policy : {uint64_t(1), uint64_t(2), uint64_t(3)}) {
        Vrr13::IntervalBuffer buffer;
        uint64_t intendedUs = 1000000, submittedUs = intendedUs, frame = 0;
        const auto observe = [&](uint64_t actualIntervalUs) {
            intendedUs += 10000;
            submittedUs += actualIntervalUs;
            buffer.observe({++frame, intendedUs, submittedUs, intendedUs,
                            intendedUs + 1000, 12000, true, true},
                           1000, 16000, 8000000, 250, true, 995000,
                           500, 120000000, 500000, 32);
        };
        for (int i = 0; i < 3000; ++i) observe(10000);
        bool sawFalling = false, sawRising = false, sawPressure = false;
        for (int i = 0; i < 180; ++i) {
            const auto before = buffer.stats();
            const auto demand = buffer.demand(12000);
            // A short burst raises the one-second error beyond its allowance,
            // while the two-minute score stays above Balanced's 99.50% target.
            observe(i < 40 ? (i % 2 ? 12000 : 8000) : 10000);
            const auto after = buffer.stats();
            expect(after.qualityPercent() >= 99.5,
                   "above-target regression fixture must meet the selected target");
            sawFalling |= after.qualityPercent() < before.qualityPercent();
            sawRising |= after.qualityPercent() > before.qualityPercent();
            sawPressure |= after.averageErrorUs > 550;
            if (policy >= 2) {
                expect(after.update.holdRemainingUs == 0,
                       "above-target score changes must not restart the shrink timer");
                expect(buffer.demand(12000) < demand,
                       "above-target rising and falling scores must not pause shrinkage");
            }
        }
        expect(sawFalling && sawRising && sawPressure,
               "fixture must exercise both score directions and current pressure");
        // Sustained attributable pressure eventually fails the long target.
        bool sawBelowTarget = false;
        for (int i = 0; i < 500; ++i) {
            const auto demand = buffer.demand(12000);
            observe(i % 2 ? 12000 : 8000);
            const auto after = buffer.stats();
            if (after.qualityPercent() < 99.5) {
                sawBelowTarget = true;
                expect(after.update.holdRemainingUs == 8000000,
                       "below-target attributable pressure must restart the hold");
                expect(buffer.demand(12000) >= demand,
                       "below-target pressure must prevent shrinkage");
            }
        }
        expect(sawBelowTarget, "fixture must cross below the 99.50% target");
    }
}

void testIntervalBufferAboveTargetCapacityDipPreservesRecovery()
{
    for (const uint64_t policy : {uint64_t(3)}) {
        Vrr13::IntervalBuffer buffer;
        uint64_t at = 1000000, frame = 0;
        for (int i = 0; i < 2000; ++i) {
            at += 10000;
            const bool overloaded = i % 300 == 299;
            const auto previousHold = buffer.stats().update.holdRemainingUs;
            buffer.observe({++frame, at, at, at, at, 5000, true, !overloaded,
                            overloaded ? uint64_t(1200000) : uint64_t(0), 0},
                           1000, 16000, 8000000, 250, true, 995000,
                           500, 120000000, 500000, 32);
            expect(buffer.stats().qualityPercent() == 100.0,
                   "capacity dips alone must not change the smoothness score");
            if (policy == 3 && overloaded && i > 850) {
                expect(buffer.stats().update.holdRemainingUs <= previousHold,
                       "above-target capacity dips must not erase earned recovery");
            }
        }
        expect(buffer.demand(5000) < 5000,
               "above-target intermittent overload must allow recovery");
    }
}

void testIntervalQualityUsesPresetHistory()
{
    Vrr13::IntervalBuffer oneMinute;
    Vrr13::IntervalBuffer fiveMinutes;
    for (uint64_t i = 0; i < 7000; ++i) {
        const uint64_t intended = 1000000 + i * 10000;
        const uint64_t offset = i == 1 ? 1000 : 0;
        const auto sample = Vrr13::IntervalBuffer::Sample{
            i, intended, intended + offset, intended,
            intended + offset, 1000, true, true};
        oneMinute.observe(sample, 1000, 24000, 10000000, 50,
                          true, 999900, 500, 60000000);
        fiveMinutes.observe(sample, 1000, 24000, 10000000, 50,
                            true, 999900, 500, 300000000);
    }
    expect(oneMinute.stats().averageValid && fiveMinutes.stats().averageValid &&
               fiveMinutes.stats().evaluatedUs > oneMinute.stats().evaluatedUs,
           "the active quality score must retain the selected preset history duration");
}

void testIntervalBufferSeparatesDeliveryFromSerialService()
{
    const auto run = [](uint64_t serialServiceUs) {
        Vrr13::IntervalBuffer buffer;
        uint64_t applied = 1000;
        unsigned growths = 0;
        bool sawNotAbsorbable = false;
        constexpr uint64_t periodUs = 8333;
        for (uint64_t i = 1; i <= 600; ++i) {
            const uint64_t intended = 1000000 + i * periodUs;
            const uint64_t late = i > 80 && i % 2 ? 3000 : 0;
            buffer.observe({i, intended, intended + late, intended,
                            intended + late, applied, true, true,
                            serialServiceUs, 0},
                           1000, 6000, 1000000, 500, true, 990000,
                           500, 60000000, 500000, 32);
            const auto update = buffer.stats().update;
            growths += update.action ==
                Vrr13::IntervalBuffer::Action::Grow ? 1 : 0;
            sawNotAbsorbable |= update.action ==
                Vrr13::IntervalBuffer::Action::NotAbsorbable;
            applied = buffer.demand(applied);
        }
        return std::array<uint64_t, 3>{applied, growths,
                                       sawNotAbsorbable ? 1ULL : 0ULL};
    };

    const auto deliveryJitter = run(1000);
    expect(deliveryJitter[0] > 1000 && deliveryJitter[1] > 1,
           "absorbable delivery jitter must acquire bounded interval protection");

    const auto saturatedService = run(9500);
    expect(saturatedService[0] == 1000 && saturatedService[1] == 0 &&
               saturatedService[2] == 1,
           "service longer than the smoothed 120 Hz slot must not authorize buffering");

    const auto rawLongIntervalTrap = run(9000);
    expect(rawLongIntervalTrap[0] == 1000 && rawLongIntervalTrap[1] == 0,
           "serial work must be compared with intended slots, not a longer raw RTP interval");
}

void testProductionCalibrationSurvivesFpsChanges()
{
    for (int mode : {0, 1, 2}) for (bool smoothing : {false, true}) {
        auto session = config(120, 120);
        session.latencyMode = mode;
        session.smoothFrameTiming = smoothing;
        const auto policy = vrrTimingParametersForSession(session);
        VrrTimingController controller(session, true, policy);
        uint64_t sourceTicks = 0, submitted = 0, initialBuffer = 0, previousBuffer = 0;
        int frameNumber = 0;
        bool calibrated = false;
        for (int rate : {120, 19, 120, 30, 99, 116, 60, 120}) {
            for (int i = 0; i < rate * 2; ++i, ++frameNumber) {
                sourceTicks += uint64_t((i + 1) * 90000 / rate - i * 90000 / rate);
                const auto arrival = decodedTimeForRtp(1000000, uint32_t(sourceTicks));
                const auto now = std::max(arrival, submitted);
                const auto decision = controller.schedule(frame(frameNumber, uint32_t(sourceTicks), true, arrival), now);
                if (!initialBuffer) initialBuffer = decision.playoutDelayUs;
                const auto ready = std::max(now, decision.renderStartUs) + 1000;
                submitted = std::max(ready, decision.targetUs);
                controller.notePreparationDuration(1000, 0, ready);
                controller.noteSchedulerDelays(0, 0, true);
                controller.noteSubmission(true, false, submitted);
                const auto stats = controller.intervalStats();
                expect(!calibrated || stats.initialCalibrationComplete,
                       "FPS/menu transitions must not restart initial calibration");
                calibrated |= stats.initialCalibrationComplete;
                expect(decision.playoutDelayUs <= initialBuffer &&
                           decision.playoutDelayUs <= decision.playoutDelayMaximumUs,
                       "clean FPS/menu transitions must not inflate startup padding or the effective ceiling");
                expect(!previousBuffer || decision.playoutDelayUs <= previousBuffer + 125,
                       "calibration and rate transitions must retain the normal application slew");
                previousBuffer = decision.playoutDelayUs;
            }
        }
        expect(calibrated, "the production rate-transition fixture must finish initial calibration");
    }
}

void testInitialIntervalCalibration()
{
    using Buffer = Vrr13::IntervalBuffer;
    for (int rate : {20, 30, 60, 116, 240}) {
        Buffer faster, historical;
        uint64_t firstQualified = 0;
        for (uint64_t i = 1; i <= 2 * uint64_t(rate); ++i) {
            const uint64_t at = 1000000 + i * 1000000 / rate;
            const Buffer::Sample sample{i, at, at + 1000, at + 1000, at + 1000, 4000, true, true};
            faster.observe(sample, 1000, 16000, 6000000, 125, true, 990000, 500, 60000000, 500000, 32);
            historical.observe(sample, 1000, 16000, 6000000, 125, true, 990000, 500, 60000000);
            if (faster.stats().averageValid && !firstQualified) {
                firstQualified = at;
                expect(faster.stats().calibrationCoverageUs >= 500000 &&
                           faster.stats().calibrationSamples >= 32,
                       "initial calibration needs both elapsed evidence and enough consecutive intervals");
                if (rate >= 60) expect(!historical.stats().averageValid,
                    "high-rate initial calibration must qualify before the historical one-second window");
            }
            expect(faster.demand(4000) == 4000,
                   "faster calibration must not inflate a clean cold-start reserve");
        }
        expect(firstQualified != 0, "low-FPS calibration must complete even with fewer than 32 intervals per second");
        faster.breakSequence();
        for (uint64_t i = 1; i <= 2 * uint64_t(rate); ++i) {
            const uint64_t at = 5000000 + i * 1000000 / rate;
            faster.observe({i, at, at + 1000, at + 1000, at + 1000, 4000, true, true},
                1000, 16000, 6000000, 125, true, 990000, 500, 60000000, 500000, 32);
            if (faster.stats().calibrationCoverageUs < 1000000)
                expect(!faster.stats().averageValid,
                       "phase/FPS breaks must not rearm the shorter initial calibration");
            expect(faster.stats().initialCalibrationComplete,
                   "a timing break must preserve completed initial calibration");
        }
        faster.reset();
        expect(!faster.stats().initialCalibrationComplete, "a new session must collect its own evidence");
    }

    Buffer variable;
    uint64_t intended = 1000000;
    for (uint64_t i = 1; i < 3000; ++i) {
        // Deliberate source variation, including menu-like slow frames, is
        // not client-added interval error or evidence for more buffering.
        intended += i % 37 == 0 ? 52632 : i % 3 == 0 ? 16667 : 8621;
        variable.observe({i, intended, intended + 1000, intended + 1000, intended + 1000,
            4000, true, true}, 1000, 16000, 6000000, 125, true, 990000, 500, 60000000, 500000, 32);
        expect(variable.demand(4000) <= 4000 && variable.stats().lastGrowthAtUs == 0,
               "changing FPS alone must not grow reserve during or after calibration");
    }

    Buffer pressure;
    uint64_t applied = 1000, lastGrowth = 0;
    unsigned growths = 0;
    for (uint64_t i = 1; i <= 500; ++i) {
        const uint64_t at = 1000000 + i * 10000;
        const uint64_t late = i % 2 ? 3000 : 0;
        pressure.observe({i, at, at + late, at, at + late, applied, true, true},
            1000, 16000, 6000000, 125, true, 990000, 500, 60000000, 500000, 32);
        const auto update = pressure.stats().update;
        if (update.requestedUs > applied) {
            expect(update.requestedUs - applied <= 250 &&
                       (!lastGrowth || update.atUs - lastGrowth >= 250000),
                   "initial calibration must not increase the steady-state attack rate");
            lastGrowth = update.atUs;
            ++growths;
        }
        applied = pressure.demand(applied);
    }
    expect(growths > 2, "the bounded-attack test must actually exercise growth");
}

void testBufferDecisionDiagnostics()
{
    Vrr13::IntervalBuffer buffer;
    bool sawCatchupGrowth = false, sawClippedGrowth = false, sawHistoryHold = false;
    uint64_t applied = 1000;
    for (uint64_t i = 1; i <= 700; ++i) {
        const uint64_t intended = 1000000 + i * 10000;
        const uint64_t late = i < 500 && i % 2 ? 3000 : 0;
        buffer.observe({i, intended, intended + late, intended, intended + late,
                        applied, true, true}, 1000, 1250, 6000000, 125, true, 990000);
        const auto stats = buffer.stats();
        const auto& update = stats.update;
        applied = buffer.demand(applied);
        expect(update.frame == i && update.requestedUs == applied && applied <= 1250,
               "buffer diagnostics must describe this outcome without retaining rejected demand");
        if (update.requestedUs > update.beforeUs && late == 0) {
            sawCatchupGrowth = true;
            expect(update.attributedFrame == i - 1 && update.latenessUs == 3000,
                   "catch-up growth must identify the preceding late frame");
        }
        if (update.clippedIncreaseUs) {
            sawClippedGrowth = true;
            expect(update.action == Vrr13::IntervalBuffer::Action::Capped &&
                   update.requestedUs == 1250 && stats.lastClippedAtUs == update.atUs,
                   "a cap must expose the rejected step even when applied demand cannot grow");
        }
        sawHistoryHold |= update.action == Vrr13::IntervalBuffer::Action::HistoryHold;
    }
    expect(sawCatchupGrowth && sawClippedGrowth && sawHistoryHold,
           "diagnostics must distinguish growth, saturation and old-score retention");
    buffer.observe({701, 8010000, 8010000, 8010000, 8010000, applied, true, true},
                   1000, 1000, 6000000, 125, true, 990000);
    expect(buffer.stats().update.beforeUs == 1250 && buffer.stats().update.requestedUs == 1000 &&
               buffer.stats().update.action == Vrr13::IntervalBuffer::Action::LimitChange,
           "a changing capacity or preset limit must not masquerade as ordinary release");
    buffer.breakSequence();
    expect(buffer.stats().update.frame == 0 && buffer.stats().lastGrowthUs == 250,
           "a broken sequence must invalidate the current cause but retain the last growth event");

    for (bool absorbable : {false, true}) {
        Vrr13::IntervalBuffer onTime;
        for (uint64_t i = 1; i <= 200; ++i) {
            const auto intended = 1000000 + i * 10000;
            onTime.observe({i, intended, intended + (i % 2 ? 3000 : 0),
                intended, intended, 1000, true, absorbable},
                1000, 4000, 6000000, 125, true, 990000);
        }
        expect(onTime.demand(1000) == 1000 && onTime.stats().update.action ==
            (absorbable ? Vrr13::IntervalBuffer::Action::NoFreshMiss :
                          Vrr13::IntervalBuffer::Action::NotAbsorbable),
            "native blocking or non-absorbable work must have an explicit no-growth reason");
    }
}

void testProductionGradualBacklogRecovery()
{
    auto session = config(100, 120);
    session.latencyMode = 1;
    auto policy = vrrTimingParametersForSession(session);
    auto historical = policy;
    historical.playoutCatchupPerMille = 0;
    historical.playoutLateRecovery = 0;
    VrrTimingController current(session, true, policy), old(session, true, historical);
    uint64_t submitted = 0, oldSubmitted = 0;
    bool differed = false;
    for (int i = 1; i <= 700; ++i) {
        const uint64_t arrival = 1000000 + i * 10000;
        const auto execute = [&](VrrTimingController& controller, uint64_t& last) {
            uint64_t now = std::max(arrival, last);
            if (i == 200) now += 15000;
            const auto decision = controller.schedule(frame(i, i * 900, true, arrival), now);
            expect(!VrrFrameDropPolicy::beforeRender(decision, controller.displayPeriodUs(),
                       now - arrival, controller.latencyFixActive()),
                   "intentional catch-up must not be misclassified as display-floor backlog and dropped");
            const auto ready = std::max(now, decision.renderStartUs) + 500;
            last = std::max(ready, decision.targetUs);
            controller.notePreparationDuration(500, 0, ready);
            controller.noteSchedulerDelays(0, 0, true);
            controller.noteSubmission(true, false, last);
            expect(last <= arrival + 20000,
                   "gradual catch-up must stay within the two-period stale horizon");
        };
        execute(current, submitted);
        execute(old, oldSubmitted);
        if (i > 200 && submitted != oldSubmitted) differed = true;
        if (i > 600) expect(submitted <= oldSubmitted + 1000,
                            "recovering a transient backlog must not leave standing latency");
    }
    expect(differed, "the production worker schedule must exercise gradual recovery after a stall");
}

void testLateFrameRecoveryWithoutQueueBacklog()
{
    for (int rate : {40, 60, 100, 116}) for (int mode : {0, 1, 2})
    for (bool smooth : {false, true}) for (bool canLatch : {false, true}) {
        auto session = config(rate, 120);
        session.latencyMode = mode;
        session.smoothFrameTiming = smooth;
        auto policy = vrrTimingParametersForSession(session);
        expect(policy.playoutLateRecovery == 1 && policy.playoutCatchupPerMille == 20,
               "late recovery must work independently of Reduce judder and timing preset");
        // Isolate recovery from adaptive buffer growth. Both controllers get
        // the same fixed 1 ms buffer and identical source/readiness samples.
        policy.playoutDelayAdaptive = 0;
        policy.sourcePlayoutDelayUs = 1000;
        auto historical = policy;
        historical.playoutLateRecovery = 0;
        historical.playoutCatchupPerMille = smooth ? 20 : 0;
        VrrTimingController current(session, canLatch, policy);
        VrrTimingController old(session, canLatch, historical);
        uint64_t last = 0, oldLast = 0;
        bool rescued = false;
        for (int i = 1; i <= 360; ++i) {
            const auto rtp = uint32_t(std::llround(i * 90000.0 / rate));
            const uint64_t arrival = decodedTimeForRtp(1000000, rtp);
            const auto a = current.schedule(frame(i, rtp, true, arrival), std::max(arrival, last));
            const auto b = old.schedule(frame(i, rtp, true, arrival), std::max(arrival, oldLast));
            expect(a.originalTargetUs == b.originalTargetUs,
                   "late rescue must preserve source clock and intended targets");
            expect(a.targetUs >= current.earliestSubmissionUs(),
                   "recovery must retain the active native presentation safety floor");
            if (i == 201) {
                rescued = a.targetUs > b.targetUs;
                expect(a.targetUs - last >= a.sourcePeriodUs * 97 / 100,
                       "a 3 ms late presentation must not be followed by a sharp catch-up interval");
            }
            expect(a.targetUs <= std::max(a.originalTargetUs,
                       std::max(arrival, last) + a.renderLeadUs) + a.sourcePeriodUs,
                   "recovery must not accumulate unbounded target latency");
            last = a.targetUs + (i == 200 ? 3000 : 0);
            oldLast = b.targetUs + (i == 200 ? 3000 : 0);
            current.noteSubmission(true, false, last);
            old.noteSubmission(true, false, oldLast);
            if (i == 360) expect(last <= oldLast + 100,
                "isolated late-frame recovery must return to the original timeline");
        }
        expect(rescued, "a late present without old queued work must activate bounded recovery");
    }
}

void testPresentTiming()
{
    using Timing = Vrr13::PresentTiming;
    const Timing::Limits limits{500, 8333, 10000, 20000};
    Timing timing;
    uint64_t frame = 1, planned = 1000000, submitted = 1000000, presented = 0;
    const auto present = [&](uint64_t plan, uint64_t submitOffset, uint64_t latency, uint64_t uncertainty = 0,
                             const Timing::Limits& with = Timing::Limits{500, 8333, 10000, 20000}) {
        planned += plan;
        submitted = planned + submitOffset;
        presented = submitted + latency;
        timing.observe({frame++, submitted, presented, planned, uncertainty, presented}, with);
    };
    // A constant submit-to-display latency is clean even when large.
    for (int i = 0; i < 100; ++i) present(10000, 0, 20000);
    expect(timing.stats().intervals == 99 && timing.stats().misses == 0,
           "constant post-submission latency must not be a present timing issue");
    // A late presentation after on-time submissions makes both of its
    // intervals uneven on screen.
    present(10000, 0, 24000);
    present(10000, 0, 20000);
    expect(timing.stats().misses == 2 && timing.stats().addedTotalUs == 8000 &&
               timing.stats().hitches == 0 && timing.stats().worstAddedUs == 4000,
           "a late presentation must count the intervals it made uneven");
    // A display that queues uneven submissions and shows them at the planned
    // spacing is not blamed for the latency changes.
    timing.breakSequence();
    for (uint64_t offset : {0, 3000, 5000, 1000, 4000, 0}) present(10000, offset, 25000 - offset);
    expect(timing.stats().misses == 2, "a display that absorbs uneven submissions must not be blamed");
    // Display spacing cannot be shorter than the panel's fastest refresh, so
    // a compressed plan shown at that refresh is not an issue.
    for (int i = 0; i < 4; ++i) {
        planned += 6000; submitted = planned; presented += 8333;
        timing.observe({frame++, submitted, presented, planned, 0, presented}, limits);
    }
    expect(timing.stats().misses == 2, "spacing limited by the panel's maximum refresh must not be blamed");
    // An on-time submission shown one refresh late is an issue, after
    // subtracting timestamp uncertainty.
    timing.breakSequence();
    present(10000, 0, 20000);
    present(10000, 0, 28333);
    expect(timing.stats().misses == 3, "a missed refresh after an on-time submission must be counted");
    // A presentation a whole frame or more late is a hitch; the percentage
    // alone would hide it among many intervals.
    timing.breakSequence();
    present(10000, 0, 20000);
    present(10000, 0, 46000);
    expect(timing.stats().misses == 4 && timing.stats().hitches == 1 &&
               timing.stats().worstAddedUs == 26000,
           "a presentation a frame or more late must be reported as a hitch");
    timing.breakSequence();
    present(10000, 0, 20000);
    present(10000, 0, 28333);
    present(10000, 0, 20000);
    present(10000, 0, 20700, 300);
    expect(timing.stats().misses == 6, "the catch-up after a missed refresh is also uneven on screen");
    present(10000, 0, 20000, 300);
    expect(timing.stats().misses == 6 && timing.stats().hitches == 1,
           "uncertain display time must not create a miss");
    // Below the panel's adaptive-refresh floor the driver repeats frames on its
    // own schedule; those intervals are paused rather than blamed.
    const auto scored = timing.stats().intervals;
    const Timing::Limits belowFloor{500, 8333, 33333, 20000};
    present(33333, 0, 20000, 0, belowFloor);
    present(8600, 0, 28700, 0, belowFloor);
    present(24700, 0, 20000, 0, belowFloor);
    expect(timing.stats().intervals == scored && timing.stats().misses == 6 &&
               timing.stats().lastPausedUs == presented,
           "intervals below the adaptive-refresh floor must pause scoring");
    // The driver keeps its repeat refreshes for a while after the floor is
    // crossed, so unevenness in that settling period is not blamed.
    present(10000, 0, 28333);
    expect(timing.stats().intervals == scored && timing.stats().misses == 6,
           "intervals settling after the adaptive-refresh floor must not be scored");
    for (int i = 0; i < 30; ++i) present(10000, 0, 20000);
    const auto resumed = timing.stats().intervals;
    expect(resumed > scored && timing.stats().misses == 6,
           "scoring must resume once the stream stays above the floor");
    // A long gap in a fast stream pauses that interval and its settling period.
    present(30000, 0, 25000);
    present(10000, 0, 28333);
    expect(timing.stats().intervals == resumed && timing.stats().misses == 6,
           "a gap beyond the floor must not be blamed on the display");
    // Drops and missing feedback start a new sequence; stale or repeated
    // frames are ignored.
    for (int i = 0; i < 30; ++i) present(10000, 0, 20000);
    const auto counted = timing.stats().intervals;
    frame += 1;
    present(10000, 0, 40000);
    timing.observe({frame - 3, submitted - 30000, presented, planned - 30000, 0, presented + 1000}, limits);
    expect(timing.stats().intervals == counted, "frame gaps and out-of-order feedback must not be scored");
    timing.breakSequence();
    present(10000, 0, 90000);
    expect(timing.stats().intervals == counted, "a broken sequence must not bridge into the next frame");
    // The 30 s window ends at the newest scored interval.
    planned = 100000000;
    present(10000, 0, 20000);
    present(10000, 0, 20000);
    expect(timing.stats().intervals == 1 && timing.stats().misses == 0,
           "present timing issues must age out of the rolling window");
    timing.reset();
    expect(timing.stats().intervals == 0 && timing.stats().lastObservedUs == 0 &&
               timing.stats().lastPausedUs == 0,
           "reset must clear present timing history");
}

void testSourceRelativeCadenceDetection()
{
    using Detection = Vrr13::CadenceDetection;
    Detection d;
    uint64_t id = 0, source = 0;
    const auto feed = [&](uint64_t period, int64_t displacement = 0, uint64_t uncertainty = 0) {
        source += period;
        const auto actual = uint64_t(int64_t(1000000 + source) + displacement);
        d.observe({++id, source, actual, actual + 1000, uncertainty, true}, 500);
    };
    // Source variation and a fixed latency are not client-added variation.
    for (auto period : {8333, 16667, 51000, 4167, 10000, 20000}) feed(period);
    expect(d.stats().intervals == 5 && d.stats().deviations == 0 &&
           d.stats().worstJerkUs == 0 && d.stats().normalizedErrorPercent() == 0,
           "faithful irregular source cadence must remain clean, including a 51 ms source interval");
    d.reset(); id = source = 0;
    feed(10000); feed(10000, 1000); feed(10000);
    expect(d.stats().deviations == 2 && d.stats().stretches == 1 && d.stats().recurring == 0 &&
           d.stats().reversals == 1 && d.stats().worstJerkUs == 2000,
           "one late frame and catch-up must be one isolated event, with its 2 ms residual jerk");
    feed(10000, 1000); feed(10000); feed(10000, 1000); feed(10000);
    expect(d.stats().recurring == 2 && d.stats().reversals == 5,
           "repeated alternating residuals must expose recurrence and direction reversals");
    // The same millisecond error has a different fraction of a source period.
    Detection slow, fast;
    for (auto* detector : {&slow, &fast}) {
        const uint64_t period = detector == &slow ? 20000 : 5000;
        detector->observe({1, 0, 1000000, 1000000, 0, true}, 500);
        detector->observe({2, period, 1000000 + period + 1000,
                          1000000 + period + 1000, 0, true}, 500);
    }
    expect(slow.stats().normalizedErrorPercent() == 5 && fast.stats().normalizedErrorPercent() == 20,
           "severity must normalize against the matching source interval, not requested FPS");
    d.reset(); id = source = 0;
    feed(10000, 0, 300); feed(10000, 700, 300); feed(10000, 0, 300);
    expect(d.stats().uncertain == 2 && d.stats().deviations == 0 && d.stats().worstErrorUs == 100 &&
           d.stats().worstJerkUs == 200 && d.stats().recurring == 0,
           "ambiguous crossings must remain unknown; jerk must include twice the shared timestamp uncertainty");
    d.reset(); id = source = 0;
    feed(10000); feed(10000, 500);
    expect(d.stats().deviations == 0 && d.stats().uncertain == 0,
           "the exact deadband boundary must not be a definite deviation");
    feed(10000, 1001);
    expect(d.stats().deviations == 1, "an error beyond the deadband must remain detectable");
    // Native APIs can report two identities at the same display instant.
    d.reset();
    d.observe({1, 0, 1000000, 1001000, 0, true}, 500);
    d.observe({2, 10000, 1000000, 1002000, 0, true}, 500);
    expect(d.stats().intervals == 1 && d.stats().coalesced == 1 && d.stats().deviations == 1,
           "coincident OS display reports must be exposed rather than silently excluded");
    d.observe({4, 30000, 1040000, 1041000, 0, true}, 500);
    expect(d.stats().intervals == 1, "missing identities must not manufacture an interval");
    d.observe({3, 20000, 1030000, 1042000, 0, true}, 500);
    d.observe({5, 40000, 1050000, 1051000, 0, true}, 500);
    expect(d.stats().intervals == 2 && d.stats().jerkPairs == 0,
           "out-of-order feedback must not replace the latest identity or bridge jerk across a gap");
    d.breakSequence();
    d.observe({6, 50000, 1090000, 1091000, 0, true}, 500);
    expect(d.stats().intervals == 2, "a phase break must retain statistics without joining the old sequence");
    d.observe({7, 60000, 1100000, 1101000, 0, true}, 750);
    expect(d.stats().intervals == 0 && d.stats().toleranceUs == 750,
           "changing the deadband must not mix incompatible counts");
    d.observe({8, 70000, 1110000, 1111000, 0, true}, 750);
    d.observe({9, 80000, 1120000, 4000000, 0, false}, 750);
    expect(d.stats().intervals == 0, "old evidence must expire even when new outcomes are invalid");
    d.observe({1, 0, 1000, 1000, 0, true}, 500);
    d.observe({2, 10000, 11000, 11000, 0, true}, 500);
    expect(d.stats().intervals == 1 && d.stats().deviations == 0,
           "a local-clock restart must reset old evidence and identities");
    d.reset(); id = source = 0;
    feed(10000, 0, UINT64_MAX); feed(10000, 1000, UINT64_MAX); feed(10000, 0, UINT64_MAX);
    expect(d.stats().uncertain == 2 && d.stats().worstJerkUs == 0,
           "large uncertainty must saturate instead of wrapping into false precision");
    for (uint64_t fps : {60, 90, 120, 144, 240}) {
        Detection clean, oscillating;
        for (uint64_t i = 0; i < fps * 2; ++i) {
            const auto sourceAt = i * 1000000 / fps;
            const auto cleanAt = 1000000 + sourceAt;
            const auto disturbedAt = cleanAt + (i % 2 ? 1000 : 0);
            clean.observe({i, sourceAt, cleanAt, cleanAt, 0, true}, 500);
            oscillating.observe({i, sourceAt, disturbedAt, disturbedAt, 0, true}, 500);
        }
        expect(clean.stats().deviations == 0 && oscillating.stats().recurring > 10 &&
               oscillating.stats().worstJerkUs == 2000,
               "clean cadence and recurring oscillation must remain distinguishable across 60-240 FPS");
    }
    // Native scheduled presentation can absorb irregular CPU enqueues.
    Detection enqueued, displayed;
    for (uint64_t i = 0; i < 100; ++i) {
        const auto sourceAt = i * 10000;
        const auto enqueueAt = 1000000 + sourceAt + (i % 2 ? 1000 : 0);
        const auto displayAt = 1020000 + sourceAt;
        enqueued.observe({i, sourceAt, enqueueAt, enqueueAt, 0, true}, 500);
        displayed.observe({i, sourceAt, displayAt, displayAt, 0, true}, 500);
    }
    expect(enqueued.stats().deviations > 90 && displayed.stats().deviations == 0,
           "uneven CPU enqueues must not become display deviations when scheduled presentation absorbs them");
}

// These are revision-6 local-predictor regressions, with its historical
// learned reserve. Revision 7's fixed 2T pad and shared movement contract are
// checked in tst_sourcegapsmoothing; it intentionally has a different budget.

int main()
{
    if (std::getenv("MOONLIGHT_VRR_TEST_RESUME_ONLY")) {
        testPreparationPredictionCannotStarveAdmission();
        return failures ? 1 : 0;
    }
    if (std::getenv("MOONLIGHT_VRR_TEST_TOLERANCE_RECOVERY_ONLY")) {
        testWithinToleranceCannotCreateQueuedRecovery();
        return failures ? 1 : 0;
    }
    testWithinToleranceCannotCreateQueuedRecovery();
    testPresentTiming();
    testSourceRelativeCadenceDetection();
    testNativeScheduledRetimingBounds();
    testLateFrameRecoveryWithoutQueueBacklog();
    testProductionGradualBacklogRecovery();
    {
        expect(VrrCatchUp::floorUs(100000, 10000, 8500, 0, 20) == 109800,
               "gentle catch-up limits interval compression to two percent");
        uint64_t previous = 109800;
        for (uint64_t age = 10000; age <= 20000; age += 100) {
            const auto floor = VrrCatchUp::floorUs(100000, 10000, 8500, age, 20);
            expect(floor <= previous && previous - floor <= 14 && floor >= 108500,
                   "recovery progressively uses headroom without crossing display safety");
            previous = floor;
        }
        expect(previous == 108500, "hard-limit edge uses all safe recovery headroom");
        expect(VrrCatchUp::floorUs(100000, 8333, 8500, 20000, 20) == 0,
               "no recovery floor is added when the display has no headroom");
        expect(VrrCatchUp::floorUs(100000, 10000, 8500, 0, 0) == 0,
               "historical captures retain their recovery behavior");
        uint64_t last = 110000; // A one-period late presentation.
        for (unsigned i = 1; i <= 60; ++i) {
            const uint64_t intended = 100000 + i * 10000;
            const auto next = std::max(intended, VrrCatchUp::floorUs(last, 10000, 8500, 0, 20));
            expect(next - last >= 9800 && next - intended <= 10000,
                   "transient backlog drains without burst presentation or growing latency");
            last = next;
        }
        expect(last == 700000, "bounded catch-up returns to the original source slots");
    }

    testIntervalBufferSeparatesDeliveryFromSerialService();
    testProductionCalibrationSurvivesFpsChanges();
    testInitialIntervalCalibration();
    testBufferDecisionDiagnostics();
    testIntervalQualityBuffer();
    testPerIntervalBufferPreventsRecoverableSpikes();
    testIntervalBufferReleaseAcrossShortGaps();
    testTwoMinuteScoreCoverage();
    testAlternatingSlowCadenceLeavesFastRate();
    testInitialPreparationIsNotTypicalRender();
    testPreparationPredictionCannotStarveAdmission();
    testIntervalBufferRestoreHoldsRestoredTarget();
    testStartDelaySeedReplacesGenericStart();
    testIntervalBufferAboveTargetDipDoesNotEraseRecovery();
    testIntervalBufferThresholdControlsShrinkage();
    testIntervalBufferAboveTargetCapacityDipPreservesRecovery();
    testIntervalQualityUsesPresetHistory();
    testProductionIntervalHistoryUsesCustomSeconds();
    testPresetIntervalTolerances();
    testCustomTimingOptions();
    testProductionIntervalParameters();
    testPresetReadinessTargets();
    testLatencyFixBufferlessLateFrameSafety();
    testLatencyPresetsAcrossSourceAndDisplayRates();
    testLatencyPresetsBoundHitchesThroughCadenceChanges();
    testLostPacketFramesDoNotGrowBuffer();
    testDelayedDisplayEventsAgreeAcrossBackends();
    testProductionPreservesRelativeGameSpacing();
    testHistoricalSourceGapSmoothFrameTiming();
    testProductionWorkerSmoothingOwnsMovement();
    testProducerClockExcludesLateGpuCollection();
    testQueuedCeilingRecoveryCannotRatchet();
    testSmoothQueueReachesItsCeilingNearRefresh();
    testPreparationKeepsLearnedLead();
    testGpuReadinessLeadIsSeparateFromTarget();
    testGpuReadinessPercentileExpiresDuplicateSamples();
    testDeferredGpuObservationDoesNotCreateBufferPressure();
    testRefreshReferencesAreNotDisplayEvents();
    testProcessingEpisodeClassification();
    testTearingPresentClearsLatchedFlip();
    testNativeFlipProtectionPreventsScanoutTears();
    testBalancedReadinessFloorFollowsLoad();
    testProfilesOrderEveryTrade();
    testFirstPresentAfterVrrFloorGapLatches();
    testExplicitAdaptiveOnlyPolicy();
    testProductionAdaptiveProtectionRecoversWithoutDrift();
    testPerFrameLatchIncludesSafetyHeadroom();
    testProductionMatchesVrr14NearRefresh();
    testPersistentImmediateUsesSafetyFloor();
    testSourceRateProtection();
    testRollingPlayoutHistory();
    testRtpWrapResetAndFallback();
    testTimingFormulaeAndReserveCap();
    testOffsetSlewUsesElapsedTime();
    testOffsetSlewIgnoresWorkerBacklogAndPreservesHistoricalClock();
    testSourceMappingIgnoresDecodePollTiming();
    testOffsetRecoveryRejectsTransitionMinimum();
    testOffsetRecoveryKeepsStartupPaddingAndNativePolicy();
    testProductionPreparationUsesAvailableSlack();
    testSmoothnessLearningWindowTracksCadence();
    testLongRunNearRefreshRtpCadence();
    testQuantizedCadenceDoesNotOscillate();
    testNegotiatedRateCeiling();
    testSpacingGuardFeedback();
    testNearRefreshRequestsLatchedPresentation();
    testLatchedPresentationUsesFullHysteresis();
    testOptionalDisplayScaledLatchedPresentationBoundary();
    testCadenceInstabilityUsesLatchedRecovery();
    testHeadroomAwareReadinessReserve();
    testCadenceGapAndRateChange();
    testFutureSourceProjectionReseedsPhase();
    testDecodeTailAdaptation();
    testRateChangeReseedsReadinessBudget();
    testFractionalQuantizedCadenceLearning();
    testCutsceneRecoveryAndHitchIsolation();
    testModerateSlowdownSelfHealsPhase();
    testContinuousCadenceSweep();
    testQuantizedCadenceProjectsSmoothTargets();
    testSkippedLocalFramePreservesCadence();
    testSchedulerDelayFeedback();
    testRenderBaselineDoesNotConsumePacingBudget();
    testRenderTailSharesHalfScanoutBudget();
    testPacingBudgetAtExtremeRefreshRates();
    testTargetWaiterBoundaries();
    testNativeSynchronizedPresentation();
    testNativeScheduledDeadlinesSurviveCompressedCadence();
    testNativeScheduledLogicalAnchorDoesNotAccumulateCpuOverhead();
    testNativeScheduledFailureAndCancellationDoNotConsumeSlots();
    testNativeScheduledSlowerCadenceAndOverloadAdmission();
    testRuntimeParametersChangePolicy();
    return failures == 0 ? 0 : 1;
}
