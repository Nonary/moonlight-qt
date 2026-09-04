// Standalone deterministic coverage for the platform-neutral VRR core.
// This file is intentionally not wired into the application build; tests/vrr
// owns the optional qmake harness.

#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// The retired metronome, for the tests that keep it replayable.
VrrTimingParameters metronomePolicy(const VrrSessionConfig& session)
{
    VrrTimingParameters policy = vrrTimingParametersForSession(session);
    policy.playoutSmoothingGainPerMille = 150;
    policy.playoutMetronomeEnabled = 1;
    policy.playoutMotionDeadbandEnabled = 0;
    policy.playoutBandWidthHz = 20;
    policy.playoutDelaySlewAcrossBands = 0;
    return policy;
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
               lowLatencyPolicy.playoutDelayPercentilePerMille == 1000 &&
               lowLatencyPolicy.playoutPrepareOnArrival == 0 &&
               lowLatencyPolicy.renderStartAfterSubmissionUs == 6000 &&
               lowLatencyPolicy.renderStartMinimumLeadUs == 2500 &&
               lowLatencyPolicy.renderLeadFloorUs == 3000 &&
               lowLatencyPolicy.playoutSmoothingGainPerMille == 0 &&
               lowLatencyPolicy.playoutSmoothingMaxLagUs == 8000 &&
               lowLatencyPolicy.playoutMetronomeEnabled == 0 &&
               lowLatencyPolicy.playoutDelayStartPeriodPerMille == 1250 &&
               lowLatencyPolicy.playoutDelayMaximumPeriodPerMille == 1250 &&
               lowLatencyPolicy.playoutSmoothingSnapPerMille == 3000 &&
               lowLatencyPolicy.playoutOffsetReseedFrames == 3 &&
               lowLatencyPolicy.playoutBandWidthHz == 20 &&
               lowLatencyPolicy.playoutMotionDeadbandEnabled == 0 &&
               lowLatencyPolicy.playoutDelaySlewAcrossBands == 1 &&
               lowLatencyPolicy.rateCandidateMinimumUs == 200000 &&
               lowLatencyPolicy.playoutStallBurstExclusion == 1 &&
               lowLatencyPolicy.latchedFloorDisabled == 1 &&
               lowLatencyPolicy.pacingLatencyExtraPeriodNumerator == 0 &&
               lowLatencyPolicy.pacingLatencyQueueModeExtra == 0 &&
               lowLatencyPolicy.readinessLowPercentile == 0 &&
               lowLatencyPolicy.readinessLoosePercentile == 80 &&
               lowLatencyPolicy.retainReadinessOnPhaseReset == 0,
           "sessions must resolve the direct adaptive p99.9 timestamp playout policy");
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
    // This test covers the fixed-delay mechanism; the calibrator, the
    // legacy cadence smoother, and the metronome have their own.
    policy.playoutDelayAdaptive = 0;
    policy.playoutSmoothingGainPerMille = 0;
    policy.playoutMetronomeEnabled = 0;
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

