#include "../../app/streaming/video/incomingframetiming.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {
int failures = 0;
using Sample = IncomingFrameTiming::Sample;

void check(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

void expect(Sample actual, uint32_t change, uint32_t reference, const char* description)
{
    check(actual.changeTicks == change && actual.referenceTicks == reference, description);
}

struct Totals {
    uint64_t change = 0;
    uint64_t reference = 0;
    void add(Sample sample) { change += sample.changeTicks; reference += sample.referenceTicks; }
    double score() const { return IncomingFrameTiming::smoothnessPercent(change, reference); }
};

Totals measure(const std::vector<uint32_t>& intervals)
{
    IncomingFrameTiming timing;
    Totals total;
    uint32_t timestamp = 0;
    uint32_t frame = 0;
    timing.observe(frame++, timestamp);
    for (const auto interval : intervals) {
        timestamp += interval;
        total.add(timing.observe(frame++, timestamp));
    }
    return total;
}

void expectScore(const std::vector<uint32_t>& intervals, double expected, const char* description)
{
    check(std::abs(measure(intervals).score() - expected) < 0.000001, description);
}

void steadyCadence(uint32_t ticks)
{
    IncomingFrameTiming timing;
    for (uint32_t i = 0; i < 300; ++i) {
        expect(timing.observe(i, i * ticks), 0, i < 2 ? 0 : ticks,
               "stable cadence must be smooth regardless of frame rate");
    }
    expectScore(std::vector<uint32_t>(300, ticks), 100.0, "steady cadence scores 100 percent");
}
}

int main()
{
    steadyCadence(750);  // 120 FPS
    steadyCadence(1500); // 60 FPS
    steadyCadence(3000); // 30 FPS cutscene
    steadyCadence(3003); // 29.97 FPS
    expectScore({720, 810, 720, 810}, 100.0 * 8 / 9, "8/9 ms variation counts below the former threshold");
    expectScore({720, 1080, 720, 1080}, 100.0 * 2 / 3, "8/12 ms variation scores 66.67 percent");
    expectScore({720, 1440, 720, 1440}, 50.0, "8/16 ms variation scores 50 percent");
    expectScore({1440, 2880, 1440, 2880}, 50.0, "relative cadence disturbance is independent of frame rate");
    check(measure({750, 751, 750}).score() < 100.0, "even one RTP tick of variation contributes");
    check(measure({750, 1019, 750}).score() > measure({750, 1020, 750}).score() &&
          measure({750, 1020, 750}).score() > measure({750, 1021, 750}).score(),
          "3 ms is no longer a scoring boundary");

    double previousScore = 100.0;
    for (const uint32_t amplitude : {1U, 45U, 90U, 270U, 450U, 800U}) {
        const double score = measure({900 - amplitude, 900 + amplitude, 900 - amplitude, 900 + amplitude}).score();
        check(score < previousScore && score >= 0.0, "larger disturbances at the same average FPS lower the score");
        previousScore = score;
    }
    previousScore = 100.0;
    for (const int count : {1, 2, 4, 8}) {
        std::vector<uint32_t> intervals(40, 750);
        for (int i = 0; i < count; ++i) { intervals[2 + i * 4] = 1020; }
        const double score = measure(intervals).score();
        check(score < previousScore, "more frequent disturbances lower the score");
        previousScore = score;
    }
    check(measure({750, 9000, 750}).score() > measure({750, 18000, 750}).score(),
          "200 ms source stall penalizes more than 100 ms without clipping");

    IncomingFrameTiming windows;
    windows.observe(1, 0);
    windows.observe(2, 720);
    Totals first;
    first.add(windows.observe(3, 1440)); // 8/8 ms
    Totals second;
    second.add(windows.observe(4, 2880)); // 8/16 ms, across the window boundary
    check(std::abs(IncomingFrameTiming::smoothnessPercent(first.change + second.change,
              first.reference + second.reference) - 100.0 * 2 / 3) < 0.000001,
          "window sums merge before division, rather than averaging percentages");

    IncomingFrameTiming loss;
    loss.observe(1, 0);
    loss.observe(2, 750);
    expect(loss.observe(4, 2250), 0, 0, "missing frame cannot be attributed to host jitter");
    expect(loss.observe(5, 3000), 0, 0, "loss breaks both interval comparisons");
    expect(loss.observe(6, 3750), 0, 750, "three consecutive frames restore coverage");
    expect(loss.observe(6, 3750), 0, 0, "duplicate frame breaks sequence");
    expect(loss.observe(5, 3000), 0, 0, "out-of-order frame breaks sequence");

    IncomingFrameTiming wrap;
    wrap.observe(UINT32_MAX - 1, UINT32_MAX - 1499);
    wrap.observe(UINT32_MAX, UINT32_MAX - 749);
    expect(wrap.observe(0, 0), 0, 750, "RTP and frame identity wrap, including timestamp zero");
    expect(wrap.observe(1, 750), 0, 750, "cadence continues across wrap");
    expect(wrap.observe(2, 700), 0, 0, "backwards RTP timestamp breaks sequence");
    expect(wrap.observe(3, 1450), 0, 0, "timestamp reset needs fresh intervals");
    expect(wrap.observe(4, 2200), 0, 750, "valid timestamps restore coverage");

    IncomingFrameTiming absent;
    for (uint32_t i = 0; i < 20; ++i) {
        expect(absent.observe(i, 0), 0, 0, "missing timing has no denominator and must display N/A");
    }
    return failures == 0 ? 0 : 1;
}
