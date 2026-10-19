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

// Recorded revision 5 (daily.188): source-gap phase correction with an
// optional standing reserve, on that release's input accounting. Production
// uses tolerance-bounded revision 4; these fixtures keep revision 5
// explicit so its recorded behavior stays covered.
VrrTimingParameters revision5Policy(const VrrSessionConfig& session, bool synchronized = false,
                                    bool scheduled = false)
{
    auto p = vrrTimingParametersForSession(session, synchronized, scheduled);
    // This recording predates stable adaptation. Do not inherit today's
    // rolling input-buffer policy into its historical reserve assertions.
    p.playoutInputStableAdaptation = 0;
    p.playoutInputHitchMissesOnly = 0;
    if (p.playoutInputPercentileEnabled) p.playoutInputBoundedRecoveryCredit = 1;
    p.playoutInputLayerSeedRatioPerMille = p.playoutInputLearningTrim = 0;
    p.playoutCatchupCarryLimitUs = p.playoutCatchupCarryLimitPeriodPerMille = 0;
    if (session.smoothFrameTiming) {
        p.playoutSmoothingWindowedCadence = 5;
        p.playoutSmoothingMaxLagUs = std::max<uint64_t>(p.playoutIntervalToleranceUs, 2000);
        p.playoutSmoothingMaxLagPeriodPerMille = 120;
        p.playoutSmoothingRecenterPerMille = 1000;
        if (session.absorbMicrostutter) {
            p.playoutSmoothingAbsorbReserveUs = 2000;
            p.playoutSmoothingAbsorbReservePeriodPerMille = 150;
            p.playoutSmoothingMaxLagUs = std::max<uint64_t>(p.playoutSmoothingMaxLagUs, 8000);
            p.playoutSmoothingMaxLagPeriodPerMille = 200;
        }
        p.playoutSmoothingReserveMaxUs = p.playoutSmoothingReserveToleranceUs = 0;
        p.playoutSmoothingReservePercentilePerMille = p.playoutSmoothingReserveReleaseUsPerSecond = 0;
        p.playoutSmoothingResetSlewUs = p.playoutSmoothingPeriodFeedbackPerMillion = 0;
    }
    return p;
}

// Recorded revision 4: T is both the dead zone and the shared phase limit.
VrrTimingParameters revision4Policy(const VrrSessionConfig& session)
{
    auto p = revision5Policy(session);
    if (session.smoothFrameTiming) {
        p.playoutSmoothingWindowedCadence = 4;
        p.playoutSmoothingMaxLagUs = p.playoutIntervalToleranceUs;
        p.playoutSmoothingRecenterPerMille = 0;
        p.playoutSmoothingAbsorbReserveUs = 0;
        p.playoutSmoothingAbsorbReservePeriodPerMille = 0;
        p.playoutSmoothingMaxLagPeriodPerMille = 0;
    }
    return p;
}

VrrTimingParameters fixedDelayPolicy(const VrrSessionConfig& session, bool production = true)
{
    auto p = production ? vrrTimingParametersForSession(session) : revision5Policy(session);
    // Isolate cadence retiming from adaptive delivery buffering and catch-up.
    // Readiness clipping and actual schedule() still run. These source-phase
    // fixtures predate the policy-9 shared applied-timing slew; keep policy 8
    // explicit here. testFixedPadAndSharedLiveSlew covers the new live policy.
    if (production && p.playoutInputHitchMissesOnly >= 9) {p.playoutInputHitchMissesOnly=8;p.playoutInputStableAdaptation=0;}
    p.playoutDelayAdaptive = 0;
    p.sourcePlayoutDelayUs = 10000;
    p.playoutDelayMinimumUs = 10000;
    p.playoutDelayMaximumUs = 10000;
    p.playoutDelayMaximumPeriodPerMille = 0;
    p.playoutDelayCapSourcePeriodPerMille = 0;
    p.latencyFixEnabled = 0;
    p.playoutCatchupPerMille = 0;
    p.playoutLateRecovery = 0;
    p.playoutOffsetSlewUsPerSecond = 0;
    return p;
}

