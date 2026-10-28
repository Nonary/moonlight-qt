#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// These fingerprints were recorded from the production controller before the
// revision cleanup. They are a regression guard, not an optical measurement.
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
    const auto policy = vrrTimingParametersForSession(session, scheduled, scheduled);
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
        0xaf0adb410618cba6ULL,
        0xbc32945c3420f64cULL,
        0x6703ca01a899804bULL,
        0x4a1472e3c9f9b061ULL,
        0xe5a780935271f37eULL,
        0x9ea62d26dc5e9f5aULL,
        0x2a4058a1eb8b393eULL,
        0xbfae1a7e97f65000ULL,
        0x0f329f96f53cd9acULL,
        0x2bff41ae6917369bULL,
        0x60d6d2c5b64a8125ULL,
        0xba1940e94ce01539ULL,
        0xdf8341e99f8540d3ULL,
        0xda7201d2a5263ff9ULL,
        0x63dd6aa504300919ULL,
        0x65e00dd90b87bc1bULL,
        0xa8b481c63f9ba21aULL,
        0x89f2863c39f29483ULL,
        0x0ee924c025e53a14ULL,
        0xbc32945c3420f64cULL,
        0x3fe77febb779c103ULL,
        0x4a1472e3c9f9b061ULL,
        0x958d9b82638a6dfdULL,
        0x9ea62d26dc5e9f5aULL,
        0x5b780a5d65ae2e5aULL,
        0xbfae1a7e97f65000ULL,
        0x8bae4b4a5a29256fULL,
        0x2bff41ae6917369bULL,
        0xb3d54fba44a480ecULL,
        0xba1940e94ce01539ULL,
        0xd960c1c33727da1fULL,
        0xda7201d2a5263ff9ULL,
        0x85095943b43dc0dbULL,
        0x65e00dd90b87bc1bULL,
        0xe0955ed0b12d183dULL,
        0x89f2863c39f29483ULL,
        0x092152de4db97de8ULL,
        0xbc32945c3420f64cULL,
        0xa305daef66a4ce6dULL,
        0x4a1472e3c9f9b061ULL,
        0xfdde907aecebe9f1ULL,
        0x9ea62d26dc5e9f5aULL,
        0xd15729091ae087ceULL,
        0xbfae1a7e97f65000ULL,
        0xe4c7fffc1ce543edULL,
        0x2bff41ae6917369bULL,
        0x6bbd3db0e0ce7adeULL,
        0xba1940e94ce01539ULL,
        0xa52c03246748d4bbULL,
        0xda7201d2a5263ff9ULL,
        0xce2b087bcfc40bb2ULL,
        0x65e00dd90b87bc1bULL,
        0xb287daf8f272849aULL,
        0x89f2863c39f29483ULL,
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
