#include <QtTest>

#include <vector>

#include "../../app/backend/clientdisplaycapabilities_win.h"

using namespace ClientDisplayCapabilitiesWin;

namespace {

QByteArray mhc2Profile(double peakNits)
{
    QByteArray profile(180, '\0');

    auto writeU32 = [&profile](int offset, quint32 value) {
        profile[offset] = static_cast<char>((value >> 24) & 0xff);
        profile[offset + 1] = static_cast<char>((value >> 16) & 0xff);
        profile[offset + 2] = static_cast<char>((value >> 8) & 0xff);
        profile[offset + 3] = static_cast<char>(value & 0xff);
    };

    writeU32(0, static_cast<quint32>(profile.size()));
    profile.replace(36, 4, "acsp");
    writeU32(128, 1);
    profile[132] = 'M';
    profile[133] = 'H';
    profile[134] = 'C';
    profile[135] = '2';
    writeU32(136, 144);
    writeU32(140, 36);
    profile[144] = 'M';
    profile[145] = 'H';
    profile[146] = 'C';
    profile[147] = '2';
    writeU32(160, static_cast<quint32>(qRound64(peakNits * 65536.0)));
    return profile;
}

struct Fixture
{
    SelectedDisplay selected;
    DisplayMapping mapping;
    ProfileProbe profile;
    DxgiOutputProbe output;
    bool identityStable = true;

    CollectorProvider provider() const
    {
        const Fixture *fixture = this;
        CollectorProvider provider;
        provider.resolveSelectedDisplay = [fixture](const QScreen *, quintptr) {
            return fixture->selected;
        };
        provider.mapDisplayConfig = [fixture](const SelectedDisplay &) {
            return fixture->mapping;
        };
        provider.readColorProfile = [fixture](const DisplayMapping &, qint64) {
            return fixture->profile;
        };
        provider.readDxgiOutput = [fixture](const DisplayMapping &) {
            return fixture->output;
        };
        provider.revalidate = [fixture](const DisplayMapping &, const DxgiOutputProbe &) {
            return fixture->identityStable;
        };
        provider.revalidateSelected = [fixture](const QScreen *, quintptr,
                                                const SelectedDisplay &,
                                                const DisplayMapping &,
                                                const DxgiOutputProbe &) {
            return fixture->identityStable;
        };
        return provider;
    }

};

Fixture validFixture()
{
    Fixture fixture;
    fixture.selected.resolved = true;
    fixture.selected.unique = true;
    fixture.selected.hiddenWindowMatches = true;
    fixture.selected.displayIndex = 2;
    fixture.selected.gdiDeviceName = QStringLiteral("\\\\.\\DISPLAY3");
    fixture.selected.geometry = QRect(-2560, 0, 2560, 1440);

    fixture.mapping.valid = true;
    fixture.mapping.uniqueActivePath = true;
    fixture.mapping.identityComplete = true;
    fixture.mapping.displayIndex = 2;
    fixture.mapping.gdiDeviceName = fixture.selected.gdiDeviceName;
    fixture.mapping.outputDeviceName = QStringLiteral("\\\\.\\DISPLAY3\\Monitor0");
    fixture.mapping.desktopBounds = fixture.selected.geometry;
    fixture.mapping.sourceAdapterLuid = 0x100;
    fixture.mapping.sourceId = 3;
    fixture.mapping.targetAdapterLuid = 0x200;
    fixture.mapping.targetId = 4;
    fixture.mapping.pathFlags = 1;
    fixture.mapping.targetAvailable = true;
    fixture.mapping.dxgiAdapterIndex = 1;
    fixture.mapping.dxgiOutputIndex = 0;

    fixture.profile.exportsAvailable = true;
    fixture.profile.osSupported = true;
    fixture.profile.scopeSupported = true;
    fixture.profile.lookupStatus = ProfileLookupStatus::ExtendedDefaultAvailable;
    fixture.profile.profileType = ProfileType::Icc;
    fixture.profile.displayColorMode = DisplayColorMode::Extended;
    fixture.profile.profileName = QStringLiteral("Display.icc");
    fixture.profile.bytes = mhc2Profile(999.5);
    fixture.profile.readCompleted = true;
    fixture.profile.elapsedMs = 10;

    fixture.output.valid = true;
    fixture.output.current = true;
    fixture.output.unique = true;
    fixture.output.attachedToDesktop = true;
    fixture.output.deviceName = fixture.mapping.outputDeviceName;
    fixture.output.desktopBounds = fixture.mapping.desktopBounds;
    fixture.output.adapterIndex = fixture.mapping.dxgiAdapterIndex;
    fixture.output.outputIndex = fixture.mapping.dxgiOutputIndex;
    fixture.output.colorSpace = DxgiColorSpace::RgbFullG2084NoneP2020;
    fixture.output.maxLuminance = 1200.0;
    return fixture;
}

}

