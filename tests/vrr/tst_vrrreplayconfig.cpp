#include "vrrreplayconfig.h"
#include "vrrreplaymodel.h"

#include <QJsonDocument>
#include <QProcess>
#include <QtTest>

#include <limits>

class VrrReplayConfigTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsRoundTrip();
    void nativeHitchPolicyRoundTrip();
    void dxgiFeedbackPolicyRoundTrip();
    void dxgiVrr12ProtectionRoundTrip();
    void smoothedHitchReferenceRoundTrip();
    void divergentDeadlineRetainsSummary();
    void inheritanceAndOverride();
    void controllerSnapshotIsAtomic();
    void rejectsInvalidInput();
    void rasterEnvelope();
    void rasterProbeOverheadRemoval();
    void rasterScanLineScaleInference();
    void rasterScanLineAudit();
    void rasterSyncAnchorMerge();
    void freeRunningRefreshTimeline();
    void rawQpcTranslation();
    void gpuCompletionBounds();
    void gpuReadyOperationAudit();
    void gpuReadyStageTimingAudit();
    void presenterSubmissionAudit();
    void spacingCorrectionAudit();
    void spacingLifecycleTimingAudit();
    void postPresentQueryTimingAudit();
    void wakeDelayInjectionEligibility();
    void waitLifecycleAudit();
    void dxgiCapabilityAudit();
    void periodicInjectionSelector();
    void rationalDisplayTiming();
};

void VrrReplayConfigTest::defaultsRoundTrip()
{
    VrrTimingParameters productionParameters;
    QCOMPARE(productionParameters.playoutNativeHitchAdaptation, uint64_t(0));
    QCOMPARE(productionParameters.playoutPreserveDxgiFeedback, uint64_t(0));
    QCOMPARE(productionParameters.dxgiVrr12Protection, uint64_t(0));
    productionParameters.playoutSmoothingSnapPerMille = 3000;
    QString validationError;
    QVERIFY2(validateVrrTimingParameters(
        productionParameters, validationError), qPrintable(validationError));

    VrrReplayConfiguration config;
    QString error;
    QVERIFY2(loadVrrReplayConfiguration(
        QJsonDocument(vrrDefaultReplayConfigurationJson()).toJson(),
        config, error), qPrintable(error));
    QCOMPARE(config.scenarios.size(), 1);
    QCOMPARE(config.scenarios.front().controller.renderLeadFloorUs,
             uint64_t(1000));
    QCOMPARE(config.scenarios.front().controller.renderLeadCeilingUs,
             uint64_t(0));
    QCOMPARE(config.scenarios.front().controller.renderBaselinePercentile,
             50U);
    QCOMPARE(config.scenarios.front().controller.pacingLatencyBudgetDivisor,
              uint64_t(2));
    QCOMPARE(
        config.scenarios.front().controller.
            pacingLatencyExtraPeriodNumerator,
        uint64_t(0));
    QCOMPARE(config.scenarios.front().controller.sourcePlayoutDelayUs,
              uint64_t(0));
    QCOMPARE(config.scenarios.front().controller.timestampPlayoutEnabled,
              uint64_t(0));
    QCOMPARE(config.scenarios.front().controller.playoutOffsetWindowUs,
              uint64_t(3000000));
    QCOMPARE(config.scenarios.front().controller.playoutOffsetSlewUs,
              uint64_t(20));
    QCOMPARE(config.scenarios.front().controller.playoutDelayAdaptive,
              uint64_t(0));
    QCOMPARE(config.scenarios.front().controller.playoutDelayPercentilePerMille,
              uint64_t(980));
    QCOMPARE(config.scenarios.front().controller.playoutBandWidthHz,
              uint64_t(20));
    QCOMPARE(config.scenarios.front().controller.readinessLearningWindowUs,
              uint64_t(0));
    QCOMPARE(
        config.scenarios.front().controller.readinessPeriodFloorDenominator,
        uint64_t(1));
    QCOMPARE(config.scenarios.front().controller.retainReadinessOnPhaseReset,
              uint64_t(0));
    QCOMPARE(config.scenarios.front().worker.queueCapacity, size_t(3));
    QCOMPARE(config.scenarios.front().display.calibrationConfirmed, 0U);
    QCOMPARE(config.scenarios.front().display.scanoutPeriodPs, uint64_t(0));
    QCOMPARE(config.scenarios.front().display.activeScanoutPs, uint64_t(0));
    QCOMPARE(config.scenarios.front().display.activeScanoutPercent, 95U);
    QCOMPARE(
        config.scenarios.front().display.syncToActiveScanoutUs,
        uint64_t(0));
    QCOMPARE(config.scenarios.front().display.phaseUncertaintyUs,
             uint64_t(250));
    QCOMPARE(config.scenarios.front().execution.decisionDelayUs,
             uint64_t(0));
    QCOMPARE(config.scenarios.front().execution.renderWakeDelayUs,
             uint64_t(0));
    QCOMPARE(config.scenarios.front().execution.targetWakeDelayUs,
             uint64_t(0));
    QCOMPARE(config.scenarios.front().execution.spacingGuardFeedbackUs,
             uint64_t(0));
    QCOMPARE(config.scenarios.front().execution.displayTransitionDelayUs,
             uint64_t(0));
    QCOMPARE(
        config.scenarios.front().execution.periodicStallBurstFrames,
        uint64_t(1));
    QCOMPARE(config.scenarios.front().execution.submissionAdvanceUs,
             uint64_t(0));
    QCOMPARE(
        config.scenarios.front().execution.
            removePrePresentRasterProbeOverhead,
        0U);
    QVERIFY(vrrReplayParameterNames().contains("controller.guard_step_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.render_baseline_percentile"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.pacing_latency_budget_divisor"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.pacing_latency_extra_period_numerator"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.source_playout_delay_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.readiness_learning_window_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.readiness_floor_period_numerator"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.retain_readiness_on_phase_reset"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.timestamp_playout_enabled"));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.playout_offset_slew_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "display.active_scanout_percent"));
    QVERIFY(vrrReplayParameterNames().contains(
        "display.calibration_confirmed"));
    QVERIFY(vrrReplayParameterNames().contains(
        "display.scanout_period_ps"));
    QVERIFY(vrrReplayParameterNames().contains(
        "display.active_scanout_ps"));
    QVERIFY(vrrReplayParameterNames().contains(
        "display.sync_to_active_scanout_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.periodic_stall_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.render_wake_delay_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.target_wake_delay_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.spacing_guard_feedback_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.display_transition_delay_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.periodic_stall_phase_frames"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.periodic_stall_burst_frames"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.periodic_preparation_stall_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.periodic_submission_stall_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.periodic_display_transition_delay_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.submission_advance_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.remove_pre_present_raster_probe_overhead"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.periodic_submission_advance_us"));
    QVERIFY(vrrReplayParameterNames().contains(
        "execution.periodic_spacing_guard_feedback_us"));
}

void VrrReplayConfigTest::inheritanceAndOverride()
{
    const QByteArray json = R"json({
      "config_schema": 1,
      "parameters": {
        "controller": {"guard_step_us": 75},
        "display": {"active_scanout_percent": 92},
        "execution": {"preparation_delay_us": 40}
      },
      "scenarios": [{
        "name": "wide-queue",
        "mode": "worker",
        "parameters": {
          "worker": {"queue_capacity": 6},
          "display": {"present_transport_us": 120},
          "execution": {
            "render_wake_delay_us": 125,
            "target_wake_delay_us": 175,
            "spacing_guard_feedback_us": 20,
            "periodic_stall_every_frames": 60,
            "periodic_stall_phase_frames": 7,
            "periodic_stall_burst_frames": 3,
            "periodic_stall_us": 2000,
            "periodic_render_wake_delay_us": 225,
            "periodic_target_wake_delay_us": 275,
            "periodic_preparation_stall_us": 500,
            "periodic_submission_stall_us": 750,
            "periodic_display_transition_delay_us": 875,
            "periodic_spacing_guard_feedback_us": 30,
            "periodic_submission_advance_us": 4000,
            "submission_advance_us": 125,
            "remove_pre_present_raster_probe_overhead": 1,
            "display_transition_delay_us": 90
          }
        },
        "assertions": [{
          "metric": "simulation.tear.modelled_interval_violations",
          "operator": "<=",
          "value": 0
        }]
      }]
    })json";
    VrrReplayConfiguration config;
    QString error;
    QVERIFY2(loadVrrReplayConfiguration(json, config, error),
             qPrintable(error));
    QCOMPARE(config.scenarios.front().controller.guardStepUs, uint64_t(75));
    QCOMPARE(config.scenarios.front().worker.queueCapacity, size_t(6));
    QCOMPARE(config.scenarios.front().display.activeScanoutPercent, 92U);
    QCOMPARE(config.scenarios.front().display.presentTransportUs,
             uint64_t(120));
    QCOMPARE(config.scenarios.front().execution.preparationDelayUs,
             uint64_t(40));
    QCOMPARE(config.scenarios.front().execution.renderWakeDelayUs,
             uint64_t(125));
    QCOMPARE(config.scenarios.front().execution.targetWakeDelayUs,
             uint64_t(175));
    QCOMPARE(config.scenarios.front().execution.spacingGuardFeedbackUs,
             uint64_t(20));
    QCOMPARE(config.scenarios.front().execution.periodicStallEveryFrames,
             uint64_t(60));
    QCOMPARE(config.scenarios.front().execution.periodicStallPhaseFrames,
             uint64_t(7));
    QCOMPARE(config.scenarios.front().execution.periodicStallBurstFrames,
             uint64_t(3));
    QCOMPARE(
        config.scenarios.front().execution.periodicRenderWakeDelayUs,
        uint64_t(225));
    QCOMPARE(
        config.scenarios.front().execution.periodicTargetWakeDelayUs,
        uint64_t(275));
    QCOMPARE(
        config.scenarios.front().execution.periodicPreparationStallUs,
        uint64_t(500));
    QCOMPARE(
        config.scenarios.front().execution.periodicSubmissionStallUs,
        uint64_t(750));
    QCOMPARE(
        config.scenarios.front().execution.
            periodicDisplayTransitionDelayUs,
        uint64_t(875));
    QCOMPARE(
        config.scenarios.front().execution.
            periodicSpacingGuardFeedbackUs,
        uint64_t(30));
    QCOMPARE(
        config.scenarios.front().execution.periodicSubmissionAdvanceUs,
        uint64_t(4000));
    QCOMPARE(config.scenarios.front().execution.submissionAdvanceUs,
             uint64_t(125));
    QCOMPARE(
        config.scenarios.front().execution.
            removePrePresentRasterProbeOverhead,
        1U);
    QCOMPARE(config.scenarios.front().execution.displayTransitionDelayUs,
             uint64_t(90));
    QCOMPARE(config.scenarios.front().assertions.size(), 1);
    QVERIFY2(applyVrrReplayOverride("controller.guard_step_us=125",
                                   config.scenarios.front(), error),
             qPrintable(error));
    QCOMPARE(config.scenarios.front().controller.guardStepUs, uint64_t(125));
    QVERIFY2(applyVrrReplayOverride("display.phase_uncertainty_us=80",
                                   config.scenarios.front(), error),
             qPrintable(error));
    QCOMPARE(config.scenarios.front().display.phaseUncertaintyUs,
             uint64_t(80));
    QVERIFY2(applyVrrReplayOverride("display.scanout_period_us=8333",
                                   config.scenarios.front(), error),
             qPrintable(error));
    QVERIFY2(applyVrrReplayOverride("display.scanout_period_ps=8333333333",
                                   config.scenarios.front(), error),
             qPrintable(error));
    QVERIFY2(applyVrrReplayOverride("display.active_scanout_us=7900",
                                   config.scenarios.front(), error),
             qPrintable(error));
    QVERIFY2(applyVrrReplayOverride(
                 "display.sync_to_active_scanout_us=100",
                 config.scenarios.front(), error),
             qPrintable(error));
    QVERIFY2(applyVrrReplayOverride("display.calibration_confirmed=1",
                                   config.scenarios.front(), error),
             qPrintable(error));
    QCOMPARE(config.scenarios.front().display.calibrationConfirmed, 1U);
    QCOMPARE(config.scenarios.front().display.scanoutPeriodPs,
             uint64_t(8333333333));
    QCOMPARE(
        config.scenarios.front().display.syncToActiveScanoutUs,
        uint64_t(100));
    QVERIFY2(applyVrrReplayOverride(
                 "execution.display_transition_delay_us=110",
                 config.scenarios.front(), error),
             qPrintable(error));
    QCOMPARE(config.scenarios.front().execution.displayTransitionDelayUs,
             uint64_t(110));
    QVERIFY2(applyVrrReplayOverride(
                 "execution.remove_pre_present_raster_probe_overhead=0",
                 config.scenarios.front(), error),
             qPrintable(error));
    QCOMPARE(
        config.scenarios.front().execution.
            removePrePresentRasterProbeOverhead,
        0U);
}

void VrrReplayConfigTest::rejectsInvalidInput()
{
    VrrReplayConfiguration config;
    QString error;
    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"controller":{"unknown":1}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("unknown controller parameter"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"controller":{"base_guard_divisor":0}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("denominators"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"controller":{"latch_base_guard_exit":2}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("latch_base_guard_exit"));

    QVERIFY2(loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"controller":{"pacing_latency_budget_divisor":0}},"scenarios":[{"name":"x"}]})",
        config, error), qPrintable(error));
    QCOMPARE(config.scenarios.front().controller.pacingLatencyBudgetDivisor,
             uint64_t(0));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"controller":{"render_baseline_percentile":100,"preparation_percentile":99}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("render percentiles"));

    VrrReplayScenario scenario;
    QVERIFY(!applyVrrReplayOverride("guard_step_us=25", scenario, error));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"active_scanout_percent":0}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("active_scanout_percent"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"calibration_confirmed":1}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("calibration requires"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"scanout_period_us":1000,"phase_uncertainty_us":501}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("half scanout_period_us"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"scanout_period_us":1000,"sync_to_active_scanout_us":1000}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("sync_to_active_scanout_us"));

    QVERIFY(loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"scanout_period_us":1000,"active_scanout_us":900,"sync_to_active_scanout_us":200}},"scenarios":[{"name":"x"}]})",
        config, error));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"scanout_period_us":1000,"scanout_period_ps":2000000000}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("scanout_period_ps"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"scanout_period_us":8333,"scanout_period_ps":8332600000,"active_scanout_us":8333}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("fit within scanout_period_ps"));

    QVERIFY2(loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"active_scanout_ps":8000000000}},"scenarios":[{"name":"x"}]})",
        config, error), qPrintable(error));
    QCOMPARE(
        config.scenarios.front().display.activeScanoutPs,
        uint64_t(8000000000ULL));

    QVERIFY2(loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"calibration_confirmed":1,"scanout_period_ps":8333333333,"active_scanout_ps":7999057239}},"scenarios":[{"name":"x"}]})",
        config, error), qPrintable(error));
    QCOMPARE(config.scenarios.front().display.calibrationConfirmed, 1U);

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"display":{"scanout_period_ps":10000000000,"active_scanout_us":7000,"active_scanout_ps":8000000000}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("active_scanout_ps must agree"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"execution":{"periodic_stall_burst_frames":0}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("periodic_stall_burst_frames"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"execution":{"remove_pre_present_raster_probe_overhead":2}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains(
        "remove_pre_present_raster_probe_overhead"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"execution":{"periodic_stall_every_frames":60,"periodic_stall_phase_frames":60,"periodic_stall_us":1}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("phase"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"parameters":{"execution":{"periodic_stall_every_frames":60,"periodic_stall_burst_frames":61,"periodic_stall_us":1}},"scenarios":[{"name":"x"}]})",
        config, error));
    QVERIFY(error.contains("burst"));

    QVERIFY(!loadVrrReplayConfiguration(
        R"({"config_schema":1,"scenarios":[{"name":"x","assertions":[{"metric":"simulation.drops","operator":"==","value":0,"typo":1}]}]})",
        config, error));
    QVERIFY(error.contains("unknown scenario assertion key"));
}

