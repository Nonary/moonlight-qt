#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include "../../app/streaming/video/timinggraph.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <utility>

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

struct DriftResult { double meanBufferUs; uint64_t missed, eligible, maximumUs; };

DriftResult runDrift(double ppm, bool sparseEarly)
{
    VrrSessionConfig config;
    config.streamRateHz = 116; config.displayRefreshHz = 120;
    config.latencyMode = 1; config.smoothFrameTiming = false;
    config.timingOptions = {1000, 9950, 60, 500};
    config = vrrProductionSessionConfig(config);
    auto policy = vrrTimingParametersForSession(config);
    policy.playoutDelayStartSeedUs = 1000;
    VrrTimingController controller(config, true, policy);
    double sourceUs = 1000000, sum = 0;
    uint64_t submitted = 0, counted = 0, maximum = 0;
    uint32_t rtp = 0;
    std::mt19937_64 rng(42);
    std::gamma_distribution<double> jitter(2.0, 400.0);
    std::uniform_real_distribution<double> sample(0, 1);
    for (uint64_t frame = 0; frame < 9000; ++frame) {
        sourceUs += 8600.0 * (1 + ppm * 1e-6);
        rtp += 774;
        const bool early = sparseEarly && sample(rng) < 0.01;
        const uint64_t ready = early ? uint64_t(sourceUs) + 300 :
            uint64_t(sourceUs) + (sparseEarly ? 1800 : 800) + uint64_t(jitter(rng)) +
            (sample(rng) < 0.003 ? 4000 : 0);
        PacedFrame input(nullptr, int(frame), rtp, true, ready);
        input.setDeliveryTimeline(ready - 800, ready - 600, ready - 500);
        const auto decision = controller.schedule(input, std::max(ready, submitted));
        const uint64_t prepared = std::max(ready, decision.renderStartUs) + 500;
        submitted = std::max(prepared, decision.targetUs);
        controller.notePreparationDuration(500, 0, prepared);
        controller.noteSchedulerDelays(0, 0, true);
        controller.noteSubmission(true, false, submitted, submitted);
        if (frame >= 3500) {
            const auto buffer = controller.intervalStats().inputPercentile.currentUs;
            sum += buffer; maximum = std::max(maximum, buffer); ++counted;
        }
    }
    const auto stats = controller.intervalStats().inputPercentile;
    return {sum / counted, stats.missed, stats.eligible, maximum};
}

void testClockDrift()
{
    for (bool sparse : {false, true}) {
        const auto steady = runDrift(0, sparse);
        for (double ppm : {-50.0, 50.0}) {
            const auto drift = runDrift(ppm, sparse);
            std::printf("clock drift %+.0f ppm sparse=%d: mean %.0f vs %.0f us, misses %llu/%llu\n",
                        ppm, sparse, drift.meanBufferUs, steady.meanBufferUs,
                        static_cast<unsigned long long>(drift.missed),
                        static_cast<unsigned long long>(drift.eligible));
            check(drift.eligible > 5000 && drift.missed <= drift.eligible,
                  "drift keeps real input observations and valid coverage accounting");
            check(drift.meanBufferUs <= steady.meanBufferUs * 1.5 + 2500,
                  "clock drift does not ratchet standing input protection");
            check(drift.maximumUs <= 10000,
                  "clock drift remains inside the configured source-relative buffer capacity");
        }
    }
}

void testRecoveryCredit()
{
    VrrSessionConfig config;
    config.streamRateHz = 76; config.displayRefreshHz = 120;
    config.timingOptions = {4000, 9995, 300, 500};
    config = vrrProductionSessionConfig(config);
    for (const auto& [target, share] : {std::pair<int, uint64_t>{9900, 1000},
                                         {9950, 500}, {9995, 50}, {9000, 1000}}) {
        auto selected = config;
        selected.timingOptions.targetHundredths = target;
        const auto policy = vrrTimingParametersForSession(selected);
        VrrTimingController controller(selected, true, policy);
        const auto physical = controller.recoveryHeadroomUs();
        const auto expected = physical * share / 1000;
        const auto actual = controller.inputRecoveryCreditUs();
        check(policy.playoutInputRecoveryCreditRoomPerMille == share &&
              actual <= physical && actual + 1 >= expected && actual <= expected + 1,
              "input recovery credit follows the selected goal and never exceeds physical room");
    }
    VrrSessionConfig nearCeiling = config;
    nearCeiling.streamRateHz = 116;
    VrrTimingController controller(nearCeiling, true, vrrTimingParametersForSession(nearCeiling));
    check(controller.inputRecoveryCreditUs() <= controller.recoveryHeadroomUs() &&
          controller.inputRecoveryCreditUs() < 500,
          "near the display ceiling recovery credit remains physically bounded");
}

void testGraphAccounting()
{
    Overlay::TimingGraphPoint point;
    point.sourceTimingValid = true; point.sourceTimeUs = 100000;
    point.bufferUs = 1000; point.inputPercentileEnabled = true;
    point.producerEligible = true; point.inputToleranceUs = 500;
    point.inputVrrHeadroomUs = 500; point.producerRequiredUs = -1000;
    const auto sample = Overlay::bufferGraphSample(point);
    check(sample.producerUs == -1500 && !sample.producerMissed() && sample.bufferUs == 1000,
          "signed readiness remains separate from applied input buffering");
}
}

int main()
{
    testRecoveryCredit();
    testGraphAccounting();
    testClockDrift();
    std::printf("current input accounting: %d failures\n", failures);
    return failures ? 1 : 0;
}
