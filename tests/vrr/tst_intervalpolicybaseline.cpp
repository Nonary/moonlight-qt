#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// These fingerprints record current interval diagnostics before removing
// the remaining retired interval policy branches. They are a regression guard, not an optical measurement.
static uint64_t run(int preset, int smoothing, int presentation, bool custom)
{
    VrrSessionConfig session;
    session.latencyMode = preset;
    session.streamRateHz = 120;
    session.displayRefreshHz = 120;
    session.smoothFrameTiming = smoothing != 0;
    session.absorbMicrostutter = smoothing == 2;
    if (custom) session.timingOptions = {1500, 9975, 30, 750};
    session = vrrProductionSessionConfig(session);
    const bool latch = presentation != 0;
    const bool scheduled = presentation == 2;
    auto policy = vrrTimingParametersForSession(session, scheduled, scheduled);
    // Compare against the captured pre-merge policy. Separate worker and
    // startup tests exercise the new ceiling and preparation safeguards.
    policy.playoutCeilingPhaseReplacement = 0;
    policy.playoutOffsetSendSpanNormalization = 0;
    policy.preparationInitialSampleExcluded = 1;
    VrrTimingController controller(session, latch, policy);
    uint64_t digest = 1469598103934665603ULL;
    const auto append = [&](uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            digest ^= uint8_t(value >> (byte * 8));
            digest *= 1099511628211ULL;
        }
    };
    uint64_t sourceUs = 1000000;
    uint32_t rtp = 0;
    uint64_t previousSubmit = 0;
    for (unsigned i = 1; i <= 3600; ++i) {
        // Exercise steady cadence, a rate transition, gaps, jitter, and recovery.
        const uint64_t period = i >= 1200 && i < 1800 ? 16667 : 8333;
        sourceUs += period;
        rtp += period == 16667 ? 1500 : 750;
        const uint64_t delay = i % 113 == 0 ? 4700 : i % 17 == 0 ? 900 : 150;
        const uint64_t ready = sourceUs + delay;
        PacedFrame frame(nullptr, int(i), rtp, true, ready);
        frame.setDeliveryTimeline(sourceUs + 20, sourceUs + 60, sourceUs + 100);
        if (i == 2300) frame.setLostPackets(1);
        const auto decision = controller.schedule(frame, ready);
        const uint64_t prepared = ready + (i % 79 == 0 ? 1800 : 350);
        controller.notePreparationDuration(prepared - ready, 0, prepared, 0);
        const uint64_t submitted = std::max({decision.targetUs, prepared, previousSubmit + 1});
        controller.noteSchedulerDelays(0, i % 149 == 0 ? 1200 : 0, true);
        controller.noteSubmission(true, false, submitted);
        previousSubmit = submitted;
        append(decision.targetUs); append(decision.renderStartUs);
        append(decision.playoutDelayUs); append(decision.sourcePeriodUs);
        append(uint64_t(decision.cadenceSmoothingUs)); append(decision.latchedPresentation);
        append(decision.rebased); append(decision.phaseDiscontinuity);
        append(submitted); append(controller.earliestSubmissionUs());
        const auto interval = controller.intervalStats();
        append(interval.evaluatedUs); append(interval.failedUs);
        append(interval.averageValid); append(interval.serviceOverloaded);
        append(interval.initialCalibrationComplete); append(interval.calibrationCoverageUs);
        append(interval.calibrationSamples); append(interval.update.atUs);
        append(interval.update.frame); append(interval.update.attributedFrame);
        append(interval.update.intervalErrorUs); append(interval.update.latenessUs);
        append(uint64_t(interval.update.action));
        const auto stats = controller.intervalStats().inputPercentile;
        append(stats.targetUs); append(stats.currentUs); append(stats.samples);
        append(stats.eligible); append(stats.ineligible); append(stats.absorbed);
        append(stats.missed); append(stats.recentEligible); append(stats.recentMissed);
        append(stats.validPairCoverageUs); append(stats.tailUncertain);
        append(stats.evidenceValid); append(stats.growthBlocked);
        append(stats.recentReleaseSamples); append(stats.recentReleaseMisses);
        // Preserve the original fingerprint positions for the retired duplicate and constant.
        append(stats.recentReleaseMisses); append(1000000);
        append(stats.effectiveFallRateUsPerSecond);
        append(stats.effectiveFallRateUsPerSecond * 1000);
        append(stats.expiredUnknown); append(stats.clipped);
    }
    return digest;
}