void VrrReplayConfigTest::controllerSnapshotIsAtomic()
{
    VrrTimingParameters parameters;
    QJsonObject snapshot;
    snapshot["latch_enter_headroom_us"] = 1500;
    snapshot["latch_exit_headroom_us"] = 2000;
    snapshot["latch_base_guard_exit"] = 1;
    snapshot["latch_enter_headroom_period_numerator"] = 3;
    snapshot["latch_enter_headroom_period_denominator"] = 1;
    snapshot["latch_exit_headroom_period_numerator"] = 13;
    snapshot["latch_exit_headroom_period_denominator"] = 4;

    QString error;
    QVERIFY2(applyVrrReplayControllerSnapshot(
                 snapshot, parameters, error), qPrintable(error));
    QCOMPARE(parameters.latchedPresentationHeadroomUs, uint64_t(1500));
    QCOMPARE(parameters.latchedPresentationExitHeadroomUs, uint64_t(2000));
    QCOMPARE(parameters.latchedPresentationBaseGuardExit, uint64_t(1));
    QCOMPARE(parameters.latchedPresentationHeadroomPeriodNumerator,
             uint64_t(3));
    QCOMPARE(parameters.latchedPresentationExitHeadroomPeriodNumerator,
             uint64_t(13));
}

void VrrReplayConfigTest::nativeHitchPolicyRoundTrip()
{
    VrrTimingParameters parameters;
    parameters.playoutReadinessDrivenAdaptation = 1;
    parameters.playoutSmoothnessFeedbackEnabled = 1;
    parameters.playoutPredictionEnabled = 1;
    parameters.playoutHistoryEnabled = 1;
    parameters.timestampPlayoutEnabled = 1;
    parameters.playoutDelayAdaptive = 1;
    QString error;
    QJsonObject snapshot{{"playout_native_hitch_adaptation", 1}};
    QVERIFY2(applyVrrReplayControllerSnapshot(snapshot, parameters, error), qPrintable(error));
    QCOMPARE(parameters.playoutNativeHitchAdaptation, uint64_t(1));
    snapshot["playout_native_hitch_adaptation"] = 2;
    QVERIFY(!applyVrrReplayControllerSnapshot(snapshot, parameters, error));
    QCOMPARE(parameters.playoutNativeHitchAdaptation, uint64_t(1));
    parameters.playoutSmoothnessFeedbackEnabled = 0;
    QVERIFY(!validateVrrTimingParameters(parameters, error));
}

void VrrReplayConfigTest::dxgiFeedbackPolicyRoundTrip()
{
    QString error;
    for (uint64_t enabled : {0ULL, 1ULL}) {
        VrrTimingParameters parameters;
        parameters.playoutPreserveDxgiFeedback = enabled;
        const auto snapshot = vrrTimingParametersToJson(parameters);
        QCOMPARE(snapshot.value("playout_preserve_dxgi_feedback").toInteger(),
                 qint64(enabled));
        VrrTimingParameters restored;
        QVERIFY2(applyVrrReplayControllerSnapshot(snapshot, restored, error),
                 qPrintable(error));
        QCOMPARE(restored.playoutPreserveDxgiFeedback, enabled);
    }

    VrrTimingParameters parameters;
    parameters.playoutPreserveDxgiFeedback = 1;
    const auto unchanged = vrrTimingParametersToJson(parameters);
    QVERIFY(!applyVrrReplayControllerSnapshot(
        QJsonObject{{"playout_preserve_dxgi_feedback", 2}}, parameters, error));
    QCOMPARE(vrrTimingParametersToJson(parameters), unchanged);

    // Older snapshots/configs omit the field and retain the legacy policy.
    auto legacySnapshot = unchanged;
    legacySnapshot.remove("playout_preserve_dxgi_feedback");
    VrrTimingParameters legacy;
    QVERIFY2(applyVrrReplayControllerSnapshot(legacySnapshot, legacy, error),
             qPrintable(error));
    QCOMPARE(legacy.playoutPreserveDxgiFeedback, uint64_t(0));
    auto root = vrrDefaultReplayConfigurationJson();
    auto sections = root.value("parameters").toObject();
    sections["controller"] = legacySnapshot;
    root["parameters"] = sections;
    VrrReplayConfiguration config;
    QVERIFY2(loadVrrReplayConfiguration(QJsonDocument(root).toJson(), config, error),
             qPrintable(error));
    QCOMPARE(config.scenarios.front().controller.playoutPreserveDxgiFeedback,
             uint64_t(0));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.playout_preserve_dxgi_feedback"));
}

void VrrReplayConfigTest::dxgiVrr12ProtectionRoundTrip()
{
    QString error;
    VrrTimingParameters parameters;
    parameters.playoutHistoryEnabled = 1;
    parameters.timestampPlayoutEnabled = 1;
    parameters.playoutDelayAdaptive = 1;
    for (uint64_t enabled : {0ULL, 1ULL}) {
        parameters.dxgiVrr12Protection = enabled;
        const auto snapshot = vrrTimingParametersToJson(parameters);
        QCOMPARE(snapshot.value("dxgi_vrr12_protection").toInteger(), qint64(enabled));
        VrrTimingParameters restored;
        QVERIFY2(applyVrrReplayControllerSnapshot(snapshot, restored, error), qPrintable(error));
        QCOMPARE(vrrTimingParametersToJson(restored), snapshot);
    }
    const auto unchanged = vrrTimingParametersToJson(parameters);
    QVERIFY(!applyVrrReplayControllerSnapshot(
        QJsonObject{{"dxgi_vrr12_protection", 2}}, parameters, error));
    QCOMPARE(vrrTimingParametersToJson(parameters), unchanged);
    auto legacySnapshot = unchanged;
    legacySnapshot.remove("dxgi_vrr12_protection");
    VrrTimingParameters legacy;
    QVERIFY2(applyVrrReplayControllerSnapshot(legacySnapshot, legacy, error), qPrintable(error));
    QCOMPARE(legacy.dxgiVrr12Protection, uint64_t(0));
    auto root = vrrDefaultReplayConfigurationJson();
    auto sections = root.value("parameters").toObject();
    sections["controller"] = legacySnapshot;
    root["parameters"] = sections;
    VrrReplayConfiguration config;
    QVERIFY2(loadVrrReplayConfiguration(QJsonDocument(root).toJson(), config, error), qPrintable(error));
    QCOMPARE(config.scenarios.front().controller.dxgiVrr12Protection, uint64_t(0));
    QVERIFY(vrrReplayParameterNames().contains("controller.dxgi_vrr12_protection"));
    VrrTimingParameters missingPrerequisites;
    const auto originalDefaults = vrrTimingParametersToJson(missingPrerequisites);
    QVERIFY(!applyVrrReplayControllerSnapshot(
        QJsonObject{{"dxgi_vrr12_protection", 1}}, missingPrerequisites, error));
    QVERIFY(error.contains("adaptive timestamp history"));
    QCOMPARE(vrrTimingParametersToJson(missingPrerequisites), originalDefaults);

    // Backend resolution is atomic and occurs after parsing: conflicting
    // experimental values cannot leave native interval zero without its floor.
    parameters.playoutPerFrameLatch = 1;
    parameters.latchedFloorDisabled = 1;
    parameters.playoutSmoothingGainPerMille = 200;
    parameters.playoutMetronomeEnabled = 1;
    parameters.sourcePlayoutDelayUs = 4321;
    parameters.playoutDelayMaximumUs = 16000;
    const auto raw = vrrTimingParametersToJson(parameters);
    const auto effective = vrrResolvePresentationParameters(parameters, true);
    QVERIFY2(validateVrrTimingParameters(effective, error), qPrintable(error));
    QCOMPARE(effective.playoutPerFrameLatch, uint64_t(0));
    QCOMPARE(effective.latchedFloorDisabled, uint64_t(0));
    QCOMPARE(effective.playoutSmoothingGainPerMille, uint64_t(0));
    QCOMPARE(effective.playoutMetronomeEnabled, uint64_t(0));
    QCOMPARE(effective.latchedPresentationHeadroomUs, uint64_t(225));
    QCOMPARE(effective.latchedPresentationExitHeadroomUs, uint64_t(400));
    QCOMPARE(effective.cadenceStabilityLatchFrames, uint64_t(64));
    QCOMPARE(effective.latchedPresentationBaseGuardExit, uint64_t(0));
    QCOMPARE(effective.latchedPresentationHeadroomPeriodNumerator, uint64_t(0));
    QCOMPARE(effective.latchedPresentationExitHeadroomPeriodNumerator, uint64_t(0));
    QCOMPARE(effective.playoutPredictionEnabled, uint64_t(1));
    QCOMPARE(effective.playoutPreserveDxgiFeedback, uint64_t(1));
    QCOMPARE(effective.sourcePlayoutDelayUs, uint64_t(4321));
    QCOMPARE(effective.playoutDelayMaximumUs, uint64_t(16000));
    QCOMPARE(vrrTimingParametersToJson(vrrResolvePresentationParameters(effective, true)),
             vrrTimingParametersToJson(effective));
    QCOMPARE(vrrTimingParametersToJson(vrrResolvePresentationParameters(parameters, false)), raw);
    parameters.dxgiVrr12Protection = 0;
    QCOMPARE(vrrTimingParametersToJson(vrrResolvePresentationParameters(parameters, true)),
             vrrTimingParametersToJson(parameters));
}

