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

QTEST_APPLESS_MAIN(VrrRatePolicyTest)

#include "tst_vrrratepolicy.moc"
