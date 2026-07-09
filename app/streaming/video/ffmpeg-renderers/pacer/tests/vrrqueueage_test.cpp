#include "../vrrqueueage.h"

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
