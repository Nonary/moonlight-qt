#include "../vrrqueueage.h"
#include "../vrrcadence.h"
#include "../vrrrenderlead.h"

#include <cstdio>

namespace {

constexpr uint64_t kSourceIntervalUs = 8620;
constexpr uint64_t kDisplayIntervalUs = 8333;
constexpr uint64_t kServiceFloorUs = 8483;
constexpr uint64_t kScheduleGuardUs = 500;
constexpr uint64_t kClampZoneUs = 2600;

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

void observeModel(VrrQueueAgeController& controller, int windows,
                  uint64_t arrivalSpreadUs = 4000,
                  uint64_t sourceIntervalUs = kSourceIntervalUs,
                  uint64_t serviceFloorUs = kServiceFloorUs,
                  bool nearCeiling = true,
                  bool hasArrivalEvidence = true,
                  uint64_t windowDurationUs = 500000)
{
    for (int i = 0; i < windows; i++) {
        controller.observeWindow(5000, 6000, arrivalSpreadUs,
                                 hasArrivalEvidence, windowDurationUs, true,
                                 sourceIntervalUs, serviceFloorUs,
                                 nearCeiling);
    }
}

VrrQueueAgeController::Target target(VrrQueueAgeController& controller,
                                     bool nearCeiling,
                                     uint64_t sourceIntervalUs =
                                         kSourceIntervalUs,
                                     uint64_t serviceFloorUs =
                                         kServiceFloorUs,
                                     uint64_t preparationFloorUs = 0)
{
    return controller.target(nearCeiling, false, sourceIntervalUs,
                             kDisplayIntervalUs, serviceFloorUs,
                             preparationFloorUs, kScheduleGuardUs,
                             kClampZoneUs);
}

void testWindowDurationScalesWithCadence()
{
    expect(VrrQueueAgeController::windowSampleCount(8620, 116) == 58,
           "116 FPS should use a half-second 58-sample window");
    expect(VrrQueueAgeController::windowSampleCount(16667, 116) == 29,
           "60 FPS should use a cadence-relative window");
    expect(VrrQueueAgeController::windowSampleCount(33333, 116) == 16,
           "low FPS should retain the 16-sample statistical floor");
}

void testNearCeilingAlignmentSlack()
{
    auto slackFor = [](int streamFps, int displayFps) {
        uint64_t scanoutUs = 1000000ULL / displayFps;
        uint64_t cadenceGuardUs = scanoutUs * 24 / 1000;
        uint64_t flipFloorUs = scanoutUs + scanoutUs * 18 / 1000;
        uint64_t serviceGuardUs = scanoutUs * 6 / 1000;
        return vrrCadenceAlignmentSlackUs(
            1000000ULL / streamFps, scanoutUs, cadenceGuardUs,
            flipFloorUs, serviceGuardUs);
    };

    expect(slackFor(58, 60) == 176,
           "58/60 alignment must stay within its 176 us service slack");
    expect(slackFor(116, 120) == 88,
           "116/120 alignment must not retain the old 600 us floor");
    expect(slackFor(232, 240) == 45,
           "232/240 alignment geometry must scale with scanout period");
    expect(slackFor(236, 240) == 0,
           "content beyond the guarded service ceiling has no align budget");

    uint64_t customFloorUs = 9333;
    uint64_t customSlackUs = vrrCadenceAlignmentSlackUs(
        10000, 8333, 199, customFloorUs, 49);
    expect(customSlackUs == 618 &&
               customFloorUs + 49 + customSlackUs == 10000,
           "custom spacing floors must remain inside source cadence");
}

void testHeadroomScaledCatchUpSpacing()
{
    constexpr uint64_t displayIntervalUs = 8333;
    constexpr uint64_t flipFloorUs = 8483;
    constexpr uint64_t highRateSourceUs = 11025;

    uint64_t smoothSpacingUs = vrrCatchUpSpacingUs(
        flipFloorUs, highRateSourceUs, displayIntervalUs, false, true);
    expect(smoothSpacingUs == 9754,
           "smooth high-rate recovery should spend half its headroom");
    expect(vrrCadenceAlignmentSlackUs(
               highRateSourceUs, displayIntervalUs, 199,
               smoothSpacingUs, 49) == 1222,
           "gentle catch-up spacing must reduce the remaining align budget");

    expect(vrrCatchUpSpacingUs(
               flipFloorUs, highRateSourceUs, displayIntervalUs,
               false, false) == flipFloorUs,
           "disabled smooth recovery should retain the fastest drain");
    expect(vrrCatchUpSpacingUs(
               flipFloorUs, 33333, displayIntervalUs,
               false, true) == 20908,
           "30 FPS smooth recovery should spend half its panel headroom");
    expect(vrrCatchUpSpacingUs(
               flipFloorUs, 33333, displayIntervalUs,
               true, false) == 29166,
           "latched recovery should keep its rate-independent gentle drain");
    expect(vrrCatchUpSpacingUs(
               flipFloorUs, 9100, displayIntervalUs,
               false, true) == 8791,
           "near-ceiling recovery should spend only half its scarce headroom");

    const int floors[] = { 20, 25, 30, 40, 50, 60, 70 };
    for (int fps : floors) {
        uint64_t sourceIntervalUs = 1000000ULL / (uint64_t)fps;
        uint64_t expectedSpacingUs = flipFloorUs +
            (sourceIntervalUs - flipFloorUs) / 2;
        expect(vrrCatchUpSpacingUs(
                   flipFloorUs, sourceIntervalUs, displayIntervalUs,
                   false, true) == expectedSpacingUs,
               "low-FPS recovery must spend half its available headroom");
    }

    uint64_t eightyFpsSpacingUs = vrrCatchUpSpacingUs(
        flipFloorUs, 12500, displayIntervalUs, false, true);
    expect(eightyFpsSpacingUs == 10491,
           "80 FPS recovery must spend half its available headroom");
}

void testDeepPressureSpendsMoreHeadroomWithoutPanelBursting()
{
    constexpr uint64_t flipFloorUs = 8483;
    constexpr uint64_t source100Us = 10000;

    uint64_t ordinaryUs = vrrPressureCatchUpSpacingUs(
        flipFloorUs, source100Us, 8333, false, true, false);
    uint64_t pressuredUs = vrrPressureCatchUpSpacingUs(
        flipFloorUs, source100Us, 8333, false, true, true);
    expect(ordinaryUs == 9241 && pressuredUs == 8862 &&
               pressuredUs > flipFloorUs,
           "deep 100 FPS pressure must spend three quarters of headroom without a panel-floor burst");

    expect(vrrPressureCatchUpSpacingUs(
               flipFloorUs, 8620, 8333, false, true, true) == 8517,
           "deep near-ceiling recovery must remain bounded by scarce physical headroom");
    expect(vrrPressureCatchUpSpacingUs(
               flipFloorUs, source100Us, 8333, true, true, true) == 8750,
           "latched recovery must retain its existing cadence-safe spacing");
    expect(vrrCadenceBacklogLimit(false) == 3 &&
               vrrCadenceBacklogLimit(true) == 1,
           "transient triples must be drainable while persistent backlog remains single-frame bounded");
}

void testCappedRecoverySpacing()
{
    constexpr uint64_t flipFloorUs = 8483;

    expect(vrrCadenceCappedSpacingUs(flipFloorUs, 10000) == 10000,
           "100 FPS recovery must retain the 10 ms source cadence");
    expect(vrrCadenceCappedSpacingUs(flipFloorUs, 33333) == 33333,
           "low-FPS recovery must not spend panel headroom on a burst");
    expect(vrrCadenceCappedSpacingUs(flipFloorUs, 8000) == flipFloorUs,
           "the physical panel floor remains authoritative above its ceiling");
    expect(vrrCadenceRecoverySpacingUs(false, flipFloorUs, 33333) == flipFloorUs,
           "a recovered fixed-refresh stream must not retain a stale source cap");
    expect(vrrCadenceRecoverySpacingUs(true, flipFloorUs, 33333) == 33333,
           "an active recovery must still honor the source cadence cap");
    expect(vrrCatchUpTargetUs(100000, 108483, 108483,
                              vrrCadenceCappedSpacingUs(flipFloorUs, 10000),
                              true) == 110000,
           "a capped recovery target must delay a panel-floor target to source cadence");
}

void testRecoverableNearCeilingTransitionDoesNotCoalesce()
{
    constexpr uint64_t flipFloorUs = 8483;

    expect(vrrNearBufferEntryReady(
               true, 9524, 8333, 1350, false, 8333, 100),
           "the first warmed 105 FPS sample must enter buffered true VRR");
    expect(vrrNearBufferEntryReady(
               true, 8620, 8333, 1350, false, 8333, 100),
           "the first warmed 116 FPS sample must enter buffered true VRR");
    expect(vrrNearBufferEntryReady(
               true, 8620, 8333, 1350, true, 8483, 100),
           "latch-capable production pacing must enter immediately at 116 FPS");
    expect(!vrrNearBufferEntryReady(
               true, 8550, 8333, 1350, true, 8483, 100),
           "latch-capable pacing must keep the guarded fast edge out of band");
    expect(!vrrNearBufferEntryReady(
               false, 8620, 8333, 1350, false, 8333, 100),
           "cold cadence must not enter the buffered band");

    expect(!vrrStrictCadenceRecoveryRequired(
               true, true, 8620, flipFloorUs, true, 1),
           "116 FPS recovery must spend its real headroom instead of dropping");
    expect(vrrCatchUpSpacingUs(
               flipFloorUs, 8620, 8333, false, true) == 8551,
           "116 FPS recovery must preserve half its scarce ceiling headroom");
    expect(!vrrStrictCadenceRecoveryRequired(
               true, true, 10000, flipFloorUs, true, 2),
           "100 FPS backlog must recover dynamically rather than coalesce");
    expect(vrrStrictCadenceRecoveryRequired(
               true, true, flipFloorUs, flipFloorUs, true, 1),
           "a stale cadence exactly at the service ceiling may coalesce");
    expect(vrrStrictCadenceRecoveryRequired(
               true, true, 8333, flipFloorUs, false, 2),
           "an unrecoverable two-frame ceiling backlog must remain bounded");

    int baseHistoryCap = VrrQueueAgeController::windowSampleCount(8620, 116);
    int drainAwareCap = vrrBacklogHistorySampleCap(
        baseHistoryCap, 8620, flipFloorUs, 8333, false, true);
    expect(baseHistoryCap == 58 && drainAwareCap >= 133,
           "116 FPS backlog history must outlive the actual smooth drain cycle");

    int fastRecoveryCap = vrrBacklogHistorySampleCap(
        VrrQueueAgeController::windowSampleCount(33333, 116),
        33333, flipFloorUs, 8333, false, true);
    expect(fastRecoveryCap == 16,
           "30 FPS recovery must retain the compact low-rate watchdog");

    uint64_t delayBudgetUs = 4500;
    uint64_t appliedDelayUs = 0;
    for (int i = 0; i < 58; i++) {
        appliedDelayUs += vrrConsumePhaseDelayBudget(200, &delayBudgetUs);
    }
    expect(appliedDelayUs == 4500 && delayBudgetUs == 0,
           "a cadence-window change must not over-apply phase build beyond its deficit");
}

void testCatchUpTargetHonorsSmoothSpacing()
{
    constexpr uint64_t lastFlipUs = 100000;
    constexpr uint64_t flipFloorUs = 8483;
    constexpr uint64_t gentleSpacingUs = 9646;

    expect(vrrCatchUpTargetUs(
               lastFlipUs, 112000, 105000,
               gentleSpacingUs, true) == 109646,
           "gentle recovery should still accelerate an overly future target");
    expect(vrrCatchUpTargetUs(
               lastFlipUs, 108500, 108000,
               gentleSpacingUs, true) == 109646,
           "gentle recovery must delay a floor-spaced stale target");
    expect(vrrCatchUpTargetUs(
               lastFlipUs, 108000, 111000,
               gentleSpacingUs, true) == 111000,
           "an already elapsed gentle target should rebase onto the present instant");
    expect(vrrCatchUpTargetUs(
               lastFlipUs, 108000, 107000,
               flipFloorUs, false) == 108000,
           "fast recovery modes must retain accelerate-only target selection");
    expect(vrrCatchUpTargetUs(
               lastFlipUs, 112000, 105000,
               flipFloorUs, false) == 108483,
           "fast recovery modes must still accelerate a future stale target");
}

void testHeadroomFallbackPeriodAvoidsFloorJudder()
{
    constexpr uint64_t minFrameIntervalUs = 8333;
    constexpr uint64_t headroomThresholdUs = 1000;
    constexpr uint32_t basePeriodSecs = 60;

    expect(vrrHeadroomFallbackPeriodSecs(
               9615, minFrameIntervalUs, headroomThresholdUs,
               basePeriodSecs, 240) == basePeriodSecs,
           "headroom rates should return to VRR after the base latch rung");
    expect(vrrHeadroomFallbackPeriodSecs(
               9333, minFrameIntervalUs, headroomThresholdUs,
               basePeriodSecs, 120) == basePeriodSecs,
           "the headroom threshold should include its exact boundary");
    expect(vrrHeadroomFallbackPeriodSecs(
               9200, minFrameIntervalUs, headroomThresholdUs,
               basePeriodSecs, 240) == 240,
           "ceiling-adjacent rates should retain the long safety latch");
    expect(vrrHeadroomFallbackPeriodSecs(
               9615, minFrameIntervalUs, headroomThresholdUs,
               basePeriodSecs, 30) == 30,
           "a first-offense latch should not be lengthened");
}

void testRateIdentityKeepsTenFpsBandsSeparate()
{
    expect(vrrCadenceRateIdentityMatches(9500, 9750),
           "rate identity should include its 250 us quantization boundary");
    expect(!vrrCadenceRateIdentityMatches(9500, 9751),
           "rate identity should split intervals beyond the quantization band");
    expect(!vrrCadenceRateIdentityMatches(9091, 9524),
           "110 and 105 FPS must not share a tear verdict");
    expect(!vrrCadenceRateIdentityMatches(9524, 10000),
           "105 and 100 FPS must not share a tear verdict");
}

void testTearProbeWaitsForSettledTransition()
{
    expect(vrrTearProbeTransitionSettled(false, false, 0, false),
           "ordinary in-band presents should feed the tear probe");
    expect(!vrrTearProbeTransitionSettled(true, false, 0, false),
           "re-lock alignment presents must not poison the tear probe");
    expect(!vrrTearProbeTransitionSettled(false, true, 0, false),
           "active queue acquisition must hold off the tear probe");
    expect(!vrrTearProbeTransitionSettled(false, false, 400, false),
           "active fast queue recovery must hold off the tear probe");
    expect(!vrrTearProbeTransitionSettled(false, false, 0, true),
           "the final fast-recovery present must still be excluded");

    expect(vrrQueueAcquisitionTransitionActive(true, true, true),
           "usable pending feedback should mark acquisition active");
    expect(!vrrQueueAcquisitionTransitionActive(false, true, true),
           "disabled queue feedback must not suppress tear probing");
    expect(!vrrQueueAcquisitionTransitionActive(true, false, true),
           "a stream without queue timestamps must not suppress tear probing");
    expect(!vrrQueueAcquisitionTransitionActive(true, true, false),
           "completed acquisition must not suppress tear probing");

    expect(vrrQueueAcquisitionOverlapsRecovery(true, true, 1000, false),
           "a later re-lock must cancel an active fast queue build");
    expect(vrrQueueAcquisitionOverlapsRecovery(true, true, 0, true),
           "the final applied fast-build step must trigger remeasurement");
    expect(!vrrQueueAcquisitionOverlapsRecovery(false, true, 1000, false),
           "out-of-band recovery must not arm near-ceiling acquisition");
    expect(!vrrQueueAcquisitionOverlapsRecovery(true, false, 1000, false),
           "ordinary queue acquisition must remain active without re-lock");
}

void testFastCadenceUpshiftAdoption()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    uint64_t targetUs = clock.nextTargetUs(sourceUs, sourceUs);