void VrrReplayConfigTest::smoothedHitchReferenceRoundTrip()
{
    QString error;
    for (uint64_t enabled : {0ULL, 1ULL}) {
        VrrTimingParameters parameters;
        parameters.playoutNativeHitchSmoothedReference = enabled;
        const auto snapshot = vrrTimingParametersToJson(parameters);
        QCOMPARE(snapshot.value("playout_native_hitch_smoothed_reference").toInteger(),
                 qint64(enabled));
        VrrTimingParameters restored;
        QVERIFY2(applyVrrReplayControllerSnapshot(snapshot, restored, error),
                 qPrintable(error));
        QCOMPARE(restored.playoutNativeHitchSmoothedReference, enabled);
    }

    VrrTimingParameters parameters;
    parameters.playoutNativeHitchSmoothedReference = 1;
    const auto unchanged = vrrTimingParametersToJson(parameters);
    QVERIFY(!applyVrrReplayControllerSnapshot(
        QJsonObject{{"playout_native_hitch_smoothed_reference", 2}}, parameters, error));
    QCOMPARE(vrrTimingParametersToJson(parameters), unchanged);

    auto legacySnapshot = unchanged;
    legacySnapshot.remove("playout_native_hitch_smoothed_reference");
    VrrTimingParameters legacy;
    QVERIFY2(applyVrrReplayControllerSnapshot(legacySnapshot, legacy, error),
             qPrintable(error));
    QCOMPARE(legacy.playoutNativeHitchSmoothedReference, uint64_t(0));
    auto root = vrrDefaultReplayConfigurationJson();
    auto sections = root.value("parameters").toObject();
    sections["controller"] = legacySnapshot;
    root["parameters"] = sections;
    VrrReplayConfiguration config;
    QVERIFY2(loadVrrReplayConfiguration(QJsonDocument(root).toJson(), config, error),
             qPrintable(error));
    QCOMPARE(config.scenarios.front().controller.playoutNativeHitchSmoothedReference,
             uint64_t(0));
    QVERIFY(vrrReplayParameterNames().contains(
        "controller.playout_native_hitch_smoothed_reference"));
}

