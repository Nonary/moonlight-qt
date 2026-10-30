#include "assertions.h"
#include "../../app/streaming/video/performancestats.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/cadencedetection.h"
#include <algorithm>
#include <iostream>

using PerformanceStats::Snapshot;
using PerformanceStats::formatVrr;
static bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

int main(int argc, char** argv)
{
    Snapshot s;
    s.vrr = s.adaptive = true;
    s.bufferUs = 5700;
    s.targetPpm = 999500; s.toleranceUs = 500;
    s.input.samples = s.input.recentEligible = 22000;
    s.input.recentMissed = 11; s.input.recentAbsorbed = 200;
    s.input.windowUs = 300000000; s.input.targetUs = 5420;
    s.input.tailUncertain = false;
    s.present.intervals = 3300; s.present.misses = 33; s.present.hitches = 1; s.present.worstAddedUs = 4200;
    s.present.lastObservedUs = s.nowUs = 10000000;
    auto text = formatVrr(s);
    // Only the CPU interval score shares a line with the profile timing goal.
    assert(has(text, "VRR buffer: 5.70 ms (target 5.42) | Client timing (CPU): collecting / 99.95% target\nInput coverage: 99.95%\n"));
    assert(has(text, "Present timing issues (30s): 1.00% >0.50 ms | hitches 1 | worst +4.2 ms\n"));
    assert(has(text, "Extra delay: 0.00 ms (smoothing/recovery) | Planned total: 5.70 ms\n"));
    assert(std::count(text.begin(), text.end(), '\n') == 4);
    if (argc > 1 && std::string(argv[1]) == "--fixture") { std::cout << text; return 0; }

    // A short reporting window responds independently of the profile history.
    s.input.reportingWindowUs=10000000;s.input.reportingEligible=1000;
    s.input.reportingMissed=0;
    assert(has(formatVrr(s), "Input coverage (10s): 100.00% | 300s 99.95%"));
    s.input.reportingMissed=2;
    assert(has(formatVrr(s), "Input coverage (10s): 99.80% | 300s 99.95%"));
    s.input.reportingEligible=0;
    assert(has(formatVrr(s), "Input coverage (10s): N/A | 300s 99.95%"));
    s.input.reportingWindowUs=0;

    // A legitimate measured zero differs from missing or stale feedback.
    s.present.misses = 0;
    assert(has(formatVrr(s), "Present timing issues (30s): 0.00% >0.50 ms"));
    s.nowUs += 1000001;
    assert(has(formatVrr(s), "Present timing issues (30s): N/A (no recent display feedback)"));
    s.present.lastPausedUs = s.nowUs;
    assert(has(formatVrr(s), "Present timing issues (30s): paused below VRR range"));
    s.input.tailUncertain = true;
    assert(has(formatVrr(s), "VRR buffer: 5.70 ms (target 5.42 learning) | "));
    // Smoothing can vary while the graph's input buffer stays fixed. Keep
    // the leading buffer value stable, with the extra delay and total explicit.
    s.smoothingUs = 1250;
    assert(has(formatVrr(s), "VRR buffer: 5.70 ms (target 5.42 learning) | "));
    assert(has(formatVrr(s), "Extra delay: 1.25 ms (smoothing/recovery) | Planned total: 6.95 ms\n"));
    s.bufferUs = 3000;
    for (const uint64_t smoothing : {0, 3000, 0, 3000}) {
        s.smoothingUs = smoothing;
        const auto value = formatVrr(s);
        assert(has(value, "VRR buffer: 3.00 ms ("));
        assert(has(value, smoothing ? "Extra delay: 3.00 ms (smoothing/recovery) | Planned total: 6.00 ms" : "Extra delay: 0.00 ms (smoothing/recovery) | Planned total: 3.00 ms"));
    }
    s.bufferUs = 5700;
    s.smoothingUs = 0;
    s.input.recentEligible = 0;
    assert(has(formatVrr(s), "Input coverage: N/A\n"));
    s.atLimit = true;
    assert(has(formatVrr(s), "VRR buffer: 5.70 ms at limit (target 5.42 learning) | "));
    // The CPU interval score remains visible without adaptive input coverage.
    s.adaptive = false; s.atLimit = false;
    assert(has(formatVrr(s), "VRR buffer: 5.70 ms | Client timing (CPU): collecting / 99.95% target\n"));
    s.clientTimingValid = true; s.clientTimingPercent = 99.53;
    assert(has(formatVrr(s), "VRR buffer: 5.70 ms | Client timing (CPU): 99.53% / 99.95% target\n"));
    assert(!has(formatVrr(s), "Input coverage"));
    const auto nonadaptive = formatVrr(s);
    assert(std::count(nonadaptive.begin(), nonadaptive.end(), '\n') == 3);
    // Completed live run: binary coverage and weighted timing legitimately diverge.
    s.adaptive = true; s.targetPpm = 990000;
    s.clientTimingPercent = 98.73;
    s.input.recentEligible = 6960; s.input.recentMissed = 284;
    const auto divergent = formatVrr(s);
    assert(has(divergent, "Client timing (CPU): 98.73% / 99.00% target\n"));
    assert(has(divergent, "Input coverage: 95.92%\n"));
    assert(!has(divergent, "95.92% / 99.00%"));
    s.vrr = false;
    assert(formatVrr(s) == "VRR pacing: inactive\n");

    // Host timestamp jitter is not a display error. Frames submitted and shown
    // exactly on plan while source timestamps alternate by 0.8 ms: the old
    // source-relative display line flagged every interval.
    for (const bool displayAdded : {false, true}) {
        Vrr13::CadenceDetection sourceRelative;
        Vrr13::PresentTiming present;
        const Vrr13::PresentTiming::Limits limits{500, 8333, 9091, 0};
        for (uint64_t i = 1; i <= 200; ++i) {
            const uint64_t plan = 1000000 + i * 9091;
            const uint64_t source = plan + (i % 2 ? 800 : 0);
            const uint64_t shown = plan + 4800 + (displayAdded && i == 100 ? 600 : 0);
            sourceRelative.observe({i, source, shown, shown + 1000, 0, true}, 500);
            present.observe({i, plan - 200, shown, plan, 0, shown + 1000}, limits);
        }
        assert(sourceRelative.stats().deviations == sourceRelative.stats().intervals);
        assert(present.stats().intervals == 199);
        // Display-added error beyond tolerance still counts, on both sides.
        assert(present.stats().misses == (displayAdded ? 2u : 0u));
        s.vrr = s.adaptive = true;
        s.present = present.stats(); s.nowUs = s.present.lastObservedUs;
        assert(has(formatVrr(s), displayAdded ? "Present timing issues (30s): 1.01% >0.50 ms" :
                                                "Present timing issues (30s): 0.00% >0.50 ms"));
    }

    // Exact 500 us is accepted; only strictly larger certain residuals count.
    for (uint64_t error : {499, 500, 501}) {
        Vrr13::CadenceDetection cadence;
        cadence.observe({1, 10000, 1000000, 1100000, 0, true}, 500);
        cadence.observe({2, 20000, 1010000 + error, 1200000, 0, true}, 500);
        assert(cadence.stats().intervals == 1);
        assert(cadence.stats().deviations == (error > 500 ? 1 : 0));
        assert(cadence.stats().uncertain == 0);
    }
    // A qualified input hitch crosses B+T strictly; recovery credit does not
    // erase an interval hitch that was actually observed.
    for (uint64_t room : {0, 760}) for (uint64_t error : {499, 500, 501}) {
        Vrr13::InputGoalBuffer input;
        input.reset(5000);
        input.setGoalSolver(120000000, 700);
        input.observe(1, 1000000, 1000000, int64_t(5000 + error), true,
            0, 20000, 999500, 300000000, 4000, 1000, 0, 500, room, 0, error > 500);
        assert(input.stats().targetUs == 5000); // One observation is not a qualified pair.
        assert(input.stats().recentMissed == (error > 500 ? 1 : 0));
    }
    Vrr13::CadenceDetection cadence;
    cadence.observe({1, 10000, 1000000, 1100000, 1, true}, 500);
    cadence.observe({2, 20000, 1010501, 1200000, 1, true}, 500);
    assert(cadence.stats().deviations == 0 && cadence.stats().uncertain == 1);
    // Missing identity breaks adjacency and contributes no fake clean pair.
    cadence.observe({4, 40000, 1030000, 1300000, 0, true}, 500);
    assert(cadence.stats().intervals == 1);
    cadence.observe({5, 50000, 1040000, 1400000, 0, true}, 500);
    assert(cadence.stats().intervals == 2);
    // Rolling 2 s buckets expire old errors and their denominator together.
    cadence.observe({6, 60000, 1050000, 3500000, 0, true}, 500);
    assert(cadence.stats().intervals == 1 && cadence.stats().deviations == 0);
    std::cout << "VRR summary lines, plan-referenced present timing, N/A feedback, strict tolerance and matched-window denominator passed\n";
}