void testControllerProvenance(bool sourceMisses, bool enabled, int tolerance, bool liveBuffer)
{
    VrrSessionConfig session;
    session.streamRateHz = 100;
    session.displayRefreshHz = 240;
    session.smoothFrameTiming = enabled;
    session.experimentalBoundedCatchup = liveBuffer;
    session.experimentalInputPercentilePpm = liveBuffer ? 995000 : 0;
    session.timingOptions = {4000, 9950, 120, tolerance};
    const auto p = fixedDelayPolicy(session);
    std::array<VrrTimingController, 3> controllers = {
        VrrTimingController(session, true, p), VrrTimingController(session, true, p),
        VrrTimingController(session, true, p)
    };
    uint32_t stamp = 0;
    std::array<int64_t, 3> previous{};
    unsigned changed = 0;
    for (int i = 0; i < 400; ++i) {
        // The timestamp sequence is identical for every client timeline.
        const uint32_t gapTicks = sourceMisses && i >= 120 && i < 180 ?
            (i % 2 ? 1035U : 765U) : 900U; // +/-1.5 ms source gaps.
        stamp += gapTicks;
        const uint64_t source = 1000000 + uint64_t(stamp) * 100 / 9;
        int64_t expected = 0;
        for (size_t j = 0; j < controllers.size(); ++j) {
            const uint64_t network = j == 1 ? uint64_t(i % 7) * 400 : 0;
            const uint64_t decode = j == 2 ? uint64_t(i % 11) * 350 : 100;
            const uint64_t output = source + network + decode;
            PacedFrame f(nullptr, i, stamp, true, output);
            f.setDeliveryTimeline(source + network, source + network, source + network);
            const auto d = controllers[j].schedule(f, output);
            if (j == 0) expected = d.cadenceSmoothingUs;
            expect(d.cadenceSmoothingUs == expected,
                   "identical source gaps must yield identical retiming with network/decode delays changed");
            expect(std::abs(d.cadenceSmoothingUs) <= tolerance,
                   "actual controller decisions must stay within the shared T phase cap");
            expect(std::abs(d.cadenceSmoothingUs - previous[j]) <= tolerance,
                   "actual consecutive decisions must not correct a source gap by more than T");
            if (!sourceMisses || !enabled)
                expect(d.cadenceSmoothingUs == 0, "delivery/decode variation and the off switch cannot activate smoothing");
            if (j == 0 && d.cadenceSmoothingUs != previous[j]) ++changed;
            if (i > 190)
                expect(d.cadenceSmoothingUs == previous[j], "clean source gaps after a miss must receive no further correction");
            previous[j] = d.cadenceSmoothingUs;
            expect(controllers[j].smoothingReserveUs() == 0, "bounded source correction must not acquire a standing reserve");
            controllers[j].notePreparationDuration(1000);
            controllers[j].noteSubmission(true, false, d.targetUs);
        }
    }
    if (sourceMisses && enabled && tolerance < 1500)
        expect(changed > 10, "genuine consecutive source-gap misses must activate bounded excess correction");
}

void testLateClientDoesNotActivate()
{
    VrrSessionConfig session;
    session.streamRateHz = 100;
    session.displayRefreshHz = 240;
    auto p = fixedDelayPolicy(session);
    VrrTimingController controller(session, true, p);
    for (int i = 0; i < 200; ++i) {
        const auto source = 1000000 + uint64_t(i) * 10000;
        const auto output = source + (i % 19 == 0 ? 20000 : 100);
        PacedFrame f(nullptr, i, uint32_t(i * 900), true, output);
        const auto d = controller.schedule(f, std::max(output, controller.lastSubmissionUs()));
        expect(d.cadenceSmoothingUs == 0, "even client delays beyond 2T must not activate source-gap smoothing");
        controller.noteSubmission(true, false, d.targetUs);
    }
}

void testRtpBoundaryRounding()
{
    // 120 Hz is 8333.333 us; an exact 500 us deviation is 45 RTP ticks.
    // Vary the timestamp phase to exercise all carried remainder positions.
    for (uint32_t seed : {0U, 1U, 2U}) for (int direction : {-1, 1}) {
        for (int deltaTicks : {44, 45, 46, 90, 135}) {
            VrrSessionConfig session;
            session.streamRateHz = 120;
            session.displayRefreshHz = 240;
            auto p = fixedDelayPolicy(session);
            // Freeze the existing nominal reference just for the boundary
            // fixture; rate-fit behavior remains covered by controller tests.
            p.minimumCadenceSamples = 10000;
            VrrTimingController controller(session, true, p);
            uint32_t stamp = seed;
            uint64_t previousTarget = 0, previousSource = 0;
            for (int i = 0; i < 66; ++i) {
                stamp += i == 64 ? uint32_t(750 + direction * deltaTicks) : 750U;
                const auto at = 1000000 + uint64_t(stamp) * 100 / 9;
                PacedFrame f(nullptr, i, stamp, true, at);
                const auto d = controller.schedule(f, at);
                const int64_t correction = deltaTicks <= 45 ? 0 : deltaTicks == 46 ? 11 : 500;
                if (i >= 64)
                    expect(d.cadenceSmoothingUs == -direction * correction,
                           "RTP rounding must preserve T, excess-only correction and the next clean gap");
                if (i == 64)
                    expect(int64_t(d.originalTargetUs - previousTarget) ==
                               int64_t(d.sourceTimeUs - previousSource) - direction * correction,
                           "the actual target gap must correct only the source excess beyond T");
                if (i == 65)
                    expect(d.originalTargetUs - previousTarget == d.sourceTimeUs - previousSource,
                           "the actual next clean target gap must stay unchanged after a correction");
                previousTarget = d.originalTargetUs;
                previousSource = d.sourceTimeUs;
                controller.noteSubmission(true, false, d.targetUs);
            }
        }
    }
}

