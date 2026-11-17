#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// These fingerprints were recorded from the production controller before the
// revision cleanup. They are a regression guard, not an optical measurement.
// Re-recorded 2026-11-09 for the absorbable-loss solver and release hold. The
// first late frames of each rate layer are budgeted against a quarter of the
// goal window, and every release waits out the hold, so 51 of 54 changed.
// Re-recorded 2026-11-13: the solver's aim may cost at most 500 us beyond what
// the goal itself needs, so 33 of 54 changed.
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
        0x739342e45e4a9457ULL,
        0x7c4c171d57778e07ULL,
        0xd5b0731f36f345f3ULL,
        0xd1d7bc7e07b47ff6ULL,
        0xa50b630bb4c71c43ULL,
        0x1f39f457639c0d43ULL,
        0x1ddd697785a68483ULL,
        0xe701bb4194167555ULL,
        0xa0c1c0155c9c729eULL,
        0x754097820ede1f74ULL,
        0x3a17aede0be56cfcULL,
        0x30468b861ee3949cULL,
        0x1a4ed208d41b34faULL,
        0x41f6fed02ec96e3bULL,
        0x74fbf5a35b767f4cULL,
        0x154501d51286d452ULL,
        0x26b49c0e8c5536b7ULL,
        0xcf2624f9aa83a229ULL,
        0x2cfc9361ccceed92ULL,
        0x7c4c171d57778e07ULL,
        0x7e5b909d838b2b10ULL,
        0xd1d7bc7e07b47ff6ULL,
        0x3d50ccd7b591a610ULL,
        0x1f39f457639c0d43ULL,
        0x186d19b9de2da6b8ULL,
        0xe701bb4194167555ULL,
        0x681f9cb53413de8fULL,
        0x754097820ede1f74ULL,
        0x5bf348f4618f0b79ULL,
        0x30468b861ee3949cULL,
        0x4b202ca6309ab1b7ULL,
        0x41f6fed02ec96e3bULL,
        0x2db0b97007df8eeeULL,
        0x154501d51286d452ULL,
        0xc9678a6f87d9dd80ULL,
        0xcf2624f9aa83a229ULL,
        0x6d79a5af7ef4d8a1ULL,
        0x7c4c171d57778e07ULL,
        0x12da09f44e4cdff4ULL,
        0xd1d7bc7e07b47ff6ULL,
        0x67787203f2e21f74ULL,
        0x1f39f457639c0d43ULL,
        0xc794af1c6e483df7ULL,
        0xe701bb4194167555ULL,
        0xb1e690a901d53ebcULL,
        0x754097820ede1f74ULL,
        0xe32babc6963637ebULL,
        0x30468b861ee3949cULL,
        0xa52c03246748d4bbULL,
        0x41f6fed02ec96e3bULL,
        0xce2b087bcfc40bb2ULL,
        0x154501d51286d452ULL,
        0xb287daf8f272849aULL,
        0xcf2624f9aa83a229ULL,
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