void VrrReplayConfigTest::divergentDeadlineRetainsSummary()
{
    QString replayPath = qEnvironmentVariable("MOONLIGHT_VRR_TEST_REPLAY");
    if (replayPath.isEmpty()) {
        replayPath = QCoreApplication::applicationDirPath() + "/vrrreplay";
#ifdef Q_OS_WIN
        replayPath += ".exe";
#endif
    }
    if (!QFileInfo::exists(replayPath)) {
        QSKIP("Build sibling vrrreplay or set MOONLIGHT_VRR_TEST_REPLAY for CLI regression coverage");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    // A legacy trace with one scheduled frame, an omitted first arrival,
    // and a deliberately divergent original deadline. This exercises the
    // real CLI/report path without a platform renderer or a user capture.
    const QByteArray header =
        "trace_schema,arrival_sequence,frame,rtp_timestamp,rtp_valid,"
        "decode_complete_us,pacer_arrival_us,arrival_queue_depth_before,"
        "queue_accepted,dequeue_us,decision_valid,decision_us,display_refresh_hz,"
        "stream_rate_hz,can_latch_present,display_period_us,sender_interval_us,"
        "source_rate_hz,source_time_us,source_period_us,ready_offset_us,"
        "readiness_budget_us,timing_budget_us,render_lead_us,render_wake_lead_us,"
        "target_wake_lead_us,guard_us,headroom_us,render_start_us,prepare_start_us,"
        "prepare_end_us,prepare_us,render_wait_overshoot_us,target_scheduler_delay_us,"
        "target_scheduler_delay_valid,target_us,submission_boundary_us,present_call_us,"
        "submit_error_us,submission_spacing_us,had_prior_submission,presented,cancelled,"
        "disposition,dropped,tear_classification,tear_risk,latch_valid,latch_submission_id,"
        "latch_present_refresh_seq,latched_present,used_rtp_timestamp,cadence_eligible,"
        "source_rate_changed,phase_discontinuity,rebased,native_present_call_us,"
        "gpu_ready_wait_us,original_target_us,controller_call_us,correction_wait_end_us,"
        "correction_wait_start_us,playout_delay_us,render_scheduler_delay_us,"
        "render_scheduler_delay_valid,render_wait_entry_us,render_wait_final_us,"
        "spacing_margin_us,stale_age_us,target_wait_entry_us,target_wait_final_us,"
        "target_wait_overshoot_us";
    const QMap<QByteArray, QByteArray> values {
        {"trace_schema", "3"}, {"arrival_sequence", "2"}, {"frame", "1"},
        {"rtp_timestamp", "9000"}, {"rtp_valid", "1"},
        {"decode_complete_us", "100000"}, {"pacer_arrival_us", "100000"},
        {"queue_accepted", "1"}, {"dequeue_us", "100000"},
        {"decision_valid", "1"}, {"decision_us", "100000"},
        {"display_refresh_hz", "120"}, {"stream_rate_hz", "116"},
        {"can_latch_present", "1"}, {"display_period_us", "8333"},
        {"sender_interval_us", "8620"}, {"source_rate_hz", "116.009"},
        {"source_time_us", "100000"}, {"source_period_us", "8620"},
        {"render_lead_us", "1000"}, {"render_start_us", "107000"},
        {"prepare_start_us", "107000"}, {"prepare_end_us", "108000"},
        {"prepare_us", "1000"}, {"target_us", "108000"},
        {"submission_boundary_us", "108000"}, {"present_call_us", "10"},
        {"presented", "1"}, {"disposition", "presented"},
        {"tear_classification", "first_submission_unknown"},
        {"rebased", "1"}, {"original_target_us", "1"},
    };
    QList<QByteArray> fields;
    for (const QByteArray& column : header.split(',')) {
        fields.append(values.value(column, "0"));
    }
    const QString tracePath = directory.filePath("divergent.csv");
    QFile trace(tracePath);
    QVERIFY(trace.open(QIODevice::WriteOnly));
    const QByteArray contents = header + '\n' + fields.join(',') + '\n';
    QCOMPARE(trace.write(contents), qint64(contents.size()));
    trace.close();

    for (int testCase : {0, 1, 2}) {
        const bool requireExact = testCase == 1;
        const bool changeNativeContract = testCase == 2;
        const QString outputPath = directory.filePath(
            requireExact ? "strict.json" : changeNativeContract ?
                "native-contract-change.json" : "exploratory.json");
        QStringList arguments {tracePath, "--output", outputPath};
        if (requireExact) arguments.append("--require-exact-baseline");
        if (changeNativeContract) {
            // A protocol fixture for a contract-changing candidate, not a
            // synthetic claim about driver service or a panel's tear behavior.
            arguments << "--set" << "controller.playout_history_enabled=1"
                      << "--set" << "controller.timestamp_playout_enabled=1"
                      << "--set" << "controller.playout_delay_adaptive=1"
                      << "--set" << "controller.dxgi_vrr12_protection=1";
        }
        QProcess replay;
        replay.start(replayPath, arguments);
        QVERIFY2(replay.waitForStarted(), qPrintable(replay.errorString()));
        QVERIFY2(replay.waitForFinished(30000), qPrintable(replay.errorString()));
        const QByteArray errors = replay.readAllStandardError();
        QCOMPARE(replay.exitStatus(), QProcess::NormalExit);
        QVERIFY2(replay.exitCode() == (requireExact ? 3 : 0), errors.constData());
        QVERIFY(errors.contains("Original deadline diverged on frame 1"));
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const auto summary = QJsonDocument::fromJson(output.readAll(), &parseError).object();
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        const auto fidelity = summary.value("fidelity").toObject();
        QVERIFY(!fidelity.value("baseline_exact").toBool(true));
        QCOMPARE(fidelity.value("invalid_controller_lifecycle_rows").toInteger(), qint64(1));
        const auto drift = fidelity.value("reference_original_target_drift_us").toObject();
        QCOMPARE(drift.value("count").toInteger(), qint64(1));
        QVERIFY(drift.value("max").toInteger() > 0);
        QVERIFY(!summary.value("capture").toObject()
                     .value("recorded_sequence_integrity_valid").toBool(true));
        const auto simulation = summary.value("simulation").toObject();
        QCOMPARE(simulation.value("native_presentation_contract_changed").toBool(),
                 changeNativeContract);
        QCOMPARE(simulation.value("recorded_native_service_compatible").toBool(),
                 !changeNativeContract);
        if (changeNativeContract) {
            QVERIFY(simulation.value("native_presentation_scope").toString().contains("nonpredictive"));
            QVERIFY(!summary.value("raster_simulation_ready").toBool(true));
            const auto controller = simulation.value("resolved_parameters").toObject()
                                        .value("controller").toObject();
            QCOMPARE(controller.value("dxgi_vrr12_protection").toInteger(), qint64(1));
            QCOMPARE(controller.value("latched_floor_disabled").toInteger(-1), qint64(0));
            QCOMPARE(controller.value("playout_per_frame_latch").toInteger(-1), qint64(0));
            QCOMPARE(controller.value("playout_smoothing_gain_per_mille").toInteger(-1), qint64(0));
            const QString comparedOutput = directory.filePath("native-contract-compare.json");
            const int outputIndex = arguments.indexOf("--output");
            QVERIFY(outputIndex >= 0);
            arguments[outputIndex + 1] = comparedOutput;
            arguments << "--compare" << directory.filePath("exploratory.json");
            QProcess comparison;
            comparison.start(replayPath, arguments);
            QVERIFY2(comparison.waitForStarted(), qPrintable(comparison.errorString()));
            QVERIFY2(comparison.waitForFinished(30000), qPrintable(comparison.errorString()));
            QCOMPARE(comparison.exitStatus(), QProcess::NormalExit);
            QCOMPARE(comparison.exitCode(), 5);
            QFile compared(comparedOutput);
            QVERIFY(compared.open(QIODevice::ReadOnly));
            const auto comparisonSummary = QJsonDocument::fromJson(compared.readAll()).object()
                                               .value("comparison").toObject();
            QVERIFY(!comparisonSummary.value("comparable").toBool(true));
            QVERIFY(!comparisonSummary.value("recorded_native_service_compatible").toBool(true));
        }
    }
}

void VrrReplayConfigTest::rasterEnvelope()
{
    VrrReplayDisplayParameters parameters;
    parameters.scanoutPeriodUs = 10000;
    parameters.activeScanoutUs = 8000;
    parameters.phaseUncertaintyUs = 0;
    parameters.anchorMaxAgeUs = 100000;

    VrrRasterPhaseResult result = evaluateVrrRasterPhase(
        true, false, 104000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::CertainActive);
    QCOMPARE(result.vrrLocked, VrrRasterPhaseState::Active);
    QCOMPARE(result.freeRunning, VrrRasterPhaseState::Active);
    QCOMPARE(result.vrrLockedWaitUs, uint64_t(4000));
    QCOMPARE(result.anchorTimeUs, uint64_t(100000));
    QCOMPARE(result.modeledTransitionUs, uint64_t(104000));
    QVERIFY(result.vrrLockedPhaseValid);
    QCOMPARE(result.vrrLockedPhasePs, uint64_t(4000000000ULL));
    QVERIFY(result.freeRunningPhaseValid);
    QCOMPARE(result.freeRunningPhasePs, uint64_t(4000000000ULL));
    QVERIFY(result.vrrLockedScanoutPositionValid);
    QCOMPARE(result.vrrLockedScanoutPositionPpm, uint64_t(500000));
    QVERIFY(result.freeRunningScanoutPositionValid);
    QCOMPARE(result.freeRunningScanoutPositionPpm, uint64_t(500000));

    result = evaluateVrrRasterPhase(
        true, false, 109000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope,
             VrrRasterEnvelopeClass::InactiveInBothModels);

    result = evaluateVrrRasterPhase(
        true, false, 111000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::PossibleActive);
    QCOMPARE(result.vrrLocked, VrrRasterPhaseState::Inactive);
    QCOMPARE(result.freeRunning, VrrRasterPhaseState::Active);
    QVERIFY(!result.vrrLockedScanoutPositionValid);
    QVERIFY(result.freeRunningScanoutPositionValid);
    QCOMPARE(result.freeRunningScanoutPositionPpm, uint64_t(125000));

    parameters.phaseUncertaintyUs = 250;
    result = evaluateVrrRasterPhase(
        true, false, 107900, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::PossibleActive);
    QCOMPARE(result.vrrLocked,
             VrrRasterPhaseState::BoundaryUncertain);
    QCOMPARE(result.freeRunning,
             VrrRasterPhaseState::BoundaryUncertain);
    result = evaluateVrrRasterPhase(
        true, false, 109900, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::PossibleActive);
    QCOMPARE(result.vrrLocked, VrrRasterPhaseState::Inactive);
    QCOMPARE(result.freeRunning,
             VrrRasterPhaseState::BoundaryUncertain);
    QCOMPARE(result.freeRunningWaitUs, uint64_t(8350));
    parameters.phaseUncertaintyUs = 0;

    parameters.syncToActiveScanoutUs = 1000;
    result = evaluateVrrRasterPhase(
        true, false, 100500, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope,
             VrrRasterEnvelopeClass::InactiveInBothModels);
    QCOMPARE(result.vrrLocked, VrrRasterPhaseState::Inactive);
    QCOMPARE(result.freeRunning, VrrRasterPhaseState::Inactive);
    QCOMPARE(result.resolvedSyncToActiveScanoutUs, uint64_t(1000));
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 100500, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::BeforeActiveScanout);
    result = evaluateVrrRasterPhase(
        true, false, 101500, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::CertainActive);
    QCOMPARE(result.vrrLockedScanoutPositionPpm, uint64_t(62500));
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 101500, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::Active);

    parameters.syncToActiveScanoutUs = 4000;
    result = evaluateVrrRasterPhase(
        true, false, 105000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::CertainActive);
    QCOMPARE(result.vrrLockedScanoutPositionPpm, uint64_t(125000));
    QCOMPARE(result.freeRunningScanoutPositionPpm, uint64_t(125000));
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 105000, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::Active);

    result = evaluateVrrRasterPhase(
        true, false, 101000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::PossibleActive);
    QCOMPARE(result.vrrLocked, VrrRasterPhaseState::Inactive);
    QCOMPARE(result.freeRunning, VrrRasterPhaseState::Active);
    parameters.syncToActiveScanoutUs = 0;

    result = evaluateVrrRasterPhase(
        true, true, 104000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::LatchedSuppressed);

    result = evaluateVrrRasterPhase(
        true, false, 104000, 0, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::Unclassified);

    parameters.anchorMaxAgeUs = 5000;
    result = evaluateVrrRasterPhase(
        true, false, 111000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::Unclassified);

    parameters.anchorMaxAgeUs = 100000;
    parameters.presentTransportUs = 5000;
    result = evaluateVrrRasterPhase(
        true, false, 104000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope,
             VrrRasterEnvelopeClass::InactiveInBothModels);
    QCOMPARE(result.modeledTransitionUs, uint64_t(109000));
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 104000, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::AfterActiveScanout);

    parameters.presentTransportUs = 0;
    parameters.phaseUncertaintyUs = 250;
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 99800, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::BoundaryUncertain);
    parameters.phaseUncertaintyUs = 0;
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 99000, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::BeforeActiveScanout);
    parameters.anchorMaxAgeUs = 500;
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 99000, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::Unclassified);
    parameters.anchorMaxAgeUs = 100000;
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 104000, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::Active);
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 false, 109000, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::AfterActiveScanout);
    QCOMPARE(evaluateVrrExactRefreshPhase(
                 true, 104000, 100000, 10000, 10000, parameters),
             VrrExactRefreshPhaseClass::LatchedSuppressed);
    QVERIFY(vrrRasterEnvelopeContradictsExactRefresh(
        VrrRasterEnvelopeClass::InactiveInBothModels,
        VrrExactRefreshPhaseClass::Active));
    QVERIFY(vrrRasterEnvelopeContradictsExactRefresh(
        VrrRasterEnvelopeClass::CertainActive,
        VrrExactRefreshPhaseClass::AfterActiveScanout));
    QVERIFY(vrrRasterEnvelopeContradictsExactRefresh(
        VrrRasterEnvelopeClass::CertainActive,
        VrrExactRefreshPhaseClass::BeforeActiveScanout));
    QVERIFY(vrrRasterEnvelopeContradictsExactRefresh(
        VrrRasterEnvelopeClass::CertainActive,
        VrrExactRefreshPhaseClass::LatchedSuppressed));
    QVERIFY(!vrrRasterEnvelopeContradictsExactRefresh(
        VrrRasterEnvelopeClass::PossibleActive,
        VrrExactRefreshPhaseClass::Active));
    QVERIFY(!vrrRasterEnvelopeContradictsExactRefresh(
        VrrRasterEnvelopeClass::Unclassified,
        VrrExactRefreshPhaseClass::Active));

    parameters.activeScanoutUs = 12000;
    result = evaluateVrrRasterPhase(
        true, false, 104000, 100000, 10000, 10000, parameters);
    QVERIFY(result.activeScanoutClamped);
    QCOMPARE(result.resolvedActiveScanoutUs, uint64_t(10000));

    // A 120 Hz period rounded to integer microseconds wraps several
    // microseconds early over an eight-refresh anchor window. Verify that the
    // precise physical rational, when available, owns phase classification.
    parameters = {};
    parameters.scanoutPeriodUs = 8333;
    parameters.activeScanoutUs = 100;
    parameters.phaseUncertaintyUs = 0;
    parameters.anchorMaxAgeUs = 70000;
    result = evaluateVrrRasterPhase(
        true, false, 166666, 100000, 8333, 8333, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::PossibleActive);
    parameters.scanoutPeriodPs = 8333333333ULL;
    result = evaluateVrrRasterPhase(
        true, false, 166666, 100000, 8333, 8333, parameters);
    QCOMPARE(result.envelope,
             VrrRasterEnvelopeClass::InactiveInBothModels);
    QCOMPARE(result.freeRunning, VrrRasterPhaseState::Inactive);
    QCOMPARE(result.resolvedScanoutPeriodPs, uint64_t(8333333333ULL));
    QCOMPARE(result.freeRunningPhasePs, uint64_t(8332666669ULL));

    // Integer-microsecond active duration would mark exactly 8000 us as a
    // boundary. The physical line geometry can end active scanout between
    // microseconds, so retain and use that exact duration instead.
    parameters = {};
    parameters.scanoutPeriodUs = 10000;
    parameters.scanoutPeriodPs = 10000000000ULL;
    parameters.activeScanoutUs = 8000;
    parameters.phaseUncertaintyUs = 0;
    parameters.anchorMaxAgeUs = 100000;
    result = evaluateVrrRasterPhase(
        true, false, 108000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope, VrrRasterEnvelopeClass::PossibleActive);
    parameters.activeScanoutPs = 7999600000ULL;
    result = evaluateVrrRasterPhase(
        true, false, 108000, 100000, 10000, 10000, parameters);
    QCOMPARE(result.envelope,
             VrrRasterEnvelopeClass::InactiveInBothModels);
    QCOMPARE(result.resolvedActiveScanoutPs, uint64_t(7999600000ULL));

    VrrSubmissionAdvanceResult advance = applyVrrSubmissionAdvance(
        10000, 8000, 1000);
    QCOMPARE(advance.submissionUs, uint64_t(9000));
    QCOMPARE(advance.appliedAdvanceUs, uint64_t(1000));
    QVERIFY(!advance.clampedByReadiness);

    advance = applyVrrSubmissionAdvance(10000, 8000, 3000);
    QCOMPARE(advance.submissionUs, uint64_t(8000));
    QCOMPARE(advance.appliedAdvanceUs, uint64_t(2000));
    QVERIFY(advance.clampedByReadiness);

    advance = applyVrrSubmissionAdvance(1000, 0, 5000);
    QCOMPARE(advance.submissionUs, uint64_t(0));
    QCOMPARE(advance.appliedAdvanceUs, uint64_t(1000));
    QVERIFY(advance.clampedByReadiness);
}