    for (int i = 0; i < 40; i++) {
        sourceUs += 33333;
        targetUs = clock.nextTargetUs(sourceUs, sourceUs);
    }
    expect(clock.smoothedIntervalUs() > 32000,
           "steady 30 FPS content must establish its slow cadence");
    clock.consumePhaseReset();

    const uint64_t fasterPatternUs[] = {
        8333, 8333, 16667, 8333, 8333, 16667,
    };
    for (int i = 0; i < 5; i++) {
        sourceUs += fasterPatternUs[i];
        targetUs = clock.nextTargetUs(sourceUs, sourceUs);
    }
    expect(clock.smoothedIntervalUs() > 20000,
           "fewer than six fast intervals must not rebase cadence");

    sourceUs += fasterPatternUs[5];
    targetUs = clock.nextTargetUs(sourceUs, sourceUs);
    expect(clock.smoothedIntervalUs() >= 11110 &&
               clock.smoothedIntervalUs() <= 11112,
           "six sustained fast intervals must adopt their averaged cadence");
    expect(targetUs == sourceUs && clock.consumePhaseReset() &&
               clock.warmedUp(),
           "fast cadence adoption must discard the old slow target phase");
}

void testQuantizedCadenceDoesNotTriggerFastAdoption()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.observeSourceTime(sourceUs);
    for (int i = 0; i < 40; i++) {
        sourceUs += 16667;
        clock.observeSourceTime(sourceUs);
    }
    expect(clock.smoothedIntervalUs() > 16000,
           "the quantization test must begin from an established 60 FPS cadence");

    const uint64_t quantizedPatternUs[] = { 8333, 8333, 8333, 8333, 16668 };
    uint64_t minimumObservedUs = 1000000;

    for (int cycle = 0; cycle < 20; cycle++) {
        for (uint64_t deltaUs : quantizedPatternUs) {
            sourceUs += deltaUs;
            clock.observeSourceTime(sourceUs);
            minimumObservedUs = qMin(minimumObservedUs,
                                     clock.smoothedIntervalUs());
        }
    }

    expect(minimumObservedUs >= 9500 &&
               clock.smoothedIntervalUs() >= 9900 &&
               clock.smoothedIntervalUs() <= 10100,
           "the long host-vsync slot must break fast adoption while the normal window converges near 100 FPS");
}