class ClientDisplayCapabilitiesWinTest : public QObject
{
    Q_OBJECT

private slots:
    void nullSelectedScreenIsFailClosed();
    void missingExportsKeepIndependentDxgiFallback();
    void standardOnlyProfileIsOmitted();
    void malformedMhc2IsOmitted();
    void oversizedOrSlowProfileIsBounded();
    void ambiguousMappingIsFailClosed();
    void negativeCoordinateMismatchIsFailClosed();
    void hiddenWindowMismatchIsFailClosed();
    void unsafeProfileNameKeepsIndependentFallback();
    void nonPqDetachedOrCloneOutputIsOmitted();
    void identityMutationInvalidatesBothSources();
    void bothSourcesAreNormalizedAndReturned();
    void missingExtendedDefaultKeepsIndependentDxgi();
    void scaledQtGeometryUsesHiddenDisplayIdentity();
    void wrongHiddenDisplayIsRejected();
    void ambiguousDisplayOriginsAreRejected();
    void truncatedMhc2HeaderIsRejected();
    void invalidIccHeaderIsRejected();
};

void ClientDisplayCapabilitiesWinTest::nullSelectedScreenIsFailClosed()
{
    Fixture fixture = validFixture();

    const auto result = collectCapabilities(nullptr, 0, fixture.provider());

    QVERIFY(!result.has_value());
}

void ClientDisplayCapabilitiesWinTest::missingExportsKeepIndependentDxgiFallback()
{
    Fixture fixture = validFixture();
    fixture.profile.exportsAvailable = false;

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(result.has_value());
    QVERIFY(!result->calibratedPeakNits.has_value());
    QVERIFY(result->edidPeakNits.has_value());
    QCOMPARE(*result->edidPeakNits, 1200);
}

void ClientDisplayCapabilitiesWinTest::standardOnlyProfileIsOmitted()
{
    Fixture fixture = validFixture();
    fixture.profile.displayColorMode = DisplayColorMode::Standard;

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(result.has_value());
    QVERIFY(!result->calibratedPeakNits.has_value());
    QVERIFY(result->edidPeakNits.has_value());
}

void ClientDisplayCapabilitiesWinTest::malformedMhc2IsOmitted()
{
    Fixture fixture = validFixture();
    fixture.profile.bytes = QByteArray(164, '\0');

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(result.has_value());
    QVERIFY(!result->calibratedPeakNits.has_value());
    QVERIFY(result->edidPeakNits.has_value());
    QCOMPARE(*result->edidPeakNits, 1200);
}

void ClientDisplayCapabilitiesWinTest::oversizedOrSlowProfileIsBounded()
{
    Fixture fixture = validFixture();
    fixture.profile.bytes = QByteArray(kMaxMhc2ProfileBytes + 1, '\0');
    QVERIFY(!parseMhc2PeakNits(fixture.profile.bytes).has_value());

    fixture.profile.bytes = mhc2Profile(1000);
    fixture.profile.elapsedMs = kProfileReadBudgetMs + 1;
    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(result.has_value());
    QVERIFY(!result->calibratedPeakNits.has_value());
    QVERIFY(result->edidPeakNits.has_value());
}

void ClientDisplayCapabilitiesWinTest::ambiguousMappingIsFailClosed()
{
    Fixture fixture = validFixture();
    fixture.mapping.uniqueActivePath = false;

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(!result.has_value());
}

void ClientDisplayCapabilitiesWinTest::negativeCoordinateMismatchIsFailClosed()
{
    Fixture fixture = validFixture();
    fixture.mapping.desktopBounds.moveLeft(0);

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(!result.has_value());
}

void ClientDisplayCapabilitiesWinTest::hiddenWindowMismatchIsFailClosed()
{
    Fixture fixture = validFixture();
    fixture.selected.hiddenWindowMatches = false;

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(!result.has_value());
}

void ClientDisplayCapabilitiesWinTest::unsafeProfileNameKeepsIndependentFallback()
{
    Fixture fixture = validFixture();
    fixture.profile.profileName = QStringLiteral("..\\outside.icc");

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(result.has_value());
    QVERIFY(!result->calibratedPeakNits.has_value());
    QVERIFY(result->edidPeakNits.has_value());
}