void VrrReplayConfigTest::rasterProbeOverheadRemoval()
{
    VrrRasterProbeOverheadRemovalResult removal =
        evaluateVrrRasterProbeOverheadRemoval(
            false, true, 0, 9000, 9200, true, 9250,
            10000, 8000);
    QVERIFY(!removal.requested);
    QVERIFY(removal.evidenceAvailable);
    QCOMPARE(removal.measuredProbeDurationUs, uint64_t(200));
    QCOMPARE(removal.submissionUs, uint64_t(10000));
    QCOMPARE(removal.appliedRemovalUs, uint64_t(0));

    removal = evaluateVrrRasterProbeOverheadRemoval(
        true, true, 0, 9000, 9200, true, 9250,
        10000, 8000);
    QVERIFY(removal.requested);
    QVERIFY(removal.evidenceAvailable);
    QCOMPARE(removal.measuredProbeDurationUs, uint64_t(200));
    QCOMPARE(removal.submissionUs, uint64_t(9800));
    QCOMPARE(removal.appliedRemovalUs, uint64_t(200));
    QVERIFY(!removal.clampedBySubmissionFloor);

    removal = evaluateVrrRasterProbeOverheadRemoval(
        true, true, 0, 9000, 9200, true, 9250,
        8100, 8000);
    QVERIFY(removal.evidenceAvailable);
    QCOMPARE(removal.submissionUs, uint64_t(8000));
    QCOMPARE(removal.appliedRemovalUs, uint64_t(100));
    QVERIFY(removal.clampedBySubmissionFloor);

    removal = evaluateVrrRasterProbeOverheadRemoval(
        true, true, 0, 9300, 9200, true, 9250,
        10000, 8000);
    QVERIFY(removal.requested);
    QVERIFY(!removal.evidenceAvailable);
    QCOMPARE(removal.submissionUs, uint64_t(10000));
    QCOMPARE(removal.appliedRemovalUs, uint64_t(0));

    removal = evaluateVrrRasterProbeOverheadRemoval(
        true, true, -1, 9000, 9200, true, 9250,
        10000, 8000);
    QVERIFY(!removal.evidenceAvailable);
}

void VrrReplayConfigTest::rasterScanLineAudit()
{
    VrrRasterScanLineAudit audit = evaluateVrrRasterScanLine(
        false, VrrRasterPhaseState::Active, true,
        4000000000ULL, 10000000000ULL, 0,
        1080, 1125, 450);
    QVERIFY(audit.comparable);
    QVERIFY(audit.matches);
    QCOMPARE(audit.predictedScanLine, uint64_t(450));
    QCOMPARE(audit.absoluteErrorLines, uint64_t(0));
    QCOMPARE(audit.signedErrorLines, int64_t(0));
    QCOMPARE(audit.toleranceLines, uint64_t(1));

    audit = evaluateVrrRasterScanLine(
        false, VrrRasterPhaseState::Active, true,
        4000000000ULL, 10000000000ULL, 0,
        1080, 1125, 451);
    QVERIFY(audit.comparable);
    QVERIFY(audit.matches);

    audit = evaluateVrrRasterScanLine(
        false, VrrRasterPhaseState::Active, true,
        4000000000ULL, 10000000000ULL, 0,
        1080, 1125, 452);
    QVERIFY(audit.comparable);
    QVERIFY(!audit.matches);
    QCOMPARE(audit.absoluteErrorLines, uint64_t(2));
    QCOMPARE(audit.signedErrorLines, int64_t(2));

    audit = evaluateVrrRasterScanLine(
        false, VrrRasterPhaseState::Active, true,
        4000000000ULL, 10000000000ULL, 0,
        1080, 1125, 448);
    QVERIFY(audit.comparable);
    QVERIFY(!audit.matches);
    QCOMPARE(audit.signedErrorLines, int64_t(-2));

    audit = evaluateVrrRasterScanLine(
        false, VrrRasterPhaseState::Active, true,
        4000000000ULL, 10000000000ULL, 100000000ULL,
        1080, 1125, 463);
    QVERIFY(audit.comparable);
    QVERIFY(audit.matches);
    QCOMPARE(audit.toleranceLines, uint64_t(13));

    audit = evaluateVrrRasterScanLine(
        true, VrrRasterPhaseState::Inactive, true,
        9900000000ULL, 10000000000ULL, 0,
        1080, 1125, 0);
    QVERIFY(!audit.comparable);

    audit = evaluateVrrRasterScanLine(
        false, VrrRasterPhaseState::Inactive, true,
        4000000000ULL, 10000000000ULL, 0,
        1080, 1125, 450);
    QVERIFY(!audit.comparable);

    audit = evaluateVrrRasterScanLine(
        false, VrrRasterPhaseState::Active, true,
        9600000000ULL, 10000000000ULL, 0,
        1080, 1125, 1079);
    QVERIFY(!audit.comparable);
}

void VrrReplayConfigTest::rasterScanLineScaleInference()
{
    VrrRasterScanLineScaleInference inference;
    QVERIFY(addVrrRasterScanLineScaleSample(
        inference, false, 0, 2160, 2250));
    QVERIFY(addVrrRasterScanLineScaleSample(
        inference, false, 6479, 2160, 2250));
    QVERIFY(addVrrRasterScanLineScaleSample(
        inference, true, 6749, 2160, 2250));
    QVERIFY(inference.valid);
    QCOMPARE(inference.samples, uint64_t(3));
    QCOMPARE(inference.scale, uint64_t(3));
    QCOMPARE(inference.maximumObservedScanLine, uint64_t(6749));

    uint64_t normalized = 0;
    QVERIFY(normalizeVrrRasterScanLine(6479, inference.scale, normalized));
    QCOMPARE(normalized, uint64_t(2159));
    QVERIFY(normalizeVrrRasterScanLine(6749, inference.scale, normalized));
    QCOMPARE(normalized, uint64_t(2249));
    QVERIFY(!normalizeVrrRasterScanLine(100, 0, normalized));

    VrrRasterScanLineScaleInference invalid;
    QVERIFY(!addVrrRasterScanLineScaleSample(
        invalid, false, 1, 0, 2250));
    QVERIFY(!invalid.valid);
}

void VrrReplayConfigTest::rasterSyncAnchorMerge()
{
    VrrRasterSyncAnchorMergeResult result =
        evaluateVrrRasterSyncAnchorMerge(
            false, 0, 0, 100, 100000,
            8333, 50, 500);
    QVERIFY(result.accepted);
    QVERIFY(!result.replacesPrevious);
    QCOMPARE(result.status,
             VrrRasterSyncAnchorMergeStatus::Appended);

    result = evaluateVrrRasterSyncAnchorMerge(
        true, 100, 100000, 100, 100050,
        8333, 50, 500);
    QVERIFY(result.accepted);
    QVERIFY(result.replacesPrevious);
    QCOMPARE(result.status,
             VrrRasterSyncAnchorMergeStatus::ReplacedSameRefresh);

    result = evaluateVrrRasterSyncAnchorMerge(
        true, 100, 100000, 100, 100051,
        8333, 50, 500);
    QVERIFY(!result.accepted);
    QCOMPARE(
        result.status,
        VrrRasterSyncAnchorMergeStatus::
            SameRefreshTimestampMismatch);

    result = evaluateVrrRasterSyncAnchorMerge(
        true, 100, 100000, 99, 108333,
        8333, 50, 500);
    QVERIFY(!result.accepted);
    QCOMPARE(result.status,
             VrrRasterSyncAnchorMergeStatus::SequenceRegression);

    result = evaluateVrrRasterSyncAnchorMerge(
        true, 100, 100000, 101, 100000,
        8333, 50, 500);
    QVERIFY(!result.accepted);
    QCOMPARE(
        result.status,
        VrrRasterSyncAnchorMergeStatus::NonadvancingTimestamp);

    result = evaluateVrrRasterSyncAnchorMerge(
        true, 100, 100000, 101, 101000,
        8333, 50, 500);
    QVERIFY(!result.accepted);
    QCOMPARE(
        result.status,
        VrrRasterSyncAnchorMergeStatus::
            ImplausiblyShortInterval);

    result = evaluateVrrRasterSyncAnchorMerge(
        true, 100, 100000, 102, 116666,
        8333, 50, 500);
    QVERIFY(result.accepted);
    QVERIFY(!result.replacesPrevious);
    QCOMPARE(result.status,
             VrrRasterSyncAnchorMergeStatus::Appended);
}

void VrrReplayConfigTest::freeRunningRefreshTimeline()
{
    VrrFreeRunningRefreshTracker tracker;

    VrrFreeRunningRefreshResult result = tracker.observe(
        100000, 10000, true, 2000, 0);
    QVERIFY(result.baselineEstablished);
    QVERIFY(!result.compared);
    QCOMPARE(result.propagatedPhase, uint64_t(2000));

    result = tracker.observe(105000, 10000, true, 7000, 0);
    QVERIFY(result.compared);
    QCOMPARE(result.refreshDeltaLower, uint64_t(0));
    QCOMPARE(result.refreshDelta, uint64_t(0));
    QCOMPARE(result.refreshDeltaUpper, uint64_t(0));
    QCOMPARE(result.scanoutAnomalyLower, uint64_t(1));
    QCOMPARE(result.scanoutAnomaly, uint64_t(1));
    QCOMPARE(result.scanoutAnomalyUpper, uint64_t(1));
    QCOMPARE(result.repeatedRefresh, uint64_t(0));
    QVERIFY(result.phaseReferenceCompared);
    QCOMPARE(result.phaseReferenceDifference, uint64_t(0));

    result = tracker.observe(110000, 10000, true, 2000, 0);
    QCOMPARE(result.refreshDelta, uint64_t(1));
    QCOMPARE(result.scanoutAnomaly, uint64_t(0));
    QCOMPARE(result.repeatedRefresh, uint64_t(0));

    result = tracker.observe(135000, 10000, true, 7000, 0);
    QCOMPARE(result.refreshDelta, uint64_t(2));
    QCOMPARE(result.repeatedRefreshLower, uint64_t(1));
    QCOMPARE(result.repeatedRefresh, uint64_t(1));
    QCOMPARE(result.repeatedRefreshUpper, uint64_t(1));

    tracker.reset();
    result = tracker.observe(200000, 10000, true, 9500, 1000);
    QVERIFY(result.baselineEstablished);
    result = tracker.observe(200500, 10000, false, 0, 1000);
    QCOMPARE(result.refreshDeltaLower, uint64_t(0));
    QCOMPARE(result.refreshDelta, uint64_t(1));
    QCOMPARE(result.refreshDeltaUpper, uint64_t(1));
    QCOMPARE(result.scanoutAnomalyLower, uint64_t(0));
    QCOMPARE(result.scanoutAnomaly, uint64_t(0));
    QCOMPARE(result.scanoutAnomalyUpper, uint64_t(1));

    tracker.reset();
    result = tracker.observe(210000, 10000, true, 0, 0);
    QVERIFY(result.baselineEstablished);
    result = tracker.observe(211000, 10000, false, 0, 0);
    QCOMPARE(result.refreshDeltaLower, uint64_t(0));
    QCOMPARE(result.refreshDelta, uint64_t(0));
    QCOMPARE(result.refreshDeltaUpper, uint64_t(0));

    tracker.reset();
    result = tracker.observe(220000, 10000, true, 4000, 1000);
    QVERIFY(result.baselineEstablished);
    result = tracker.observe(225000, 10000, false, 0, 1000);
    QCOMPARE(result.refreshDeltaLower, uint64_t(0));
    QCOMPARE(result.refreshDelta, uint64_t(0));
    QCOMPARE(result.refreshDeltaUpper, uint64_t(1));
    QCOMPARE(result.scanoutAnomalyLower, uint64_t(0));
    QCOMPARE(result.scanoutAnomaly, uint64_t(1));
    QCOMPARE(result.scanoutAnomalyUpper, uint64_t(1));

    result = tracker.observe(201000, 9000, true, 1000, 1000);
    QVERIFY(result.periodChanged);
    QVERIFY(result.baselineEstablished);
    QVERIFY(!result.compared);

    result = tracker.observe(200000, 9000, true, 0, 1000);
    QVERIFY(result.timeRegression);
    QVERIFY(result.baselineEstablished);
    QVERIFY(!result.compared);

    tracker.reset();
    result = tracker.observe(300000, 10000, false, 0, 0);
    QVERIFY(!result.baselineEstablished);
    QVERIFY(!result.compared);
}