void testModerateCadenceUpshiftUsesPhaseDrift()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.nextTargetUs(sourceUs, sourceUs);
    for (int i = 0; i < 70; i++) {
        sourceUs += 10526; // 95 FPS
        clock.nextTargetUs(sourceUs, sourceUs);
        clock.consumePhaseReset();
    }
    expect(clock.smoothedIntervalUs() >= 10524 &&
               clock.smoothedIntervalUs() <= 10528,
           "moderate upshift test must establish 95 FPS first");

    int adoptionFrames = 0;
    for (; adoptionFrames < 20; adoptionFrames++) {
        sourceUs += 9346; // 107 FPS
        clock.nextTargetUs(sourceUs, sourceUs);
        clock.consumePhaseReset();
        if (clock.smoothedIntervalUs() >= 9344 &&
                clock.smoothedIntervalUs() <= 9348) {
            adoptionFrames++;
            break;
        }
    }
    expect(adoptionFrames >= 6 && adoptionFrames <= 10 &&
               clock.smoothedIntervalUs() >= 9344 &&
               clock.smoothedIntervalUs() <= 9348,
           "95 -> 107 FPS must rebase after one interval of phase drift, before queue overflow");
}

void testModerateCadenceDownshiftUsesPhaseDrift()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.nextTargetUs(sourceUs, sourceUs);
    for (int i = 0; i < 70; i++) {
        sourceUs += 9346; // 107 FPS
        clock.nextTargetUs(sourceUs, sourceUs);
        clock.consumePhaseReset();
    }
    expect(clock.smoothedIntervalUs() >= 9344 &&
               clock.smoothedIntervalUs() <= 9348,
           "moderate downshift test must establish 107 FPS first");

    int adoptionFrames = 0;
    for (; adoptionFrames < 20; adoptionFrames++) {
        sourceUs += 10526; // 95 FPS
        clock.nextTargetUs(sourceUs, sourceUs);
        clock.consumePhaseReset();
        if (clock.smoothedIntervalUs() >= 10524 &&
                clock.smoothedIntervalUs() <= 10528) {
            adoptionFrames++;
            break;
        }
    }
    expect(adoptionFrames >= 6 && adoptionFrames <= 10 &&
               clock.smoothedIntervalUs() >= 10524 &&
               clock.smoothedIntervalUs() <= 10528,
           "107 -> 95 FPS must rebase before the old fast schedule becomes refresh chatter");
}

void testQuantizedCadenceDoesNotTripPhaseDrift()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.nextTargetUs(sourceUs, sourceUs);
    for (int i = 0; i < 70; i++) {
        sourceUs += 10000;
        clock.nextTargetUs(sourceUs, sourceUs);
        clock.consumePhaseReset();
    }

    const uint64_t quantized100FpsUs[] = {
        8333, 8333, 8333, 8333, 16668,
    };
    uint64_t minimumObservedUs = UINT64_MAX;
    uint64_t maximumObservedUs = 0;
    for (int cycle = 0; cycle < 40; cycle++) {
        for (uint64_t deltaUs : quantized100FpsUs) {
            sourceUs += deltaUs;
            clock.nextTargetUs(sourceUs, sourceUs);
            clock.consumePhaseReset();
            minimumObservedUs = qMin(minimumObservedUs,
                                     clock.smoothedIntervalUs());
            maximumObservedUs = qMax(maximumObservedUs,
                                     clock.smoothedIntervalUs());
        }
    }
    expect(minimumObservedUs >= 9800 && maximumObservedUs <= 10200,
           "120 Hz host quantization around 100 FPS must not trigger moderate-rate rebases");
}

void testIsolatedHighRateHitchDoesNotTripPhaseDrift()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.nextTargetUs(sourceUs, sourceUs);
    for (int i = 0; i < 70; i++) {
        sourceUs += 8621;
        clock.nextTargetUs(sourceUs, sourceUs);
        clock.consumePhaseReset();
    }

    sourceUs += 20000;
    clock.nextTargetUs(sourceUs, sourceUs);
    clock.consumePhaseReset();
    for (int i = 0; i < 12; i++) {
        sourceUs += 8621;
        clock.nextTargetUs(sourceUs, sourceUs);
        clock.consumePhaseReset();
    }
    expect(clock.smoothedIntervalUs() <= 9000,
           "one high-rate hitch must not be adopted as a slower moderate cadence");
}

void testFastCadenceAdoptionRespectsNominalCap()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.nextTargetUs(sourceUs, sourceUs);
    for (int i = 0; i < 40; i++) {
        sourceUs += 33333;
        clock.nextTargetUs(sourceUs, sourceUs);
    }
    clock.consumePhaseReset();
    for (int i = 0; i < 6; i++) {
        sourceUs += 5000;
        clock.nextTargetUs(sourceUs, sourceUs);
    }

    expect(clock.smoothedIntervalUs() == 1000000ULL / 116 &&
               clock.consumePhaseReset(),
           "fast adoption must clamp and rebase at negotiated FPS");

    for (int i = 0; i < 20; i++) {
        sourceUs += 5000;
        clock.nextTargetUs(sourceUs, sourceUs);
        expect(clock.smoothedIntervalUs() == 1000000ULL / 116,
               "subsequent timestamp updates must retain the negotiated FPS cap");
        expect(!clock.consumePhaseReset(),
               "impossible above-nominal timestamps must not repeatedly rebase");
    }
}

void testSlowerCadenceDownshiftAdoption()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.nextTargetUs(sourceUs, sourceUs);

    for (int i = 0; i < 40; i++) {
        sourceUs += 8621;
        clock.nextTargetUs(sourceUs, sourceUs);
    }
    expect(clock.smoothedIntervalUs() >= 8619 &&
               clock.smoothedIntervalUs() <= 8622,
           "the downshift test must begin from an established near-ceiling cadence");
    clock.consumePhaseReset();

    for (int i = 0; i < 5; i++) {
        sourceUs += 14286;
        clock.nextTargetUs(sourceUs, sourceUs);
    }
    expect(clock.smoothedIntervalUs() < 13000,
           "fewer than six slower intervals must not rebase cadence");

    sourceUs += 14286;
    uint64_t targetUs = clock.nextTargetUs(sourceUs, sourceUs);
    expect(clock.smoothedIntervalUs() >= 14284 &&
               clock.smoothedIntervalUs() <= 14288,
           "six sustained slower intervals must adopt their averaged cadence");
    expect(targetUs == sourceUs && clock.consumePhaseReset() &&
               clock.warmedUp(),
           "slower cadence adoption must discard the old high-rate target phase");
}

void testTwentyFpsCadenceLock()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.nextTargetUs(sourceUs, sourceUs);

    for (int i = 0; i < 20; i++) {
        sourceUs += 50000;
        clock.nextTargetUs(sourceUs, sourceUs);
    }

    expect(clock.smoothedIntervalUs() >= 49999 &&
               clock.smoothedIntervalUs() <= 50001,
           "steady 20 FPS delivery must become a supported cadence");
    expect(clock.warmedUp(),
           "steady 20 FPS delivery must leave the cadence-cold latch");
}

void testLongHostHitchesRemainStalls()
{
    VrrCadenceClock clock(116, 120);
    uint64_t sourceUs = 1000000;
    clock.observeSourceTime(sourceUs);
    for (int i = 0; i < 40; i++) {
        sourceUs += 8620;
        clock.observeSourceTime(sourceUs);
    }

    sourceUs += 65000;
    clock.observeSourceTime(sourceUs);
    sourceUs += 65000;
    clock.observeSourceTime(sourceUs);

    expect(clock.smoothedIntervalUs() < 20000,
           "repeated 60-80ms host hitches must remain stalls, not a fake cadence");
}

void testRobustWindowStatistics()
{
    uint32_t agesUs[] = {
        14000, 3500, 12000, 9000, 15500,
        11000, 30000, 7000, 13000, 12000,
        10000, 16500, 8000, 14500, 12500,
        16000, 11500, 13500, 15000, 12000,
    };
    auto stats = VrrQueueAgeController::summarizeWindow(
        agesUs, (int)(sizeof(agesUs) / sizeof(agesUs[0])));

    expect(stats.p10Us == 7000 && stats.p90Us == 16000,
           "robust spread bounds must reject isolated low and high outliers");
    expect(stats.p20Us == 9000 && stats.medianUs == 12000,
           "window control percentiles must describe typical queue age");

    int64_t arrivalPhaseUs[] = {
        100000, 100200, 100400, 100600, 100800,
        101000, 101200, 101400, 101600, 101800,
        102000, 102200, 102400, 102600, 102800,
        103000, 103200, 103400, 103600, 120000,
    };
    expect(VrrQueueAgeController::summarizeArrivalSpread(
               arrivalPhaseUs,
               (int)(sizeof(arrivalPhaseUs) /
                     sizeof(arrivalPhaseUs[0]))) == 3600,
           "arrival learning must reject an isolated clean-window phase extreme");
}

