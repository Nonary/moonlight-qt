// Integrated adaptive-input contracts that need the live timing controller.
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace {
int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct RunResult {
    Vrr13::InputPercentileBuffer::Stats input;
    uint64_t maxTargetLatencyUs = 0;
    uint64_t maxSubmittedLatencyUs = 0;
    uint64_t maxDecodedAgeUs = 0;
    uint64_t maxPlayoutDelayUs = 0;
    uint64_t measuredSourcePeriodUs = 0;
    uint64_t renderWaitCount = 0;
    uint64_t targetWaitCount = 0;
    uint64_t finalAppliedDelayUs = 0;
};

RunResult runSustainedDownstreamLoad(bool injectInputHitches)
{
    VrrSessionConfig session;
    session.streamRateHz = 60;
    session.displayRefreshHz = 240;
    session.latencyMode = 1;
    session.smoothFrameTiming = false;
    session.experimentalInputPercentilePpm = 1;
    session.timingOptions = {1000, 9950, 10, 500};

    auto policy = vrrTimingParametersForSession(session);
    // Explicit slack makes the worker path arrive before its learned render
    // start early in the run; the active input learner must release it again
    // on clean input. Keep this seeded so the timing-wait check has a causal
    // source of slack rather than a fabricated local/source clock offset.
    policy.playoutDelayStartSeedUs = 6000;
    check(policy.playoutInputHitchMissesOnly == 11 &&
          policy.playoutInputStableAdaptation == 10,
          "integration uses the selected production input policy");
    VrrTimingController controller(session, true, policy);
    uint64_t previousSubmission = 0;
    uint64_t maxLatency = 0;
    uint64_t maxSubmittedLatency = 0;
    uint64_t maxDecodedAge = 0;
    uint64_t maxBuffer = 0;
    uint64_t renderWaitCount = 0;
    uint64_t targetWaitCount = 0;
    uint64_t finalAppliedDelayUs = 0;

    // Source intervals use rational 60 Hz timestamps; downstream service is
    // shorter, so the serial path remains sustainable without queuing.
    constexpr uint64_t renderOvershootUs = 500;
    constexpr uint64_t preparationServiceUs = 2000;
    constexpr uint64_t targetOvershootUs = 500;
    for (int frameNumber = 0; frameNumber < 1800; ++frameNumber) {
        const uint64_t source = 1000000 + uint64_t(frameNumber) * 1000000 / 60;
        const uint64_t inputHitch = injectInputHitches && frameNumber > 1200 &&
                                    frameNumber % 20 == 0 ? 6000 : 0;
        const uint64_t decoded = source + 1000 + inputHitch;
        PacedFrame frame(nullptr, frameNumber,
                         uint32_t(uint64_t(frameNumber) * 1500), true, decoded);
        frame.setDeliveryTimeline(decoded - 800, decoded - 600, decoded - 500);
        const uint64_t decisionTime = std::max(decoded, previousSubmission);
        const auto decision = controller.schedule(frame, decisionTime);
        finalAppliedDelayUs = decision.playoutDelayUs;

        // Simulate a sustained post-input preparation/scheduler load. These
        // costs affect readiness and presentation, while the input timestamps
        // remain exactly the same in both the clean and fault-control runs.
        const bool hadRenderWait = decisionTime < decision.renderStartUs;
        const uint64_t actualRenderOvershoot = hadRenderWait ? renderOvershootUs : 0;
        const uint64_t preparationStart = std::max(decisionTime, decision.renderStartUs) +
                                          actualRenderOvershoot;
        const uint64_t ready = preparationStart + preparationServiceUs;
        const bool hadTargetWait = ready < decision.targetUs;
        const uint64_t beforeTargetWait = std::max(ready, decision.targetUs);
        const uint64_t actualTargetOvershoot = hadTargetWait ? targetOvershootUs : 0;
        const uint64_t submitted = beforeTargetWait + actualTargetOvershoot;
        controller.notePreparationDuration(preparationServiceUs, 0, ready);
        controller.noteSchedulerDelays(actualRenderOvershoot, actualTargetOvershoot,
                                       hadTargetWait);
        controller.noteSubmission(true, false, submitted, submitted);
        renderWaitCount += hadRenderWait;
        targetWaitCount += hadTargetWait;

        previousSubmission = submitted;
        maxLatency = std::max(maxLatency, decision.targetUs > source ?
                              decision.targetUs - source : 0);
        maxSubmittedLatency = std::max(maxSubmittedLatency,
            submitted > source ? submitted - source : 0);
        maxDecodedAge = std::max(maxDecodedAge,
            submitted > decoded ? submitted - decoded : 0);
        maxBuffer = std::max(maxBuffer, decision.playoutDelayUs);
    }

    const auto stats = controller.intervalStats().inputPercentile;
    return {stats, maxLatency, maxSubmittedLatency, maxDecodedAge, maxBuffer, 0,
            renderWaitCount, targetWaitCount, finalAppliedDelayUs};
}

void testDownstreamWorkCannotBuyInputBuffer()
{
    const auto clean = runSustainedDownstreamLoad(false);
    const auto hitched = runSustainedDownstreamLoad(true);
    std::fprintf(stderr, "clean eligible=%llu missed=%llu raises=%llu current=%llu min=%llu max=%llu latency=%llu; "
        "hitched eligible=%llu missed=%llu raises=%llu current=%llu min=%llu max=%llu latency=%llu\n",
        (unsigned long long)clean.input.eligible, (unsigned long long)clean.input.missed,
        (unsigned long long)clean.input.raises, (unsigned long long)clean.input.currentUs,
        (unsigned long long)clean.input.minimumUs, (unsigned long long)clean.input.maximumUs,
        (unsigned long long)clean.maxTargetLatencyUs,
        (unsigned long long)hitched.input.eligible, (unsigned long long)hitched.input.missed,
        (unsigned long long)hitched.input.raises, (unsigned long long)hitched.input.currentUs,
        (unsigned long long)hitched.input.minimumUs, (unsigned long long)hitched.input.maximumUs,
        (unsigned long long)hitched.maxTargetLatencyUs);

    check(clean.input.eligible > 1500,
          "sustained downstream-load run establishes qualified input evidence");
    check(clean.renderWaitCount == 0 && clean.targetWaitCount > 0,
          "current policy starts preparation immediately and exercises target waits without inventing render waits");
    check(clean.input.missed == 0 && clean.input.raises == 0 &&
          clean.input.currentUs <= clean.input.minimumUs + 250,
          "post-input preparation and scheduler load cannot be relabeled as input misses or grow input protection");

    check(hitched.input.eligible > 1500 && hitched.input.missed > clean.input.missed &&
          hitched.input.raises > clean.input.raises &&
          hitched.input.currentUs > hitched.input.minimumUs,
          "same downstream load still permits buffer growth when actual late input causes preventable hitches");
    check(clean.finalAppliedDelayUs <= clean.input.minimumUs + 250 &&
          hitched.finalAppliedDelayUs > clean.finalAppliedDelayUs + 1000,
          "scheduled frame delay releases the clean seed and acquires the protection earned by late input");

    // This bound covers the whole target's source-to-presentation latency,
    // including learned input delay, scheduling and readiness, rather than
    // summing component estimates that may overlap. It is a synthetic normal
    // workload bound; physical readiness remains a separate mandatory floor.
    check(clean.maxTargetLatencyUs < 25000 && hitched.maxTargetLatencyUs < 25000,
          "combined source-to-target latency stays bounded under the modeled workload");
    check(clean.maxSubmittedLatencyUs < 25000 && hitched.maxSubmittedLatencyUs < 25000 &&
          clean.maxDecodedAgeUs < 20000 && hitched.maxDecodedAgeUs < 20000,
          "actual source-to-submission latency and decoder-output age stay bounded under the modeled workload");
    check(clean.maxPlayoutDelayUs <= clean.input.maximumUs &&
          hitched.maxPlayoutDelayUs <= hitched.input.maximumUs,
          "applied input buffer remains within each run's dynamic maximum");
}

RunResult runFractionalSourceCadence(unsigned int nominalRate,
                                    unsigned int displayRate,
                                    uint64_t sourceRateNumerator,
                                    uint64_t sourceRateDenominator)
{
    VrrSessionConfig session;
    session.streamRateHz = nominalRate;
    session.displayRefreshHz = displayRate;
    session.latencyMode = 1;
    session.smoothFrameTiming = false;
    session.experimentalInputPercentilePpm = 1;
    session.timingOptions = {1000, 9950, 10, 500};
    VrrTimingController controller(session, true,
                                   vrrTimingParametersForSession(session));
    uint64_t previousSubmission = 0;
    uint64_t maxTargetLatency = 0;
    uint64_t maxSubmittedLatency = 0;
    uint64_t maxDecodedAge = 0;
    uint64_t maxBuffer = 0;
    uint64_t measuredSourcePeriod = 0;

    for (int frameNumber = 0; frameNumber < 1200; ++frameNumber) {
        const uint64_t pts = uint64_t(frameNumber) * 90000 * sourceRateDenominator /
                             sourceRateNumerator;
        const uint64_t source = 1000000 + pts * 1000000 / 90000;
        const uint64_t decoded = source + 1000;
        PacedFrame frame(nullptr, frameNumber, uint32_t(pts), true, decoded);
        frame.setDeliveryTimeline(decoded - 800, decoded - 600, decoded - 500);
        const auto decision = controller.schedule(frame, std::max(decoded, previousSubmission));
        const uint64_t ready = std::max(decoded, decision.renderStartUs) + 500;
        const uint64_t submitted = std::max(ready, decision.targetUs);
        controller.notePreparationDuration(500, 0, ready);
        controller.noteSchedulerDelays(0, 0, true);
        controller.noteSubmission(true, false, submitted, submitted);
        previousSubmission = submitted;
        measuredSourcePeriod = decision.sourcePeriodUs;

        maxTargetLatency = std::max(maxTargetLatency,
            decision.targetUs > source ? decision.targetUs - source : 0);
        maxSubmittedLatency = std::max(maxSubmittedLatency,
            submitted > source ? submitted - source : 0);
        maxDecodedAge = std::max(maxDecodedAge,
            submitted > decoded ? submitted - decoded : 0);
        maxBuffer = std::max(maxBuffer, decision.playoutDelayUs);
    }

    return {controller.intervalStats().inputPercentile, maxTargetLatency,
            maxSubmittedLatency, maxDecodedAge, maxBuffer, measuredSourcePeriod};
}

void testFractionalCadenceAtDisplayCeilings()
{
    // Fractional source clocks close to 60, 144 and 240 Hz exercise the
    // source-period and display-period floors where there is little room for
    // learned input protection. Idle/low-rate behavior is not inferred from
    // an unusually long source interval; every frame here has an explicit PTS.
    const auto near60 = runFractionalSourceCadence(60, 60, 60000, 1001);
    const auto near144 = runFractionalSourceCadence(144, 144, 1439, 10);
    const auto near240 = runFractionalSourceCadence(240, 240, 240000, 1001);
    struct Case { const RunResult* result; uint64_t rationalPeriodUs; uint64_t nominalDisplayPeriodUs; };
    const Case cases[] = {
        {&near60, 1000000ULL * 1001 / 60000, 1000000ULL / 60},
        {&near144, 1000000ULL * 10 / 1439, 1000000ULL / 144},
        {&near240, 1000000ULL * 1001 / 240000, 1000000ULL / 240},
    };
    for (const auto& testCase : cases) {
        const auto* result = testCase.result;
        check(result->input.eligible > 1000 && result->input.missed == 0 &&
              result->input.raises == 0,
              "fractional source cadence at the display ceiling is clean input evidence");
        check(result->maxTargetLatencyUs < 25000 &&
              result->maxSubmittedLatencyUs < 25000 &&
              result->maxDecodedAgeUs < 20000,
              "fractional source and display periods preserve bounded combined presentation age");
        check(result->maxPlayoutDelayUs <= result->input.maximumUs,
              "fractional cadence buffer demand respects the live dynamic maximum");
        const uint64_t periodError = result->measuredSourcePeriodUs > testCase.rationalPeriodUs ?
            result->measuredSourcePeriodUs - testCase.rationalPeriodUs :
            testCase.rationalPeriodUs - result->measuredSourcePeriodUs;
        check(result->measuredSourcePeriodUs != 0 && periodError <= 100,
              "controller's measured source period matches the explicitly generated fractional cadence");
        const uint64_t nominalError = result->measuredSourcePeriodUs > testCase.nominalDisplayPeriodUs ?
            result->measuredSourcePeriodUs - testCase.nominalDisplayPeriodUs :
            testCase.nominalDisplayPeriodUs - result->measuredSourcePeriodUs;
        check(nominalError <= testCase.nominalDisplayPeriodUs / 100,
              "fractional source rate remains within one percent of the named display ceiling");
    }
}

void testInvalidEpochsEventuallyReleaseOldProtection()
{
    VrrSessionConfig session;
    session.streamRateHz = 100;
    session.displayRefreshHz = 240;
    session.latencyMode = 1;
    session.smoothFrameTiming = false;
    session.experimentalInputPercentilePpm = 1;
    session.timingOptions = {1000, 9950, 10, 500};
    const auto policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session, true, policy);

    uint64_t previousSubmission = 0;
    uint64_t source = 1000000;
    uint32_t pts = 0;
    uint64_t learnedAppliedDelayUs = 0;
    for (int frameNumber = 0; frameNumber < 700; ++frameNumber) {
        source += 10000;
        pts += 900;
        const uint64_t decoded = source + 1000 + (frameNumber % 20 == 0 ? 6000 : 0);
        PacedFrame frame(nullptr, frameNumber, pts, true, decoded);
        frame.setDeliveryTimeline(decoded - 800, decoded - 600, decoded - 500);
        const auto decision = controller.schedule(frame, std::max(decoded, previousSubmission));
        learnedAppliedDelayUs = decision.playoutDelayUs;
        const uint64_t ready = std::max(decoded, decision.renderStartUs) + 500;
        const uint64_t submitted = std::max(ready, decision.targetUs);
        controller.notePreparationDuration(500, 0, ready);
        controller.noteSchedulerDelays(0, 0, true);
        controller.noteSubmission(true, false, submitted, submitted);
        previousSubmission = submitted;
    }
    const auto learned = controller.intervalStats().inputPercentile;
    check(learned.currentUs > learned.minimumUs + 1000 && learned.missed > 0,
          "fault control first earns buffer protection from real input-caused misses");
    check(learnedAppliedDelayUs > learned.minimumUs + 1000,
          "earned protection reaches the actual scheduled delay before invalid evidence begins");

    const uint64_t eligibleBeforeInvalid = learned.eligible;
    uint64_t maxSourceToSubmitted = 0;
    uint64_t settledAppliedDelayUs = 0;
    for (int frameNumber = 700; frameNumber < 4300; ++frameNumber) {
        source += 10000;
        pts += 900;
        if (frameNumber % 60 == 0) controller.rebase();
        const uint64_t decoded = source + 1000;
        PacedFrame frame(nullptr, frameNumber, pts, true, decoded);
        frame.setDeliveryTimeline(decoded - 800, decoded - 600, decoded - 500);
        frame.setLostPackets(1);
        const auto decision = controller.schedule(frame, std::max(decoded, previousSubmission));
        if (frameNumber >= 4000)
            settledAppliedDelayUs = std::max(settledAppliedDelayUs, decision.playoutDelayUs);
        const uint64_t ready = std::max(decoded, decision.renderStartUs) + 500;
        const uint64_t submitted = std::max(ready, decision.targetUs);
        controller.notePreparationDuration(500, 0, ready);
        controller.noteSchedulerDelays(0, 0, true);
        controller.noteSubmission(true, false, submitted, submitted);
        previousSubmission = submitted;
        maxSourceToSubmitted = std::max(maxSourceToSubmitted,
            submitted > source ? submitted - source : 0);
    }
    const auto recovered = controller.intervalStats().inputPercentile;
    check(recovered.eligible == eligibleBeforeInvalid && recovered.expiredUnknown,
          "lost-frame observations and repeated source epochs do not renew eligible evidence");
    check(recovered.currentUs <= recovered.minimumUs + 500 && recovered.unknownRelease > 0,
          "time-based expiry releases stale miss protection despite recurring invalid epochs");
    check(settledAppliedDelayUs <= recovered.minimumUs + 500 &&
          settledAppliedDelayUs + 1000 < learnedAppliedDelayUs,
          "expiry releases the delay applied to every settled frame, not just observer telemetry");
    check(maxSourceToSubmitted < 25000,
          "invalid-evidence recovery keeps actual source-to-submission age bounded");
}
}

int main()
{
    testDownstreamWorkCannotBuyInputBuffer();
    testFractionalCadenceAtDisplayCeilings();
    testInvalidEpochsEventuallyReleaseOldProtection();
    if (failures) {
        std::fprintf(stderr, "%d input contract assertion(s) failed\n", failures);
        return 1;
    }
    std::puts("VRR integrated input contracts passed");
    return 0;
}
