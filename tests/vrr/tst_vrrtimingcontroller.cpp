// Standalone deterministic coverage for the platform-neutral VRR core.
// This file is intentionally not wired into the application build; tests/vrr
// owns the optional qmake harness.

#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

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

void testSourcePlayoutDelayOffsetsProjectedTargets()
{
    VrrTimingParameters parameters;
    parameters.sourcePlayoutDelayUs = 12000;
    VrrTimingController buffered(config(60, 120), true, parameters);
    VrrTimingController direct(config(60, 120), true,
                               VrrTimingParameters {});

    const VrrTimingDecision bufferedFirst = buffered.schedule(
        frame(1, 0, true, 100000), 100000);
    const VrrTimingDecision directFirst = direct.schedule(
        frame(1, 0, true, 100000), 100000);
    expect(bufferedFirst.targetUs ==
               directFirst.targetUs + parameters.sourcePlayoutDelayUs,
           "source playout delay must offset the projected target instead of the arrival-time clamp");
    expect(bufferedFirst.timingBudgetUs ==
               directFirst.timingBudgetUs + parameters.sourcePlayoutDelayUs,
           "source playout delay must be reported in the timing budget");

    const VrrSessionConfig lowLatency = config(60, 120);
    VrrSessionConfig smoothness = lowLatency;
    smoothness.allowAdditionalQueuedFrame = true;
    const VrrTimingParameters lowLatencyPolicy =
        vrrTimingParametersForSession(lowLatency);
    const VrrTimingParameters smoothnessPolicy =
        vrrTimingParametersForSession(smoothness);
    expect(lowLatencyPolicy.timestampPlayoutEnabled == 1 &&
               lowLatencyPolicy.playoutDelayAdaptive == 1 &&
               lowLatencyPolicy.sourcePlayoutDelayUs == 3000 &&
               lowLatencyPolicy.playoutDelayStartUs == 6000 &&
               lowLatencyPolicy.playoutDelayMinimumUs == 1000 &&
               lowLatencyPolicy.playoutDelayMaximumUs == 8000 &&
               lowLatencyPolicy.playoutDelayPercentilePerMille == 990 &&
               lowLatencyPolicy.pacingLatencyExtraPeriodNumerator == 0 &&
               lowLatencyPolicy.pacingLatencyQueueModeExtra == 0 &&
               lowLatencyPolicy.readinessLowPercentile == 0 &&
               lowLatencyPolicy.readinessLoosePercentile == 80 &&
               lowLatencyPolicy.retainReadinessOnPhaseReset == 0,
           "sessions must resolve the single adaptive p99 timestamp playout policy");
    expect(smoothnessPolicy.playoutDelayAdaptive ==
                   lowLatencyPolicy.playoutDelayAdaptive &&
               smoothnessPolicy.playoutDelayStartUs ==
                   lowLatencyPolicy.playoutDelayStartUs &&
               smoothnessPolicy.playoutDelayMaximumUs ==
                   lowLatencyPolicy.playoutDelayMaximumUs &&
               smoothnessPolicy.playoutDelayPercentilePerMille ==
                   lowLatencyPolicy.playoutDelayPercentilePerMille &&
               smoothnessPolicy.pacingLatencyQueueModeExtra == 0,
           "the retired smoothness flag must not change the session policy");
    VrrSessionConfig legacySmoothness = config(100, 120);
    legacySmoothness.allowAdditionalQueuedFrame = true;
    VrrTimingParameters legacyParameters;
    legacyParameters.timestampPlayoutEnabled = 0;
    VrrTimingController legacy(legacySmoothness, true, legacyParameters);
    VrrTimingController current(legacySmoothness, true, smoothnessPolicy);
    expect(legacy.diagnostics().pacingLatencyBudgetUs ==
               current.diagnostics().pacingLatencyBudgetUs +
                   legacy.sourcePeriodUs(),
           "only captures made under the retired option keep its extra render budget");
}