void testRenderLeadForgetsIsolatedSpikesButProtectsSustainedTail()
{
    constexpr uint64_t sourceIntervalUs = 10000;
    VrrRenderLeadController controller(4000);

    VrrRenderLeadController::Update update = {};
    for (int i = 0; i < 220; i++) {
        update = controller.observe(250 + (i % 5) * 50,
                                    sourceIntervalUs, true);
    }
    expect(update.marginUs <= 1250 && update.tailOvershootUs <= 450,
           "a steady render must converge near the one-millisecond lead floor");

    update = controller.observe(5000, sourceIntervalUs, true);
    expect(update.lateSample && update.marginUs == 5500,
           "a disproven lead margin must protect the next frame immediately");

    for (int i = 0; i < 200; i++) {
        update = controller.observe(300, sourceIntervalUs, true);
    }
    expect(update.marginUs <= 1250 && update.tailOvershootUs <= 450,
           "one render hitch must age out within the short clean-sample window");

    uint64_t beforeUncleanUs = update.marginUs;
    update = controller.observe(6000, sourceIntervalUs, false);
    expect(!update.lateSample && update.marginUs == beforeUncleanUs,
           "recovery and loading stalls must not poison steady render lead");

    for (int i = 0; i < 200; i++) {
        int64_t overshootUs = i % 10 == 0 ? 2800 : 350;
        update = controller.observe(overshootUs, sourceIntervalUs, true);
    }
    expect(update.tailOvershootUs >= 2800 && update.marginUs >= 3200,
           "a recurring render tail must retain enough lead for smooth presentation");
}

void testArrivalPhaseRtpWrapIsContinuous()
{
    constexpr uint32_t referenceTimestamp90k = 0xfffff000U;
    constexpr uint32_t wrappedTimestamp90k = 0x00000800U;
    constexpr uint64_t referenceDecodeUs = 1000000;
    constexpr uint64_t decodeDeltaUs = 68800;

    int64_t phaseUs = 0;
    uint64_t sourceDeltaUs =
        (uint64_t)(wrappedTimestamp90k - referenceTimestamp90k) *
        1000000ULL / 90000ULL;
    expect(VrrQueueAgeController::arrivalPhaseFromRtpTimestamps(
               wrappedTimestamp90k, referenceTimestamp90k,
               referenceDecodeUs + decodeDeltaUs, referenceDecodeUs,
               &phaseUs) &&
               phaseUs == (int64_t)decodeDeltaUs - (int64_t)sourceDeltaUs &&
               qAbs(phaseUs) < 1000,
           "RTP wrap must preserve a small relative arrival phase");
    expect(!VrrQueueAgeController::arrivalPhaseFromRtpTimestamps(
                referenceTimestamp90k - 1, referenceTimestamp90k,
                referenceDecodeUs + 1000, referenceDecodeUs, &phaseUs),
           "backward RTP timestamps must not become arrival evidence");
    expect(!VrrQueueAgeController::arrivalPhaseFromRtpTimestamps(
                referenceTimestamp90k + 90000, referenceTimestamp90k,
                referenceDecodeUs + 10000, referenceDecodeUs, &phaseUs),
           "a large forward RTP discontinuity must not poison arrival learning");
    expect(!VrrQueueAgeController::arrivalPhaseFromRtpTimestamps(
                referenceTimestamp90k + 270000, referenceTimestamp90k,
                referenceDecodeUs + 3000000, referenceDecodeUs, &phaseUs),
           "arrival evidence must remain inside the observation safety horizon");
}

void testPhaseDecisionUsesOneSetpoint()
{
    VrrQueueAgeController::PhaseDecisionInput input = {};
    input.stats = { 3500, 5000, 12000, 16000 };
    input.targetAgeUs = 8000;
    input.previousTargetAgeUs = 8000;
    input.sourceIntervalUs = kSourceIntervalUs;
    input.sampleCount = 58;
    input.nearCeiling = true;

    auto trim = VrrQueueAgeController::decidePhase(input);
    expect(trim.advanceUs > 0 && trim.delayUs == 0,
           "standing median excess must schedule phase trim");

    input.stats = { 500, 1000, 8000, 14000 };
    auto settled = VrrQueueAgeController::decidePhase(input);
    expect(settled.advanceUs == 0 && settled.delayUs == 0,
           "a low p20 must not rebuild when the controlled median is settled");

    input.stats = { 2000, 3000, 5000, 7000 };
    auto build = VrrQueueAgeController::decidePhase(input);
    expect(build.delayUs > 0 && build.advanceUs == 0,
           "median shortage must build gradually near the ceiling");

    input.stats = { 5000, 6500, 8000, 9500 };
    input.hasSetpointError = true;
    input.medianSetpointErrorUs = 2000;
    auto relativeTrim = VrrQueueAgeController::decidePhase(input);
    expect(relativeTrim.advanceUs > 0 && relativeTrim.delayUs == 0,
           "contemporaneous excess must trim when a moving target hides it in the raw median");
    input.medianSetpointErrorUs = -2000;
    auto relativeBuild = VrrQueueAgeController::decidePhase(input);
    expect(relativeBuild.delayUs > 0 && relativeBuild.advanceUs == 0,
           "contemporaneous shortage must build when a moving target hides it in the raw median");
    input.hasSetpointError = false;
    input.stats = { 2000, 3000, 5000, 7000 };
    input.nearCeiling = false;
    expect(VrrQueueAgeController::decidePhase(input).delayUs > 0,
           "headroom-aware targets must remain servo-controlled out of band");

    input.nearCeiling = true;
    input.windowTainted = true;
    input.stats = { 3500, 5000, 12000, 16000 };
    expect(VrrQueueAgeController::decidePhase(input).advanceUs > 0,
           "taint must freeze learning and build, not excess-latency trim");

    input.windowTainted = false;
    input.overfillEligible = true;
    input.targetStable = true;
    input.stats = { 12000, 13000, 14000, 16000 };
    auto drop = VrrQueueAgeController::decidePhase(input);
    expect(drop.requestOverfillDrop && drop.advanceUs > 0,
           "a stalled high-age trim may request coalescing without pausing trim");
    input.windowTainted = true;
    auto recoveryWindow = VrrQueueAgeController::decidePhase(input);
    expect(!recoveryWindow.requestOverfillDrop && recoveryWindow.advanceUs > 0,
           "recovery-tainted windows may trim but must not trigger a drop");
    input.windowTainted = false;
    input.overfillEligible = false;
    auto firstPass = VrrQueueAgeController::decidePhase(input);
    expect(!firstPass.requestOverfillDrop && firstPass.advanceUs > 0,
           "the first high-age window must try gradual trim before a drop");

    expect(!VrrQueueAgeController::shouldDiscardPhaseAdvance(false, false) &&
               VrrQueueAgeController::shouldDiscardPhaseAdvance(false, true) &&
               VrrQueueAgeController::shouldDiscardPhaseAdvance(true, false),
           "only an actual recovery rebase or stale catch-up invalidates trim");
}

void testHeadroomContinuouslyReducesReserve()
{
    VrrQueueAgeController nearController(4500, false);
    target(nearController, true);
    observeModel(nearController, 10, 4000,
                 kSourceIntervalUs, kServiceFloorUs, true);
    auto near = target(nearController, true);

    VrrQueueAgeController headroomController(4500, false);
    target(headroomController, false, 16667, kServiceFloorUs);
    observeModel(headroomController, 10, 4000,
                 16667, kServiceFloorUs, false);
    auto headroom = target(headroomController, false,
                           16667, kServiceFloorUs);

    expect(near.modelDemandUs == headroom.modelDemandUs,
           "raw arrival demand must be independent of cadence headroom");
    expect(near.headroomCreditUs < 200 &&
               headroom.headroomCreditUs > 3000,
           "usable cadence slack must become a continuous headroom credit");
    expect(headroom.queueAgeUs < near.queueAgeUs &&
               headroom.queueAgeUs < 500,
           "Balanced lower-rate content must shed its policy floor and retain only uncovered measured jitter");
}