void testReadinessCannotReapplyCorrectionOnCleanGaps()
{
    for (bool live : {false, true}) {
        VrrSessionConfig session;
        session.streamRateHz = 100;
        session.displayRefreshHz = 240;
        session.experimentalBoundedCatchup = live;
        session.experimentalInputPercentilePpm = live ? 995000 : 0;
        auto p = fixedDelayPolicy(session);
        p.minimumCadenceSamples = 10000;
        VrrTimingController controller(session, true, p);
        uint32_t stamp = 0;
        uint64_t previousTarget = 0, previousSource = 0;
        for (int i = 0; i < 100; ++i) {
            stamp += i == 64 ? 1035U : 900U;
            const auto source = 1000000 + uint64_t(stamp) * 100 / 9;
            const auto delay = i >= 65 && i % 3 ? (i % 3 == 1 ? 9900 : 11000) : 100;
            PacedFrame f(nullptr, i, stamp, true, source + delay);
            f.setDeliveryTimeline(source, source, source);
            const auto d = controller.schedule(f, std::max(f.decoderOutputUs(), controller.lastSubmissionUs()));
            if (i >= 64)
                expect(d.cadenceSmoothingUs == -500,
                       "late readiness must not remove and reapply retained correction on clean source gaps");
            if (i >= 65)
                expect(d.originalTargetUs - previousTarget == d.sourceTimeUs - previousSource,
                       "a clean source gap must not acquire a new intended blip when readiness changes");
            expect(d.targetUs >= f.decoderOutputUs(),
                   "source-only smoothing must retain the independent actual readiness floor");
            previousTarget = d.originalTargetUs;
            previousSource = d.sourceTimeUs;
            controller.notePreparationDuration(1000);
            controller.noteSubmission(true, false, d.targetUs);
        }
    }
}

void testPolicyAndInvalidTimestamps()
{
    for (int mode : {0, 1, 2}) for (int custom : {0, 250, 500, 750, 2000}) {
        VrrSessionConfig session;
        session.streamRateHz = 100;
        session.displayRefreshHz = 240;
        session.latencyMode = mode;
        session.timingOptions.toleranceUs = custom;
        const auto tolerance = session.timingOptions.resolved(mode).toleranceUs;
        for (bool native : {false, true}) {
            const auto live = vrrTimingParametersForSession(session, native, native);
            expect(live.playoutIntervalToleranceUs == uint64_t(tolerance) &&
                   live.playoutSmoothingWindowedCadence == 4 &&
                   live.playoutSmoothingMaxLagUs == uint64_t(tolerance) &&
                   live.playoutSmoothingRecenterPerMille == 0 &&
                   live.playoutSmoothingMaxLagPeriodPerMille == 0 &&
                   live.playoutSmoothingAbsorbReserveUs == 0 &&
                   live.playoutSmoothingReserveMaxUs == 0 &&
                   live.playoutSmoothingPeriodFeedbackPerMillion == 0 &&
                   live.playoutSmoothingResetSlewUs == 0,
                   "production Reduce judder must ignore errors within T and share one T phase budget on every presenter");
            const auto recorded = revision4Policy(session);
            expect(live.playoutSmoothingWindowedCadence == recorded.playoutSmoothingWindowedCadence &&
                   live.playoutSmoothingMaxLagUs == recorded.playoutSmoothingMaxLagUs,
                   "production must select the tested one-tolerance source-gap contract");
            const auto p = revision5Policy(session, native, native);
            expect(p.playoutIntervalToleranceUs == uint64_t(tolerance) &&
                   p.playoutSmoothingMaxLagUs == std::max<uint64_t>(tolerance, 2000) &&
                   p.playoutSmoothingWindowedCadence == 5 &&
                   p.playoutSmoothingMaxLagPeriodPerMille == 120 &&
                   p.playoutSmoothingRecenterPerMille == 1000 &&
                   p.playoutSmoothingAbsorbReserveUs == 0 &&
                   p.playoutSmoothingAbsorbReservePeriodPerMille == 0 &&
                   p.playoutSmoothingReserveMaxUs == 0 && p.playoutSmoothingResetSlewUs == 0,
                   "preset/custom T must apply equally on native and worker-paced sessions");
        }
        auto p = fixedDelayPolicy(session);
        VrrTimingController controller(session, true, p);
        for (int i = 0; i < 100; ++i) {
            PacedFrame f(nullptr, i, uint32_t(i * 900), false, 1000000 + i * 10000);
            const auto d = controller.schedule(f, f.decoderOutputUs());
            expect(d.cadenceSmoothingUs == 0, "missing RTP cannot supply an observed source-gap miss");
            controller.noteSubmission(true, false, d.targetUs);
        }
    }
}
void testRevision5Boundaries()
{
    using Vrr13::sourceGapPhaseUs;
    for (uint64_t t : {250ULL, 500ULL, 1000ULL, 2000ULL}) {
        for (int direction : {-1, 1}) {
            const auto gap = [direction](int64_t error) { return uint64_t(10000 + direction * error); };
            expect(sourceGapPhaseUs(gap(t), 10000, t, 4000, 125, 0) == 0,
                   "revision 5 must leave an error of exactly T untouched");
            for (int64_t error : {int64_t(t) + 1, 2 * int64_t(t), 3 * int64_t(t), int64_t(3000)}) {
                if (error <= int64_t(t)) continue;
                const int64_t phase = sourceGapPhaseUs(gap(error), 10000, t, 100000, 0, 0);
                expect(phase == -direction * (error - int64_t(t)),
                       "revision 5 must correct the whole excess beyond T, not at most T");
                expect(std::abs(int64_t(gap(error)) + phase - 10000) == int64_t(t),
                       "a corrected interval must land on T");
            }
            expect(sourceGapPhaseUs(gap(8000), 10000, t, 4000, 0, 0) == -direction * 4000,
                   "the retained phase must stop at its limit");
        }
    }
    expect(8900 + sourceGapPhaseUs(8900, 8330, 500, 2000, 0, 0) == 8830,
           "the user's 8.9 ms gap must still land on 8.83 ms");
    // One frame stamped 3 ms late, then back on its slot: both intervals end
    // within T instead of 12.5/9.5 ms under revision 4.
    const int64_t late = sourceGapPhaseUs(13000, 10000, 500, 4000, 125, 0);
    const int64_t back = sourceGapPhaseUs(7000, 10000, 500, 4000, 125, late);
    expect(late == -2500 && back == 0 && 13000 + late == 10500 && 7000 + back - late == 9500,
           "a displaced frame must be restored to within T on both sides");
    expect(13000 + Vrr13::sourceGapSmoothingUs(13000, 10000, 500, 0) == 12500,
           "revision 4 must keep its recorded bounded correction");
}

