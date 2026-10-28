#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/sourcegapsmoothing.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void expect(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

void testBoundaries()
{
    using Vrr13::sourceGapSmoothingUs;
    // The user's decimal example, without RTP quantization: 8.9 -> 8.83 ms.
    expect(sourceGapSmoothingUs(8900, 8330, 500, 0) == -70,
           "8.9 ms versus 8.33 ms must correct only 0.07 ms beyond tolerance");
    expect(8900 + sourceGapSmoothingUs(8900, 8330, 500, 0) == 8830,
           "the user's gap must land on 8.83 ms, not on the ideal 8.33 ms");
    expect(sourceGapSmoothingUs(11478, 11499, 500, -490) == -490,
           "recorded frame 2719's 21 us blip must keep its prior phase without a new correction");
    for (uint64_t t : {250ULL, 500ULL, 750ULL, 1000ULL, 2000ULL}) {
        for (int direction : {-1, 1}) {
            const auto gap = [direction](int64_t error) { return uint64_t(10000 + direction * error); };
            expect(sourceGapSmoothingUs(gap(t - 1), 10000, t, 0) == 0,
                   "below T must remain untouched in both directions");
            expect(sourceGapSmoothingUs(gap(t), 10000, t, 0) == 0,
                   "exactly T must remain untouched in both directions");
            expect(sourceGapSmoothingUs(gap(t + 1), 10000, t, 0) == -direction,
                   "one microsecond beyond T must correct only one microsecond");
            expect(sourceGapSmoothingUs(gap(2 * t), 10000, t, 0) == -direction * int64_t(t),
                   "exactly 2T may use the whole additional T");
            const auto capped = sourceGapSmoothingUs(gap(3 * t), 10000, t, 0);
            expect(capped == -direction * int64_t(t), "beyond 2T must never buy more than T correction");
            expect(std::abs(int64_t(gap(3 * t)) + capped - 10000) == 2 * int64_t(t),
                   "an oversized source hitch must retain the uncorrected excess");
        }
    }
    expect(sourceGapSmoothingUs(8900, 8330, 0, 123) == 0,
           "a zero allowance cannot carry smoothing");
}

void testConsecutiveGaps()
{
    using Vrr13::sourceGapSmoothingUs;
    int64_t phase = 0;
    // Repeated 600 us misses spend 100 us each, then stop at the shared 500 us cap.
    for (int i = 0; i < 1000; ++i) {
        const auto next = sourceGapSmoothingUs(10600, 10000, 500, phase);
        expect(next == -std::min<int64_t>(500, (i + 1) * 100),
               "consecutive excess corrections must saturate rather than accumulate without bound");
        expect(std::abs(next - phase) <= 500, "every interval correction must fit the additional T");
        phase = next;
    }
    for (uint64_t cleanGap : {9500ULL, 9750ULL, 10000ULL, 10250ULL, 10500ULL}) {
        const auto next = sourceGapSmoothingUs(cleanGap, 10000, 500, phase);
        expect(next == phase, "clean gaps must retain phase so their actual spacing stays unchanged");
    }
    for (int i = 0; i < 1000; ++i) {
        const auto next = sourceGapSmoothingUs(i % 2 ? 8500 : 11500, 10000, 500, phase);
        expect(std::abs(next) <= 500 && std::abs(next - phase) <= 500,
               "alternating over-limit gaps must obey both phase and interval bounds");
        phase = next;
    }
}

