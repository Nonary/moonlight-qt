#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// These fingerprints record current interval diagnostics before removing
// the remaining retired interval policy branches. They are a regression guard, not an optical measurement.
// Re-recorded 2026-11-09 for the absorbable-loss solver and release hold. The
// first late frames of each rate layer are budgeted against a quarter of the
// goal window, and every release waits out the hold, so 51 of 54 changed.
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
        0x7dacaaa7b9516e7fULL,
        0xfaf73f9cef2836d4ULL,
        0x55497c6ee38ab7b9ULL,
        0xe65a2e178a9b2639ULL,
        0xb1c7b827fa3bad09ULL,
        0xe52a54a6681e531eULL,
        0x3d992b07371e854fULL,
        0x91a6420beea3b47fULL,
        0xfcc33cc0d53d659fULL,
        0xa519959e0307a0e6ULL,
        0x64a07f7b1541a176ULL,
        0x87704e3990d56eb0ULL,
        0x896382a9475d3675ULL,
        0xde8d2d5469c608a3ULL,
        0xef97ff2244776212ULL,
        0xfac3fd000a9106d7ULL,
        0x98b63d07f348fd80ULL,
        0xda333fd560088283ULL,
        0x9f3044957504babcULL,
        0xfaf73f9cef2836d4ULL,
        0x2c55091cb75b7082ULL,
        0xe65a2e178a9b2639ULL,
        0xbb20346d0ad173c8ULL,
        0xe52a54a6681e531eULL,
        0xb42d739139ccedc6ULL,
        0x91a6420beea3b47fULL,
        0xc308fe5a731bfb46ULL,
        0xa519959e0307a0e6ULL,
        0xa3c653be7655aee3ULL,
        0x87704e3990d56eb0ULL,
        0xa735e331ea079c5cULL,
        0xde8d2d5469c608a3ULL,
        0x34c2a0bb611c0b74ULL,
        0xfac3fd000a9106d7ULL,
        0xe2c24d32bb489638ULL,
        0xda333fd560088283ULL,
        0xe27f35fa4624790dULL,
        0xfaf73f9cef2836d4ULL,
        0xde503935f1ccd284ULL,
        0xe65a2e178a9b2639ULL,
        0x57b59253cf163d88ULL,
        0xe52a54a6681e531eULL,
        0x4e366ca9236fe489ULL,
        0x91a6420beea3b47fULL,
        0xd35db8a846459123ULL,
        0xa519959e0307a0e6ULL,
        0x4b093f44d74ea17bULL,
        0x87704e3990d56eb0ULL,
        0x80cc7d3a52260368ULL,
        0xde8d2d5469c608a3ULL,
        0x3650d9294fe5df4cULL,
        0xfac3fd000a9106d7ULL,
        0x8e3f8988333255dbULL,
        0xda333fd560088283ULL,
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
