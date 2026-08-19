#include <QtTest>

#include <limits>

#include "../../app/backend/clientdisplaycapabilities.h"

class ClientDisplayCapabilitiesTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizationRoundsToWholeNits();
    void normalizationRejectsInvalidValues();
    void sourceValuesRemainLocalValueOnlyData();
};

void ClientDisplayCapabilitiesTest::normalizationRoundsToWholeNits()
{
    QCOMPARE(ClientDisplayCapabilities::normalizePeakLuminance(999.4), std::optional<int> {999});
    QCOMPARE(ClientDisplayCapabilities::normalizePeakLuminance(999.5), std::optional<int> {1000});
    QCOMPARE(ClientDisplayCapabilities::normalizePeakLuminance(999.6), std::optional<int> {1000});
}

void ClientDisplayCapabilitiesTest::normalizationRejectsInvalidValues()
{
    const double invalidValues[] = {
        0.0,
        -1.0,
        100000.1,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
    };

    for (const double value : invalidValues) {
        QVERIFY(!ClientDisplayCapabilities::normalizePeakLuminance(value).has_value());
    }
}

void ClientDisplayCapabilitiesTest::sourceValuesRemainLocalValueOnlyData()
{
    ClientDisplayCapabilities capabilities;
    capabilities.calibrated = ClientDisplayCapabilitySource {
        QStringLiteral("windows-icc-mhc2"),
        1600.0,
    };
    capabilities.edid = ClientDisplayCapabilitySource {
        QStringLiteral("dxgi-output"),
        1200.0,
    };

    QCOMPARE(capabilities.calibrated->source, QStringLiteral("windows-icc-mhc2"));
    QCOMPARE(capabilities.calibrated->peakLuminanceNits, 1600.0);
    QCOMPARE(capabilities.edid->source, QStringLiteral("dxgi-output"));
    QCOMPARE(capabilities.edid->peakLuminanceNits, 1200.0);
}

QTEST_APPLESS_MAIN(ClientDisplayCapabilitiesTest)

#include "tst_clientdisplaycapabilities.moc"