void testRevision5Recentering()
{
    using Vrr13::sourceGapPhaseUs;
    for (uint64_t t : {250ULL, 500ULL, 1000ULL}) for (int64_t start : {-4000, 4000}) {
        int64_t phase = start;
        for (int i = 0; i < 400; ++i) {
            const uint64_t gap = uint64_t(10000 + (i % 3 - 1) * int64_t(t) / 2);
            const int64_t next = sourceGapPhaseUs(gap, 10000, t, 4000, 125, phase);
            expect(std::abs(int64_t(gap) + next - phase - 10000) <= int64_t(t),
                   "recentring must keep every clean interval within T");
            expect(std::abs(next) <= std::abs(phase) && (next == 0 || (next > 0) == (phase > 0)),
                   "recentring must only move the phase toward zero");
            phase = next;
        }
        expect(phase == 0, "clean gaps must return the retained phase to zero");
    }
    expect(sourceGapPhaseUs(10200, 10000, 500, 4000, 0, -1500) == -1500,
           "without recentring a clean gap keeps its phase and its spacing");
    // An interval already at +T leaves no room to lengthen it further.
    expect(sourceGapPhaseUs(10500, 10000, 500, 4000, 1000, -1500) == -1500,
           "recentring must not use tolerance the interval has already spent");
}

void testRevision5PresentableFloor()
{
    using Vrr13::sourceGapPhaseUs;
    // 115 FPS on a 120 Hz panel: reference - T = 8.18 ms is below 8.333 ms.
    const uint64_t reference = 8680, t = 500, floor = 8333;
    expect(7140 + sourceGapPhaseUs(7140, reference, t, 4000, 1000, 0) == 8180,
           "without a floor a short gap lands on reference - T");
    expect(7140 + sourceGapPhaseUs(7140, reference, t, 4000, 1000, 0, floor) == int64_t(floor),
           "a short gap must land on the display period, not below it");
    expect(9680 + sourceGapPhaseUs(9680, reference, t, 4000, 1000, 0, floor) == 9180,
           "the floor must not change the upper edge");
    expect(sourceGapPhaseUs(8400, reference, t, 4000, 0, 0, floor) == 0,
           "a gap already between the floor and the reference stays untouched");
    expect(7000 + sourceGapPhaseUs(7000, 8000, t, 4000, 0, 0, floor) == 8000,
           "a floor above the reference is capped at the reference");
    int64_t phase = 2000;
    for (int i = 0; i < 64; ++i) {
        const int64_t next = sourceGapPhaseUs(reference, reference, t, 4000, 1000, phase, floor);
        expect(int64_t(reference) + next - phase >= int64_t(floor),
               "recentring must not shorten an interval below the floor");
        expect(next >= 0 && next <= phase, "recentring must move the phase toward zero");
        phase = next;
    }
    expect(phase == 0, "clean gaps must still return the phase to zero above the floor");
}