void testPreparationFloorLeavesRealProtectionAtGameplayRates()
{
    constexpr uint64_t preparationUs = 4500;
    const int rates[] = { 75, 90, 100, 105 };
    for (int fps : rates) {
        uint64_t sourceIntervalUs = 1000000ULL / (uint64_t)fps;
        VrrQueueAgeController controller(6000, false);
        target(controller, false, sourceIntervalUs, kServiceFloorUs,
               preparationUs);
        observeModel(controller, 8, 8000, sourceIntervalUs,
                     kServiceFloorUs, false);
        auto protectedTarget = target(controller, false, sourceIntervalUs,
                                      kServiceFloorUs, preparationUs);
        expect(protectedTarget.preparationFloorUs == preparationUs &&
                   protectedTarget.protectionReserveUs > 2000 &&
                   protectedTarget.queueAgeUs ==
                       protectedTarget.preparationFloorUs +
                           protectedTarget.protectionReserveUs &&
                   protectedTarget.queueAgeUs <= sourceIntervalUs,
               "75-105 FPS must retain controllable dejitter reserve after renderer preparation");
    }

    VrrQueueAgeController lowRate(6000, false);
    auto lowTarget = target(lowRate, false, 33333, kServiceFloorUs,
                            preparationUs);
    expect(lowTarget.queueAgeUs == preparationUs &&
               lowTarget.preparationFloorUs == preparationUs &&
               lowTarget.protectionReserveUs == 0,
           "30 FPS must report unavoidable preparation without adding an idle queue");
}

void testPreparationFloorEstimatorRejectsStalls()
{
    uint64_t floorUs = VrrQueueAgeController::updatePreparationFloorUs(
        0, 4500, 11111, true);
    expect(floorUs == 4500,
           "the first clean render-preparation sample must seed its floor");

    floorUs = VrrQueueAgeController::updatePreparationFloorUs(
        floorUs, 6500, 11111, true);
    expect(floorUs == 5000,
           "the preparation floor must attack a higher clean workload without jumping to one spike");

    uint64_t beforeRejectedUs = floorUs;
    floorUs = VrrQueueAgeController::updatePreparationFloorUs(
        floorUs, 20000, 11111, true);
    floorUs = VrrQueueAgeController::updatePreparationFloorUs(
        floorUs, 7000, 11111, false);
    expect(floorUs == beforeRejectedUs,
           "stalls and recovery-tainted preparation must not inflate standing queue age");

    floorUs = VrrQueueAgeController::updatePreparationFloorUs(
        floorUs, 4000, 11111, true);
    expect(floorUs < beforeRejectedUs && floorUs > 4000,
           "the preparation floor must glide down instead of toggling with frame noise");
}

void testPolicyControlsSlewRatherThanPadding()
{
    VrrQueueAgeController lowest(2500, false);
    VrrQueueAgeController balanced(4500, false);
    VrrQueueAgeController smoothest(6000, false);
    target(lowest, true);
    target(balanced, true);
    target(smoothest, true);

    observeModel(lowest, 3);
    observeModel(balanced, 3);
    observeModel(smoothest, 3);
    uint64_t lowAttackUs = target(lowest, true).queueAgeUs;
    uint64_t balancedAttackUs = target(balanced, true).queueAgeUs;
    uint64_t smoothAttackUs = target(smoothest, true).queueAgeUs;
    expect(lowAttackUs < balancedAttackUs &&
               balancedAttackUs < smoothAttackUs,
           "policies must control how aggressively reserve scales upward");

    observeModel(lowest, 20);
    observeModel(balanced, 20);
    observeModel(smoothest, 20);
    auto lowSettled = target(lowest, true);
    auto balancedSettled = target(balanced, true);
    auto smoothSettled = target(smoothest, true);
    expect(lowSettled.modelDemandUs == balancedSettled.modelDemandUs &&
               balancedSettled.modelDemandUs == smoothSettled.modelDemandUs,
           "policy must not alter the learned raw timing model");
    expect(lowSettled.queueAgeUs <= balancedSettled.queueAgeUs &&
               balancedSettled.queueAgeUs <= smoothSettled.queueAgeUs &&
               smoothSettled.queueAgeUs <= kSourceIntervalUs,
           "policies must apply distinct protection envelopes without exceeding one frame");
}

void testNearCeilingEntryAddsNoAutomaticPadding()
{
    VrrQueueAgeController controller(4500, false);
    auto before = target(controller, false);
    controller.enterNearCeiling(kSourceIntervalUs);
    auto after = target(controller, true);
    expect(before.queueAgeUs == after.queueAgeUs &&
               after.pressureReserveUs == 0,
           "near-ceiling membership alone must never create safety padding");
}

void testStableAndUnavailableArrivalEvidence()
{
    VrrQueueAgeController unavailable(4500, false);
    target(unavailable, true);
    observeModel(unavailable, 8, 0, kSourceIntervalUs, kServiceFloorUs,
                 true, false);
    expect(target(unavailable, true).modelDemandUs == 2500 &&
               unavailable.cacheEntryCount() == 0,
           "missing source timestamps must not be mistaken for zero jitter");

    VrrQueueAgeController stable(4500, false);
    target(stable, true);
    observeModel(stable, 4, 0);
    auto learned = target(stable, true);
    expect(learned.modelDemandUs == 500 && stable.cacheEntryCount() == 1,
           "a valid zero arrival spread must learn the minimum reserve");
}

void testReadinessPressureRequiresASetpointMiss()
{
    expect(!VrrQueueAgeController::isReadinessNearMiss(500, 500, false),
           "a stable low-latency equilibrium must not attack itself");
    expect(!VrrQueueAgeController::isReadinessNearMiss(700, 800, false),
           "a small setpoint error must remain inside the pressure deadband");
    expect(VrrQueueAgeController::isReadinessNearMiss(500, 1500, false),
           "a young frame materially below target must count as pressure");
    expect(!VrrQueueAgeController::isReadinessNearMiss(500, 1500, true),
           "recovery-induced young frames must not count as pressure");
}

void testPolicySelectionUsesLegacyValuesAsDynamicsOnly()
{
    using Policy = VrrQueueAgeController::Policy;

    expect(VrrQueueAgeController::policyFromLegacyValue(3000) ==
               Policy::LowestLatency &&
               VrrQueueAgeController::policyFromLegacyValue(3001) ==
                   Policy::Balanced,
           "the lowest-latency policy boundary must remain stable");
    expect(VrrQueueAgeController::policyFromLegacyValue(5499) ==
               Policy::Balanced &&
               VrrQueueAgeController::policyFromLegacyValue(5500) ==
                   Policy::Smoothest,
           "the smoothest policy boundary must remain stable");

    VrrQueueAgeController lowest(2500, false);
    VrrQueueAgeController balanced(4500, false);
    VrrQueueAgeController smoothest(6000, false);
    expect(lowest.policy() == Policy::LowestLatency &&
               balanced.policy() == Policy::Balanced &&
               smoothest.policy() == Policy::Smoothest,
           "saved UI values must select the intended adaptation dynamics");

    auto lowCold = target(lowest, true);
    auto balancedCold = target(balanced, true);
    auto smoothCold = target(smoothest, true);
    expect(lowCold.queueAgeUs <= balancedCold.queueAgeUs &&
               balancedCold.queueAgeUs <= smoothCold.queueAgeUs,
           "cold targets must reflect each policy's latency/smoothness envelope");
}

void testCacheValidationAndConfidenceCap()
{
    using CacheEntry = VrrQueueAgeController::CacheEntry;

    auto rejected = [](const CacheEntry& entry) {
        VrrQueueAgeController controller(4500, false);
        return !controller.restoreModel(entry) &&
            controller.cacheEntryCount() == 0 &&
            target(controller, true).confidence == 0;
    };

    expect(rejected({ 25, 600, 4000, 500 }),
           "cache restore must reject non-bucket headroom values");
    expect(rejected({ 1000, 600, 4000, 500 }),
           "cache restore must reject headroom beyond the model range");
    expect(rejected({ 0, 600, 499, 500 }) &&
               rejected({ 0, 600, 12001, 500 }),
           "cache restore must reject demand outside hard model bounds");
    expect(rejected({ 0, 600, 4000, 12001 }),
           "cache restore must reject impossible volatility");
    expect(rejected({ 0, 199, 4000, 500 }) &&
               rejected({ 0, 1001, 4000, 500 }),
           "cache restore must reject untrusted or malformed confidence");

    VrrQueueAgeController capped(4500, false);
    expect(capped.restoreModel({ 0, 1000, 4000, 500 }),
           "a structurally valid high-confidence cache entry must restore");
    auto restored = target(capped, true);
    expect(restored.restored && restored.confidence == 900,
           "high-refresh cache confidence must warm-start strongly but remain bounded");

    VrrQueueAgeController weak(4500, false);
    VrrQueueAgeController strong(4500, false);
    expect(weak.restoreModel({ 0, 200, 12000, 0 }) &&
               strong.restoreModel({ 0, 1000, 12000, 0 }),
           "valid extreme cache entries must remain recoverable");
    auto weakTarget = target(weak, true);
    auto strongTarget = target(strong, true);
    expect(weakTarget.modelDemandUs == 12000 &&
               weakTarget.effectiveDemandUs == 4400 &&
               strongTarget.effectiveDemandUs == 11050 &&
               weakTarget.queueAgeUs < strongTarget.queueAgeUs,
           "restored confidence must limit a stale model's warm-start authority");

    VrrQueueAgeController weakLow(4500, false);
    VrrQueueAgeController strongLow(4500, false);
    weakLow.restoreModel({ 0, 200, 500, 0 });
    strongLow.restoreModel({ 0, 1000, 500, 0 });
    auto weakLowTarget = target(weakLow, true);
    auto strongLowTarget = target(strongLow, true);
    expect(weakLowTarget.effectiveDemandUs == 2100 &&
               strongLowTarget.effectiveDemandUs == 700 &&
               weakLowTarget.queueAgeUs > strongLowTarget.queueAgeUs,
           "weak stale-low evidence must retain more of the safe common prior");
}

