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
        0x398d438869f56288ULL,
        0xa97f92de6559adddULL,
        0x89ac3693629f9e02ULL,
        0xf31c4a034118e019ULL,
        0xc25a953e67917327ULL,
        0xe870f72cbd9feeeeULL,
        0x2ec7df1048af9924ULL,
        0x21c537501d1d02f1ULL,
        0x65339c6f99eb3027ULL,
        0xefa2a27e2dc84a66ULL,
        0x47304c0868f60770ULL,
        0x1c9107718a6a05edULL,
        0x1a4ed208d41b34faULL,
        0x41f6fed02ec96e3bULL,
        0x74fbf5a35b767f4cULL,
        0x154501d51286d452ULL,
        0x26b49c0e8c5536b7ULL,
        0xcf2624f9aa83a229ULL,
        0x57184dc32623a817ULL,
        0xa97f92de6559adddULL,
        0xadb228a3fad4c9a9ULL,
        0xf31c4a034118e019ULL,
        0xbe03c555f8434ddeULL,
        0xe870f72cbd9feeeeULL,
        0x11c9ad8216f4b472ULL,
        0x21c537501d1d02f1ULL,
        0xc59ad7fb0864ba46ULL,
        0xefa2a27e2dc84a66ULL,
        0xd510c94528b1b3a7ULL,
        0x1c9107718a6a05edULL,
        0x91def6ed4936f830ULL,
        0x41f6fed02ec96e3bULL,
        0x8593aad61b110b8eULL,
        0x154501d51286d452ULL,
        0x88e075b2d3a22917ULL,
        0xcf2624f9aa83a229ULL,
        0x6d79a5af7ef4d8a1ULL,
        0xa97f92de6559adddULL,
        0x12da09f44e4cdff4ULL,
        0xf31c4a034118e019ULL,
        0x67787203f2e21f74ULL,
        0xe870f72cbd9feeeeULL,
        0xc794af1c6e483df7ULL,
        0x21c537501d1d02f1ULL,
        0xb1e690a901d53ebcULL,
        0xefa2a27e2dc84a66ULL,
        0xe32babc6963637ebULL,
        0x1c9107718a6a05edULL,
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