VrrSessionConfig absorbSession(bool smoothing, bool absorb)
{
    VrrSessionConfig session;
    session.streamRateHz = 120;
    session.displayRefreshHz = 240;
    session.smoothFrameTiming = smoothing;
    session.absorbMicrostutter = absorb;
    session.experimentalBoundedCatchup = true;
    session.experimentalInputPercentilePpm = 995000;
    return session;
}

void testAbsorbPolicySelection()
{
    for (bool native : {false, true}) {
        const auto liveRj = vrrTimingParametersForSession(absorbSession(true, false), native, native);
        const auto liveAbsorb = vrrTimingParametersForSession(absorbSession(true, true), native, native);
        expect(liveAbsorb.playoutSmoothingWindowedCadence == 7 &&
                   liveAbsorb.playoutSmoothingGainPerMille == liveRj.playoutSmoothingGainPerMille &&
                   liveAbsorb.playoutSmoothingMaxLagUs == 2 * liveAbsorb.playoutIntervalToleranceUs &&
                   liveAbsorb.playoutSmoothingReserveMaxUs == 0 &&
                   liveAbsorb.playoutSmoothingReservePercentilePerMille == 0 &&
                   liveAbsorb.playoutSmoothingAbsorbReserveUs == 2 * liveAbsorb.playoutIntervalToleranceUs &&
                   liveRj.playoutSmoothingReserveMaxUs == liveAbsorb.playoutSmoothingReserveMaxUs &&
                   liveRj.playoutSmoothingMaxLagUs < liveAbsorb.playoutSmoothingMaxLagUs,
               "production Absorb uses local cadence and a fixed 2T pad without a learned reserve");
        const auto rj = revision5Policy(absorbSession(true, false), native, native);
        const auto absorb = revision5Policy(absorbSession(true, true), native, native);
        const auto off = vrrTimingParametersForSession(absorbSession(false, true), native, native);
        expect(rj.playoutSmoothingAbsorbReserveUs == 0 && rj.playoutSmoothingAbsorbReservePeriodPerMille == 0,
               "Reduce judder alone must not hold a standing reserve");
        expect(absorb.playoutSmoothingWindowedCadence == 5 &&
                   absorb.playoutSmoothingAbsorbReserveUs == 2000 &&
                   absorb.playoutSmoothingAbsorbReservePeriodPerMille == 150 &&
                   absorb.playoutSmoothingMaxLagUs == 8000 &&
                   absorb.playoutSmoothingMaxLagPeriodPerMille == 200 &&
                   absorb.playoutSmoothingRecenterPerMille == rj.playoutSmoothingRecenterPerMille &&
                   absorb.playoutSmoothingReserveMaxUs == 0,
               "Absorb microstutter must select the standing reserve on every presenter");
        expect(off.playoutSmoothingGainPerMille == 0 && off.playoutSmoothingAbsorbReserveUs == 0 &&
                   off.playoutSmoothingMaxLagPeriodPerMille == 0,
               "Absorb microstutter must have no effect without Reduce judder");
    }
}
void testFixedPadAndSharedLiveSlew()
{
    for(const uint64_t version : {9,10}) for(const int tolerance : {250,500,1000}) {
        auto session=absorbSession(true,true);
        session.streamRateHz=120;session.displayRefreshHz=240;
        session.experimentalInputPercentilePpm=1;
        session.experimentalBoundedCatchup=true;
        session.timingOptions={4000,9950,120,tolerance};
        auto p=vrrTimingParametersForSession(session);
        p.playoutInputHitchMissesOnly=version;p.playoutInputStableAdaptation=0;
        p.playoutDelayStartSeedUs=4000;
        VrrTimingController c(session,true,p);
        uint32_t pts=0;uint64_t previousTotal=0,previousExtra=0,previousInput=0,previousAt=0,submitted=0;
        for(int f=0;f<700;++f) {
            pts+=f>=150 && f<400?(f%2?990:810):900;
            const uint64_t at=1000000+uint64_t(pts)*100/9;
            PacedFrame frame(nullptr,f,pts,true,at+1000);
            frame.setDeliveryTimeline(at+200,at+400,at+500);
            const uint64_t now=std::max(at+1000,submitted);
            const auto d=c.schedule(frame,now);
            expect(c.smoothingReserveUs()==uint64_t(tolerance*2),
                   "Absorb retains exactly its fixed 2T reserve throughout source jitter");
            expect(d.cadenceSmoothingUs>=0 && d.cadenceSmoothingUs<=int64_t(tolerance*2),
                   "Absorb timing spends only its fixed pad and cannot add another reserve");
            const uint64_t total=d.playoutDelayUs+d.plannedExtraDelayUs();
            if(previousAt) {
                const uint64_t extra=d.plannedExtraDelayUs();
                const uint64_t change=version==9 ? std::max(total,previousTotal)-std::min(total,previousTotal) :
                    std::max(extra,previousExtra)-std::min(extra,previousExtra);
                expect(change<=uint64_t(tolerance)*(now-previousAt)/1000000+2,
                       "extra-delay movement stays bounded; recorded policy 9 also charges input movement");
                if (version==10) {
                    const uint64_t inputChange=std::max(d.playoutDelayUs,previousInput)-std::min(d.playoutDelayUs,previousInput);
                    const uint64_t rate=d.playoutDelayUs>previousInput ? 1000 : 5000;
                    expect(inputChange<=rate*(now-previousAt)/1000000+2,
                           "live input uses its normal rise/release bound independently of extra delay");
                }
            }
            previousAt=now;previousTotal=total;previousExtra=d.plannedExtraDelayUs();previousInput=d.playoutDelayUs;
            submitted=std::max(d.targetUs,now+500);
            c.notePreparationDuration(500,0,now+500);
            c.noteSchedulerDelays(0,0,true);
            c.noteSubmission(true,false,submitted,submitted);
        }
    }
}