// Check the resolved production paths rather than retired smoothing revisions.
void testProductionSelection()
{
    for (bool native : {false, true}) for (int tolerance : {250, 500, 1000, 2000}) {
        VrrSessionConfig session;
        session.streamRateHz = 120;
        session.displayRefreshHz = 240;
        session.smoothFrameTiming = true;
        session.timingOptions.toleranceUs = tolerance;
        session = vrrProductionSessionConfig(session);
        const auto ordinary = vrrTimingParametersForSession(session, native, native);
        expect(ordinary.playoutIntervalToleranceUs == uint64_t(tolerance), "production uses the selected tolerance");
        expect(ordinary.smoothingMode == (native ? 4u : 2u),
               "production selects the native or worker smoothing path");
        expect(ordinary.playoutSmoothingOwnMovement == (native ? 0u : 1u),
               "worker smoothing owns its movement and native scheduling keeps its gate");
        session.absorbMicrostutter = true;
        const auto absorb = vrrTimingParametersForSession(session, native, native);
        expect(absorb.smoothingMode == 7 &&
               absorb.playoutSmoothingMaxLagUs == uint64_t(2 * tolerance) &&
               absorb.playoutSmoothingAbsorbReserveUs == uint64_t(2 * tolerance) &&
               absorb.playoutSmoothingReserveMaxUs == 0 &&
               absorb.playoutSmoothingOwnMovement == (native ? 0u : 1u),
               "Absorb uses its fixed 2T pad and the presenter's movement ownership");
        session.smoothFrameTiming = false;
        const auto disabled = vrrTimingParametersForSession(session, native, native);
        expect(disabled.playoutSmoothingGainPerMille == 0 &&
               disabled.playoutSmoothingAbsorbReserveUs == 0,
               "disabling smooth timing also disables Absorb");
    }
}

void testAbsorbCurrentController()
{
    for (bool native : {false, true}) for (int tolerance : {250, 500, 1000}) {
        VrrSessionConfig session;
        session.streamRateHz = 120;
        session.displayRefreshHz = 240;
        session.smoothFrameTiming = true;
        session.absorbMicrostutter = true;
        session.timingOptions = {4000, 9950, 120, tolerance};
        session = vrrProductionSessionConfig(session);
        auto policy = vrrTimingParametersForSession(session, native, native);
        policy.playoutDelayStartSeedUs = 4000;
        VrrTimingController controller(session, true, policy);
        uint32_t stamp = 0;
        uint64_t previousAt = 0, previousBuffer = 0, submitted = 0;
        for (int frameNumber = 0; frameNumber < 700; ++frameNumber) {
            stamp += frameNumber >= 150 && frameNumber < 400 ? (frameNumber % 2 ? 990 : 810) : 900;
            const uint64_t source = 1000000 + uint64_t(stamp) * 100 / 9;
            PacedFrame frame(nullptr, frameNumber, stamp, true, source + 1000);
            frame.setDeliveryTimeline(source + 200, source + 400, source + 500);
            const uint64_t now = std::max(source + 1000, submitted);
            const auto decision = controller.schedule(frame, now);
            expect(controller.smoothingReserveUs() == uint64_t(tolerance * 2),
                   "Absorb keeps the configured pad during source jitter");
            expect(decision.cadenceSmoothingUs >= 0 && decision.cadenceSmoothingUs <= int64_t(tolerance * 2),
                   "Absorb phase remains within its configured pad");
            expect(decision.targetUs >= frame.decoderOutputUs(), "the target respects decoder readiness");
            if (previousAt) {
                const uint64_t difference = std::max(decision.playoutDelayUs, previousBuffer) -
                    std::min(decision.playoutDelayUs, previousBuffer);
                const uint64_t rate = decision.playoutDelayUs > previousBuffer ? 1000 : 5000;
                expect(difference <= rate * (now - previousAt) / 1000000 + 2,
                       "input-buffer rise and release retain their own rate bounds");
            }
            previousAt = now;
            previousBuffer = decision.playoutDelayUs;
            submitted = std::max(decision.targetUs, now + 500);
            controller.notePreparationDuration(500, 0, now + 500);
            controller.noteSchedulerDelays(0, 0, true);
            controller.noteSubmission(true, false, submitted, submitted);
        }
    }
}
} // namespace

int main()
{
    testBoundaries();
    testConsecutiveGaps();
    testProductionSelection();
    testAbsorbCurrentController();
    if (!failures) std::puts("Current source-gap smoothing tests passed");
    return failures ? 1 : 0;
}
