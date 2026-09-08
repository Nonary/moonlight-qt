#include "../../app/streaming/video/incomingframetiming.h"

#include <cstdio>
#include <cstdint>

namespace {
int failures = 0;
using Sample = IncomingFrameTiming::Sample;

void expect(Sample actual, Sample expected, const char* description)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

void steadyCadence(uint32_t ticks)
{
    IncomingFrameTiming timing;
    for (uint32_t i = 0; i < 300; ++i) {
        expect(timing.observe(i, i * ticks), i < 2 ? Sample::Unavailable : Sample::Smooth,
               "stable cadence must be smooth regardless of frame rate");
    }
}

void threshold(uint32_t change, Sample expected)
{
    IncomingFrameTiming timing;
    timing.observe(1, 0);
    timing.observe(2, 750);
    expect(timing.observe(3, 1500 + change), expected, "strict 3 ms stretch threshold");
    expect(timing.observe(4, 2250 + change), expected, "strict 3 ms catch-up threshold");
}
}

int main()
{
    steadyCadence(750);  // 120 FPS
    steadyCadence(1500); // 60 FPS
    steadyCadence(3000); // 30 FPS cutscene
    steadyCadence(3003); // 29.97 FPS
    threshold(269, Sample::Smooth);
    threshold(270, Sample::Smooth);
    threshold(271, Sample::Uneven);

    IncomingFrameTiming stall;
    stall.observe(1, 0);
    stall.observe(2, 750);
    expect(stall.observe(3, 9750), Sample::Uneven, "100 ms host stall must count");
    expect(stall.observe(4, 10500), Sample::Uneven, "host stall recovery must count");
    expect(stall.observe(5, 11250), Sample::Smooth, "steady cadence recovers");

    IncomingFrameTiming transition;
    transition.observe(1, 0);
    transition.observe(2, 750);
    expect(transition.observe(3, 3750), Sample::Uneven, "120 to 30 FPS transition changes cadence");
    expect(transition.observe(4, 6750), Sample::Smooth, "steady 30 FPS after transition is smooth");

    IncomingFrameTiming loss;
    loss.observe(1, 0);
    loss.observe(2, 750);
    expect(loss.observe(4, 2250), Sample::Unavailable, "missing frame cannot be attributed to host jitter");
    expect(loss.observe(5, 3000), Sample::Unavailable, "loss breaks both interval comparisons");
    expect(loss.observe(6, 3750), Sample::Smooth, "three consecutive frames restore coverage");
    expect(loss.observe(6, 3750), Sample::Unavailable, "duplicate frame breaks sequence");
    expect(loss.observe(5, 3000), Sample::Unavailable, "out-of-order frame breaks sequence");

    IncomingFrameTiming wrap;
    wrap.observe(UINT32_MAX - 1, UINT32_MAX - 1499);
    wrap.observe(UINT32_MAX, UINT32_MAX - 749);
    expect(wrap.observe(0, 0), Sample::Smooth, "RTP and frame identity wrap, including timestamp zero");
    expect(wrap.observe(1, 750), Sample::Smooth, "cadence continues across wrap");
    expect(wrap.observe(2, 700), Sample::Unavailable, "backwards RTP timestamp breaks sequence");
    expect(wrap.observe(3, 1450), Sample::Unavailable, "timestamp reset needs fresh intervals");
    expect(wrap.observe(4, 2200), Sample::Smooth, "valid timestamps restore coverage");

    IncomingFrameTiming absent;
    for (uint32_t i = 0; i < 20; ++i) {
        expect(absent.observe(i, 0), Sample::Unavailable, "missing host timing must not report 100 percent");
    }
    return failures == 0 ? 0 : 1;
}