void VrrReplayConfigTest::rawQpcTranslation()
{
    VrrRawQpcTranslationTracker tracker;

    VrrRawQpcTranslationResult result = tracker.observe(
        1000000, 1000000, false, 0, 2);
    QVERIFY(!result.baselineEstablished);
    QVERIFY(!result.compared);
    QVERIFY(!result.frequencyMismatch);
    QVERIFY(!result.translationMismatch);

    result = tracker.observe(2000000, 1000000, true, 5000000, 2);
    QVERIFY(result.baselineEstablished);
    QVERIFY(!result.compared);
    QVERIFY(!result.frequencyMismatch);
    QVERIFY(!result.translationMismatch);

    result = tracker.observe(2001234, 1000000, true, 5001234, 2);
    QVERIFY(!result.baselineEstablished);
    QVERIFY(result.compared);
    QVERIFY(!result.frequencyMismatch);
    QVERIFY(!result.translationMismatch);

    result = tracker.observe(2002234, 1000000, true, 5002238, 2);
    QVERIFY(result.compared);
    QVERIFY(result.translationMismatch);

    tracker.reset();
    result = tracker.observe(3000000, 3000000, true, 7000000, 2);
    QVERIFY(result.baselineEstablished);
    result = tracker.observe(3000004, 3000000, true, 7000002, 2);
    QVERIFY(result.compared);
    QVERIFY(!result.translationMismatch);

    tracker.reset();
    (void)tracker.observe(4000000, 1000000, true, 8000000, 2);
    result = tracker.observe(4002000, 2000000, true, 8001000, 2);
    QVERIFY(result.frequencyMismatch);
    QVERIFY(result.compared);
    QVERIFY(result.translationMismatch);

    tracker.reset();
    (void)tracker.observe(5000000, 1000000, true, 9000000, 2);
    result = tracker.observe(4999999, 1000000, true, 9000001, 2);
    QVERIFY(result.compared);
    QVERIFY(result.translationMismatch);

    tracker.reset();
    (void)tracker.observe(6000000, 1000000, false, 0, 2);
    result = tracker.observe(6000100, 2000000, true, 10000000, 2);
    QVERIFY(result.baselineEstablished);
    QVERIFY(result.frequencyMismatch);

    VrrQpcCorrelationResult correlation = evaluateVrrQpcCorrelation(
        2000000, 1000000, 5000000,
        1000000, 4000000, 3, 0);
    QVERIFY(correlation.translated);
    QCOMPARE(correlation.expectedTimeUs, uint64_t(5000000));
    QVERIFY(correlation.matches);
    QVERIFY(correlation.uncertaintyValid);
    QCOMPARE(correlation.halfSpanUncertaintyUs, uint64_t(2));

    correlation = evaluateVrrQpcCorrelation(
        500000, 1000000, 3500001,
        1000000, 4000000, 0, 1);
    QVERIFY(correlation.translated);
    QCOMPARE(correlation.expectedTimeUs, uint64_t(3500000));
    QVERIFY(correlation.matches);
    QVERIFY(correlation.uncertaintyValid);
    QCOMPARE(correlation.halfSpanUncertaintyUs, uint64_t(0));

    correlation = evaluateVrrQpcCorrelation(
        500000, 1000000, 3500002,
        1000000, 4000000, 0, 1);
    QVERIFY(correlation.translated);
    QVERIFY(!correlation.matches);

    correlation = evaluateVrrQpcCorrelation(
        500000, 0, 3500000,
        1000000, 4000000, 0, 1);
    QVERIFY(!correlation.translated);
    QVERIFY(!correlation.uncertaintyValid);
}

void VrrReplayConfigTest::rationalDisplayTiming()
{
    uint64_t periodPs = 0;
    QVERIFY(vrrPeriodPicosecondsFromRefreshRational(
        120, 1, periodPs));
    QCOMPARE(periodPs, uint64_t(8333333333));

    QVERIFY(vrrPeriodPicosecondsFromRefreshRational(
        120000, 1001, periodPs));
    QCOMPARE(periodPs, uint64_t(8341666667));

    QVERIFY(vrrPeriodPicosecondsFromRefreshRational(
        60000, 1001, periodPs));
    QCOMPARE(periodPs, uint64_t(16683333333));

    QVERIFY(!vrrPeriodPicosecondsFromRefreshRational(
        0, 1, periodPs));
    QCOMPARE(periodPs, uint64_t(0));
    QVERIFY(!vrrPeriodPicosecondsFromRefreshRational(
        120, 0, periodPs));

    uint64_t activeScanoutUs = 0;
    QVERIFY(vrrActiveScanoutMicrosecondsFromSignal(
        120, 1, 1920, 1080, 2200, 1125,
        activeScanoutUs));
    QCOMPARE(activeScanoutUs, uint64_t(7999));
    QVERIFY(vrrActiveScanoutMicrosecondsFromSignal(
        120000, 1001, 1920, 1080, 2200, 1125,
        activeScanoutUs));
    QCOMPARE(activeScanoutUs, uint64_t(8007));
    QVERIFY(vrrActiveScanoutMicrosecondsFromSignal(
        60000, 1001, 3840, 2160, 4400, 2250,
        activeScanoutUs));
    QCOMPARE(activeScanoutUs, uint64_t(16015));
    QVERIFY(!vrrActiveScanoutMicrosecondsFromSignal(
        120, 1, 1920, 0, 2200, 1125,
        activeScanoutUs));
    QVERIFY(!vrrActiveScanoutMicrosecondsFromSignal(
        120, 1, 1920, 1126, 2200, 1125,
        activeScanoutUs));
    QVERIFY(!vrrActiveScanoutMicrosecondsFromSignal(
        120, 1, 2201, 1080, 2200, 1125,
        activeScanoutUs));

    uint64_t activeScanoutPs = 0;
    QVERIFY(vrrActiveScanoutPicosecondsFromSignal(
        120, 1, 1920, 1080, 2200, 1125,
        activeScanoutPs));
    QCOMPARE(activeScanoutPs, uint64_t(7999057239));
    QVERIFY(vrrActiveScanoutPicosecondsFromSignal(
        120000, 1001, 1920, 1080, 2200, 1125,
        activeScanoutPs));
    QCOMPARE(activeScanoutPs, uint64_t(8007056297));
    // D3DKMT remains vertically active through the trailing horizontal
    // blank of the final active line. Using totalWidth as activeWidth derives
    // that observation interval without changing the visible tear window.
    QVERIFY(vrrActiveScanoutPicosecondsFromSignal(
        120, 1, 2200, 1080, 2200, 1125,
        activeScanoutPs));
    QCOMPARE(activeScanoutPs, uint64_t(8000000000ULL));
    QVERIFY(vrrActiveScanoutPicosecondsFromSignal(
        120000, 1001, 2200, 1080, 2200, 1125,
        activeScanoutPs));
    QCOMPARE(activeScanoutPs, uint64_t(8008000000ULL));
    QVERIFY(vrrActiveScanoutPicosecondsFromSignal(
        120, 1, 2200, 1, 2200, 1125,
        activeScanoutPs));
    QCOMPARE(activeScanoutPs, uint64_t(7407407ULL));
    QVERIFY(!vrrActiveScanoutPicosecondsFromSignal(
        120, 1, 0, 1080, 2200, 1125,
        activeScanoutPs));

    QVERIFY(vrrRefreshRationalsEqual(
        120000, 1000, 120, 1));
    QVERIFY(vrrRefreshRationalsEqual(
        60000, 1001, 120000, 2002));
    QVERIFY(!vrrRefreshRationalsEqual(
        120000, 1001, 120, 1));
    QVERIFY(!vrrRefreshRationalsEqual(
        0, 1, 120, 1));

    VrrDisplaySignalConsistency signalConsistency =
        evaluateVrrDisplaySignalConsistency(
            148500000, 67500, 1, 60, 1,
            2200, 1125, 100);
    QVERIFY(signalConsistency.inputsValid);
    QVERIFY(signalConsistency.withinTolerance);
    QVERIFY(signalConsistency.pixelRateToHsyncErrorPpm <= 1);
    QVERIFY(signalConsistency.hsyncToVsyncErrorPpm <= 1);
    QVERIFY(signalConsistency.pixelRateToVsyncErrorPpm <= 1);

    signalConsistency = evaluateVrrDisplaySignalConsistency(
        148351648, 67500000, 1001, 60000, 1001,
        2200, 1125, 100);
    QVERIFY(signalConsistency.inputsValid);
    QVERIFY(signalConsistency.withinTolerance);

    signalConsistency = evaluateVrrDisplaySignalConsistency(
        148500000, 67500, 1, 60, 1,
        2199, 1125, 100);
    QVERIFY(signalConsistency.inputsValid);
    QVERIFY(!signalConsistency.withinTolerance);
    QVERIFY(signalConsistency.pixelRateToHsyncErrorPpm > 100);

    signalConsistency = evaluateVrrDisplaySignalConsistency(
        0, 67500, 1, 60, 1,
        2200, 1125, 100);
    QVERIFY(!signalConsistency.inputsValid);
    QVERIFY(!signalConsistency.withinTolerance);
}

void VrrReplayConfigTest::gpuCompletionBounds()
{
    VrrGpuCompletionBounds bounds = evaluateVrrGpuCompletionBounds(
        100, 200, 110, 120, 121, 5, 4, false, 122, 150);
    QVERIFY(bounds.valid);
    QVERIFY(bounds.fenceRelationshipValid);
    QCOMPARE(bounds.lowerBoundUs, uint64_t(120));
    QCOMPARE(bounds.upperBoundUs, uint64_t(150));
    QCOMPARE(bounds.uncertaintyUs, uint64_t(30));

    bounds = evaluateVrrGpuCompletionBounds(
        100, 200, 110, 120, 121, 5, 5, true, 122, 123);
    QVERIFY(bounds.valid);
    QCOMPARE(bounds.lowerBoundUs, uint64_t(110));
    QCOMPARE(bounds.upperBoundUs, uint64_t(121));
    QCOMPARE(bounds.uncertaintyUs, uint64_t(11));

    bounds = evaluateVrrGpuCompletionBounds(
        100, 200, 110, 120, 121, 5, 4, false, 119, 150);
    QVERIFY(!bounds.valid);

    bounds = evaluateVrrGpuCompletionBounds(
        100, 140, 110, 120, 121, 5, 4, false, 122, 150);
    QVERIFY(!bounds.valid);

    bounds = evaluateVrrGpuCompletionBounds(
        100, 200, 110, 120, 121, 5, 3, false, 122, 150);
    QVERIFY(!bounds.fenceRelationshipValid);
    QVERIFY(!bounds.valid);

    bounds = evaluateVrrGpuCompletionBounds(
        100, 200, 110, 120, 121, 5, 5, false, 122, 123);
    QVERIFY(!bounds.fenceRelationshipValid);
    QVERIFY(!bounds.valid);

    bounds = evaluateVrrGpuCompletionBounds(
        100, 200, 110, 120, 121, 5,
        std::numeric_limits<uint64_t>::max(),
        true, 122, 123);
    QVERIFY(!bounds.fenceRelationshipValid);
    QVERIFY(!bounds.valid);
}

