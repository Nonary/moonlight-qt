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
    void launchAndResumeQueryGating_data();
    void launchAndResumeQueryGating();
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

void ClientDisplayCapabilitiesTest::launchAndResumeQueryGating_data()
{
    QTest::addColumn<QString>("verb");
    QTest::addColumn<int>("version");
    QTest::addColumn<bool>("hdr");
    QTest::addColumn<int>("calibrated");
    QTest::addColumn<int>("edid");
    QTest::addColumn<QString>("expected");

    for (const auto& verb : {QStringLiteral("launch"), QStringLiteral("resume")}) {
        const auto row = [&verb](const char* name, int version, bool hdr,
                                 int calibrated, int edid, const QString& expected) {
            const QByteArray label = verb.toLatin1() + '-' + name;
            QTest::newRow(label.constData()) << verb << version << hdr << calibrated << edid << expected;
        };
        row("both", 1, true, 1600, 1200,
            QStringLiteral("&clientHdrPeakCalibrated=1600&clientHdrPeakEdid=1200"));
        row("boundaries", 1, true, 1, 100000,
            QStringLiteral("&clientHdrPeakCalibrated=1&clientHdrPeakEdid=100000"));
        row("missing-version", 0, true, 1600, 1200, {});
        row("future-version", 2, true, 1600, 1200, {});
        row("negative-version", -1, true, 1600, 1200, {});
        row("sdr", 1, false, 1600, 1200, {});
        row("absent", 1, true, 0, 0, {});
        row("invalid", 1, true, -1, 100001, {});
        row("calibrated-only", 1, true, 1600, 100001,
            QStringLiteral("&clientHdrPeakCalibrated=1600"));
        row("edid-only", 1, true, -1, 1200,
            QStringLiteral("&clientHdrPeakEdid=1200"));
    }
    QTest::newRow("other-verb") << QStringLiteral("quit") << 1 << true << 1600 << 1200 << QString();
}

void ClientDisplayCapabilitiesTest::launchAndResumeQueryGating()
{
    QFETCH(QString, verb);
    QFETCH(int, version);
    QFETCH(bool, hdr);
    QFETCH(int, calibrated);
    QFETCH(int, edid);
    QFETCH(QString, expected);
    QCOMPARE(ClientDisplayCapabilities::hdrPeakQueryArguments(verb, version, hdr, calibrated, edid),
             expected);
}

QTEST_APPLESS_MAIN(ClientDisplayCapabilitiesTest)

#include "tst_clientdisplaycapabilities.moc"