int main(int argc, char** argv)
{
    const bool generate = argc == 2 && std::strcmp(argv[1], "--generate") == 0;
    // Recorded before removing the historical policies.
    static const uint64_t expected[] = {
        0x6367de537a964c09ULL,
        0x12f00f4bb9eb314cULL,
        0x3c8b5189fcfa3b2cULL,
        0xe7930afc7674d547ULL,
        0xd64aaef9058f8333ULL,
        0x0c34ccb96d92735dULL,
        0x8b5648d460246b7dULL,
        0x1a714cf445674f9aULL,
        0x6b5dcc7519893f97ULL,
        0x505559a36e326bdcULL,
        0x0d7e6c709f2b5966ULL,
        0x4fc2a4f2f15c1037ULL,
        0x2aab7e356117c0d4ULL,
        0x758a5d0c4b4500f1ULL,
        0x1736c2052a661bffULL,
        0xa84a11f43e5e97c5ULL,
        0x30d98445d189187eULL,
        0x1f034a4f2821d1adULL,
        0x344840633fe7c2deULL,
        0x12f00f4bb9eb314cULL,
        0x321766c8987efea9ULL,
        0xe7930afc7674d547ULL,
        0x0bbf6f8aa763da43ULL,
        0x0c34ccb96d92735dULL,
        0x2859f9bdc0a073cfULL,
        0x1a714cf445674f9aULL,
        0xadb663fc75f3efc2ULL,
        0x505559a36e326bdcULL,
        0x23bf9ff0dfeccdb4ULL,
        0x4fc2a4f2f15c1037ULL,
        0x18e3451e241961a7ULL,
        0x758a5d0c4b4500f1ULL,
        0x95a00594c13fed0bULL,
        0xa84a11f43e5e97c5ULL,
        0xdac1e882d112bbe6ULL,
        0x1f034a4f2821d1adULL,
        0x516ffbb436b2df16ULL,
        0x12f00f4bb9eb314cULL,
        0x7d0034df578f2b74ULL,
        0xe7930afc7674d547ULL,
        0x1ed31d9a5b77c53dULL,
        0x0c34ccb96d92735dULL,
        0xbdbd9073784a3312ULL,
        0x1a714cf445674f9aULL,
        0x4992db06ccb5233cULL,
        0x505559a36e326bdcULL,
        0x1d1ffbb3c588c796ULL,
        0x4fc2a4f2f15c1037ULL,
        0x80cc7d3a52260368ULL,
        0x758a5d0c4b4500f1ULL,
        0x3650d9294fe5df4cULL,
        0xa84a11f43e5e97c5ULL,
        0x8e3f8988333255dbULL,
        0x1f034a4f2821d1adULL,
    };
    size_t index = 0;
    int failures = 0;
    for (int preset = 0; preset < 3; ++preset)
        for (int smoothing = 0; smoothing < 3; ++smoothing)
            for (int presentation = 0; presentation < 3; ++presentation)
                for (bool custom : {false, true}) {
                    const auto result = run(preset, smoothing, presentation, custom);
                    if (generate) std::printf("0x%016llxULL,\n", static_cast<unsigned long long>(result));
                    else if (index >= sizeof(expected) / sizeof(expected[0]) || result != expected[index]) {
                        std::fprintf(stderr, "policy fingerprint mismatch: preset=%d smoothing=%d presentation=%d custom=%d got=%016llx\n",
                                     preset, smoothing, presentation, int(custom), static_cast<unsigned long long>(result));
                        ++failures;
                    }
                    ++index;
                }
    return failures ? 1 : 0;
}