void VrrReplayConfigTest::gpuReadyOperationAudit()
{
    VrrGpuReadyOperationAudit audit = evaluateVrrGpuReadyOperation(
        false, false, 0, false, 0, false, 0, false, 0, 0);
    QVERIFY(audit.relationshipValid);
    QVERIFY(!audit.exactSuccess);

    audit = evaluateVrrGpuReadyOperation(
        true, true, 0, true, 0, true, 0, true, 100, 1);
    QVERIFY(audit.signalSucceeded);
    QVERIFY(audit.setEventSucceeded);
    QVERIFY(audit.waitSucceeded);
    QVERIFY(audit.relationshipValid);
    QVERIFY(audit.exactSuccess);

    audit = evaluateVrrGpuReadyOperation(
        true, true, -1, false, 0, false, 0, false, 100, 1);
    QVERIFY(!audit.signalSucceeded);
    QVERIFY(audit.relationshipValid);
    QVERIFY(!audit.exactSuccess);

    audit = evaluateVrrGpuReadyOperation(
        true, true, 0, true, -1, false, 0, false, 100, 1);
    QVERIFY(audit.signalSucceeded);
    QVERIFY(!audit.setEventSucceeded);
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrGpuReadyOperation(
        true, true, 0, true, 0, true, 258, false, 100, 1);
    QVERIFY(audit.setEventSucceeded);
    QVERIFY(!audit.waitSucceeded);
    QVERIFY(audit.relationshipValid);

    // Positive HRESULT statuses advance the production stage chain, but are
    // deliberately not accepted as exact diagnostic success.
    audit = evaluateVrrGpuReadyOperation(
        true, true, 1, true, 0, true, 0, true, 100, 1);
    QVERIFY(audit.relationshipValid);
    QVERIFY(!audit.exactSuccess);

    audit = evaluateVrrGpuReadyOperation(
        true, true, 0, true, 0, false, 0, true, 100, 1);
    QVERIFY(!audit.relationshipValid);

    audit = evaluateVrrGpuReadyOperation(
        true, true, 0, true, 0, true, 0, true, 0, 1);
    QVERIFY(!audit.relationshipValid);
}

