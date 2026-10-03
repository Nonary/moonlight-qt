#include <QtTest>
#include <QTemporaryDir>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include "settings/gamestreamingsettings.h"
#include "backend/profilemanager.h"
#include "cli/commandlineparser.h"

// Only storage scope and platform probes are faked. The resolver, serializer,
// CLI parser and all settings/navigation QML are the production implementations.
static QString activeProfile = "default";
QString ProfileManager::activeProfileId() { return activeProfile; }
bool ProfileManager::hasActiveProfile() { return !activeProfile.isEmpty(); }
void ProfileManager::beginProfileSettings(QSettings& settings) { beginProfileSettings(settings, activeProfile); }
void ProfileManager::beginProfileSettings(QSettings& settings, QString profileId) { settings.beginGroup("profiles/" + profileId); }
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class GameSettingsFactory : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE GameStreamingSettings* create(int appId, QObject* owner) { return new GameStreamingSettings(activeProfile, "host-a", appId, owner); }
    Q_INVOKABLE bool remove(int appId) { return GameStreamingSettings::remove(activeProfile, "host-a", appId); }
};

class GameSettingsTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_SettingsDirectory;
    GameSettingsFactory m_Factory;
    std::unique_ptr<QQmlEngine> m_Engine;
    std::unique_ptr<QQuickWindow> m_Window;

    StreamingPreferences* base() { return StreamingPreferences::get(); }
    QVariantMap stored(int appId = 1, QString host = "host-a", QString profile = "default") {
        return GameStreamingSettings::load(profile, host, appId);
    }
    QVariant evaluate(const QString& code, QObject* scope = nullptr) {
        if (!scope) scope = m_Window.get();
        QQmlExpression expression(qmlContext(scope), scope, code);
        auto result = expression.evaluate();
        if (expression.hasError()) qFatal("%s", qPrintable(expression.error().toString()));
        return result;
    }
    bool busy() { return evaluate("stackView.busy").toBool(); }
    QObject* page() { return evaluate("stackView.currentItem").value<QObject*>(); }
    void openEditor() {
        m_Engine = std::make_unique<QQmlEngine>();
        m_Engine->rootContext()->setContextProperty("initialView", "qrc:/gui/ProfileSelectionView.qml");
        m_Engine->rootContext()->setContextProperty("runConfigChecks", false);
        m_Engine->rootContext()->setContextProperty("gameSettingsFactory", &m_Factory);
        QQmlComponent component(m_Engine.get(), QUrl("qrc:/gui/main.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        m_Window.reset(qobject_cast<QQuickWindow*>(component.create()));
        QVERIFY(m_Window);
        m_Window->requestActivate();
        QTRY_VERIFY(m_Window->isActive());
        QTRY_VERIFY(!busy());
        evaluate("stackView.currentItem.openComputer(0, 'Test PC', false)");
        QTRY_VERIFY(!busy());
        evaluate("stackView.currentItem.openGameSettings(1, 'Test Game')");
        QTRY_VERIFY(!busy());
        QCOMPARE(evaluate("stackView.depth").toInt(), 4);
    }

private slots:
    void initTestCase() {
        QGuiApplication::setFont(QFont("Segoe UI", 10));
        QVERIFY(m_SettingsDirectory.isValid());
        QCoreApplication::setOrganizationName("MoonlightTests");
        QCoreApplication::setApplicationName("GameSettings");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_SettingsDirectory.path());
        const QUrl backend("qrc:/navigation-tests/mocks/Backend.qml");
        for (const auto name : {"ProfileManager", "ComputerManager", "SdlGamepadKeyNavigation", "SystemProperties", "AutoUpdateChecker",
                               "PyroWaveCalibrator", "NetworkBuffers"})
            qmlRegisterSingletonType(backend, name, 1, 0, name);
        qmlRegisterSingletonType<StreamingPreferences>("StreamingPreferences", 1, 0, "StreamingPreferences",
            [](QQmlEngine*, QJSEngine*) -> QObject* {
                auto preferences = StreamingPreferences::get();
                QQmlEngine::setObjectOwnership(preferences, QQmlEngine::CppOwnership);
                return preferences;
            });
        qmlRegisterUncreatableType<GameStreamingSettings>("GameStreamingSettings", 1, 0, "GameStreamingSettings", "Test factory");
        qmlRegisterType(QUrl("qrc:/navigation-tests/mocks/ComputerModel.qml"), "ComputerModel", 1, 0, "ComputerModel");
        qmlRegisterType(QUrl("qrc:/navigation-tests/mocks/AppModel.qml"), "AppModel", 1, 0, "AppModel");
    }

    void init() {
        activeProfile = "default";
        QSettings settings;
        settings.clear();
        base()->reload();
        base()->setProperty("exportingDiagnostics", false);
        base()->setDiagnosticsStatus(QString());
    }
    void cleanup() {
        m_Window.reset();
        m_Engine.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void noOverridesPreserveExactProfile() {
        base()->bitrateKbps = 43210;
        base()->packetSize = 1200;
        base()->enableVrr = true;
        base()->vrrLatencyMode = StreamingPreferences::VLM_LOW_LATENCY;
        base()->smoothVrrFrameTiming = false;
        base()->traceVrrFrames = true;
        QVERIFY(base()->setProperty("exportingDiagnostics", true));
        base()->setDiagnosticsStatus("export in progress");
        auto resolved = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QCOMPARE(resolved->gameValues(), base()->gameValues());
        QCOMPARE(resolved->packetSize, 1200);
        QVERIFY(resolved->enableVrr);
        QCOMPARE(resolved->vrrLatencyMode, StreamingPreferences::VLM_LOW_LATENCY);
        QVERIFY(!resolved->smoothVrrFrameTiming);
        QVERIFY(resolved->traceVrrFrames);
        QVERIFY(!resolved->property("exportingDiagnostics").toBool());
        QVERIFY(resolved->property("diagnosticsStatus").toString().isEmpty());
        resolved->quitAppAfter = true;
        resolved->save();
        QVERIFY(!base()->quitAppAfter);
        QVERIFY(QSettings().allKeys().isEmpty());
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        QVERIFY(editor.save());
        QVERIFY(QSettings().allKeys().isEmpty());
    }

    void sparseOverridesAndIsolation() {
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->fps = 120;
        editor.preferences()->enableVsync = false;
        QVERIFY(editor.save());
        QCOMPARE(stored(), (QVariantMap{{"fps", 120}, {"vsync", false}}));
        QVERIFY(stored(2).isEmpty());
        QVERIFY(stored(1, "host-b").isEmpty());
        QVERIFY(stored(1, "host-a", "second").isEmpty());
        QCOMPARE(base()->fps, 60);
        QVERIFY(base()->enableVsync);
        base()->width = 2560;
        base()->height = 1440;
        auto effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QCOMPARE(effective->width, 2560);
        QCOMPARE(effective->height, 1440);
        QCOMPARE(effective->fps, 120);
        QCOMPARE(effective->bitrateKbps, base()->getDefaultBitrate(2560, 1440, 120, false));
        activeProfile = "second";
        base()->reload();
        base()->fps = 90;
        GameStreamingSettings second(activeProfile, "host-a", 1);
        QCOMPARE(second.preferences()->fps, 90);
        second.preferences()->fps = 144;
        QVERIFY(second.save());
        QCOMPARE(stored(1, "host-a", "second").value("fps"), QVariant(144));
        QCOMPARE(stored().value("fps"), QVariant(120));
    }

    void resolutionIsAtomicAndResetIsIdempotent() {
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->width = 1920;
        editor.preferences()->fps = 120;
        QVERIFY(editor.save());
        QCOMPARE(stored().value("height").toInt(), 720);
        editor.reset("width");
        QVERIFY(editor.save());
        QCOMPARE(stored(), (QVariantMap{{"fps", 120}}));
        editor.reset();
        QVERIFY(editor.save());
        QVERIFY(editor.save());
        QVERIFY(stored().isEmpty());
        QVERIFY(QSettings().allKeys().isEmpty());
        QCOMPARE(editor.preferences()->gameValues(), base()->gameValues());
    }

    void manualAndAutomaticBitrate() {
        base()->autoAdjustBitrate = false;
        base()->bitrateKbps = 25000;
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->fps = 120;
        QVERIFY(editor.save());
        auto effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QCOMPARE(effective->bitrateKbps, 25000);
        editor.preferences()->bitrateKbps = 50000;
        QVERIFY(editor.save());
        QCOMPARE(stored().value("autoadjustbitrate"), QVariant(false));
        QCOMPARE(stored().value("bitrate"), QVariant(50000));
        editor.preferences()->autoAdjustBitrate = true;
        QVERIFY(editor.save());
        QVERIFY(!stored().contains("bitrate"));
        QCOMPARE(stored().value("autoadjustbitrate"), QVariant(true));
        effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QCOMPARE(effective->bitrateKbps, base()->getDefaultBitrate(1280, 720, 120, false));
        editor.reset("bitrate");
        QVERIFY(editor.save());
        effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QCOMPARE(effective->bitrateKbps, 25000);
    }

    void unchangedOverrideSurvivesMatchingProfile() {
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->fps = 120;
        QVERIFY(editor.save());
        base()->fps = 120;
        GameStreamingSettings reopened(activeProfile, "host-a", 1);
        QVERIFY(reopened.save());
        QCOMPARE(stored().value("fps"), QVariant(120));
        reopened.preferences()->setProperty("fps", 60);
        reopened.preferences()->setProperty("fps", 120);
        QVERIFY(reopened.save());
        QVERIFY(stored().isEmpty());
        reopened.preferences()->fps = 60;
        QVERIFY(reopened.save());
        reopened.preferences()->fps = 120;
        QVERIFY(reopened.save());
        QVERIFY(stored().isEmpty());
    }

    void invalidStoredValuesFallBackToProfile() {
        base()->enableHdr = true;
        base()->traceVrrFrames = true;
        QSettings settings;
        ProfileManager::beginProfileSettings(settings, activeProfile);
        settings.beginGroup("gameStreamingSettings/host-a/1");
        const QVariantMap invalid{{"width", 1920}, {"height", -1}, {"fps", 0},
            {"bitrate", 20000.5}, {"vsync", "maybe"}, {"audiocfg", 99}, {"videocfg", 3},
            {"windowmode", 1.5}, {"renderer", -1}, {"hdr", false}, {"capturesyskeys", -1},
            {"language", 1}, {"mdns", false}, {"uidisplaymode", 2}, {"richpresence", false},
            {"tracevrrframes", false}, {"diagnosticsStatus", "stored status"}, {"exportingDiagnostics", true}};
        for (auto it = invalid.cbegin(); it != invalid.cend(); ++it) settings.setValue(it.key(), it.value());
        settings.sync();
        const auto effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        auto expected = base()->gameValues();
        expected["hdr"] = false;
        QCOMPARE(effective->gameValues(), expected);
        QCOMPARE(effective->language, base()->language);
        QCOMPARE(effective->uiDisplayMode, base()->uiDisplayMode);
        QCOMPARE(effective->enableMdns, base()->enableMdns);
        QCOMPARE(effective->richPresence, base()->richPresence);
        QVERIFY(effective->traceVrrFrames);
        QVERIFY(!effective->property("exportingDiagnostics").toBool());
        QVERIFY(effective->property("diagnosticsStatus").toString().isEmpty());
    }

    void invalidScopesCannotPersist() {
        QVERIFY(!GameStreamingSettings::remove("../default", "host-a", 1));
        QVERIFY(!GameStreamingSettings::remove("default", "../host-a", 1));
        QVERIFY(!GameStreamingSettings::remove("default\n", "host-a", 1));
        QVERIFY(!GameStreamingSettings::remove("default", "host-a\n", 1));
        GameStreamingSettings invalid("default", "host-a", -1);
        invalid.preferences()->fps = 120;
        QVERIFY(!invalid.save());
        QVERIFY(QSettings().allKeys().isEmpty());
    }

    void staleEditorAndHostDeletion() {
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->fps = 120;
        activeProfile = "second";
        QVERIFY(!editor.save());
        activeProfile = "default";
        QVERIFY(editor.save());
        GameStreamingSettings other(activeProfile, "host-b", 1);
        other.preferences()->fps = 90;
        QVERIFY(other.save());
        editor.invalidate();
        GameStreamingSettings::removeHost(activeProfile, "host-a");
        editor.preferences()->fps = 144;
        QVERIFY(!editor.save());
        QVERIFY(stored().isEmpty());
        QCOMPARE(stored(1, "host-b").value("fps"), QVariant(90));
    }

    void cliExplicitValuesWin() {
        base()->smoothVrrFrameTiming = false;
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->fps = 120;
        editor.preferences()->enableVsync = false;
        editor.preferences()->enableVrr = true;
        editor.preferences()->smoothVrrFrameTiming = true;
        QVERIFY(editor.save());
        auto effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        StreamCommandLineParser parser;
        parser.parse({"moonlight", "stream", "host-a", "Game", "--fps", "60", "--vsync", "--yuv444",
                      "--no-vrr", "--no-vrr-smooth-frame-timing"}, effective.get());
        QCOMPARE(effective->fps, 60);
        QVERIFY(effective->enableVsync);
        QVERIFY(!effective->enableVrr);
        QVERIFY(!effective->smoothVrrFrameTiming);
        QCOMPARE(effective->bitrateKbps, base()->getDefaultBitrate(1280, 720, 60, true));
        editor.preferences()->autoAdjustBitrate = false;
        editor.preferences()->bitrateKbps = 50000;
        QVERIFY(editor.save());
        auto manual = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        parser.parse({"moonlight", "stream", "host-a", "Game", "--fps", "90",
                      "--resolution", "1920x1080"}, manual.get());
        QCOMPARE(manual->fps, 90);
        QCOMPARE(manual->width, 1920);
        QCOMPARE(manual->height, 1080);
        QCOMPARE(manual->bitrateKbps, 50000);
        QVERIFY(!manual->autoAdjustBitrate);
        QCOMPARE(stored().value("bitrate"), QVariant(50000));
        parser.parse({"moonlight", "stream", "host-a", "Game", "--fps", "90", "--bitrate", "27000"}, effective.get());
        QCOMPARE(effective->bitrateKbps, 27000);
        effective->autoAdjustBitrate = false;
        parser.parse({"moonlight", "stream", "host-a", "Game", "--no-yuv444"}, effective.get());
        QCOMPARE(effective->bitrateKbps, 27000);
        QCOMPARE(stored().value("fps"), QVariant(120));
        QCOMPARE(stored().value("enablevrr"), QVariant(true));
        QCOMPARE(stored().value("smoothvrrframetiming"), QVariant(true));
    }

    void enumsAndFalseOverridesRoundTrip() {
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->audioConfig = StreamingPreferences::AC_71_SURROUND;
        editor.preferences()->captureSysKeysMode = StreamingPreferences::CSK_ALWAYS;
        editor.preferences()->multiController = false;
        QVERIFY(editor.save());
        QCOMPARE(stored(), (QVariantMap{{"audiocfg", 2}, {"capturesyskeys", 2}, {"multicontroller", false}}));
        const auto effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QCOMPARE(effective->audioConfig, StreamingPreferences::AC_71_SURROUND);
        QCOMPARE(effective->captureSysKeysMode, StreamingPreferences::CSK_ALWAYS);
        QVERIFY(!effective->multiController);
        QVERIFY(base()->multiController);
    }

    void vrrModesRoundTripAndRejectInvalid_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("valid");
        QTest::newRow("smooth") << 0 << true;
        QTest::newRow("balanced") << 1 << true;
        QTest::newRow("low-latency") << 2 << true;
        QTest::newRow("negative") << -1 << false;
        QTest::newRow("past-last-mode") << 3 << false;
    }

    void vrrModesRoundTripAndRejectInvalid() {
        QFETCH(int, mode);
        QFETCH(bool, valid);
        base()->vrrLatencyMode = mode == 1 ? 0 : 1;
        base()->traceVrrFrames = true;
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->enableVrr = true;
        editor.preferences()->smoothVrrFrameTiming = false;
        editor.preferences()->vrrLatencyMode = mode;
        editor.preferences()->traceVrrFrames = false;
        QVERIFY(editor.save());
        auto expected = QVariantMap{{"enablevrr", true}, {"smoothvrrframetiming", false}};
        if (valid) expected.insert("vrrlatencymode", mode);
        QCOMPARE(stored(), expected);
        const auto effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QVERIFY(effective->enableVrr);
        QVERIFY(!effective->smoothVrrFrameTiming);
        QCOMPARE(effective->vrrLatencyMode, valid ? mode : base()->vrrLatencyMode);
        QVERIFY(effective->traceVrrFrames);
    }

    void pyroWaveBitrateAndCliOverrides() {
        base()->videoCodecConfig = StreamingPreferences::VCC_FORCE_PYROWAVE;
        base()->bitrateKbps = base()->getEffectiveDefaultBitrate();
        GameStreamingSettings hdrOnly(activeProfile, "host-a", 2);
        hdrOnly.preferences()->enableHdr = true;
        QVERIFY(hdrOnly.save());
        auto effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 2);
        QCOMPARE(effective->bitrateKbps, base()->getDefaultPyroWaveBitrate(1280, 720, 60, false, true));

        base()->videoCodecConfig = StreamingPreferences::VCC_AUTO;
        base()->bitrateKbps = base()->getEffectiveDefaultBitrate();
        GameStreamingSettings editor(activeProfile, "host-a", 1);
        editor.preferences()->videoCodecConfig = StreamingPreferences::VCC_FORCE_PYROWAVE;
        editor.preferences()->fps = 120;
        editor.preferences()->autoAdjustBitrate = false;
        editor.preferences()->bitrateKbps = 3000000;
        QVERIFY(editor.save());
        QCOMPARE(stored().value("videocfg"), QVariant(5));
        QCOMPARE(stored().value("bitrate"), QVariant(3000000));
        effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QCOMPARE(effective->videoCodecConfig, StreamingPreferences::VCC_FORCE_PYROWAVE);
        QCOMPARE(effective->bitrateKbps, 3000000);
        QVERIFY(!effective->autoAdjustBitrate);

        editor.preferences()->autoAdjustBitrate = true;
        editor.preferences()->width = 1920;
        editor.preferences()->height = 1080;
        editor.preferences()->enableHdr = true;
        editor.preferences()->enableYUV444 = true;
        QVERIFY(editor.save());
        QVERIFY(!stored().contains("bitrate"));
        effective = GameStreamingSettings::resolve(*base(), activeProfile, "host-a", 1);
        QCOMPARE(effective->bitrateKbps, base()->getDefaultPyroWaveBitrate(1920, 1080, 120, true, true));

        StreamCommandLineParser parser;
        parser.parse({"moonlight", "stream", "host-a", "Game", "--no-hdr"}, effective.get());
        QCOMPARE(effective->bitrateKbps, base()->getDefaultPyroWaveBitrate(1920, 1080, 120, true, false));
        parser.parse({"moonlight", "stream", "host-a", "Game", "--no-yuv444"}, effective.get());
        QCOMPARE(effective->bitrateKbps, base()->getDefaultPyroWaveBitrate(1920, 1080, 120, false, false));
        parser.parse({"moonlight", "stream", "host-a", "Game", "--fps", "90"}, effective.get());
        QCOMPARE(effective->bitrateKbps, base()->getDefaultPyroWaveBitrate(1920, 1080, 90, false, false));
        parser.parse({"moonlight", "stream", "host-a", "Game", "--fps", "144", "--hdr", "--yuv444",
                      "--video-codec", "H.264"}, effective.get());
        QCOMPARE(effective->videoCodecConfig, StreamingPreferences::VCC_FORCE_H264);
        QCOMPARE(effective->bitrateKbps, base()->getDefaultBitrate(1920, 1080, 144, true));
        parser.parse({"moonlight", "stream", "host-a", "Game", "--fps", "90", "--hdr", "--yuv444",
                      "--bitrate", "3000000", "--video-codec", "PyroWave"}, effective.get());
        QCOMPARE(effective->videoCodecConfig, StreamingPreferences::VCC_FORCE_PYROWAVE);
        QCOMPARE(effective->bitrateKbps, 3000000);
        QCOMPARE(stored().value("fps"), QVariant(120));
        QVERIFY(!stored().contains("bitrate"));
    }

    void qmlOpenAndCloseDoesNotCustomize() {
        base()->enableVsync = false;
        base()->framePacing = true;
        base()->bitrateKbps = 43210;
        const auto before = base()->gameValues();
        openEditor();
        QCOMPARE(evaluate("editor.customSettings.length", page()).toInt(), 0);
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!busy());
        QCOMPARE(evaluate("stackView.depth").toInt(), 3);
        QVERIFY(stored().isEmpty());
        QCOMPARE(base()->gameValues(), before);
        QVERIFY(qobject_cast<QQuickItem*>(page())->hasActiveFocus());
        QVERIFY(!evaluate("SdlGamepadKeyNavigation.uiNavMode").toBool());
    }

    void qmlChangeSaveAndControllerReset() {
        openEditor();
        QPointer<QObject> editor = evaluate("editor", page()).value<QObject*>();
        evaluate("settingsLoader.item.preferences.fps = 120", page());
        evaluate("settingsLoader.item.preferences.enableVsync = false", page());
        QCOMPARE(evaluate("editor.customSettings.length", page()).toInt(), 2);
        // Reach the footer by the same Tab traversal generated by controller navigation.
        bool reached = false;
        for (int i = 0; i < 100; ++i) {
            QTest::keyClick(m_Window.get(), Qt::Key_Tab);
            if (m_Window->activeFocusItem() && m_Window->activeFocusItem()->objectName() == "resetGameSetting") {
                reached = true;
                break;
            }
        }
        QVERIFY(reached);
        QTest::keyClick(m_Window.get(), Qt::Key_Space);
        QTRY_COMPARE(evaluate("editor.customSettings.length", page()).toInt(), 1);
        QVERIFY(!evaluate("settingsLoader.item.preferences.enableVsync", page()).toBool());
        evaluate("settingsLoader.item.preferences.fps = 144", page());
        for (int i = 0; i < 12; ++i) QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!busy());
        QCOMPARE(evaluate("stackView.depth").toInt(), 3);
        QCOMPARE(stored(), (QVariantMap{{"fps", 144}, {"vsync", false}}));
        QCOMPARE(page()->property("currentIndex").toInt(), 0);
        QVERIFY(qobject_cast<QQuickItem*>(page())->hasActiveFocus());
        QTRY_VERIFY(editor.isNull());
    }

    void qmlRealControlsAndResetAll() {
        openEditor();
        auto settings = evaluate("settingsLoader.item", page()).value<QObject*>();
        settings->findChild<QQuickItem*>("vsyncCheck")->forceActiveFocus();
        QTest::keyClick(m_Window.get(), Qt::Key_Space);
        QVERIFY(!evaluate("preferences.enableVsync", settings).toBool());
        QVERIFY(base()->enableVsync);
        settings->findChild<QQuickItem*>("videoBitrateSlider")->forceActiveFocus();
        QTest::keyClick(m_Window.get(), Qt::Key_Right);
        QVERIFY(!evaluate("preferences.autoAdjustBitrate", settings).toBool());
        settings->findChild<QQuickItem*>("captureSysKeysCheck")->forceActiveFocus();
        QTest::keyClick(m_Window.get(), Qt::Key_Space);
        QCOMPARE(evaluate("preferences.captureSysKeysMode", settings).toInt(), int(StreamingPreferences::CSK_FULLSCREEN));
        QVERIFY(!settings->findChild<QQuickItem*>("languageComboBox")->isVisible());
        QVERIFY(!settings->findChild<QQuickItem*>("uiDisplayModeComboBox")->isVisible());
        QVERIFY(!settings->findChild<QQuickItem*>("enableMdns")->isVisible());
        QVERIFY(!evaluate("settingsButton.visible").toBool());
        auto button = page()->findChild<QQuickItem*>("resetAllGameSettings");
        QVERIFY(button);
        button->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(m_Window.get(), Qt::Key_Space);
        QTRY_VERIFY(evaluate("resetDialog.opened", page()).toBool());
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!evaluate("resetDialog.visible", page()).toBool());
        QVERIFY(evaluate("editor.customSettings.length", page()).toInt() > 0);
        button->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(m_Window.get(), Qt::Key_Space);
        QTRY_VERIFY(evaluate("resetDialog.opened", page()).toBool());
        evaluate("resetDialog.standardButton(Dialog.Yes).forceActiveFocus()", page());
        QTest::keyClick(m_Window.get(), Qt::Key_Space);
        QTRY_COMPARE(evaluate("editor.customSettings.length", page()).toInt(), 0);
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!busy());
        QVERIFY(stored().isEmpty());
    }

    void qmlContextMenuAndSelectionRestore() {
        openEditor();
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!busy());
        auto games = page();
        // Open the actual menu through controller X (Key_Menu), not by calling the editor directly.
        QTest::keyClick(m_Window.get(), Qt::Key_Menu);
        QTRY_VERIFY(evaluate("currentItem.appContextMenu.opened", games).toBool());
        evaluate("currentItem.appContextMenu.itemAt(2).forceActiveFocus()", games);
        QTest::keyClick(m_Window.get(), Qt::Key_Return);
        QTRY_VERIFY(!busy());
        QCOMPARE(evaluate("stackView.depth").toInt(), 4);
        evaluate("settingsLoader.item.preferences.fps = 120", page());
        // Simulate host refresh inserting another game ahead of the edited one.
        evaluate("appModel.insert(0, {appid: 2, name: 'Another Game', running: false, hidden: false, directLaunch: false, isAppCollectorGame: false, customStreamingSettings: false, boxart: 'qrc:/res/no_app_image.png'})", games);
        QTest::keyClick(m_Window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!busy());
        QCOMPARE(page()->property("currentIndex").toInt(), 1);
        QCOMPARE(evaluate("currentItem.appContextMenu.initiator.grid.currentIndex", games).toInt(), 1);
        evaluate("appModel.setProperty(1, 'customStreamingSettings', true)", games);
        QTest::keyClick(m_Window.get(), Qt::Key_Menu);
        QTRY_VERIFY(evaluate("currentItem.appContextMenu.opened", games).toBool());
        evaluate("currentItem.appContextMenu.itemAt(3).forceActiveFocus()", games);
        QTest::keyClick(m_Window.get(), Qt::Key_Return);
        QTRY_VERIFY(evaluate("removeSettingsDialog.opened", games).toBool());
        evaluate("removeSettingsDialog.standardButton(Dialog.Yes).forceActiveFocus()", games);
        QTest::keyClick(m_Window.get(), Qt::Key_Return);
        QTRY_VERIFY(!evaluate("removeSettingsDialog.visible", games).toBool());
        QVERIFY(stored().isEmpty());
        QTRY_VERIFY(qobject_cast<QQuickItem*>(games)->hasActiveFocus());
        QTest::keyClick(m_Window.get(), Qt::Key_Left);
        QCOMPARE(games->property("currentIndex").toInt(), 0);
    }

    void qmlWindowCloseSaves() {
        openEditor();
        auto settings = evaluate("settingsLoader.item", page()).value<QObject*>();
        settings->findChild<QQuickItem*>("fpsComboBox")->forceActiveFocus();
        QTest::keyClick(m_Window.get(), Qt::Key_Left);
        QCOMPARE(evaluate("preferences.fps", settings).toInt(), 30);
        m_Window.reset();
        QCOMPARE(stored().value("fps"), QVariant(30));
    }

    void qmlVrrChangesAndResetPreserveDisplayMode() {
        base()->fps = 117;
        base()->windowMode = StreamingPreferences::WM_WINDOWED;
        openEditor();
        auto settings = evaluate("settingsLoader.item", page()).value<QObject*>();
        auto draft = qobject_cast<StreamingPreferences*>(evaluate("preferences", settings).value<QObject*>());
        QVERIFY(draft);
        QVERIFY(draft->setProperty("fps", 73));
        auto vrr = settings->findChild<QQuickItem*>("vrrCheck");
        auto fps = settings->findChild<QQuickItem*>("fpsComboBox");
        QVERIFY(vrr);
        QVERIFY(fps);
        vrr->forceActiveFocus();
        QTest::keyClick(m_Window.get(), Qt::Key_Space);
        QVERIFY(draft->enableVrr);
        QCOMPARE(draft->fps, 73);
        QCOMPARE(draft->windowMode, StreamingPreferences::WM_WINDOWED);
        QTRY_VERIFY(fps->property("currentText").toString().contains("73"));
        QCOMPARE(base()->fps, 117);
        QCOMPARE(base()->windowMode, StreamingPreferences::WM_WINDOWED);
        QVERIFY(!base()->enableVrr);
        auto trace = settings->findChild<QQuickItem*>("traceVrrFramesCheck");
        QVERIFY(trace);
        QVERIFY(!trace->isVisible());
        auto buffers = m_Engine->singletonInstance<QObject*>(qmlTypeId("NetworkBuffers", 1, 0, "NetworkBuffers"));
        QVERIFY(buffers);
        QCOMPARE(buffers->property("refreshCount").toInt(), 0);

        auto latency = settings->findChild<QQuickItem*>("vrrLatencyModeComboBox");
        auto judder = settings->findChild<QQuickItem*>("reduceJudderCheck");
        QVERIFY(latency);
        QVERIFY(judder);
        latency->forceActiveFocus();
        QTest::keyClick(m_Window.get(), Qt::Key_Left);
        QCOMPARE(draft->vrrLatencyMode, StreamingPreferences::VLM_LOW_LATENCY);
        judder->forceActiveFocus();
        QTest::keyClick(m_Window.get(), Qt::Key_Space);
        QVERIFY(!draft->smoothVrrFrameTiming);
        evaluate("editor.reset('enablevrr')", page());
        QVERIFY(!draft->enableVrr);
        QCOMPARE(draft->fps, 73);
        QCOMPARE(draft->windowMode, StreamingPreferences::WM_WINDOWED);
        QTRY_VERIFY(fps->property("currentText").toString().contains("73"));
        evaluate("editor.reset()", page());
        QCOMPARE(draft->fps, 117);
        QCOMPARE(draft->windowMode, StreamingPreferences::WM_WINDOWED);
        QCOMPARE(evaluate("editor.customSettings.length", page()).toInt(), 0);
    }
};

QTEST_MAIN(GameSettingsTest)
#include "tst_game_settings.moc"