void testAbsorbReserveIsStandingLatency()
{
    const auto plainSession = absorbSession(false, false);
    const auto absorbingSession = absorbSession(true, true);
    VrrTimingController plain(plainSession, true, revision5Policy(plainSession));
    VrrTimingController absorbing(absorbingSession, true, revision5Policy(absorbingSession));
    uint32_t stamp = 0;
    for (int i = 0; i < 600; ++i) {
        stamp += 750;
        const uint64_t source = 1000000 + uint64_t(stamp) * 100 / 9;
        const uint64_t delivered = source + 1500 + uint64_t(i % 7) * 300;
        PacedFrame a(nullptr, i, stamp, true, delivered), b(nullptr, i, stamp, true, delivered);
        a.setDeliveryTimeline(delivered, delivered, delivered);
        b.setDeliveryTimeline(delivered, delivered, delivered);
        const auto da = plain.schedule(a, delivered);
        const auto db = absorbing.schedule(b, delivered);
        if (i > 100) {
            const auto reserve = int64_t(absorbing.absorbReserveUs());
            const auto period = int64_t(absorbing.sourcePeriodUs());
            expect(reserve == std::min<int64_t>(2000, period * 150 / 1000) && reserve > 1200 && reserve < 1300,
                   "the reserve must be 15 percent of the 120 FPS source period");
            expect(db.playoutDelayUs == da.playoutDelayUs,
                   "the input buffer must not release the standing reserve");
            expect(db.cadenceSmoothingUs == reserve,
                   "a clean source must keep the whole reserve standing");
            expect(int64_t(db.originalTargetUs - da.originalTargetUs) == reserve,
                   "every target must move later by exactly the reserve");
        }
        plain.notePreparationDuration(1000);
        absorbing.notePreparationDuration(1000);
        plain.noteSubmission(true, false, da.targetUs);
        absorbing.noteSubmission(true, false, db.targetUs);
    }
}

struct DisplacedResult {
    int64_t worstPlannedErrorUs = 0;
    unsigned readinessPushes = 0;
};

DisplacedResult runDisplacedFrames(const VrrSessionConfig& session, const VrrTimingParameters& policy,
                                   int firstDisplaced, uint32_t displacementTicks = 135)
{
    // From firstDisplaced on, every tenth frame is stamped 1.5 ms late and
    // the source then returns to its slot. Each frame is delivered 1.5 ms
    // after its own stamp.
    VrrTimingController controller(session, true, policy);
    DisplacedResult result;
    uint64_t previousTarget = 0;
    uint64_t base = 0;
    for (int i = 0; i < 1200; ++i) {
        base += 750;
        const bool displaced = i >= firstDisplaced && i % 10 == 5;
        const uint32_t stamp = uint32_t(base + (displaced ? displacementTicks : 0));
        const uint64_t source = 1000000 + uint64_t(stamp) * 100 / 9;
        const uint64_t delivered = source + 1500;
        PacedFrame f(nullptr, i, stamp, true, delivered);
        f.setDeliveryTimeline(delivered, delivered, delivered);
        const auto d = controller.schedule(f, delivered);
        if (i > 300 && i > firstDisplaced) {
            const int64_t error = int64_t(d.originalTargetUs - previousTarget) - 8333;
            result.worstPlannedErrorUs = std::max(result.worstPlannedErrorUs, std::abs(error));
        }
        if (i >= firstDisplaced && (firstDisplaced != 0 || i > 300))
            result.readinessPushes += d.targetUs > d.originalTargetUs;
        previousTarget = d.originalTargetUs;
        controller.notePreparationDuration(1000);
        controller.noteSubmission(true, false, d.targetUs);
    }
    return result;
}