void testCacheBucketInterpolation()
{
    VrrQueueAgeController controller(4500, false);
    expect(controller.restoreModel({ 0, 600, 2000, 200 }) &&
               controller.restoreModel({ 100, 600, 6000, 600 }),
           "bracketing cache models must restore independently");

    auto interpolated = target(controller, false, 10000, 9500);
    expect(interpolated.headroomPermille == 50 &&
               interpolated.modelDemandUs == 4000 &&
               interpolated.effectiveDemandUs == 3175 &&
               interpolated.confidence == 450 &&
               interpolated.restored,
           "a missing headroom bucket must interpolate demand and discount its control authority");
    expect(interpolated.headroomCreditUs == 250 &&
               interpolated.queueAgeUs == 2925,
           "confidence-weighted demand must use the live cadence's headroom credit");
}

void testHighRefreshCachePrioritizesBalancedWarmStart()
{
    // This is the high-refresh geometry from the observed 116-on-120 trace:
    // 8.620 ms source cadence, 8.483 ms guarded service floor, and an
    // approximately 8 ms calibrated raw arrival reserve. Balanced should be
    // effectively as smooth as the explicit Smoothest policy here, while the
    // same cache must not make a 30 FPS session retain standing latency.
    constexpr uint64_t sourceUs = 8620;
    constexpr uint64_t floorUs = 8483;
    const VrrQueueAgeController::CacheEntry highRefreshCache =
        { 0, 1000, 8000, 1500 };

    VrrQueueAgeController balanced(4500, false);
    VrrQueueAgeController smoothest(6000, false);
    expect(balanced.restoreModel(highRefreshCache) &&
               smoothest.restoreModel(highRefreshCache),
           "the trace-derived high-refresh cache model must restore");
    auto balancedWarm = target(balanced, false, sourceUs, floorUs);
    auto smoothestWarm = target(smoothest, false, sourceUs, floorUs);
    uint64_t policyDeltaUs = balancedWarm.queueAgeUs > smoothestWarm.queueAgeUs ?
        balancedWarm.queueAgeUs - smoothestWarm.queueAgeUs :
        smoothestWarm.queueAgeUs - balancedWarm.queueAgeUs;
    expect(balancedWarm.restored && balancedWarm.confidence == 900 &&
               balancedWarm.queueAgeUs >= 7300 &&
               policyDeltaUs <= 100,
           "Balanced must use nearly all high-refresh cached protection without a smoothness gap");
    auto preparedWarm = target(balanced, false, sourceUs, floorUs, 4500);
    expect(preparedWarm.queueAgeUs == sourceUs &&
               preparedWarm.preparationFloorUs == 4500 &&
               preparedWarm.protectionReserveUs == sourceUs - 4500,
           "a trusted high-refresh cache must fill the available one-frame envelope after preparation");

    VrrQueueAgeController lowRate(4500, false);
    expect(lowRate.restoreModel(highRefreshCache),
           "the same cache must restore for the low-rate isolation check");
    auto lowRateTarget = target(lowRate, false, 33333, floorUs);
    expect(lowRateTarget.queueAgeUs == 0 &&
               lowRateTarget.confidence == 0,
           "a high-refresh cache must not keep a low-FPS session sticky");

    VrrQueueAgeController reentry(4500, false);
    expect(reentry.restoreModel(highRefreshCache),
           "the cache re-entry simulation must restore");
    auto beforeDownshift = target(reentry, false, sourceUs, floorUs);
    target(reentry, false, 33333, floorUs);
    reentry.observeWindow(5000, 6000, 0, false, 533328, true,
                          33333, floorUs, false);
    auto lowRelease = target(reentry, false, 33333, floorUs);
    auto highReentry = target(reentry, false, sourceUs, floorUs);
    expect(lowRelease.queueAgeUs == 0 &&
               highReentry.queueAgeUs == beforeDownshift.queueAgeUs,
           "a low-rate release must restore the validated high-refresh cache in one handoff");
}

void testResetLearningClearsPersistentAndTransientState()
{
    VrrQueueAgeController controller(4500, false);
    target(controller, true);
    observeModel(controller, 8, 5000);
    controller.notePressure(2000, kSourceIntervalUs, kServiceFloorUs);
    expect(controller.cacheEntryCount() == 1 &&
               target(controller, true).pressureReserveUs != 0,
           "reset test must begin with learned and transient reserve state");

    controller.resetLearning();
    expect(controller.cacheEntryCount() == 0 &&
               controller.measuredReserveUs() == UINT64_MAX,
           "hard invalidation must discard every persisted model bucket");
    auto cold = target(controller, true);
    expect(cold.modelDemandUs == 2500 &&
               cold.queueAgeUs + cold.headroomCreditUs ==
                   cold.modelDemandUs &&
               cold.pressureReserveUs == 0 && cold.confidence == 0 &&
               !cold.learned && !cold.restored,
           "hard invalidation must clear pressure and return to the headroom-adjusted common prior");
}

void testCacheWarmStartAndInterpolation()
{
    VrrQueueAgeController learned(4500, false);
    target(learned, true);
    observeModel(learned, 10);
    expect(learned.cacheEntryCount() == 1,
           "a confident live headroom model must be persistable");

    VrrQueueAgeController restored(4500, false);
    expect(restored.restoreModel(learned.cacheEntryAt(0)),
           "a valid cached model must restore");
    auto warm = target(restored, true);
    expect(warm.restored && warm.learned &&
               warm.queueAgeUs > 2500 &&
               warm.queueAgeUs < warm.modelDemandUs,
           "a cache must warm-start with authority proportional to restored confidence");
    expect(restored.cacheEntryCount() == 0,
           "a restored model must not refresh its cache before live validation");
    observeModel(restored, 3);
    expect(restored.cacheEntryCount() == 0 && target(restored, true).restored,
           "partial live validation must retain the restored state without saving");
    observeModel(restored, 1);
    expect(restored.cacheEntryCount() == 1 && !target(restored, true).restored,
           "sustained live validation must replace the cache provenance");

    VrrQueueAgeController adjacent(4500, false);
    adjacent.restoreModel(learned.cacheEntryAt(0));
    auto interpolated = target(adjacent, true, 9000, kServiceFloorUs);
    expect(interpolated.restored &&
               interpolated.modelDemandUs == warm.modelDemandUs &&
               interpolated.queueAgeUs < warm.queueAgeUs,
           "a neighboring headroom bucket must borrow the raw model and scale by its own slack");
}

void testSoftDriftInvalidationSelfHeals()
{
    VrrQueueAgeController source(4500, false);
    target(source, true);
    observeModel(source, 10, 5000);
    auto cached = source.cacheEntryAt(0);

    VrrQueueAgeController healed(4500, false);
    healed.restoreModel(cached);
    auto before = target(healed, true);
    observeModel(healed, 3, 1000);
    auto after = target(healed, true);
    expect(after.modelDemandUs < before.modelDemandUs - 1000,
           "three sustained drift windows must move the stale model materially");
    expect(after.confidence < before.confidence && after.restored &&
               healed.cacheEntryCount() == 0,
           "soft invalidation must lower stale cache authority and restart validation");
    expect(after.effectiveDemandUs < before.effectiveDemandUs &&
               after.queueAgeUs <= before.queueAgeUs,
           "self-healing drift must reduce stale reserve demand without an abrupt target rise");

    observeModel(healed, 1, 1000);
    auto firstValidation = target(healed, true);
    expect(firstValidation.restored && healed.cacheEntryCount() == 0 &&
               firstValidation.effectiveDemandUs <= after.effectiveDemandUs,
           "soft invalidation must restart cache validation without restoring stale authority");
    observeModel(healed, 2, 1000);
    expect(target(healed, true).restored && healed.cacheEntryCount() == 0,
           "three post-invalidation windows must not refresh the cache");
    observeModel(healed, 1, 1000);
    expect(!target(healed, true).restored &&
               healed.cacheEntryCount() == 1,
           "four consistent post-invalidation windows must earn live authority and persistence");
}

void testModerateDriftInvalidationSelfHeals()
{
    VrrQueueAgeController source(4500, false);
    target(source, true);
    observeModel(source, 10, 4000);

    VrrQueueAgeController healed(4500, false);
    healed.restoreModel(source.cacheEntryAt(0));
    auto before = target(healed, true);
    observeModel(healed, 3, 2800);
    auto after = target(healed, true);
    expect(after.confidence < before.confidence &&
               after.modelDemandUs < before.modelDemandUs - 500,
           "a sustained moderate drift must invalidate cache authority");

    VrrQueueAgeController healedUp(4500, false);
    healedUp.restoreModel(source.cacheEntryAt(0));
    auto beforeUp = target(healedUp, true);
    observeModel(healedUp, 3, 5200);
    auto afterUp = target(healedUp, true);
    expect(afterUp.confidence < beforeUp.confidence &&
               afterUp.modelDemandUs > beforeUp.modelDemandUs + 500,
           "a sustained moderate upward drift must invalidate cache authority");
}

