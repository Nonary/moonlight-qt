#include <QtTest>

#include "../../app/streaming/vrrratepolicy.h"

class VrrRatePolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void calculatedRates();
    void adaptiveHeadroomQualification();
    void vrrChoicesOmitNativeRefresh();
    void disabledChoicesKeepNativeRefresh();
};

void VrrRatePolicyTest::calculatedRates()
{
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(60), 59);
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(120), 116);
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(144), 138);
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(165), 157);
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(0), 0);

    QCOMPARE(VrrRatePolicy::lowLatencyRateForRefresh(60), 50);
    QCOMPARE(VrrRatePolicy::lowLatencyRateForRefresh(120), 100);
    QCOMPARE(VrrRatePolicy::lowLatencyRateForRefresh(144), 120);
    QCOMPARE(VrrRatePolicy::lowLatencyRateForRefresh(165), 135);
    QCOMPARE(VrrRatePolicy::lowLatencyRateForRefresh(0), 0);
}

void VrrRatePolicyTest::adaptiveHeadroomQualification()
{
    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(59, 60));
    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(116, 120));
    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(138, 144));

    QVERIFY(!VrrRatePolicy::hasAdaptiveHeadroom(60, 60));
    QVERIFY(!VrrRatePolicy::hasAdaptiveHeadroom(119, 120));
    QVERIFY(!VrrRatePolicy::hasAdaptiveHeadroom(121, 120));
    QVERIFY(!VrrRatePolicy::hasAdaptiveHeadroom(0, 120));
    QVERIFY(!VrrRatePolicy::hasAdaptiveHeadroom(60, 0));
}

void VrrRatePolicyTest::vrrChoicesOmitNativeRefresh()
{
    const std::vector<VrrFpsChoice> choices = VrrRatePolicy::buildChoices({120}, 90, true);

    QCOMPARE(static_cast<int>(choices.size()), 5);
    QCOMPARE(choices[0].fps, 30);
    QCOMPARE(static_cast<int>(choices[0].kind), static_cast<int>(VrrFpsChoiceKind::Baseline));
    QCOMPARE(choices[1].fps, 60);
    QCOMPARE(static_cast<int>(choices[1].kind), static_cast<int>(VrrFpsChoiceKind::Baseline));
    QCOMPARE(choices[2].fps, 90);
    QCOMPARE(static_cast<int>(choices[2].kind), static_cast<int>(VrrFpsChoiceKind::Custom));
    QCOMPARE(choices[3].fps, 100);
    QCOMPARE(static_cast<int>(choices[3].kind), static_cast<int>(VrrFpsChoiceKind::LowLatencyVrr));
    QCOMPARE(choices[4].fps, 116);
    QCOMPARE(static_cast<int>(choices[4].kind), static_cast<int>(VrrFpsChoiceKind::Vrr));

    for (const VrrFpsChoice& choice : choices) {
        QVERIFY(choice.fps != 120);
    }
}

void VrrRatePolicyTest::disabledChoicesKeepNativeRefresh()
{
    const std::vector<VrrFpsChoice> choices = VrrRatePolicy::buildChoices({120, 144}, 90, false);

    QCOMPARE(static_cast<int>(choices.size()), 5);
    QCOMPARE(choices[0].fps, 30);
    QCOMPARE(choices[1].fps, 60);
    QCOMPARE(choices[2].fps, 90);
    QCOMPARE(static_cast<int>(choices[2].kind), static_cast<int>(VrrFpsChoiceKind::Custom));
    QCOMPARE(choices[3].fps, 120);
    QCOMPARE(static_cast<int>(choices[3].kind), static_cast<int>(VrrFpsChoiceKind::Native));
    QCOMPARE(choices[4].fps, 144);
    QCOMPARE(static_cast<int>(choices[4].kind), static_cast<int>(VrrFpsChoiceKind::Native));
}

QTEST_APPLESS_MAIN(VrrRatePolicyTest)

#include "tst_vrrratepolicy.moc"