void testCadenceSmoothingEvensJitteredSource()
{
    // A steady 60 FPS game whose host stamps jitter +-3 ms per frame. With
    // smoothing off the presented intervals copy that jitter; with the
    // session policy they must be far more even, at a bounded lag behind
    // the raw slot, without ever re-anchoring.
    VrrSessionConfig session = config(60, 120);
    const uint64_t epochUs = 1000000;
    const auto stampFor = [](int i) {
        // Deterministic jitter in [-3000, 3000) us on the sender stamp.
        const int64_t jitterUs =
            static_cast<int64_t>((static_cast<uint64_t>(i) * 7919ULL) % 6000ULL) -
            3000;
        // Start 10 ms in so the jitter never makes the first stamp wrap.
        const int64_t idealUs = 10000 + static_cast<int64_t>(i) * 1000000LL / 60LL;
        return static_cast<uint32_t>((idealUs + jitterUs) * 90LL / 1000LL);
    };
    const auto run = [&](uint64_t gainPerMille, double& spreadUs,
                         int64_t& maximumLagUs, unsigned int& resets) {
        VrrTimingParameters policy = vrrTimingParametersForSession(session);
        policy.playoutDelayAdaptive = 0;
        policy.sourcePlayoutDelayUs = 8000;
        policy.playoutSmoothingGainPerMille = gainPerMille;
        // The legacy gain smoother is what older captures replay under.
        policy.playoutMetronomeEnabled = 0;
        VrrTimingController controller(session, true, policy);
        std::vector<int64_t> intervals;
        uint64_t previousTargetUs = 0;
        maximumLagUs = 0;
        resets = 0;
        for (int i = 0; i < 600; ++i) {
            const uint32_t timestamp = stampFor(i);
            // The frame decodes a fixed transport delay after its stamp.
            const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 500;
            const VrrTimingDecision decision = controller.schedule(
                frame(i + 1, timestamp, true, decodedUs), decodedUs);
            controller.noteSubmission(true, false, decision.targetUs);
            if (i > 0 && (decision.rebased || decision.phaseDiscontinuity)) {
                ++resets;
            }
            if (i >= 100) {
                intervals.push_back(static_cast<int64_t>(decision.targetUs) -
                                    static_cast<int64_t>(previousTargetUs));
                maximumLagUs = std::max(maximumLagUs,
                                        decision.cadenceSmoothingUs);
            }
            previousTargetUs = decision.targetUs;
        }
        double mean = 0;
        for (int64_t value : intervals) mean += static_cast<double>(value);
        mean /= static_cast<double>(intervals.size());
        double variance = 0;
        for (int64_t value : intervals) {
            variance += (static_cast<double>(value) - mean) *
                (static_cast<double>(value) - mean);
        }
        spreadUs = std::sqrt(variance / static_cast<double>(intervals.size()));
    };
    double rawSpreadUs = 0;
    double smoothSpreadUs = 0;
    int64_t rawLagUs = 0;
    int64_t smoothLagUs = 0;
    unsigned int rawResets = 0;
    unsigned int smoothResets = 0;
    run(0, rawSpreadUs, rawLagUs, rawResets);
    run(150, smoothSpreadUs, smoothLagUs, smoothResets);
    std::fprintf(stderr,
                 "cadence smoothing: raw spread=%.0f us lag=%lld resets=%u; smoothed spread=%.0f us lag=%lld resets=%u\n",
                 rawSpreadUs, static_cast<long long>(rawLagUs), rawResets,
                 smoothSpreadUs, static_cast<long long>(smoothLagUs),
                 smoothResets);
    expect(rawSpreadUs > 1500.0,
           "with smoothing off the presented intervals must copy the sender jitter");
    expect(smoothSpreadUs < rawSpreadUs / 3.0,
           "the session smoother must cut the presented interval spread by at least 3x");
    expect(smoothLagUs <= 8000 && smoothLagUs > 0,
           "the smoothed schedule must lag the raw slot by at most the configured maximum");
    expect(rawResets == 0 && smoothResets == 0,
           "sender stamp jitter must never re-anchor the timestamp clock");
}