void testPressureAttackIsEvidenceDrivenAndPolicyAware()
{
    VrrQueueAgeController lowest(2500, false);
    VrrQueueAgeController smoothest(6000, false);
    target(lowest, true);
    target(smoothest, true);
    uint64_t lowRiseUs = lowest.notePressure(
        2000, kSourceIntervalUs, kServiceFloorUs);
    uint64_t smoothRiseUs = smoothest.notePressure(
        2000, kSourceIntervalUs, kServiceFloorUs);
    expect(lowRiseUs > 0 && smoothRiseUs > lowRiseUs,
           "confirmed pressure must scale Smoothest faster than Lowest Latency");
    expect(target(lowest, true).pressureReserveUs <
               target(smoothest, true).pressureReserveUs,
           "policy must control transient pressure, not cached raw demand");
}

void testBacklogReliefOpposesReserveUntilReadinessReturns()
{
    VrrQueueAgeController controller(4500, false);
    target(controller, true);
    observeModel(controller, 4, 6000);
    controller.notePressure(1500, kSourceIntervalUs, kServiceFloorUs);
    auto pressured = target(controller, true);

    uint64_t reliefAddedUs = controller.noteBacklog();
    auto relieved = target(controller, true);
    expect(reliefAddedUs == 500 && relieved.backlogReliefUs == 500 &&
               relieved.pressureReserveUs == 0 &&
               relieved.queueAgeUs < pressured.queueAgeUs,
           "a Balanced backlog drop must trim reserve and clear contradictory pressure");

    controller.notePressure(750, kSourceIntervalUs, kServiceFloorUs);
    auto restored = target(controller, true);
    expect(restored.backlogReliefUs == 0 &&
               restored.pressureReserveUs > 0 &&
               restored.queueAgeUs >= relieved.queueAgeUs,
           "fresh readiness pressure must consume backlog relief before applying positive protection");

    controller.noteBacklog();
    for (int i = 0; i < 4; i++) {
        controller.observeWindow(4000, 5000, 6000, true, 500000, true,
                                 kSourceIntervalUs, kServiceFloorUs, true);
    }
    expect(target(controller, true).backlogReliefUs == 0,
           "clean service must release a Balanced backlog correction over two seconds");
}

void testPressureDoesNotDecayOnBucketNoise()
{
    VrrQueueAgeController controller(4500, false);
    target(controller, true);
    controller.notePressure(2000, kSourceIntervalUs, kServiceFloorUs);
    uint64_t initialPressureUs = target(controller, true).pressureReserveUs;

    target(controller, true, 9000, kServiceFloorUs);
    target(controller, true);
    expect(target(controller, true).pressureReserveUs == initialPressureUs,
           "cadence bucket jitter must not erase active readiness pressure");
}

void testPressureReleaseUsesCadenceHeadroom()
{
    constexpr uint64_t lowRateIntervalUs = 16667;
    VrrQueueAgeController highRate(4500, false);
    VrrQueueAgeController lowRate(4500, false);
    target(highRate, true);
    target(lowRate, false, lowRateIntervalUs, kServiceFloorUs);
    highRate.notePressure(2000, kSourceIntervalUs, kServiceFloorUs);
    lowRate.notePressure(2000, lowRateIntervalUs, kServiceFloorUs);

    uint64_t initialHighPressureUs =
        target(highRate, true).pressureReserveUs;
    uint64_t initialLowPressureUs = target(
        lowRate, false, lowRateIntervalUs,
        kServiceFloorUs).pressureReserveUs;
    highRate.observeWindow(4000, 5000, 0, false, 500000, true,
                           kSourceIntervalUs, kServiceFloorUs, true);
    lowRate.observeWindow(4000, 5000, 0, false, 500000, true,
                          lowRateIntervalUs, kServiceFloorUs, false);
    uint64_t releasedHighPressureUs =
        target(highRate, true).pressureReserveUs;
    uint64_t releasedLowPressureUs = target(
        lowRate, false, lowRateIntervalUs,
        kServiceFloorUs).pressureReserveUs;

    expect(initialHighPressureUs == initialLowPressureUs &&
               initialHighPressureUs - releasedHighPressureUs == 250,
           "high-refresh pressure release must retain the validated Balanced rate");
    expect(initialLowPressureUs - releasedLowPressureUs == 750,
           "a clean 60 FPS window must release transient pressure faster using its service headroom");
}

void testLowRateReleaseUsesElapsedTime()
{
    constexpr uint64_t lowRateIntervalUs = 50000;
    constexpr uint64_t lowRateWindowUs = lowRateIntervalUs * 16;

    VrrQueueAgeController controller(6000, false);
    target(controller, true);
    observeModel(controller, 10, 8000);
    auto transitioned = target(controller, false, lowRateIntervalUs,
                               kServiceFloorUs);
    for (int i = 0; i < 4; i++) {
        controller.observeWindow(5000, 6000, 0, false,
                                 lowRateWindowUs, true,
                                 lowRateIntervalUs, kServiceFloorUs, false);
    }
    auto released = target(controller, false, lowRateIntervalUs,
                           kServiceFloorUs);
    expect(transitioned.queueAgeUs > 1000 &&
               released.queueAgeUs + 280 <= transitioned.queueAgeUs,
           "low-rate release must be time-scaled instead of waiting six long windows");
}

void testTraceDerivedLowRateReleaseSimulation()
{
    // The captured run spent roughly 21 s stepping a 7.05 ms reserve down at
    // 30 FPS. Simulate the same high-to-low timing geometry: one clean low
    // rate window must release the standing reserve, and the cadence-scaled
    // trim must consume the finite phase error in well under half a second.
    constexpr uint64_t highRateUs = 8620;
    constexpr uint64_t lowRateUs = 33333;

    VrrQueueAgeController controller(4500, false);
    target(controller, true, highRateUs, kServiceFloorUs);
    observeModel(controller, 4, 8000, highRateUs, kServiceFloorUs, true);
    auto beforeRelease = target(controller, false, lowRateUs,
                                kServiceFloorUs);
    controller.observeWindow(5000, 6000, 0, false,
                             lowRateUs * 16, true,
                             lowRateUs, kServiceFloorUs, false);
    auto released = target(controller, false, lowRateUs, kServiceFloorUs);
    uint64_t trimStepUs = controller.phaseAdvanceStepLimitUs(
        lowRateUs, kServiceFloorUs);
    uint64_t excessUs = beforeRelease.queueAgeUs > released.queueAgeUs ?
        beforeRelease.queueAgeUs - released.queueAgeUs : 0;
    uint64_t trimFrames = (excessUs + trimStepUs - 1) / trimStepUs;

    expect(beforeRelease.queueAgeUs > released.queueAgeUs &&
               released.queueAgeUs == 0,
           "one clean low-rate window must shed high-refresh standing reserve");
    expect(trimStepUs >= lowRateUs / 40 &&
               trimFrames * lowRateUs <= 500000,
           "30 FPS reserve recovery must complete smoothly within half a second");
}

void testNearCeilingCapConstrainsPressureState()
{
    VrrQueueAgeController controller(6000, false);
    target(controller, true);
    observeModel(controller, 4, 12000);
    auto beforePressure = target(controller, true);
    controller.notePressure(2500, kSourceIntervalUs, kServiceFloorUs);
    auto capped = target(controller, true);
    auto released = target(controller, false);
    expect(capped.queueAgeUs == beforePressure.queueAgeUs &&
               released.queueAgeUs <= capped.queueAgeUs,
           "near-ceiling pressure must not hide reserve above the physical cap");
}

void testBalancedCalibratesQuicklyWithinOneFrame()
{
    VrrQueueAgeController balanced(4500, false);
    VrrQueueAgeController lowest(2500, false);
    uint64_t balancedColdUs = target(balanced, true).queueAgeUs;
    uint64_t lowestColdUs = target(lowest, true).queueAgeUs;

    observeModel(balanced, 1, 5000);
    observeModel(lowest, 1, 5000);
    uint64_t balancedFirstWindowUs = target(balanced, true).queueAgeUs;
    uint64_t lowestFirstWindowUs = target(lowest, true).queueAgeUs;
    expect(balancedFirstWindowUs >= balancedColdUs + 1500 &&
               lowestFirstWindowUs <= lowestColdUs + 150,
           "Balanced must attack after one clean window while Lowest Latency waits for stronger evidence");

    observeModel(balanced, 1, 5000);
    auto converged = target(balanced, true);
    expect(converged.queueAgeUs >= 4500 &&
               converged.queueAgeUs <= kSourceIntervalUs,
           "Balanced must converge materially within one second and remain below one frame");
}