void VrrReplayConfigTest::gpuReadyStageTimingAudit()
{
    VrrGpuReadyStageTimingAudit audit =
        evaluateVrrGpuReadyStageTiming(
            100, 300, true, true, true,
            110, 112, 113, 120, 121, 122,
            123, 124, 125, 150);
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrGpuReadyStageTiming(
        100, 300, true, false, false,
        110, 112, 0, 0, 0, 0, 0, 0, 0, 0);
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrGpuReadyStageTiming(
        100, 300, true, true, false,
        110, 112, 113, 120, 121, 122,
        0, 0, 0, 0);
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrGpuReadyStageTiming(
        100, 300, true, true, true,
        110, 112, 111, 120, 121, 122,
        123, 124, 125, 150);
    QVERIFY(!audit.relationshipValid);

    audit = evaluateVrrGpuReadyStageTiming(
        100, 300, true, false, false,
        110, 112, 113, 120, 0, 0, 0, 0, 0, 0);
    QVERIFY(!audit.relationshipValid);

    audit = evaluateVrrGpuReadyStageTiming(
        0, 0, false, false, false,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrGpuReadyStageTiming(
        0, 0, false, false, false,
        1, 1, 0, 0, 0, 0, 0, 0, 0, 0);
    QVERIFY(!audit.relationshipValid);
}

void VrrReplayConfigTest::presenterSubmissionAudit()
{
    VrrPresenterSubmissionAudit audit =
        evaluateVrrPresenterSubmission(
            true, true, 120, 100, 140, true, 120);
    QVERIFY(audit.expectedPresenterTimeUsed);
    QCOMPARE(audit.expectedSubmissionBoundaryUs, uint64_t(120));
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrPresenterSubmission(
        true, false, 0, 100, 140, false, 100);
    QVERIFY(!audit.expectedPresenterTimeUsed);
    QCOMPARE(audit.expectedSubmissionBoundaryUs, uint64_t(100));
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrPresenterSubmission(
        true, true, 90, 100, 140, false, 100);
    QVERIFY(!audit.expectedPresenterTimeUsed);
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrPresenterSubmission(
        false, true, 120, 100, 140, false, 0);
    QVERIFY(!audit.expectedPresenterTimeUsed);
    QCOMPARE(audit.expectedSubmissionBoundaryUs, uint64_t(0));
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrPresenterSubmission(
        true, true, 120, 100, 140, false, 120);
    QVERIFY(!audit.relationshipValid);

    audit = evaluateVrrPresenterSubmission(
        true, true, 120, 100, 140, true, 121);
    QVERIFY(!audit.relationshipValid);

    audit = evaluateVrrPresenterSubmission(
        true, true, 120, 140, 100, false, 140);
    QVERIFY(!audit.expectedPresenterTimeUsed);
    QVERIFY(audit.relationshipValid);
}

void VrrReplayConfigTest::spacingCorrectionAudit()
{
    QVERIFY(evaluateVrrSpacingCorrection(
        true, 0, 0, false, false).relationshipValid);
    QVERIFY(evaluateVrrSpacingCorrection(
        true, 50, 50, true, true).relationshipValid);
    QVERIFY(evaluateVrrSpacingCorrection(
        true, 50, 0, true, false).relationshipValid);
    QVERIFY(evaluateVrrSpacingCorrection(
        true, 50, 25, true, true).relationshipValid);
    QVERIFY(evaluateVrrSpacingCorrection(
        false, 50, 0, true, false).relationshipValid);

    QVERIFY(!evaluateVrrSpacingCorrection(
        true, 50, 50, false, true).relationshipValid);
    QVERIFY(!evaluateVrrSpacingCorrection(
        true, 50, 51, true, true).relationshipValid);
    QVERIFY(!evaluateVrrSpacingCorrection(
        false, 50, 50, true, true).relationshipValid);
    QVERIFY(!evaluateVrrSpacingCorrection(
        true, 50, 50, true, false).relationshipValid);
    QVERIFY(!evaluateVrrSpacingCorrection(
        true, 0, 0, false, true).relationshipValid);
}

void VrrReplayConfigTest::spacingLifecycleTimingAudit()
{
    // Latched presentation can disable the software floor while retaining
    // deficit telemetry and a zero-deadline correction wait.
    auto latched = evaluateVrrSpacingLifecycleTiming(
        true, true, 1000, 100, 1050, 0, 0,
        1050, 1050, 1060, 40, 40, true,
        0, 1061, 1062, 1063);
    QVERIFY(latched.relationshipValid);
    QCOMPARE(latched.expectedRecheckDeficitUs, uint64_t(40));
    auto missingFloor = evaluateVrrSpacingLifecycleTiming(
        true, true, 1000, 100, 1050, 0, 1120,
        1050, 1050, 1060, 40, 40, true,
        0, 1061, 1120, 1121);
    QVERIFY(!missingFloor.relationshipValid);

    VrrSpacingLifecycleTimingAudit audit =
        evaluateVrrSpacingLifecycleTiming(
            true, false, 0, 100, 1000, 0, 0,
            900, 1000, 1001, 0, 0, false,
            0, 0, 0, 1010);
    QVERIFY(audit.relationshipValid);
    QCOMPARE(audit.expectedPresentationFloorUs, uint64_t(1000));

    // The first floor can require a wait while the immediate recheck is
    // clean. This is corrected timing, but not guard-learning feedback.
    audit = evaluateVrrSpacingLifecycleTiming(
        true, true, 800, 100, 900, 950, 940,
        900, 950, 1000, 50, 0, true,
        0, 0, 0, 1010);
    QVERIFY(audit.relationshipValid);
    QCOMPARE(audit.expectedFirstCheckDeficitUs, uint64_t(50));
    QCOMPARE(audit.expectedRecheckDeficitUs, uint64_t(0));

    audit = evaluateVrrSpacingLifecycleTiming(
        true, true, 1000, 100, 1120, 1120, 1150,
        1120, 1120, 1090, 10, 10, true,
        1150, 1091, 1150, 1160);
    QCOMPARE(audit.expectedRecheckDeficitUs, uint64_t(10));
    QVERIFY(!audit.relationshipValid);

    audit = evaluateVrrSpacingLifecycleTiming(
        false, false, 0, 0, 0, 0, 0,
        0, 0, 1, 0, 0, false,
        0, 0, 0, 0);
    QVERIFY(!audit.relationshipValid);
}

void VrrReplayConfigTest::postPresentQueryTimingAudit()
{
    VrrPostPresentQueryTimingAudit audit =
        evaluateVrrPostPresentQueryTiming(
            true, true, true, 120,
            121, 125, 126, 140, 150);
    QVERIFY(audit.expectedTiming);
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrPostPresentQueryTiming(
        false, true, true, 0,
        0, 0, 0, 0, 0);
    QVERIFY(!audit.expectedTiming);
    QVERIFY(audit.relationshipValid);

    audit = evaluateVrrPostPresentQueryTiming(
        false, true, true, 0,
        1, 1, 0, 0, 0);
    QVERIFY(!audit.relationshipValid);

    audit = evaluateVrrPostPresentQueryTiming(
        true, true, true, 120,
        121, 130, 129, 140, 150);
    QVERIFY(!audit.relationshipValid);

    audit = evaluateVrrPostPresentQueryTiming(
        true, true, true, 120,
        121, 125, 126, 151, 150);
    QVERIFY(!audit.relationshipValid);
}

void VrrReplayConfigTest::periodicInjectionSelector()
{
    QVERIFY(vrrPeriodicInjectionSelected(60, 60, 0, 1));
    QVERIFY(vrrPeriodicInjectionSelected(120, 60, 0, 1));
    QVERIFY(!vrrPeriodicInjectionSelected(1, 60, 0, 1));

    QVERIFY(vrrPeriodicInjectionSelected(30, 120, 30, 4));
    QVERIFY(vrrPeriodicInjectionSelected(31, 120, 30, 4));
    QVERIFY(vrrPeriodicInjectionSelected(33, 120, 30, 4));
    QVERIFY(!vrrPeriodicInjectionSelected(34, 120, 30, 4));
    QVERIFY(vrrPeriodicInjectionSelected(150, 120, 30, 4));

    QVERIFY(vrrPeriodicInjectionSelected(59, 60, 59, 3));
    QVERIFY(vrrPeriodicInjectionSelected(60, 60, 59, 3));
    QVERIFY(vrrPeriodicInjectionSelected(61, 60, 59, 3));
    QVERIFY(!vrrPeriodicInjectionSelected(62, 60, 59, 3));

    QVERIFY(!vrrPeriodicInjectionSelected(1, 0, 0, 1));
    QVERIFY(!vrrPeriodicInjectionSelected(1, 60, 60, 1));
    QVERIFY(!vrrPeriodicInjectionSelected(1, 60, 0, 0));
    QVERIFY(!vrrPeriodicInjectionSelected(1, 60, 0, 61));
}

void VrrReplayConfigTest::wakeDelayInjectionEligibility()
{
    VrrWakeDelayInjectionResult result =
        evaluateVrrWakeDelayInjection(
            1000, 1000, 500, 0, 500, 125);
    QVERIFY(!result.deadlineInFuture);
    QVERIFY(!result.coarseSleepExpected);
    QCOMPARE(result.appliedDelayUs, uint64_t(0));
    QCOMPARE(result.suppressedDelayUs, uint64_t(125));

    result = evaluateVrrWakeDelayInjection(
        1000, 1500, 500, 0, 500, 125);
    QVERIFY(result.deadlineInFuture);
    QVERIFY(!result.coarseSleepExpected);
    QCOMPARE(result.appliedDelayUs, uint64_t(0));

    result = evaluateVrrWakeDelayInjection(
        1000, 1501, 500, 0, 500, 125);
    QVERIFY(result.coarseSleepExpected);
    QCOMPARE(result.appliedDelayUs, uint64_t(125));
    QCOMPARE(result.suppressedDelayUs, uint64_t(0));

    result = evaluateVrrWakeDelayInjection(
        1000, 1900, 500, 400, 500, 175);
    QVERIFY(!result.coarseSleepExpected);
    result = evaluateVrrWakeDelayInjection(
        1000, 1901, 500, 400, 500, 175);
    QVERIFY(result.coarseSleepExpected);
    QCOMPARE(result.appliedDelayUs, uint64_t(175));

    result = evaluateVrrWakeDelayInjection(
        1000, 2001, 500, 900, 500, 175);
    QVERIFY(result.coarseSleepExpected);
}

void VrrReplayConfigTest::waitLifecycleAudit()
{
    VrrWaitLifecycleEvidence evidence;
    evidence.callEntryUs = 900;
    evidence.deadlineUs = 2000;
    evidence.initialNowUs = 901;
    evidence.finalNowUs = 2000;
    evidence.activeWaitUs = 500;
    evidence.coarseSleepCount = 1;
    evidence.coarseSleepRequestedUs = 599;
    evidence.coarseSleepRequestedWakeUs = 1500;
    evidence.coarseSleepReturnUs = 1600;
    evidence.activeWaitEntered = true;
    evidence.activeWaitStartUs = 1600;
    evidence.activeWaitLimitUs = 2100;
    evidence.activeWaitYieldCount = 20;
    evidence.schedulerDelayValid = true;
    VrrWaitLifecycleAudit audit = evaluateVrrWaitLifecycle(
        evidence, 500, 500);
    QVERIFY(audit.coarseSleepExpected);
    QVERIFY(audit.relationshipValid);
    QVERIFY(audit.completedDeadline);
    QVERIFY(audit.cleanCompletion);

    VrrWakeDelayInjectionResult injection =
        evaluateVrrRecordedWakeDelayInjection(
            evidence, 900, 2000, 0, 500, 500, 300);
    QVERIFY(injection.coarseSleepExpected);
    QVERIFY(injection.recordedPathMatchesCandidate);
    QVERIFY(injection.usedRecordedFinalResidual);
    QVERIFY(injection.usedRecordedCoarseTelemetry);
    QCOMPARE(injection.appliedDelayUs, uint64_t(300));
    QCOMPARE(injection.absorbedDelayUs, uint64_t(300));
    QCOMPARE(injection.executionDelayUs, uint64_t(0));
    QCOMPARE(injection.simulatedFinalUs, uint64_t(2000));
    QCOMPARE(injection.schedulerDelayUs, uint64_t(0));

    injection = evaluateVrrRecordedWakeDelayInjection(
        evidence, 900, 2000, 0, 500, 500, 700);
    QCOMPARE(injection.appliedDelayUs, uint64_t(700));
    QCOMPARE(injection.absorbedDelayUs, uint64_t(400));
    QCOMPARE(injection.executionDelayUs, uint64_t(300));
    QCOMPARE(injection.simulatedCoarseReturnUs, uint64_t(2300));
    QCOMPARE(injection.simulatedFinalUs, uint64_t(2300));
    QCOMPARE(injection.schedulerDelayUs, uint64_t(300));
    QVERIFY(injection.schedulerDelayValid);

    evidence = VrrWaitLifecycleEvidence {};
    evidence.callEntryUs = 1000;
    evidence.deadlineUs = 1400;
    evidence.initialNowUs = 1001;
    evidence.finalNowUs = 1400;
    evidence.activeWaitUs = 500;
    evidence.activeWaitEntered = true;
    evidence.activeWaitStartUs = 1001;
    evidence.activeWaitLimitUs = 1501;
    evidence.activeWaitYieldCount = 10;
    audit = evaluateVrrWaitLifecycle(evidence, 500, 500);
    QVERIFY(!audit.coarseSleepExpected);
    QVERIFY(audit.cleanCompletion);
    injection = evaluateVrrRecordedWakeDelayInjection(
        evidence, 1000, 1400, 0, 500, 500, 250);
    QVERIFY(injection.recordedPathMatchesCandidate);
    QVERIFY(injection.usedRecordedFinalResidual);
    QCOMPARE(injection.appliedDelayUs, uint64_t(0));
    QCOMPARE(injection.suppressedDelayUs, uint64_t(250));
    QCOMPARE(injection.simulatedFinalUs, uint64_t(1400));

    // Moving the candidate deadline inside the active-wait window changes
    // paths from the recorded coarse sleep. There is no captured OS residual
    // for that counterfactual path, so replay must expose and use the ideal
    // deadline rather than borrowing the unrelated coarse-return residual.
    evidence = VrrWaitLifecycleEvidence {};
    evidence.callEntryUs = 900;
    evidence.deadlineUs = 2000;
    evidence.initialNowUs = 901;
    evidence.finalNowUs = 2050;
    evidence.activeWaitUs = 500;
    evidence.coarseSleepCount = 1;
    evidence.coarseSleepRequestedUs = 599;
    evidence.coarseSleepRequestedWakeUs = 1500;
    evidence.coarseSleepReturnUs = 1600;
    evidence.activeWaitEntered = true;
    evidence.activeWaitStartUs = 1600;
    evidence.activeWaitLimitUs = 2100;
    evidence.activeWaitYieldCount = 20;
    evidence.schedulerDelayValid = true;
    injection = evaluateVrrRecordedWakeDelayInjection(
        evidence, 900, 1300, 0, 500, 500, 250);
    QVERIFY(!injection.coarseSleepExpected);
    QVERIFY(!injection.recordedPathMatchesCandidate);
    QVERIFY(!injection.usedRecordedFinalResidual);
    QVERIFY(!injection.usedRecordedCoarseTelemetry);
    QCOMPARE(injection.baselineFinalUs, uint64_t(1300));
    QCOMPARE(injection.simulatedFinalUs, uint64_t(1300));
    QCOMPARE(injection.suppressedDelayUs, uint64_t(250));

    evidence = VrrWaitLifecycleEvidence {};
    evidence.callEntryUs = 1499;
    evidence.deadlineUs = 1400;
    evidence.initialNowUs = 1500;
    evidence.finalNowUs = 1500;
    evidence.activeWaitUs = 500;
    evidence.deadlineAlreadyElapsed = true;
    audit = evaluateVrrWaitLifecycle(evidence, 500, 500);
    QVERIFY(audit.relationshipValid);
    QVERIFY(audit.cleanCompletion);

    evidence.activeWaitUs = 499;
    audit = evaluateVrrWaitLifecycle(evidence, 500, 500);
    QVERIFY(!audit.relationshipValid);

    // Two interrupted coarse sleeps may be valid, but the recorded total
    // request must include the full first request plus at least one
    // microsecond for every retry. The final retry must reach the requested
    // early-wake boundary unless the explicit clock-stall exit fired.
    evidence = VrrWaitLifecycleEvidence {};
    evidence.callEntryUs = 900;
    evidence.deadlineUs = 2000;
    evidence.initialNowUs = 901;
    evidence.finalNowUs = 2000;
    evidence.activeWaitUs = 500;
    evidence.coarseSleepCount = 2;
    evidence.coarseSleepRequestedUs = 998;
    evidence.coarseSleepRequestedWakeUs = 1500;
    evidence.coarseSleepReturnUs = 1500;
    evidence.activeWaitEntered = true;
    evidence.activeWaitStartUs = 1500;
    evidence.activeWaitLimitUs = 2000;
    evidence.activeWaitYieldCount = 10;
    evidence.schedulerDelayValid = true;
    audit = evaluateVrrWaitLifecycle(evidence, 500, 500);
    QVERIFY(audit.cleanCompletion);

    evidence.coarseSleepRequestedUs = 599;
    audit = evaluateVrrWaitLifecycle(evidence, 500, 500);
    QVERIFY(!audit.relationshipValid);

    evidence.coarseSleepRequestedUs = 998;
    evidence.coarseSleepReturnUs = 1499;
    evidence.activeWaitStartUs = 1499;
    evidence.activeWaitLimitUs = 1999;
    audit = evaluateVrrWaitLifecycle(evidence, 500, 500);
    QVERIFY(!audit.relationshipValid);

    // New waiters are bounded by elapsed active time rather than a
    // machine-dependent yield count.
    evidence = VrrWaitLifecycleEvidence {};
    evidence.callEntryUs = 900;
    evidence.deadlineUs = 2000;
    evidence.initialNowUs = 1000;
    evidence.finalNowUs = 2000;
    evidence.additionalWakeLeadUs = 500;
    evidence.activeWaitUs = 1000;
    evidence.activeWaitEntered = true;
    evidence.activeWaitStartUs = 1000;
    evidence.activeWaitLimitUs = 2000;
    evidence.activeWaitYieldCount = 8000;
    audit = evaluateVrrWaitLifecycle(evidence, 500, 500);
    QVERIFY(audit.relationshipValid);
    QVERIFY(audit.cleanCompletion);

    // Schema-5 captures made by the old fixed-4096 waiter remain auditable.
    evidence.finalNowUs = 1900;
    evidence.activeWaitYieldCount = 4096;
    evidence.activeWaitYieldLimitReached = true;
    audit = evaluateVrrWaitLifecycle(evidence, 500, 500);
    QVERIFY(audit.relationshipValid);
    QVERIFY(!audit.completedDeadline);
    QVERIFY(!audit.cleanCompletion);
}

void VrrReplayConfigTest::dxgiCapabilityAudit()
{
    VrrDxgiCapabilityAudit audit = evaluateVrrDxgiCapability(
        true,
        true, 0, true, true,
        true, 0, 0x800, 4, true,
        true, 0, false, 0x1001, true);
    QVERIFY(audit.declarationsMatchBackend);
    QVERIFY(audit.featureQuerySucceeded);
    QVERIFY(audit.descriptorQuerySucceeded);
    QVERIFY(audit.fullscreenQuerySucceeded);
    QVERIFY(audit.flipModel);
    QVERIFY(audit.borderlessWindow);
    QVERIFY(audit.swapChainAllowsTearing);
    QVERIFY(audit.relationshipsValid);
    QVERIFY(audit.exactEligible);

    // SUCCEEDED(S_FALSE) matches the production derivation but is not exact
    // enough for a replay-grade capability proof.
    audit = evaluateVrrDxgiCapability(
        true,
        true, 1, true, true,
        true, 0, 0x800, 3, true,
        true, 0, false, 0x1001, true);
    QVERIFY(audit.relationshipsValid);
    QVERIFY(!audit.featureQuerySucceeded);
    QVERIFY(!audit.exactEligible);

    audit = evaluateVrrDxgiCapability(
        true,
        true, 0, true, true,
        true, 0, 0, 4, true,
        true, 0, false, 0x1001, true);
    QVERIFY(!audit.relationshipsValid);
    QVERIFY(!audit.exactEligible);

    audit = evaluateVrrDxgiCapability(
        true,
        true, 0, true, true,
        true, 0, 0x800, 1, true,
        true, 0, false, 0x1001, false);
    QVERIFY(audit.relationshipsValid);
    QVERIFY(!audit.flipModel);
    QVERIFY(!audit.exactEligible);

    audit = evaluateVrrDxgiCapability(
        true,
        true, 0, true, true,
        true, 0, 0x800, 4, true,
        true, 0, true, 0x1001, false);
    QVERIFY(audit.relationshipsValid);
    QVERIFY(!audit.borderlessWindow);
    QVERIFY(!audit.exactEligible);

    audit = evaluateVrrDxgiCapability(
        false,
        false, 0, false, false,
        false, 0, 0, 0, false,
        false, 0, false, 0, false);
    QVERIFY(audit.declarationsMatchBackend);
    QVERIFY(audit.relationshipsValid);
    QVERIFY(!audit.exactEligible);

    audit = evaluateVrrDxgiCapability(
        false,
        false, 0, false, false,
        false, 0, 0, 0, false,
        false, 0, false, 0x1001, false);
    QVERIFY(!audit.relationshipsValid);
}

// The CLI regression locates and launches the sibling replay executable.
// A QCoreApplication supplies its executable path without requiring a GUI.
QTEST_GUILESS_MAIN(VrrReplayConfigTest)

#include "tst_vrrreplayconfig.moc"
