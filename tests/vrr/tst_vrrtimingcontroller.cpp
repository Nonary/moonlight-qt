#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

VrrSessionConfig config(bool lowLatency)
{
    VrrSessionConfig value;
    value.displayRefreshHz = 120;
    value.streamRateHz = 100;
    value.lowLatency = lowLatency;
    return value;
}

PacedFrame frame(int number, uint32_t timestamp, uint64_t decodedUs)
{
    return PacedFrame(nullptr, number, timestamp, true, decodedUs);
}

void testLowLatencyUsesArrivalDeadline()
{
    VrrTimingController controller(config(true), true);
    VrrTimingDecision decision = controller.schedule(
        frame(1, 0, 100000), 100000);

    expect(!decision.latchedPresentation,
           "low-latency VRR must request adaptive presentation");

    controller.noteSubmission(true, false, decision.targetUs);
    decision = controller.schedule(frame(2, 900, 105000), 105000);

    expect(decision.targetUs < 111000,
           "low-latency VRR must not wait for the projected sender clock");
    expect(!decision.latchedPresentation,
           "arrival-driven presentation must not select fixed-vsync latching");
}

void testLowLatencyAbsorbsLateDecodeWithDisplayFloor()
{
    VrrTimingController controller(config(true), true);
    VrrTimingDecision decision = controller.schedule(
        frame(0, 0, 100000), 100000);
    controller.noteSubmission(true, false, decision.targetUs);

    decision = controller.schedule(frame(1, 900, 114000), 114000);
    controller.noteSubmission(true, false, decision.targetUs);

    decision = controller.schedule(frame(2, 1800, 120000), 120000);
    expect(decision.targetUs >= 124300 && decision.targetUs < 124500,
           "low-latency VRR must keep a post-jitter burst inside variable blank while catching up");
}

void trainArrivalModel(VrrTimingController& controller)
{
    constexpr uint64_t epochUs = 100000;
    for (int i = 0; i < 64; ++i) {
        const uint64_t sourceUs = epochUs +
            static_cast<uint64_t>(i) * 10000;
        const uint64_t jitterUs = i % 5 == 4 ? 4000 : 0;
        VrrTimingDecision decision = controller.schedule(
            frame(i, static_cast<uint32_t>(i * 900), sourceUs + jitterUs),
            sourceUs + jitterUs);
        controller.notePreparationDuration(1000);
        controller.noteSubmission(true, false, decision.targetUs);
    }
}

void testLowLatencyDoesNotRetainArrivalTail()
{
    VrrTimingController lowLatency(config(true), true);
    VrrTimingController smooth(config(false), true);
    trainArrivalModel(lowLatency);
    trainArrivalModel(smooth);

    const VrrTimingDecision lowLatencyDecision = lowLatency.schedule(
        frame(64, 57600, 740000), 740000);
    const VrrTimingDecision smoothDecision = smooth.schedule(
        frame(64, 57600, 740000), 740000);

    expect(lowLatencyDecision.targetUs + 3000 < smoothDecision.targetUs,
           "low-latency VRR must not turn an absorbed decode/network tail into standing latency");
    expect(!lowLatencyDecision.latchedPresentation,
           "low-latency VRR must stay adaptive after readiness training");
    expect(smoothDecision.latchedPresentation,
           "smooth 100 FPS at 120 Hz must retain the existing latch policy");
}

} // namespace

int main()
{
    testLowLatencyUsesArrivalDeadline();
    testLowLatencyAbsorbsLateDecodeWithDisplayFloor();
    testLowLatencyDoesNotRetainArrivalTail();
    return failures == 0 ? 0 : 1;
}