void testSmoothnessTimestampPlayoutHoldsFixedDelay()
{
    // 60 FPS on 120 Hz so the display-spacing floor never binds and every
    // target is decided by the playout schedule alone.
    VrrSessionConfig session = config(60, 120);
    session.allowAdditionalQueuedFrame = true;
    VrrTimingParameters policy = vrrTimingParametersForSession(session);
    // This test covers the fixed-delay mechanism; the calibrator has its own.
    policy.playoutDelayAdaptive = 0;
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    const uint64_t delayUs = policy.sourcePlayoutDelayUs;

    const auto rtpFor = [](int i) {
        return static_cast<uint32_t>(static_cast<uint64_t>(i) * 90000ULL / 60ULL);
    };
    // Deterministic jitter in [0, 3000) us, all below the 5 ms delay. Every
    // fiftieth frame arrives with zero jitter so the three-second offset
    // window always contains the true floor and the mapping stays fixed.
    const auto jitterFor = [](int i) {
        if (i % 50 == 0) {
            return static_cast<uint64_t>(0);
        }
        return static_cast<uint64_t>((static_cast<uint64_t>(i) * 7919ULL) % 3001ULL);
    };

    VrrTimingDecision decision;
    uint64_t previousTargetUs = 0;
    unsigned int spacingErrors = 0;
    unsigned int reserveReports = 0;
    for (int i = 0; i < 400; ++i) {
        const uint32_t timestamp = rtpFor(i);
        const uint64_t idealUs = decodedTimeForRtp(epochUs, timestamp);
        const uint64_t decodedUs = idealUs + jitterFor(i);
        decision = controller.schedule(
            frame(i + 1, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        expect(i == 0 || (!decision.rebased && !decision.phaseDiscontinuity),
               "sub-delay jitter must never re-anchor the timestamp clock");
        if (decision.readinessBudgetUs != 0 ||
                controller.diagnostics().appliedReadinessReserveUs != 0) {
            ++reserveReports;
        }
        if (i > 0) {
            const uint64_t expectedSpacingUs =
                idealUs - decodedTimeForRtp(epochUs, rtpFor(i - 1));
            const uint64_t spacingUs = decision.targetUs - previousTargetUs;
            if (spacingUs != expectedSpacingUs) {
                if (spacingErrors < 5) {
                    std::fprintf(stderr,
                                 "playout spacing i=%d target=%llu ideal=%llu decoded=%llu spacing=%llu expected=%llu offset=%lld ready=%lld source=%llu lead=%llu flags=%d%d%d\n",
                                 i,
                                 static_cast<unsigned long long>(decision.targetUs),
                                 static_cast<unsigned long long>(idealUs),
                                 static_cast<unsigned long long>(decodedUs),
                                 static_cast<unsigned long long>(spacingUs),
                                 static_cast<unsigned long long>(expectedSpacingUs),
                                 static_cast<long long>(controller.playoutOffsetUs()),
                                 static_cast<long long>(decision.readyOffsetUs),
                                 static_cast<unsigned long long>(decision.sourceTimeUs),
                                 static_cast<unsigned long long>(decision.renderLeadUs),
                                 decision.rebased ? 1 : 0,
                                 decision.phaseDiscontinuity ? 1 : 0,
                                 decision.sourceRateChanged ? 1 : 0);
                }
                ++spacingErrors;
            }
        }
        previousTargetUs = decision.targetUs;
    }
    expect(spacingErrors == 0,
           "timestamp playout must reproduce sender spacing exactly under sub-delay jitter");
    expect(reserveReports == 0,
           "timestamp playout must not apply or report a learned readiness reserve");
    expect(controller.timestampPlayoutActive(),
           "smoothness sessions with RTP timestamps must use timestamp playout");
    expect(controller.playoutOffsetUs() == static_cast<int64_t>(epochUs),
           "the applied offset must be the earliest arrival in the window");
    expect(decision.targetUs ==
               decodedTimeForRtp(epochUs, rtpFor(399)) + delayUs +
                   decision.renderLeadUs,
           "every target must sit exactly one fixed delay after mapped sender time");
    expect(controller.timingBudgetUs() == delayUs,
           "the timing budget must report only the fixed playout delay");

    // A frame later than the delay clamps to now and nothing else moves.
    const uint32_t lateTimestamp = rtpFor(400);
    const uint64_t lateIdealUs = decodedTimeForRtp(epochUs, lateTimestamp);
    const uint64_t lateDecodedUs = lateIdealUs + delayUs + 1500;
    const VrrTimingDecision late = controller.schedule(
        frame(401, lateTimestamp, true, lateDecodedUs), lateDecodedUs);
    controller.noteSubmission(true, false, late.targetUs);
    expect(late.targetUs == lateDecodedUs + late.renderLeadUs,
           "a frame later than the delay must present as soon as it is ready");
    expect(!late.phaseDiscontinuity && !late.rebased &&
               controller.playoutOffsetUs() == static_cast<int64_t>(epochUs),
           "one late frame must not move the sender clock mapping");

    const uint32_t nextTimestamp = rtpFor(401);
    const uint64_t nextIdealUs = decodedTimeForRtp(epochUs, nextTimestamp);
    const VrrTimingDecision next = controller.schedule(
        frame(402, nextTimestamp, true, nextIdealUs + 200), nextIdealUs + 200);
    controller.noteSubmission(true, false, next.targetUs);
    expect(next.targetUs == nextIdealUs + delayUs + next.renderLeadUs,
           "the frame after a late frame must return to its own slot");

    // Slow clock drift is followed at the slew rate, never as a jump.
    const int64_t offsetBeforeDrift = controller.playoutOffsetUs();
    uint64_t maximumSpacingErrorUs = 0;
    previousTargetUs = next.targetUs;
    for (int i = 402; i < 1400; ++i) {
        const uint32_t timestamp = rtpFor(i);
        const uint64_t idealUs = decodedTimeForRtp(epochUs, timestamp);
        // Frames arrive one microsecond later per frame relative to RTP.
        const uint64_t decodedUs = idealUs + static_cast<uint64_t>(i - 401);
        decision = controller.schedule(
            frame(i + 1, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        const uint64_t expectedSpacingUs =
            idealUs - decodedTimeForRtp(epochUs, rtpFor(i - 1));
        const uint64_t spacingUs = decision.targetUs - previousTargetUs;
        const uint64_t errorUs = spacingUs > expectedSpacingUs ?
            spacingUs - expectedSpacingUs : expectedSpacingUs - spacingUs;
        maximumSpacingErrorUs = std::max(maximumSpacingErrorUs, errorUs);
        previousTargetUs = decision.targetUs;
    }
    expect(controller.playoutOffsetUs() > offsetBeforeDrift + 500,
           "the mapping offset must follow sustained drift");
    expect(maximumSpacingErrorUs <= policy.playoutOffsetSlewUs,
           "drift tracking must never move a target by more than the slew per frame");
}

void testAdaptivePlayoutDelayCalibratesPerBand()
{
    VrrSessionConfig session = config(116, 120);
    const VrrTimingParameters policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    const auto rtpFor = [](int i, int rateHz) {
        return static_cast<uint32_t>(
            static_cast<uint64_t>(i) * 90000ULL /
            static_cast<uint64_t>(rateHz));
    };
    // Uniform-ish jitter in [0, 3000) us with a zero every 50th frame so the
    // mapping floor stays fixed: p98 is about 2940 us.
    const auto jitterFor = [](int i) {
        if (i % 50 == 0) {
            return static_cast<uint64_t>(0);
        }
        return static_cast<uint64_t>((static_cast<uint64_t>(i) * 7919ULL) % 3001ULL);
    };

    VrrTimingDecision decision;
    bool startHeld = true;
    uint64_t maximumStepUs = 0;
    uint64_t previousDelayUs = 0;
    for (int i = 0; i < 1500; ++i) {
        const uint32_t timestamp = rtpFor(i, 116);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + jitterFor(i);
        decision = controller.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        if (i < 400 && decision.playoutDelayUs != policy.playoutDelayStartUs) {
            startHeld = false;
        }
        if (i > 0) {
            const uint64_t stepUs = decision.playoutDelayUs > previousDelayUs ?
                decision.playoutDelayUs - previousDelayUs :
                previousDelayUs - decision.playoutDelayUs;
            maximumStepUs = std::max(maximumStepUs, stepUs);
        }
        previousDelayUs = decision.playoutDelayUs;
    }
    expect(startHeld,
           "the delay must hold the start value until the band has enough samples to release");
    expect(controller.playoutBandIndex() == 116 / 20,
           "the band must follow the fitted source rate");
    const uint64_t expectedUs = 2940 + policy.playoutDelayMarginUs;
    expect(decision.playoutDelayUs >= expectedUs - 200 &&
               decision.playoutDelayUs <= expectedUs + 200,
           "the delay must converge to the target lateness percentile plus margin");
    expect(maximumStepUs <= std::max(policy.playoutDelayAttackUs,
                                     policy.playoutDelayReleaseUs),
           "the delay must never move more than one slew step per frame within a band");
    expect(controller.timingBudgetUs() == decision.playoutDelayUs,
           "the timing budget must report the applied adaptive delay");

    // A worse regime: one frame in ten arrives 6 ms late. The delay must
    // attack upward at the attack rate, never in one jump.
    const uint64_t delayBeforeUs = decision.playoutDelayUs;
    uint64_t maximumRiseUs = 0;
    for (int i = 1500; i < 2500; ++i) {
        const uint32_t timestamp = rtpFor(i, 116);
        const uint64_t lateUs = (i % 10 == 0) ? 6000 : 0;
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + jitterFor(i) + lateUs;
        decision = controller.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        if (decision.playoutDelayUs > previousDelayUs) {
            maximumRiseUs = std::max(maximumRiseUs, decision.playoutDelayUs - previousDelayUs);
        }
        previousDelayUs = decision.playoutDelayUs;
    }
    expect(decision.playoutDelayUs > delayBeforeUs + 2000,
           "a sustained late regime must raise the delay");
    expect(maximumRiseUs <= policy.playoutDelayAttackUs,
           "the delay must rise at most one attack step per frame");

    // A cadence change to 60 FPS opens a new band that starts high again.
    const uint64_t baseUs = decodedTimeForRtp(epochUs, rtpFor(2500, 116));
    bool sawStartInNewBand = false;
    for (int i = 0; i < 200; ++i) {
        const uint32_t timestamp = rtpFor(2500, 116) + rtpFor(i, 60);
        const uint64_t decodedUs = baseUs + static_cast<uint64_t>(i) * 1000000ULL / 60ULL;
        decision = controller.schedule(frame(2501 + i, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        if (controller.playoutBandIndex() == 60 / 20 &&
                decision.playoutDelayUs == policy.playoutDelayStartUs) {
            sawStartInNewBand = true;
        }
    }
    expect(controller.playoutBandIndex() == 60 / 20,
           "a material rate change must move to the new band");
    expect(sawStartInNewBand,
           "a new band must start from the mode's high start value");
}

void testSmoothnessLearningWindowTracksCadence()
{
    // The quarter-second learning window remains a replay-selectable policy
    // even though the production smoothness session no longer uses it.
    const auto learnedSampleCount = [](int sourceRateHz) {
        VrrSessionConfig session = config(sourceRateHz, 240);
        session.allowAdditionalQueuedFrame = true;
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

    // Old traces did exit as soon as the guard reached its base value. Keep
    // that behavior selectable so exact baseline replay remains possible.
    VrrTimingParameters legacyParameters;
    legacyParameters.latchedPresentationBaseGuardExit = 1;
    legacyParameters.cadenceStabilityLatchFrames = 0;
    VrrTimingController legacy(config(115, 120), true, legacyParameters);
    decision = legacy.schedule(frame(1, 0, true, 100000), 100000);
    const uint64_t legacyBaseGuardUs = decision.guardUs;
    legacy.noteSpacingDeficit(
        decision.headroomUs -
            legacyParameters.latchedPresentationHeadroomUs + 1);
    decision = legacy.schedule(
        frame(2, 783, true, 108696), 108696);
    expect(decision.latchedPresentation,
           "legacy replay policy must still enter latching");
    decayGuardToBase(legacy, legacyBaseGuardUs);
    decision = legacy.schedule(
        frame(3, 1565, true, 117392), 117392);
    expect(!decision.latchedPresentation,
           "legacy replay policy must retain base-guard exit semantics");
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
    VrrSessionConfig smoothnessConfig = config(116, 120);
    smoothnessConfig.allowAdditionalQueuedFrame = true;
    VrrTimingController controller(smoothnessConfig);
    VrrTimingDecision decision = controller.schedule(
        frame(0, 0, true, epochUs), epochUs);
    controller.noteSubmission(true, false, decision.targetUs);

    uint64_t previousTargetUs = decision.targetUs;
    unsigned int measuredSpacings = 0;
    unsigned int largeErrors = 0;
    for (int i = 1; i <= 300; ++i) {
        const uint32_t timestamp = quantizedRtpTimestamp(i, 100);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp);
        decision = controller.schedule(
            frame(i, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);

        if (i > 180) {
            const uint64_t spacingUs = decision.targetUs - previousTargetUs;
            const uint64_t errorUs = spacingUs > expectedPeriodUs ?
                spacingUs - expectedPeriodUs : expectedPeriodUs - spacingUs;
            ++measuredSpacings;
            if (errorUs > 500) {
                ++largeErrors;
            }
        }
        previousTargetUs = decision.targetUs;
    }

    if (largeErrors * 20 > measuredSpacings) {
        std::fprintf(stderr,
                     "quantized target errors: %u/%u, readiness=%lld us, budget=%llu us\n",
                     largeErrors, measuredSpacings,
                     static_cast<long long>(controller.readinessBudgetUs()),
                     static_cast<unsigned long long>(controller.timingBudgetUs()));
    }
    expect(largeErrors * 20 <= measuredSpacings,
           "a learned quantized cadence must project at least 95 percent of targets within 500 us");
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

void testSmoothnessAddsOneSourcePeriodOfReserve()
{
    VrrSessionConfig lowLatencyConfig = config(100, 120);
    VrrSessionConfig smoothnessConfig = lowLatencyConfig;
    smoothnessConfig.allowAdditionalQueuedFrame = true;
    VrrTimingController lowLatency(lowLatencyConfig);
    VrrTimingController smoothness(smoothnessConfig);

    const VrrTimingDiagnostics lowLatencyDiagnostics =
        lowLatency.diagnostics();
    const VrrTimingDiagnostics smoothnessDiagnostics =
        smoothness.diagnostics();
    expect(smoothnessDiagnostics.pacingLatencyBudgetUs ==
               lowLatencyDiagnostics.pacingLatencyBudgetUs +
                   smoothness.sourcePeriodUs(),
           "smoothness mode must grant exactly one source period of reserve");
}

void testLegacyReplayPolicyRetainsAbsoluteRenderCeiling()
{
    VrrTimingParameters parameters;
    parameters.renderLeadCeilingUs = 6500;
    parameters.pacingLatencyBudgetDivisor = 0;
    VrrTimingController controller(config(100, 120), true, parameters);
    trainRenderDurations(controller, 9000, 9000);

    const VrrTimingDiagnostics diagnostics = controller.diagnostics();
    expect(controller.renderLeadUs() == 6500,
           "legacy replay mode must retain the captured absolute render ceiling");
    expect(diagnostics.pacingLatencyBudgetUs == 0,
           "legacy replay mode must mark the pacing latency policy disabled");
    expect(controller.timingBudgetUs() ==
               diagnostics.appliedReadinessReserveUs +
                   controller.renderLeadUs(),
           "legacy replay timing budget must include the full render lead");
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

} // namespace

int main()
{
    testRtpWrapResetAndFallback();
    testTimingFormulaeAndReserveCap();
    testSourcePlayoutDelayOffsetsProjectedTargets();
    testSmoothnessTimestampPlayoutHoldsFixedDelay();
    testAdaptivePlayoutDelayCalibratesPerBand();
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
    testSmoothnessAddsOneSourcePeriodOfReserve();
    testLegacyReplayPolicyRetainsAbsoluteRenderCeiling();
    testTargetWaiterBoundaries();
    testRuntimeParametersChangePolicy();
    return failures == 0 ? 0 : 1;
}
