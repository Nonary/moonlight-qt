#include "vrrreplayconfig.h"

#include <QJsonDocument>
#include <QtTest>

class VrrReplayConfigTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsRoundTrip();
    void inheritanceAndOverride();
    void rejectsInvalidInput();
};

void VrrReplayConfigTest::defaultsRoundTrip()
{
    VrrReplayConfiguration config;
    QString error;
    QVERIFY2(loadVrrReplayConfiguration(
        QJsonDocument(vrrDefaultReplayConfigurationJson()).toJson(),
        config, error), qPrintable(error));
    QCOMPARE(config.scenarios.size(), 1);
    QCOMPARE(config.scenarios.front().controller.renderLeadFloorUs,
             uint64_t(1000));
    QCOMPARE(config.scenarios.front().worker.queueCapacity, size_t(3));
    QVERIFY(vrrReplayParameterNames().contains("controller.guard_step_us"));
}

void VrrReplayConfigTest::inheritanceAndOverride()
{
    const QByteArray json = R"json({
      "config_schema": 1,
      "parameters": {"controller": {"guard_step_us": 75}},
      "scenarios": [{
        "name": "wide-queue",
        "mode": "worker",
        "parameters": {"worker": {"queue_capacity": 6}},
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
    QCOMPARE(config.scenarios.front().assertions.size(), 1);
    QVERIFY2(applyVrrReplayOverride("controller.guard_step_us=125",
                                   config.scenarios.front(), error),
             qPrintable(error));
    QCOMPARE(config.scenarios.front().controller.guardStepUs, uint64_t(125));
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

    VrrReplayScenario scenario;
    QVERIFY(!applyVrrReplayOverride("guard_step_us=25", scenario, error));
}

QTEST_APPLESS_MAIN(VrrReplayConfigTest)

#include "tst_vrrreplayconfig.moc"
