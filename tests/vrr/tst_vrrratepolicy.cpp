#include <QtTest>
#include <limits>

#include "../../app/streaming/vrrratepolicy.h"

class VrrRatePolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void calculatedRates();
    void protectedRates();
    void adaptiveHeadroomQualification();
    void choicesIncludeDisplayRates();
    void windowsVrrPresetsAndToggle();
    void savedChoicesRemainSelectable();
};

void VrrRatePolicyTest::calculatedRates()
{
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(60), 59);
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(120), 116);
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(144), 138);
    QCOMPARE(VrrRatePolicy::vrrRateForRefresh(165), 157);
    for (int invalid : {-1, 0, 1, 3600, std::numeric_limits<int>::max()}) {
        QCOMPARE(VrrRatePolicy::vrrRateForRefresh(invalid), 0);
    }

}

void VrrRatePolicyTest::protectedRates()
{
    QCOMPARE(VrrRatePolicy::protectedRateForRefresh(60), 59);
    QCOMPARE(VrrRatePolicy::protectedRateForRefresh(120), 116);
    QCOMPARE(VrrRatePolicy::protectedRateForRefresh(144), 138);
    QCOMPARE(VrrRatePolicy::protectedRateForRefresh(165), 157);
    QCOMPARE(VrrRatePolicy::protectedRateForRefresh(240), 224);
    QCOMPARE(VrrRatePolicy::protectedRateForRefresh(360), 324);
    for (int invalid : {-1, 0, 1, 3600, std::numeric_limits<int>::max()}) {
        QCOMPARE(VrrRatePolicy::protectedRateForRefresh(invalid), 0);
    }
}

void VrrRatePolicyTest::adaptiveHeadroomQualification()
{
    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(59, 60));
    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(116, 120));
    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(138, 144));

    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(60, 60));
    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(119, 120));
    QVERIFY(VrrRatePolicy::hasAdaptiveHeadroom(120, 120));
    QVERIFY(!VrrRatePolicy::hasAdaptiveHeadroom(121, 120));
    QVERIFY(!VrrRatePolicy::hasAdaptiveHeadroom(0, 120));
    QVERIFY(!VrrRatePolicy::hasAdaptiveHeadroom(60, 0));
}

void VrrRatePolicyTest::choicesIncludeDisplayRates()
{
    const auto choices = VrrRatePolicy::buildChoices({0, 1, 60, 144, 120, 120}, 90);
    const std::vector<int> expected = {30, 59, 60, 90, 116, 120, 138, 144};
    QCOMPARE(choices.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        QCOMPARE(choices[i].fps, expected[i]);
        QCOMPARE(choices[i].kind, expected[i] == 90 ? VrrFpsChoiceKind::Custom : VrrFpsChoiceKind::Fixed);
    }
}

void VrrRatePolicyTest::savedChoicesRemainSelectable()
{
    for (int saved : {90, 100, 116, 120}) {
        const auto choices = VrrRatePolicy::buildChoices({120}, saved);
        int matches = 0;
        for (const auto& choice : choices) {
            if (choice.fps == saved) {
                ++matches;
                QCOMPARE(choice.kind, saved == 116 || saved == 120 ?
                         VrrFpsChoiceKind::Fixed : VrrFpsChoiceKind::Custom);
            }
        }
        QCOMPARE(matches, 1);
    }
}

void VrrRatePolicyTest::windowsVrrPresetsAndToggle()
{
    struct Rates { int display; int near; int low; };
    for (const auto rates : {Rates{120, 116, 100}, Rates{144, 138, 120}, Rates{288, 264, 240}}) {
        QCOMPARE(VrrRatePolicy::lowLatencyRateForRefresh(rates.display), rates.low);
        QCOMPARE(VrrRatePolicy::ultraLowLatencyMaxFps(rates.display, true), rates.near);
        QCOMPARE(VrrRatePolicy::ultraLowLatencyMaxFps(rates.display, false), rates.low);

        const auto choices = VrrRatePolicy::buildChoices({rates.display}, rates.display, true);
        QCOMPARE(choices.size(), size_t(5));
        QCOMPARE(choices[2].fps, rates.low);
        QCOMPARE(choices[2].kind, VrrFpsChoiceKind::LowLatencyVrr);
        QCOMPARE(choices[3].fps, rates.near);
        QCOMPARE(choices[3].kind, VrrFpsChoiceKind::Fixed);
        QCOMPARE(choices[4].fps, rates.display);
        QCOMPARE(choices[4].kind, VrrFpsChoiceKind::Fixed);
        for (int cap : {rates.near, rates.low}) {
            // A saved FPS above the cap must not reappear as a custom choice.
            const auto limited = VrrRatePolicy::buildChoices({rates.display}, 999, true, cap);
            QCOMPARE(limited.back().fps, cap);
            QVERIFY(limited.back().kind != VrrFpsChoiceKind::Custom);
        }

        QCOMPARE(VrrRatePolicy::rateAfterVrrToggle(rates.display, rates.display, true), rates.near);
        QCOMPARE(VrrRatePolicy::rateAfterVrrToggle(rates.near, rates.display, false), rates.display);
        QCOMPARE(VrrRatePolicy::rateAfterVrrToggle(rates.low, rates.display, false), rates.low);
        QCOMPARE(VrrRatePolicy::rateAfterVrrToggle(90, rates.display, true), 90);
    }

    const auto mixed = VrrRatePolicy::buildChoices({120, 144}, 120, true);
    QCOMPARE(mixed[4].fps, 120);
    QCOMPARE(mixed[4].kind, VrrFpsChoiceKind::Fixed);
    QCOMPARE(VrrRatePolicy::rateAfterVrrToggle(120, 0, true), 120);
    QCOMPARE(VrrRatePolicy::ultraLowLatencyMaxFps(0, true), 0);
}

QTEST_APPLESS_MAIN(VrrRatePolicyTest)

#include "tst_vrrratepolicy.moc"