void testDisplacedFramesAreAbsorbed()
{
    const auto session = absorbSession(true, false);
    const auto absorbing = absorbSession(true, true);
    const auto fixedPolicy = revision5Policy(session);
    const auto absorbPolicy = revision5Policy(absorbing);
    const auto recorded = runDisplacedFrames(session, revision4Policy(session), 0);
    const auto fixed = runDisplacedFrames(session, fixedPolicy, 0);
    const auto absorbed = runDisplacedFrames(absorbing, absorbPolicy, 0);
    expect(recorded.worstPlannedErrorUs >= 900,
           "revision 4 leaves most of a 1.5 ms displacement in the planned interval");
    expect(fixed.worstPlannedErrorUs <= 520 && absorbed.worstPlannedErrorUs <= 520,
           "revision 5 must plan every interval within T of the source period");
    expect(absorbed.readinessPushes == 0,
           "a displacement inside the standing reserve must never wait for readiness");
    // After a clean stretch the input buffer has not learned an early
    // retiming larger than the existing slack (2.5 ms displacements here);
    // the standing reserve covers it from the first displaced frame.
    const auto suddenFixed = runDisplacedFrames(session, fixedPolicy, 600, 225);
    const auto suddenAbsorbed = runDisplacedFrames(absorbing, absorbPolicy, 600, 225);
    expect(suddenAbsorbed.readinessPushes == 0 && suddenFixed.readinessPushes > 0,
           "the standing reserve must cover displaced frames before the buffer has learned them");
    std::fprintf(stderr, "displaced 1.5 ms: worst planned error %lld / %lld / %lld us; readiness pushes %u / %u / %u; 2.5 ms after a clean stretch %u / %u\n",
                 static_cast<long long>(recorded.worstPlannedErrorUs),
                 static_cast<long long>(fixed.worstPlannedErrorUs),
                 static_cast<long long>(absorbed.worstPlannedErrorUs),
                 recorded.readinessPushes, fixed.readinessPushes, absorbed.readinessPushes,
                 suddenFixed.readinessPushes, suddenAbsorbed.readinessPushes);
}
struct LowRateResult {
    int64_t worstPlannedErrorUs = 0;
    uint64_t phaseLimitUs = 0;
};

LowRateResult runJitteredSource(const VrrSessionConfig& session, const VrrTimingParameters& policy,
                                uint32_t periodTicks)
{
    // Source frames sit on a fixed grid, each stamped up to 3.5 ms off its
    // slot: every gap misses the period, then the source returns to its grid.
    static constexpr std::array<int32_t, 8> kOffsetTicks = {0, 315, -270, 180, -90, 0, 0, 0};
    VrrTimingController controller(session, true, policy);
    LowRateResult result;
    uint64_t previousTarget = 0;
    for (int i = 0; i < 1200; ++i) {
        const uint32_t stamp = uint32_t(int64_t(i) * periodTicks + kOffsetTicks[i % kOffsetTicks.size()]);
        const uint64_t source = 1000000 + uint64_t(stamp) * 100 / 9;
        const uint64_t delivered = source + 1500;
        PacedFrame f(nullptr, i, stamp, true, delivered);
        f.setDeliveryTimeline(delivered, delivered, delivered);
        const auto d = controller.schedule(f, delivered);
        if (i > 300) {
            const int64_t periodUs = int64_t(periodTicks) * 100 / 9;
            const int64_t error = int64_t(d.originalTargetUs - previousTarget) - periodUs;
            result.worstPlannedErrorUs = std::max(result.worstPlannedErrorUs, std::abs(error));
        }
        previousTarget = d.originalTargetUs;
        controller.notePreparationDuration(1000);
        controller.noteSubmission(true, false, d.targetUs);
    }
    result.phaseLimitUs = controller.smoothingPhaseLimitUs();
    return result;
}

void testLowRatePhaseLimit()
{
    // A fixed 10 ms delay keeps the earliest allowed retiming out of the way,
    // so only the phase limit bounds the correction.
    const auto session = absorbSession(true, false);
    const auto absorbing = absorbSession(true, true);
    const auto policy = fixedDelayPolicy(session, false);
    const auto absorbPolicy = fixedDelayPolicy(absorbing, false);
    auto fixedPolicy = policy;
    fixedPolicy.playoutSmoothingMaxLagPeriodPerMille = 0;
    const auto high = runJitteredSource(session, policy, 750);
    const auto highAbsorb = runJitteredSource(absorbing, absorbPolicy, 750);
    expect(high.phaseLimitUs == 2000 && highAbsorb.phaseLimitUs == 8000,
           "at 120 FPS the period share must not change the fixed phase limits");
    const auto fixed = runJitteredSource(session, fixedPolicy, 3000);
    const auto scaled = runJitteredSource(session, policy, 3000);
    const auto scaledAbsorb = runJitteredSource(absorbing, absorbPolicy, 3000);
    expect(scaled.phaseLimitUs >= 3990 && scaled.phaseLimitUs <= 4010 &&
               scaledAbsorb.phaseLimitUs == 8000,
           "at 30 FPS the period share must widen Reduce judder while Absorb keeps its larger limit");
    expect(fixed.worstPlannedErrorUs >= 2000,
           "a fixed 2 ms limit must leave 3.5 ms source jitter visible at 30 FPS");
    // T plus the fitted reference's own movement under this jitter.
    expect(scaled.worstPlannedErrorUs <= 900 && scaledAbsorb.worstPlannedErrorUs <= 900,
           "a period-scaled limit must plan 30 FPS jitter within T of the fitted reference");
    std::fprintf(stderr, "30 FPS jitter: worst planned error fixed %lld / scaled %lld / absorb %lld us; limits %llu / %llu us\n",
                 static_cast<long long>(fixed.worstPlannedErrorUs),
                 static_cast<long long>(scaled.worstPlannedErrorUs),
                 static_cast<long long>(scaledAbsorb.worstPlannedErrorUs),
                 static_cast<unsigned long long>(scaled.phaseLimitUs),
                 static_cast<unsigned long long>(scaledAbsorb.phaseLimitUs));
}

