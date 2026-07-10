#include "../vrrqueueage.h"
#include "../vrrcadence.h"

#include <cstdio>

namespace {

constexpr uint64_t kSourceIntervalUs = 8620;
constexpr uint64_t kDisplayIntervalUs = 8333;
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

void observeLowSpread(VrrQueueAgeController& controller, int windows,
                      bool nearCeiling = false)
{
    for (int i = 0; i < windows; i++) {
        controller.observeWindow(5000, 6000, true, nearCeiling);
    }
}

VrrQueueAgeController::Target target(VrrQueueAgeController& controller,
                                     bool nearCeiling)
{
    return controller.target(nearCeiling, false, kSourceIntervalUs,
                             kDisplayIntervalUs, kScheduleGuardUs,
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

void testSmoothHighRateCatchUpSpacing()
{
    constexpr uint64_t displayIntervalUs = 8333;
    constexpr uint64_t flipFloorUs = 8483;
    constexpr uint64_t highRateSourceUs = 11025;

    uint64_t smoothSpacingUs = vrrCatchUpSpacingUs(
        flipFloorUs, highRateSourceUs, displayIntervalUs, false, true);
    expect(smoothSpacingUs == 9646,
           "smooth high-rate recovery should drain at seven-eighths cadence");
    expect(vrrCadenceAlignmentSlackUs(
               highRateSourceUs, displayIntervalUs, 199,
               smoothSpacingUs, 49) == 1330,
           "gentle catch-up spacing must reduce the remaining align budget");

    expect(vrrCatchUpSpacingUs(
               flipFloorUs, highRateSourceUs, displayIntervalUs,
               false, false) == flipFloorUs,
           "disabled smooth recovery should retain the fastest drain");
    expect(vrrCatchUpSpacingUs(
               flipFloorUs, 33333, displayIntervalUs,
               false, true) == flipFloorUs,
           "low-FPS upshifts should retain the fastest drain");
    expect(vrrCatchUpSpacingUs(
               flipFloorUs, 33333, displayIntervalUs,
               true, false) == 29166,
           "latched recovery should keep its rate-independent gentle drain");
    expect(vrrCatchUpSpacingUs(
               flipFloorUs, 9100, displayIntervalUs,
               false, true) == flipFloorUs,
           "near-ceiling recovery should remain bounded by the flip floor");

    uint64_t fullGentleLimitUs = (displayIntervalUs * 4 + 2) / 3;
    uint64_t fullFloorLimitUs = (displayIntervalUs * 8 + 4) / 5;
    uint64_t atGentleLimitUs = vrrCatchUpSpacingUs(
        flipFloorUs, fullGentleLimitUs, displayIntervalUs, false, true);
    uint64_t justInsideBlendUs = vrrCatchUpSpacingUs(
        flipFloorUs, fullGentleLimitUs + 1,
        displayIntervalUs, false, true);
    uint64_t justBeforeFloorUs = vrrCatchUpSpacingUs(
        flipFloorUs, fullFloorLimitUs - 1,
        displayIntervalUs, false, true);
    expect(justInsideBlendUs <= atGentleLimitUs &&
               atGentleLimitUs - justInsideBlendUs <= 4,
           "the high-rate recovery boundary must not jump pacing phase");
    expect(justBeforeFloorUs >= flipFloorUs &&
               justBeforeFloorUs - flipFloorUs <= 4 &&
               vrrCatchUpSpacingUs(
                   flipFloorUs, fullFloorLimitUs, displayIntervalUs,
                   false, true) == flipFloorUs,
           "the low-rate edge must blend continuously back to fast recovery");
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
    input.nearCeiling = false;
    expect(VrrQueueAgeController::decidePhase(input).delayUs == 0,
           "out-of-band delivery must not build a standing queue");

    input.nearCeiling = true;
    input.windowTainted = true;
    input.stats = { 3500, 5000, 12000, 16000 };
    expect(VrrQueueAgeController::decidePhase(input).advanceUs > 0,
           "taint must freeze learning and build, not excess-latency trim");

    input.windowTainted = false;
    input.overfillEligible = true;
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

void testFastAttackAndBoundedRelease()
{
    VrrQueueAgeController controller(6000, false);
    observeLowSpread(controller, 8);
    expect(target(controller, false).queueAgeUs == 1750,
           "eight clean windows should learn spread plus guard");

    controller.observeWindow(3000, 8000, true, false);
    expect(target(controller, false).queueAgeUs == 5750,
           "larger measured variation should attack immediately");

    for (int i = 0; i < 23; i++) {
        controller.observeWindow(5000, 6000, true, false);
    }
    uint64_t beforeEvictionUs = target(controller, false).queueAgeUs;
    controller.observeWindow(5000, 6000, true, false);
    uint64_t afterEvictionUs = target(controller, false).queueAgeUs;
    expect(beforeEvictionUs >= afterEvictionUs &&
               beforeEvictionUs - afterEvictionUs <= 250,
           "evicting a high-spread sample must release at a bounded rate");

    uint64_t previousUs = afterEvictionUs;
    for (int i = 0; i < 20; i++) {
        controller.observeWindow(5000, 6000, true, false);
        uint64_t currentUs = target(controller, false).queueAgeUs;
        expect(previousUs >= currentUs && previousUs - currentUs <= 250,
               "every measured-reserve release step must remain bounded");
        previousUs = currentUs;
    }
    expect(previousUs == 1750,
           "bounded release should still converge to the measured reserve");
}

void testColdEntryConvergesAfterEightCleanWindows()
{
    VrrQueueAgeController controller(6000, false);
    controller.enterNearCeiling(kSourceIntervalUs);

    observeLowSpread(controller, 7, true);
    expect(!target(controller, true).learned,
           "cold entry must retain protection for seven clean windows");
    observeLowSpread(controller, 1, true);
    expect(target(controller, true).learned,
           "cold entry must become learned on the eighth clean window");
}

void testTaintFreezesUntilFreshAcquisition()
{
    VrrQueueAgeController controller(6000, false);
    observeLowSpread(controller, 8);
    controller.enterNearCeiling(kSourceIntervalUs);

    // Clear recovery hold, release delay, and the quarter-interval reserve.
    observeLowSpread(controller, 24, true);
    auto settled = target(controller, true);
    uint64_t settledMeasuredReserveUs = controller.measuredReserveUs();
    expect(settled.learned && settled.safetyReserveUs == 0,
           "stable near-ceiling delivery should release acquisition reserve");

    for (int i = 0; i < 12; i++) {
        controller.observeWindow(UINT64_MAX, 0, false, true);
    }
    auto frozen = target(controller, true);
    expect(frozen.learned && frozen.queueAgeUs == settled.queueAgeUs,
           "unusable evidence should freeze rather than inflate the target");
    expect(controller.measuredReserveUs() == settledMeasuredReserveUs &&
               frozen.safetyReserveUs == settled.safetyReserveUs,
           "unusable evidence must not mutate learned or safety reserves");

    controller.leaveNearCeiling();
    controller.enterNearCeiling(kSourceIntervalUs);
    auto recovered = target(controller, true);
    expect(!recovered.learned && recovered.safetyReserveUs > 0,
           "a fresh near-ceiling acquisition should restore protection");

    observeLowSpread(controller, 7, true);
    expect(!target(controller, true).learned,
           "recovery protection should hold for seven clean windows");
    observeLowSpread(controller, 1, true);
    expect(target(controller, true).learned,
           "the eighth clean window may return to learned policy");
}

void testRateResetAndPresetSeparation()
{
    VrrQueueAgeController lowest(2500, false);
    VrrQueueAgeController balanced(4500, false);
    VrrQueueAgeController smoothest(6000, false);
    observeLowSpread(lowest, 8);
    observeLowSpread(balanced, 8);
    observeLowSpread(smoothest, 8);
    lowest.enterNearCeiling(kSourceIntervalUs);
    balanced.enterNearCeiling(kSourceIntervalUs);
    smoothest.enterNearCeiling(kSourceIntervalUs);
    observeLowSpread(lowest, 24, true);
    observeLowSpread(balanced, 24, true);
    observeLowSpread(smoothest, 24, true);

    uint64_t lowUs = target(lowest, true).queueAgeUs;
    uint64_t balancedUs = target(balanced, true).queueAgeUs;
    uint64_t smoothUs = target(smoothest, true).queueAgeUs;
    expect(lowUs < balancedUs && balancedUs < smoothUs,
           "all three policies should remain distinct after learning");
    expect(target(lowest, true).phaseReserveUs <= 2500 &&
               target(balanced, true).phaseReserveUs <= 4500 &&
               target(smoothest, true).phaseReserveUs <= 6000,
           "steady phase reserve must remain inside each selected policy");

    smoothest.observeWindow(3000, 8000, true, false);
    smoothest.resetLearning();
    smoothest.enterNearCeiling(kSourceIntervalUs);
    auto reset = target(smoothest, true);
    expect(!reset.learned && reset.safetyReserveUs > 0,
           "a material rate reset should reacquire conservative protection");
    observeLowSpread(smoothest, 8, true);
    expect(smoothest.measuredReserveUs() == 1750,
           "a rate reset must discard the previous spread ring");
}

void testBoundsAndLowFpsHeadroom()
{
    VrrQueueAgeController controller(6000, false);
    observeLowSpread(controller, 8);
    uint64_t lowFpsTargetUs = controller.target(
        false, false, 33333, kDisplayIntervalUs,
        kScheduleGuardUs, kClampZoneUs).queueAgeUs;
    uint64_t highFpsTargetUs = controller.target(
        false, false, 8620, kDisplayIntervalUs,
        kScheduleGuardUs, kClampZoneUs).queueAgeUs;
    expect(lowFpsTargetUs == highFpsTargetUs,
           "display headroom must not make the out-of-band target scale with FPS");

    VrrQueueAgeController fastController(6000, false);
    fastController.enterNearCeiling(4167);
    auto fast = fastController.target(true, false, 4167, 4167, 250, 1300);
    expect(fast.queueAgeUs <= 4417,
           "near-ceiling target must respect the interval-relative hard cap");

    VrrQueueAgeController staticController(4500, true);
    observeLowSpread(staticController, 16);
    expect(!staticController.target(false, false, 33333,
                                    kDisplayIntervalUs, kScheduleGuardUs,
                                    kClampZoneUs).learned,
           "force-static mode must ignore learned observations");
    expect(staticController.target(false, false, 33333,
                                   kDisplayIntervalUs, kScheduleGuardUs,
                                   kClampZoneUs).queueAgeUs == 4500,
           "force-static out-of-band target must stay at the policy value");

    auto fixed = controller.target(true, true, kSourceIntervalUs,
                                   kDisplayIntervalUs, kScheduleGuardUs,
                                   kClampZoneUs);
    expect(!fixed.learned && fixed.queueAgeUs == 9120,
           "fixed near target must report and use its interval-sized mode");
}

} // namespace

int main()
{
    testWindowDurationScalesWithCadence();
    testNearCeilingAlignmentSlack();
    testSmoothHighRateCatchUpSpacing();
    testCatchUpTargetHonorsSmoothSpacing();
    testHeadroomFallbackPeriodAvoidsFloorJudder();
    testRateIdentityKeepsTenFpsBandsSeparate();
    testTearProbeWaitsForSettledTransition();
    testFastCadenceUpshiftAdoption();
    testQuantizedCadenceDoesNotTriggerFastAdoption();
    testFastCadenceAdoptionRespectsNominalCap();
    testSlowerCadenceDownshiftAdoption();
    testRobustWindowStatistics();
    testPhaseDecisionUsesOneSetpoint();
    testFastAttackAndBoundedRelease();
    testColdEntryConvergesAfterEightCleanWindows();
    testTaintFreezesUntilFreshAcquisition();
    testRateResetAndPresetSeparation();
    testBoundsAndLowFpsHeadroom();

    if (failures != 0) {
        std::fprintf(stderr, "%d VRR queue-age test(s) failed\n", failures);
        return 1;
    }
    std::puts("VRR queue-age controller tests passed");
    return 0;
}