void testAdaptivePlayoutDelaySlewsAcrossBands()
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
    constexpr int kReleaseFrames = 2600;
    for (int i = 0; i < kReleaseFrames; ++i) {
        const uint32_t timestamp = rtpFor(i, 116);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + jitterFor(i);
        decision = controller.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        // The start value is two fitted source periods; the reservoir opens
        // on the negotiated 120 FPS period and the fitted 116 FPS one
        // follows, so both scaled starts are acceptable.
        const uint64_t scaledStartUs =
            decision.sourcePeriodUs * policy.playoutDelayStartPeriodPerMille / 1000;
        if (i < 400 && decision.playoutDelayUs < scaledStartUs - 500) {
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
           "the delay must hold the start value until the reservoir has enough samples to release");
    expect(controller.playoutBandIndex() == 116 / 20,
           "the band must follow the fitted source rate");
    const uint64_t expectedUs = 2940 + policy.playoutDelayMarginUs;
    expect(decision.playoutDelayUs >= expectedUs - 200 &&
               decision.playoutDelayUs <= expectedUs + 200,
           "the delay must converge to the target lateness percentile plus margin");
    expect(maximumStepUs <= std::max(policy.playoutDelayAttackUs,
                                     policy.playoutDelayReleaseUs),
           "the delay must never move more than one slew step per frame within a band");
    // The target is built with the delay in force when its slot was mapped;
    // the budget reports the calibrator's latest value, one slew step on.
    const uint64_t budgetGapUs = controller.timingBudgetUs() > decision.playoutDelayUs ?
        controller.timingBudgetUs() - decision.playoutDelayUs :
        decision.playoutDelayUs - controller.timingBudgetUs();
    expect(budgetGapUs <= std::max(policy.playoutDelayAttackUs,
                                   policy.playoutDelayReleaseUs),
           "the timing budget must report the applied adaptive delay");

    // A worse regime: one frame in ten arrives 6 ms late. The delay must
    // attack upward at the attack rate, never in one jump.
    const uint64_t delayBeforeUs = decision.playoutDelayUs;
    uint64_t maximumRiseUs = 0;
    for (int i = kReleaseFrames; i < kReleaseFrames + 1000; ++i) {
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

    // A cadence change to 60 FPS opens a new band at its start value, and
    // the delay in force walks there one slew step per frame.
    const int lastFrame = kReleaseFrames + 1000;
    const uint64_t baseUs = decodedTimeForRtp(epochUs, rtpFor(lastFrame, 116));
    const uint64_t delayAtChangeUs = decision.playoutDelayUs;
    uint64_t maximumStepAcrossRatesUs = 0;
    bool rateChanged = false;
    for (int i = 0; i < 200; ++i) {
        const uint32_t timestamp = rtpFor(lastFrame, 116) + rtpFor(i, 60);
        const uint64_t decodedUs = baseUs + static_cast<uint64_t>(i) * 1000000ULL / 60ULL;
        decision = controller.schedule(frame(lastFrame + 1 + i, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        rateChanged = rateChanged || decision.sourceRateChanged;
        const uint64_t stepUs = decision.playoutDelayUs > previousDelayUs ?
            decision.playoutDelayUs - previousDelayUs :
            previousDelayUs - decision.playoutDelayUs;
        maximumStepAcrossRatesUs = std::max(maximumStepAcrossRatesUs, stepUs);
        previousDelayUs = decision.playoutDelayUs;
    }
    std::fprintf(stderr, "band slew: rateChanged=%d band=%u period=%llu delay at change=%llu after=%llu max step=%llu\n",
                 rateChanged ? 1 : 0, controller.playoutBandIndex(),
                 static_cast<unsigned long long>(controller.sourcePeriodUs()),
                 static_cast<unsigned long long>(delayAtChangeUs),
                 static_cast<unsigned long long>(decision.playoutDelayUs),
                 static_cast<unsigned long long>(maximumStepAcrossRatesUs));
    // A gradual glide of the endpoint fit never crosses the material
    // threshold in one step, so the flag stays clear; the period itself
    // must arrive at the new rate.
    (void) rateChanged;
    expect(std::abs(static_cast<int64_t>(controller.sourcePeriodUs()) - 16667) < 100 &&
               controller.playoutBandIndex() == 60 / 20,
           "a material rate change must move to the new band");
    expect(maximumStepAcrossRatesUs <= std::max(policy.playoutDelayAttackUs,
                                                policy.playoutDelayReleaseUs),
           "a band change must never move the delay by more than one slew step");
    expect(decision.playoutDelayUs > delayAtChangeUs + 4000,
           "the delay must walk toward the new band's start value");
}

void testPrepareOnArrivalSpendsTheCushion()
{
    // With preparation on arrival a frame that arrives on time starts its
    // preparation at once: the render start sits a whole playout delay
    // before the usual lead, at the frame's mapped slot.
    VrrSessionConfig session = config(116, 120);
    VrrTimingParameters policy = vrrTimingParametersForSession(session);
    policy.playoutPrepareOnArrival = 1;
    policy.renderStartAfterSubmissionUs = 0;
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    VrrTimingDecision decision;
    for (int i = 0; i < 300; ++i) {
        const uint32_t timestamp = static_cast<uint32_t>(static_cast<uint64_t>(i) * 90000ULL / 116ULL);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 700;
        decision = controller.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
    }
    expect(decision.renderStartUs + decision.renderLeadUs +
               decision.renderWakeLeadUs + decision.playoutDelayUs ==
               decision.targetUs,
           "the render start must precede the target by the lead plus the playout delay");
    expect(decision.renderStartUs <= decision.sourceTimeUs + 1,
           "an on-time frame must be ready to prepare at its mapped slot");
    expect(decision.playoutDelayUs >= 6000,
           "the whole-tail cushion must not release below the start before it has evidence");
}

void testRenderStartKeepsClearOfPreviousPresent()
{
    // Preparation must not begin within the acquire-blocking window after
    // the previous present, but it must keep the minimum lead before the
    // target when the interval is too short for both.
    VrrSessionConfig session = config(116, 120);
    const VrrTimingParameters policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    VrrTimingDecision decision;
    uint64_t lastSubmissionUs = 0;
    bool clear = true;
    bool leadKept = true;
    for (int i = 0; i < 300; ++i) {
        const uint32_t timestamp = static_cast<uint32_t>(static_cast<uint64_t>(i) * 90000ULL / 116ULL);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 700;
        decision = controller.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
        if (i > 50) {
            const uint64_t gapUs = decision.renderStartUs - lastSubmissionUs;
            const uint64_t leadUs = decision.targetUs - decision.renderStartUs;
            // Inside the blocking window only when the minimum lead forced it.
            if (gapUs < policy.renderStartAfterSubmissionUs &&
                    leadUs > policy.renderStartMinimumLeadUs) {
                clear = false;
            }
            // The guard never leaves less than the minimum lead; a lead
            // already shorter than that was the learned lead, not the guard.
            if (gapUs >= policy.renderStartAfterSubmissionUs &&
                    leadUs < policy.renderStartMinimumLeadUs &&
                    decision.renderLeadUs + decision.renderWakeLeadUs >=
                        policy.renderStartMinimumLeadUs) {
                leadKept = false;
            }
        }
        controller.noteSubmission(true, false, decision.targetUs);
        lastSubmissionUs = decision.targetUs;
    }
    expect(clear,
           "preparation must start no sooner than the acquire-safe gap after the previous present");
    expect(leadKept,
           "preparation must keep the minimum lead before the target");
}

void testShortHitchDoesNotRefitSourceRate()
{
    // Four host frames stamped 33 ms apart are a hitch, not a 30 Hz source.
    // The fitted rate, the metronome period and the delay must hold; a real
    // cutscene that keeps the slow cadence for longer is still accepted.
    VrrSessionConfig session = config(116, 120);
    const VrrTimingParameters policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    int frameNumber = 0;
    uint32_t timestamp = 0;
    VrrTimingDecision decision;
    for (int i = 0; i < 700; ++i) {
        timestamp = static_cast<uint32_t>(static_cast<uint64_t>(i) * 90000ULL / 116ULL);
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 800;
        decision = controller.schedule(frame(++frameNumber, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
    }
    const uint64_t stablePeriodUs = controller.sourcePeriodUs();
    const uint64_t stableDelayUs = decision.playoutDelayUs;

    bool refitted = false;
    uint64_t maximumDelayStepUs = 0;
    uint64_t previousDelayUs = stableDelayUs;
    for (int i = 0; i < 4; ++i) {
        timestamp += 3000;
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 800;
        decision = controller.schedule(frame(++frameNumber, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        refitted = refitted || decision.sourceRateChanged;
    }
    for (int i = 0; i < 100; ++i) {
        timestamp += 776;
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 800;
        decision = controller.schedule(frame(++frameNumber, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        refitted = refitted || decision.sourceRateChanged;
        const uint64_t stepUs = decision.playoutDelayUs > previousDelayUs ?
            decision.playoutDelayUs - previousDelayUs :
            previousDelayUs - decision.playoutDelayUs;
        maximumDelayStepUs = std::max(maximumDelayStepUs, stepUs);
        previousDelayUs = decision.playoutDelayUs;
    }
    expect(!refitted,
           "a four-frame hitch must not be accepted as a new source rate");
    expect(std::abs(static_cast<int64_t>(controller.sourcePeriodUs()) -
                    static_cast<int64_t>(stablePeriodUs)) < 100,
           "a four-frame hitch must leave the fitted source period alone");
    expect(maximumDelayStepUs <= std::max(policy.playoutDelayAttackUs,
                                          policy.playoutDelayReleaseUs),
           "a hitch must never move the playout delay by more than one slew step");

    VrrTimingDecision cutscene;
    bool accepted = false;
    for (int i = 0; i < 15; ++i) {
        timestamp += 3000;
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, timestamp) + 800;
        cutscene = controller.schedule(frame(++frameNumber, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, cutscene.targetUs);
        accepted = accepted || cutscene.sourceRateChanged;
    }
    expect(accepted &&
               std::abs(1000000.0 /
                   static_cast<double>(controller.sourcePeriodUs()) - 30.0) < 0.5,
           "a 30 FPS cutscene must be accepted once it has lasted 200 ms");
}

void testMotionDeadbandHonorsStampSteps()
{
    // Small stamp wobble is capture noise the grid absorbs. A stamp that
    // departs from the grid by more than the learned bound is the host
    // reporting a change in frame timing: the frame presents on its stamp
    // and the grid restarts there instead of paying the difference back
    // over dozens of frames.
    // A 60 FPS stream on the 120 Hz panel, so the display floor never
    // stands between a stamp and its slot. Replay-only policy.
    VrrSessionConfig session = config(60, 120);
    VrrTimingParameters policy = metronomePolicy(session);
    policy.playoutMotionDeadbandEnabled = 1;
    // One reservoir, so the slowdown below is not also a band change.
    policy.playoutBandWidthHz = 1000;
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    const int64_t periodUs = 1000000 / 60;
    const auto wobbleUs = [](int i) {
        return static_cast<int64_t>((static_cast<uint64_t>(i) * 7919ULL) % 600ULL) - 300;
    };
    int64_t stampUs = 20000;
    VrrTimingDecision decision;
    uint64_t previousTargetUs = 0;
    int64_t worstSteadyDeviationUs = 0;
    int frameNumber = 0;
    const auto step = [&](int64_t stampIntervalUs, int64_t jitterUs) {
        stampUs += stampIntervalUs;
        const uint32_t timestamp = static_cast<uint32_t>((stampUs + jitterUs) * 90LL / 1000LL);
        const uint64_t decodedUs = static_cast<uint64_t>(
            static_cast<int64_t>(epochUs) + stampUs + jitterUs + 900);
        decision = controller.schedule(frame(++frameNumber, timestamp, true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
    };
    for (int i = 0; i < 600; ++i) {
        step(periodUs, wobbleUs(i));
        if (i >= 300) {
            const int64_t deviationUs = static_cast<int64_t>(decision.targetUs) -
                static_cast<int64_t>(previousTargetUs) - periodUs;
            worstSteadyDeviationUs = std::max(
                worstSteadyDeviationUs, deviationUs < 0 ? -deviationUs : deviationUs);
        }
        previousTargetUs = decision.targetUs;
    }
    expect(worstSteadyDeviationUs < 300,
           "stamp wobble inside the deadband must not reach the presented cadence");

    // The game's phase steps 3 ms earlier than the grid would predict.
    // The grid must re-anchor within a frame or two rather than walk the
    // offset back at the bounded step.
    step(periodUs - 3000, 0);
    previousTargetUs = decision.targetUs;
    int64_t worstResidualUs = 0;
    std::fprintf(stderr, "deadband phase step residuals:");
    for (int i = 0; i < 40; ++i) {
        step(periodUs, wobbleUs(i));
        std::fprintf(stderr, " %lld", static_cast<long long>(decision.cadenceSmoothingUs));
        if (i >= 3) {
            const int64_t residualUs = decision.cadenceSmoothingUs;
            worstResidualUs = std::max(worstResidualUs,
                                       residualUs < 0 ? -residualUs : residualUs);
        }
        previousTargetUs = decision.targetUs;
    }
    std::fprintf(stderr, "\n");
    expect(worstResidualUs < 800,
           "after a phase step the grid must sit on the stamps again within a few frames");

    // The game slows to 24 ms frames. Every slowed frame must present on
    // its own stamp: the interval follows the stamp interval at once.
    int64_t worstSlowIntervalErrorUs = 0;
    std::fprintf(stderr, "deadband slow intervals:");
    for (int i = 0; i < 20; ++i) {
        step(24000, wobbleUs(i));
        const int64_t intervalUs = static_cast<int64_t>(decision.targetUs) -
            static_cast<int64_t>(previousTargetUs);
        const int64_t errorUs = intervalUs - 24000;
        std::fprintf(stderr, " %lld/%lld", static_cast<long long>(intervalUs),
                     static_cast<long long>(decision.cadenceSmoothingUs));
        if (i >= 1) {
            worstSlowIntervalErrorUs = std::max(
                worstSlowIntervalErrorUs, errorUs < 0 ? -errorUs : errorUs);
        }
        previousTargetUs = decision.targetUs;
    }
    std::fprintf(stderr, "\n");
    expect(worstSlowIntervalErrorUs < 700,
           "a real change in frame timing must present on the stamp, not on the old grid");
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

void testMetronomeHoldsCadenceThroughJitterAndLateFrames()
{
    // A steady 116 FPS game. Host stamps wobble +-3 ms per frame and
    // delivery adds [0, 2 ms) of arrival jitter that is independent of the
    // stamp. The metronome must present at the fitted period with at most
    // a bounded step per frame: neither kind of jitter reaches the panel.
    VrrSessionConfig session = config(116, 120);
    const VrrTimingParameters policy = metronomePolicy(session);
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    const int64_t periodUs = 1000000 / 116;
    const auto idealUs = [&](int i) {
        return static_cast<int64_t>(20000) + static_cast<int64_t>(i) * 1000000LL / 116LL;
    };
    const auto stampJitterUs = [](int i) {
        return static_cast<int64_t>((static_cast<uint64_t>(i) * 7919ULL) % 6000ULL) - 3000;
    };
    const auto arrivalJitterUs = [](int i) {
        return static_cast<int64_t>((static_cast<uint64_t>(i) * 104729ULL) % 2000ULL);
    };
    const auto rtpFor = [&](int i) {
        return static_cast<uint32_t>((idealUs(i) + stampJitterUs(i)) * 90LL / 1000LL);
    };
    const auto decodedFor = [&](int i, int64_t extraUs) {
        return static_cast<uint64_t>(static_cast<int64_t>(epochUs) + idealUs(i) +
                                     500 + arrivalJitterUs(i) + extraUs);
    };

    const int64_t stepCeilingUs = std::max<int64_t>(
        static_cast<int64_t>(policy.playoutPhaseStepMinimumUs),
        periodUs * static_cast<int64_t>(policy.playoutPhaseStepPeriodPerMille) / 1000);
    // In steady state the tick moves by at most one minimum residual step
    // plus the calibrator's own slew, which it follows exactly.
    const int64_t steadyToleranceUs =
        static_cast<int64_t>(policy.playoutPhaseStepMinimumUs) +
        static_cast<int64_t>(std::max(policy.playoutDelayAttackUs,
                                      policy.playoutDelayReleaseUs)) + 2;
    const int64_t toleranceUs = stepCeilingUs +
        static_cast<int64_t>(policy.playoutPhaseStepMinimumUs) + 2;

    VrrTimingDecision decision;
    uint64_t previousTargetUs = 0;
    int64_t worstDeviationUs = 0;
    unsigned int resets = 0;
    unsigned int steppedFrames = 0;
    unsigned int buckets[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 1200; ++i) {
        const uint64_t decodedUs = decodedFor(i, 0);
        decision = controller.schedule(frame(i + 1, rtpFor(i), true, decodedUs),
                                       decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        if (i > 0 && (decision.rebased || decision.phaseDiscontinuity)) {
            ++resets;
        }
        if (i >= 200) {
            const int64_t intervalUs = static_cast<int64_t>(decision.targetUs) -
                static_cast<int64_t>(previousTargetUs);
            const int64_t deviationUs = intervalUs - periodUs;
            const int64_t magnitudeUs = deviationUs < 0 ? -deviationUs : deviationUs;
            worstDeviationUs = std::max(worstDeviationUs, magnitudeUs);
            if (magnitudeUs > steadyToleranceUs) {
                ++steppedFrames;
            }
            ++buckets[magnitudeUs <= 12 ? 0 : magnitudeUs <= 32 ? 1 :
                      magnitudeUs <= 62 ? 2 : magnitudeUs <= 120 ? 3 : 4];
        }
        previousTargetUs = decision.targetUs;
    }
    std::fprintf(stderr,
                 "metronome: worst steady interval deviation=%lld us (tolerance %lld), frames over minimum step=%u, resets=%u delay=%llu, |dev| buckets <=12:%u <=32:%u <=62:%u <=120:%u more:%u\n",
                 static_cast<long long>(worstDeviationUs),
                 static_cast<long long>(toleranceUs), steppedFrames, resets,
                 static_cast<unsigned long long>(decision.playoutDelayUs),
                 buckets[0], buckets[1], buckets[2], buckets[3], buckets[4]);
    expect(resets == 0,
           "stamp and arrival jitter must never re-anchor the metronome");
    expect(worstDeviationUs <= toleranceUs,
           "the metronome must never move the presented slot by more than one bounded step");
    expect(steppedFrames == 0,
           "in steady state no interval may move beyond one residual step plus the delay slew");
    expect(buckets[0] >= 700,
           "in steady state most intervals must sit within a dozen microseconds of the period");
    expect(decision.playoutDelayUs >= 4000,
           "with 2 ms arrival jitter over a 3 ms stamp floor the cushion must stay several ms");

    // A 30 ms arrival stall lands four frames at once. Emulating the
    // worker, a frame that missed its tick by a whole period yields to the
    // fresher frame already waiting; the last one presents as soon as it is
    // ready (one stretched interval). Nothing after it presents early, and
    // the lag it took on is paid back one bounded step per frame.
    int64_t lateIntervalUs = 0;
    int64_t worstRecoveryDeviationUs = 0;
    int64_t worstShortIntervalUs = 0;
    int64_t lagAfterUs = 0;
    unsigned int emulatedDrops = 0;
    bool sawStretch = false;
    const uint64_t stallEndUs = decodedFor(1200, 30000);
    std::string lagTrace;
    for (int i = 1200; i < 1700; ++i) {
        const bool inBurst = i < 1204;
        const uint64_t decodedUs = inBurst ?
            stallEndUs + static_cast<uint64_t>(i - 1200) * 100 : decodedFor(i, 0);
        decision = controller.schedule(frame(i + 1, rtpFor(i), true, decodedUs),
                                       decodedUs);
        const bool successorQueued = i < 1203;
        if (decision.missedTicks != 0 && successorQueued) {
            controller.noteSubmission(false, false, 0);
            ++emulatedDrops;
            continue;
        }
        controller.noteSubmission(true, false, decision.targetUs);
        const int64_t intervalUs = static_cast<int64_t>(decision.targetUs) -
            static_cast<int64_t>(previousTargetUs);
        if (!sawStretch) {
            // The first burst frame to present ends the stall.
            sawStretch = true;
            lateIntervalUs = intervalUs;
        }
        else {
            const int64_t deviationUs = intervalUs - periodUs;
            worstRecoveryDeviationUs = std::max(
                worstRecoveryDeviationUs, deviationUs < 0 ? -deviationUs : deviationUs);
            worstShortIntervalUs = std::min(worstShortIntervalUs, deviationUs);
        }
        if ((i >= 1200 && i < 1212) || i % 100 == 0) {
            char entry[64];
            std::snprintf(entry, sizeof(entry), " %d:%lld/%llu", i,
                          static_cast<long long>(decision.cadenceSmoothingUs),
                          static_cast<unsigned long long>(decision.playoutDelayUs));
            lagTrace += entry;
        }
        previousTargetUs = decision.targetUs;
        if (i >= 1600) {
            // The per-frame lag carries the raw slot's own stamp wobble;
            // average it out to see where the schedule actually sits.
            lagAfterUs += decision.cadenceSmoothingUs;
        }
    }
    lagAfterUs /= 100;
    std::fprintf(stderr,
                 "metronome: stall drops=%u late interval=%lld us, recovery worst deviation=%lld us, shortest=%lld us, lag after=%lld us\n"
                 "metronome lag/delay trace:%s\n",
                 emulatedDrops, static_cast<long long>(lateIntervalUs),
                 static_cast<long long>(worstRecoveryDeviationUs),
                 static_cast<long long>(worstShortIntervalUs),
                 static_cast<long long>(lagAfterUs), lagTrace.c_str());
    expect(emulatedDrops >= 2,
           "frames overtaken by whole periods during a stall must yield to the freshest one");
    expect(lateIntervalUs > periodUs,
           "the frame that ends a stall must present when ready, not be smeared into the cadence");
    expect(worstRecoveryDeviationUs <= toleranceUs,
           "after a stall every following interval must stay within one phase step of the period");
    expect(lagAfterUs > -600 && lagAfterUs < 600,
           "the lag from a stall must be paid back within a few hundred frames");
}

void testMetronomeIgnoresSingleEarlyOutlier()
{
    // A single frame whose stamp is far behind its arrival maps more than a
    // period into the future. The production policy must wait for it rather
    // than re-seed the clock on it, so the frames around it keep their
    // slots and cadence.
    VrrSessionConfig session = config(116, 120);
    const VrrTimingParameters policy = metronomePolicy(session);
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    const int64_t periodUs = 1000000 / 116;
    const auto rtpFor = [](int i) {
        return static_cast<uint32_t>(static_cast<uint64_t>(i) * 90000ULL / 116ULL);
    };
    VrrTimingDecision decision;
    uint64_t previousTargetUs = 0;
    int64_t worstDeviationUs = 0;
    unsigned int discontinuities = 0;
    for (int i = 0; i < 900; ++i) {
        uint64_t decodedUs = decodedTimeForRtp(epochUs, rtpFor(i)) + 800;
        if (i == 600) {
            // The stamp says this frame is two periods newer than it is.
            decodedUs -= static_cast<uint64_t>(2 * periodUs);
        }
        decision = controller.schedule(frame(i + 1, rtpFor(i), true, decodedUs),
                                       decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        if (i > 0 && (decision.rebased || decision.phaseDiscontinuity)) {
            ++discontinuities;
        }
        if (i >= 400) {
            const int64_t deviationUs = static_cast<int64_t>(decision.targetUs) -
                static_cast<int64_t>(previousTargetUs) - periodUs;
            worstDeviationUs = std::max(worstDeviationUs,
                                        deviationUs < 0 ? -deviationUs : deviationUs);
        }
        previousTargetUs = decision.targetUs;
    }
    std::fprintf(stderr, "early outlier: discontinuities=%u worst deviation=%lld us\n",
                 discontinuities, static_cast<long long>(worstDeviationUs));
    expect(discontinuities == 0,
           "one early outlier must not re-seed the sender clock mapping");
    expect(worstDeviationUs < 1000,
           "one early outlier must not move the presented cadence");
}

void testBurstExclusionKeepsDelayAfterStall()
{
    // A clean link learns a small cushion. A 60 ms arrival stall then lands
    // seven frames at once. Their lateness is the stall's backlog, not the
    // link's jitter, and must not pin the cushion at its cap.
    VrrSessionConfig session = config(116, 120);
    const VrrTimingParameters policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session, true, policy);
    const uint64_t epochUs = 1000000;
    const auto rtpFor = [](int i) {
        return static_cast<uint32_t>(static_cast<uint64_t>(i) * 90000ULL / 116ULL);
    };
    const auto jitterFor = [](int i) {
        return static_cast<uint64_t>((static_cast<uint64_t>(i) * 7919ULL) % 1500ULL);
    };
    VrrTimingDecision decision;
    for (int i = 0; i < 2500; ++i) {
        const uint64_t decodedUs = decodedTimeForRtp(epochUs, rtpFor(i)) + jitterFor(i);
        decision = controller.schedule(frame(i + 1, rtpFor(i), true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
    }
    const uint64_t delayBeforeUs = decision.playoutDelayUs;
    expect(delayBeforeUs < 4000,
           "a clean link must release the cushion well below the start value");

    const uint64_t stallEndUs = decodedTimeForRtp(epochUs, rtpFor(2500)) + 60000;
    uint64_t worstDelayUs = delayBeforeUs;
    for (int i = 2500; i < 3000; ++i) {
        uint64_t decodedUs = decodedTimeForRtp(epochUs, rtpFor(i)) + jitterFor(i);
        if (i >= 2500 && i < 2507) {
            decodedUs = stallEndUs + static_cast<uint64_t>(i - 2500) * 100;
        }
        decision = controller.schedule(frame(i + 1, rtpFor(i), true, decodedUs), decodedUs);
        controller.noteSubmission(true, false, decision.targetUs);
        worstDelayUs = std::max(worstDelayUs, decision.playoutDelayUs);
    }
    std::fprintf(stderr, "burst exclusion: delay before=%llu worst after stall=%llu\n",
                 static_cast<unsigned long long>(delayBeforeUs),
                 static_cast<unsigned long long>(worstDelayUs));
    expect(worstDelayUs <= delayBeforeUs + 500,
           "the backlog behind an arrival stall must not raise the cushion");
}

void testLatchedPresentationDropsSoftwareFloor()
{
    // At the refresh rate the source period equals the display period, so a
    // software floor of one display period plus a guard can never be met
    // and sheds a frame every second. Latched presents are ordered by the
    // flip queue instead, so the production policy reports no floor there.
    VrrSessionConfig session = config(120, 120);
    VrrTimingController production(session, true,
                                   vrrTimingParametersForSession(session));
    VrrTimingController legacy(session, true, VrrTimingParameters {});
    const uint64_t startUs = 1000000;
    for (int i = 0; i < 10; ++i) {
        const uint32_t timestamp = static_cast<uint32_t>(i * 750);
        const uint64_t decodedUs = startUs + static_cast<uint64_t>(i) * 8333;
        const VrrTimingDecision produced =
            production.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
        production.noteSubmission(true, false, produced.targetUs);
        const VrrTimingDecision legacyDecision =
            legacy.schedule(frame(i + 1, timestamp, true, decodedUs), decodedUs);
        legacy.noteSubmission(true, false, legacyDecision.targetUs);
        expect(produced.latchedPresentation && legacyDecision.latchedPresentation,
               "a source at the refresh rate must request latched presentation");
    }
    expect(production.earliestSubmissionUs() == 0,
           "latched production presentation must not impose a software spacing floor");
    expect(legacy.earliestSubmissionUs() != 0,
           "the legacy policy must keep its spacing floor for replay fidelity");
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
    testCadenceSmoothingEvensJitteredSource();
    testAdaptivePlayoutDelaySlewsAcrossBands();
    testPrepareOnArrivalSpendsTheCushion();
    testRenderStartKeepsClearOfPreviousPresent();
    testShortHitchDoesNotRefitSourceRate();
    testMotionDeadbandHonorsStampSteps();
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
    testMetronomeHoldsCadenceThroughJitterAndLateFrames();
    testMetronomeIgnoresSingleEarlyOutlier();
    testBurstExclusionKeepsDelayAfterStall();
    testLatchedPresentationDropsSoftwareFloor();
    testRuntimeParametersChangePolicy();
    return failures == 0 ? 0 : 1;
}