void testAbsorbDoesNotClipShortDisplacements()
{
    // A 6 ms displacement at 60 FPS returns to the original slot on the next
    // frame. Both source intervals fit the display's range. Ample fixed
    // delivery slack isolates correction clipping from input learning.
    auto session = absorbSession(true, true);
    session.streamRateHz = 60;
    session.displayRefreshHz = 120;
    for (bool native : {false, true}) {
        auto current = revision5Policy(session, true, native);
        current.playoutDelayAdaptive = 0;
        current.sourcePlayoutDelayUs = 10000;
        current.playoutDelayMinimumUs = current.playoutDelayMaximumUs = 10000;
        current.playoutDelayMaximumPeriodPerMille = 0;
        current.playoutDelayCapSourcePeriodPerMille = 0;
        current.playoutOffsetSlewUsPerSecond = 0;
        current.minimumCadenceSamples = 10000;
        current.boundedCatchupEnabled = 0;
        auto clipped = current;
        clipped.playoutSmoothingMaxLagUs = 4000;
        for (bool historical : {true, false}) {
            VrrTimingController controller(session, true, historical ? clipped : current);
            uint64_t lastSubmission = 0, lastInterval = 0, worstJerk = 0;
            for (int i = 0; i < 400; ++i) {
                const uint32_t stamp = (i + 1) * 1500 + (i > 100 && i % 20 == 5 ? 540 : 0);
                const uint64_t source = 1000000 + uint64_t(stamp) * 100 / 9;
                const uint64_t delivered = source + 1500;
                PacedFrame f(nullptr, i, stamp, true, delivered);
                f.setDeliveryTimeline(delivered, delivered, delivered);
                const auto d = controller.schedule(f, delivered);
                expect(d.targetUs >= delivered, "absorption must respect known readiness");
                if (lastSubmission) {
                    const uint64_t interval = d.targetUs - lastSubmission;
                    expect(interval >= controller.displayPeriodUs(),
                           "absorption must retain the physical presentation floor");
                    if (i > 110 && lastInterval)
                        worstJerk = std::max(worstJerk, interval > lastInterval ?
                            interval - lastInterval : lastInterval - interval);
                    lastInterval = interval;
                }
                lastSubmission = d.targetUs;
                controller.notePreparationDuration(1000);
                controller.noteSubmission(true, false, d.targetUs);
            }
            expect(historical ? worstJerk >= 2000 : worstJerk <= 1002,
                   historical ? "the recorded 4 ms limit must reproduce the unabsorbed blip" :
                                "Absorb must correct short displacements within the configured tolerance");
            std::fprintf(stderr, "6 ms source displacement (%s, %s): max presented jerk %llu us\n",
                         native ? "scheduled" : "worker", historical ? "4 ms" : "current",
                         static_cast<unsigned long long>(worstJerk));
        }
    }
}
} // namespace

int main()
{
    testAbsorbDoesNotClipShortDisplacements();
    testLowRatePhaseLimit();
    testRevision5Boundaries();
    testRevision5Recentering();
    testRevision5PresentableFloor();
    testAbsorbPolicySelection();
    testFixedPadAndSharedLiveSlew();
    testAbsorbReserveIsStandingLatency();
    testDisplacedFramesAreAbsorbed();
    testBoundaries();
    testConsecutiveGaps();
    testPolicyAndInvalidTimestamps();
    testLateClientDoesNotActivate();
    testRtpBoundaryRounding();
    testReadinessCannotReapplyCorrectionOnCleanGaps();
    for (int t : {250, 500, 750, 2000}) for (bool misses : {false, true})
        for (bool enabled : {false, true}) for (bool live : {false, true})
            testControllerProvenance(misses, enabled, t, live);
    if (!failures) std::puts("Source-gap smoothing boundary, consecutive-frame, policy and provenance tests passed");
    return failures ? 1 : 0;
}