void ClientDisplayCapabilitiesWinTest::nonPqDetachedOrCloneOutputIsOmitted()
{
    Fixture fixture = validFixture();
    fixture.output.colorSpace = DxgiColorSpace::Srgb;
    auto result = collectCapabilities(fixture.selected, fixture.provider());
    QVERIFY(result.has_value());
    QVERIFY(!result->edidPeakNits.has_value());

    fixture.output.colorSpace = DxgiColorSpace::RgbFullG2084NoneP2020;
    fixture.output.attachedToDesktop = false;
    result = collectCapabilities(fixture.selected, fixture.provider());
    QVERIFY(result.has_value());
    QVERIFY(!result->edidPeakNits.has_value());

    fixture.output.attachedToDesktop = true;
    fixture.output.unique = false;
    result = collectCapabilities(fixture.selected, fixture.provider());
    QVERIFY(result.has_value());
    QVERIFY(!result->edidPeakNits.has_value());
}

void ClientDisplayCapabilitiesWinTest::identityMutationInvalidatesBothSources()
{
    Fixture fixture = validFixture();
    fixture.identityStable = false;

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(!result.has_value());
}

void ClientDisplayCapabilitiesWinTest::bothSourcesAreNormalizedAndReturned()
{
    const Fixture fixture = validFixture();

    const auto result = collectCapabilities(fixture.selected, fixture.provider());

    QVERIFY(result.has_value());
    QVERIFY(result->calibratedPeakNits.has_value());
    QCOMPARE(*result->calibratedPeakNits, 1000);
    QVERIFY(result->edidPeakNits.has_value());
    QCOMPARE(*result->edidPeakNits, 1200);
}

void ClientDisplayCapabilitiesWinTest::missingExtendedDefaultKeepsIndependentDxgi()
{
    for (const auto status : {ProfileLookupStatus::NoExtendedDefault,
                              ProfileLookupStatus::Failed,
                              ProfileLookupStatus::Unknown}) {
        Fixture fixture = validFixture();
        // Even well-formed MHC2 bytes are unusable without an explicit WCS
        // extended-default result tied to the selected display.
        fixture.profile.lookupStatus = status;

        const auto result = collectCapabilities(fixture.selected, fixture.provider());

        QVERIFY(result.has_value());
        QVERIFY(!result->calibratedPeakNits.has_value());
        QCOMPARE(result->edidPeakNits, std::optional<int> {1200});
    }
}

void ClientDisplayCapabilitiesWinTest::scaledQtGeometryUsesHiddenDisplayIdentity()
{
    const std::vector<QRect> displayBounds = {
        QRect(0, 0, 3840, 2160),
    };

    const auto result = resolveDisplayIndex(
        QRect(0, 0, 1920, 1080),
        0,
        displayBounds);

    QVERIFY(result.has_value());
    QCOMPARE(*result, 0);
}

void ClientDisplayCapabilitiesWinTest::wrongHiddenDisplayIsRejected()
{
    const std::vector<QRect> bounds = {
        QRect(0, 0, 3840, 2160),
        QRect(-2560, -1440, 2560, 1440),
    };
    QVERIFY(!resolveDisplayIndex(QRect(-2560, -1440, 1280, 720), 0, bounds));
    QCOMPARE(resolveDisplayIndex(QRect(-2560, -1440, 1280, 720), 1, bounds),
             std::optional<int> {1});
    QCOMPARE(resolveDisplayIndex(QRect(-2560, -1440, 1280, 720), -1, bounds),
             std::optional<int> {1});
}

void ClientDisplayCapabilitiesWinTest::ambiguousDisplayOriginsAreRejected()
{
    const std::vector<QRect> bounds = {
        QRect(0, 0, 3840, 2160), QRect(0, 0, 1920, 1080),
    };
    QVERIFY(!resolveDisplayIndex(QRect(0, 0, 1920, 1080), 0, bounds));
}

void ClientDisplayCapabilitiesWinTest::truncatedMhc2HeaderIsRejected()
{
    auto profile = mhc2Profile(1000);
    profile[143] = 20;
    QVERIFY(!parseMhc2PeakNits(profile));
}

void ClientDisplayCapabilitiesWinTest::invalidIccHeaderIsRejected()
{
    auto profile = mhc2Profile(1000);
    profile[36] = '\0';
    QVERIFY(!parseMhc2PeakNits(profile));
    profile = mhc2Profile(1000);
    profile[3] = static_cast<char>(profile.size() - 1);
    QVERIFY(!parseMhc2PeakNits(profile));
}

QTEST_APPLESS_MAIN(ClientDisplayCapabilitiesWinTest)

#include "tst_clientdisplaycapabilities_win.moc"