void testBalancedReserveIsCappedAtOneSourceFrame()
{
    const int rates[] = { 80, 90, 100, 105, 110, 116 };
    for (int fps : rates) {
        uint64_t sourceIntervalUs = 1000000ULL / (uint64_t)fps;
        VrrQueueAgeController controller(4500, false);
        target(controller, false, sourceIntervalUs, kServiceFloorUs);
        observeModel(controller, 6, 12000, sourceIntervalUs,
                     kServiceFloorUs, false);
        controller.notePressure(2500, sourceIntervalUs, kServiceFloorUs);
        auto outOfBand = target(controller, false, sourceIntervalUs,
                                kServiceFloorUs);
        auto inBand = target(controller, true, sourceIntervalUs,
                             kServiceFloorUs);
        expect(outOfBand.queueAgeUs <= sourceIntervalUs &&
                   inBand.queueAgeUs <= sourceIntervalUs,
               "cache, learning, and pressure must never request more than one source frame");
    }
}

void testBucketDitherCannotSawtoothAppliedTarget()
{
    auto sourceForHeadroom = [](uint16_t headroom) {
        uint64_t denominator = 1000 - headroom;
        return (kServiceFloorUs * 1000 + denominator - 1) / denominator;
    };

    // The gameplay range on a 120 Hz display spans roughly buckets 0-6.
    for (int lowerBucket = 0; lowerBucket < 6; lowerBucket++) {
        uint16_t lowerPermille = lowerBucket * 50;
        uint16_t upperPermille = (lowerBucket + 1) * 50;
        uint16_t boundary = lowerPermille + 25;
        VrrQueueAgeController controller(4500, false);
        expect(controller.restoreModel(
                   { lowerPermille, 600, 1000, 200 }) &&
                   controller.restoreModel(
                   { upperPermille, 600, 6000, 600 }),
               "rate continuity test must restore adjacent model knots");

        uint64_t slowSideUs = sourceForHeadroom(boundary - 2);
        uint64_t fastSideUs = sourceForHeadroom(boundary + 2);
        uint64_t beforeUs = target(controller, false, slowSideUs,
                                   kServiceFloorUs).queueAgeUs;
        uint64_t maximumStepUs = 0;
        uint64_t previousUs = beforeUs;
        for (int i = 0; i < 40; i++) {
            uint64_t sourceUs = (i & 1) ? slowSideUs : fastSideUs;
            uint64_t currentUs = target(controller, false, sourceUs,
                                        kServiceFloorUs).queueAgeUs;
            uint64_t stepUs = currentUs > previousUs ?
                currentUs - previousUs : previousUs - currentUs;
            maximumStepUs = qMax(maximumStepUs, stepUs);
            previousUs = currentUs;
        }
        expect(maximumStepUs <= 250 && previousUs >= beforeUs,
               "bucket-edge cadence dither must not create repeated opposing target handoffs");
    }
}

void testPhaseMotionScalesWithCeilingHeadroom()
{
    VrrQueueAgeController balanced(4500, false);
    uint64_t nearAdvanceUs = balanced.phaseAdvanceStepLimitUs(
        kSourceIntervalUs, kServiceFloorUs);
    uint64_t farAdvanceUs = balanced.phaseAdvanceStepLimitUs(
        12500, kServiceFloorUs);
    uint64_t nearDelayUs = balanced.phaseDelayStepLimitUs(
        kSourceIntervalUs, kServiceFloorUs);
    uint64_t farDelayUs = balanced.phaseDelayStepLimitUs(
        12500, kServiceFloorUs);
    expect(nearAdvanceUs < farAdvanceUs && nearDelayUs > farDelayUs,
           "Balanced must trim gently but rebuild protection faster near the ceiling");

    VrrQueueAgeController::PhaseDecisionInput input = {};
    input.stats = { 7000, 8000, 12000, 14000 };
    input.targetAgeUs = 1500;
    input.previousTargetAgeUs = 1500;
    input.sourceIntervalUs = kSourceIntervalUs;
    input.maxAdvanceStepUs = nearAdvanceUs;
    input.maxDelayStepUs = nearDelayUs;
    input.sampleCount = 58;
    input.nearCeiling = true;
    input.overfillEligible = true;
    input.targetStable = true;
    auto subFrame = VrrQueueAgeController::decidePhase(input);
    expect(!subFrame.requestOverfillDrop && subFrame.advanceUs > 0 &&
               subFrame.advanceUs <= nearAdvanceUs,
           "a stable sub-frame queue must trim within the headroom-aware cap without dropping");
}

void testHardBoundsAndStaticABMode()
{
    VrrQueueAgeController fastController(6000, false);
    auto fast = fastController.target(true, false, 4167, 4167,
                                      4242, 0, 250, 1300);
    expect(fast.queueAgeUs <= 4167,
           "near-ceiling target must respect the one-source-frame hard cap");

    VrrQueueAgeController staticController(4500, true);
    observeModel(staticController, 16);
    expect(!staticController.target(false, false, 33333,
                                    kDisplayIntervalUs, kServiceFloorUs,
                                    0, kScheduleGuardUs,
                                    kClampZoneUs).learned,
           "force-static mode must ignore learned observations");
    expect(staticController.target(false, false, 33333,
                                   kDisplayIntervalUs, kServiceFloorUs,
                                   0, kScheduleGuardUs,
                                   kClampZoneUs).queueAgeUs == 4500,
           "force-static A/B mode must retain the legacy selected value");

    auto fixed = fastController.target(true, true, kSourceIntervalUs,
                                       kDisplayIntervalUs,
                                       kServiceFloorUs, 0, kScheduleGuardUs,
                                       kClampZoneUs);
    expect(!fixed.learned && fixed.queueAgeUs == kSourceIntervalUs,
           "fixed near target must report and use its interval-sized mode");
}

} // namespace

int main()
{
    testWindowDurationScalesWithCadence();
    testNearCeilingAlignmentSlack();
    testHeadroomScaledCatchUpSpacing();
    testDeepPressureSpendsMoreHeadroomWithoutPanelBursting();
    testCappedRecoverySpacing();
    testRecoverableNearCeilingTransitionDoesNotCoalesce();
    testCatchUpTargetHonorsSmoothSpacing();
    testHeadroomFallbackPeriodAvoidsFloorJudder();
    testRateIdentityKeepsTenFpsBandsSeparate();
    testTearProbeWaitsForSettledTransition();
    testFastCadenceUpshiftAdoption();
    testQuantizedCadenceDoesNotTriggerFastAdoption();
    testModerateCadenceUpshiftUsesPhaseDrift();
    testModerateCadenceDownshiftUsesPhaseDrift();
    testQuantizedCadenceDoesNotTripPhaseDrift();
    testIsolatedHighRateHitchDoesNotTripPhaseDrift();
    testFastCadenceAdoptionRespectsNominalCap();
    testSlowerCadenceDownshiftAdoption();
    testTwentyFpsCadenceLock();
    testLongHostHitchesRemainStalls();
    testRobustWindowStatistics();
    testRenderLeadForgetsIsolatedSpikesButProtectsSustainedTail();
    testArrivalPhaseRtpWrapIsContinuous();
    testPhaseDecisionUsesOneSetpoint();
    testHeadroomContinuouslyReducesReserve();
    testPreparationFloorLeavesRealProtectionAtGameplayRates();
    testPreparationFloorEstimatorRejectsStalls();
    testPolicyControlsSlewRatherThanPadding();
    testNearCeilingEntryAddsNoAutomaticPadding();
    testStableAndUnavailableArrivalEvidence();
    testReadinessPressureRequiresASetpointMiss();
    testPolicySelectionUsesLegacyValuesAsDynamicsOnly();
    testCacheValidationAndConfidenceCap();
    testCacheBucketInterpolation();
    testHighRefreshCachePrioritizesBalancedWarmStart();
    testResetLearningClearsPersistentAndTransientState();
    testCacheWarmStartAndInterpolation();
    testSoftDriftInvalidationSelfHeals();
    testModerateDriftInvalidationSelfHeals();
    testPressureAttackIsEvidenceDrivenAndPolicyAware();
    testBacklogReliefOpposesReserveUntilReadinessReturns();
    testPressureDoesNotDecayOnBucketNoise();
    testPressureReleaseUsesCadenceHeadroom();
    testLowRateReleaseUsesElapsedTime();
    testTraceDerivedLowRateReleaseSimulation();
    testNearCeilingCapConstrainsPressureState();
    testBalancedCalibratesQuicklyWithinOneFrame();
    testBalancedReserveIsCappedAtOneSourceFrame();
    testBucketDitherCannotSawtoothAppliedTarget();
    testPhaseMotionScalesWithCeilingHeadroom();
    testHardBoundsAndStaticABMode();

    if (failures != 0) {
        std::fprintf(stderr, "%d VRR queue-age test(s) failed\n", failures);
        return 1;
    }
    std::puts("VRR queue-age controller tests passed");
    return 0;
}
