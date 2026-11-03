#include "settings/streamingpreferences.h"

#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

// The real preferences need these queries, but this test must not inspect GPUs.
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class Spatial71SettingsTests : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    std::unique_ptr<QQmlEngine> engine;
    QByteArray audioPage;

    bool addMock(const char* name, const QByteArray& body)
    {
        const QString filename = directory.filePath(QString::fromLatin1(name) + ".qml");
        QFile file(filename);
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write("pragma Singleton\nimport QtQml 2.2\nQtObject {\n" + body + "\n}\n");
        file.close();
        return qmlRegisterSingletonType(QUrl::fromLocalFile(filename), name, 1, 0, name) >= 0;
    }

    std::unique_ptr<QQuickItem> createPage(QQuickWindow& window)
    {
        QQmlComponent component(engine.get());
        // The original URL retains resolution of the real AutoResizingComboBox.
        component.setData(audioPage, QUrl::fromLocalFile(SPATIAL_SETTINGS_SOURCE));
        if (!component.isReady()) {
            qWarning().noquote() << component.errorString();
            return {};
        }
        std::unique_ptr<QQuickItem> page(qobject_cast<QQuickItem*>(component.create()));
        if (page) page->setParentItem(window.contentItem());
        return page;
    }

    static QQuickItem* control(QQuickItem* page, const char* property)
    {
        return qobject_cast<QQuickItem*>(page->property(property).value<QObject*>());
    }

    static bool savePreview(QQuickWindow& window, const QString& name)
    {
        const QString destination = qEnvironmentVariable("SPATIAL_SETTINGS_SCREENSHOT_DIR");
        if (destination.isEmpty()) return true;
        if (!QDir().mkpath(destination)) return false;
        QTest::qWait(150); // Allow the real Material control to finish its transition.
        return window.grabWindow().save(QDir(destination).filePath(name));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("MoonlightSpatialSettingsTest");
        QCoreApplication::setApplicationName("IsolatedPreferences");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory.path());
        QVERIFY(QSettings().fileName().startsWith(directory.path()));

        qmlRegisterSingletonType<StreamingPreferences>("StreamingPreferences", 1, 0,
            "StreamingPreferences", [](QQmlEngine* qml, QJSEngine*) -> QObject* {
                auto* prefs = StreamingPreferences::get(qml);
                QQmlEngine::setObjectOwnership(prefs, QQmlEngine::CppOwnership);
                return prefs;
            });
        QVERIFY(addMock("SystemProperties", "property bool hasDesktopEnvironment: true"));
        QVERIFY(addMock("SdlGamepadKeyNavigation",
                        "function setUiNavMode(value) {}"));
        qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");
        QQuickStyle::setStyle("Material");
        engine = std::make_unique<QQmlEngine>();

        QFile source(SPATIAL_SETTINGS_SOURCE);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray qml = source.readAll();
        const auto audioId = qml.indexOf("id: audioSettingsGroupBox");
        const auto hostId = qml.indexOf("id: hostSettingsGroupBox", audioId);
        QVERIFY(audioId >= 0 && hostId > audioId);
        const auto audioStart = qml.lastIndexOf("GroupBox {", audioId);
        const auto audioEnd = qml.lastIndexOf("GroupBox {", hostId);
        QVERIFY(audioStart >= 0 && audioEnd > audioStart);
        const auto saveStart = qml.indexOf("Component.onDestruction: {");
        const auto saveEnd = qml.indexOf('}', saveStart);
        QVERIFY(saveStart >= 0 && saveEnd > saveStart);

        // Execute the production audio group and page-close handler verbatim.
        // Unrelated discovery/calibration/video controls are not instantiated.
        audioPage = "import QtQuick 2.9\nimport QtQuick.Controls 2.2\n"
                    "import StreamingPreferences 1.0\nimport SystemProperties 1.0\n"
                    "Item { width: 760; height: 340\n"
                    "property alias selector: audioComboBox\n"
                    "property alias spatialControl: spatialAudioCheck\n";
        audioPage += qml.mid(saveStart, saveEnd - saveStart + 1);
        audioPage += "\nColumn { width: parent.width; padding: 10\n";
        audioPage += qml.mid(audioStart, audioEnd - audioStart);
        audioPage += "\n}\n}\n";
    }

    void init()
    {
        QSettings().clear();
        StreamingPreferences::get()->reload();
    }

    void defaultIsOff()
    {
        QVERIFY(!QSettings().contains("spatialaudio"));
        QVERIFY(!StreamingPreferences::get()->spatialAudio);
    }

    void persistence_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<int>("layout");
        for (int layout = 0; layout < 3; ++layout) {
            QTest::newRow(qPrintable(QString("off-layout-%1").arg(layout))) << false << layout;
            QTest::newRow(qPrintable(QString("on-layout-%1").arg(layout))) << true << layout;
        }
    }

    void persistence()
    {
        QFETCH(bool, enabled);
        QFETCH(int, layout);
        auto* prefs = StreamingPreferences::get();
        prefs->audioConfig = static_cast<StreamingPreferences::AudioConfig>(layout);
        prefs->playAudioOnHost = true;
        prefs->muteOnFocusLoss = true;
        prefs->spatialAudio = enabled;
        prefs->save();
        QCOMPARE(QSettings().value("spatialaudio").toBool(), enabled);
        prefs->spatialAudio = !enabled;
        prefs->audioConfig = StreamingPreferences::AC_STEREO;
        prefs->playAudioOnHost = false;
        prefs->muteOnFocusLoss = false;
        prefs->reload();
        QCOMPARE(prefs->spatialAudio, enabled);
        QCOMPARE(static_cast<int>(prefs->audioConfig), layout);
        QVERIFY(prefs->playAudioOnHost);
        QVERIFY(prefs->muteOnFocusLoss);
    }

    void actualAudioControlsAndReopen()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->audioConfig = StreamingPreferences::AC_71_SURROUND;
        prefs->playAudioOnHost = true;
        prefs->muteOnFocusLoss = true;
        QQuickWindow window;
        window.resize(760, 340);
        window.setColor(QColor("#202020"));
        auto page = createPage(window);
        QVERIFY(page);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* checkbox = control(page.get(), "spatialControl");
        auto* selector = control(page.get(), "selector");
        QVERIFY(checkbox && selector);
#ifdef Q_OS_WIN
        QVERIFY(checkbox->isVisible());
#else
        QVERIFY(!checkbox->isVisible());
        QSKIP("Interactive checkbox is Windows-only; visibility verified.");
#endif
        QVERIFY(checkbox->isEnabled());
        QVERIFY(!checkbox->property("checked").toBool());
        QCOMPARE(selector->property("currentIndex").toInt(), 2);
        QVERIFY(savePreview(window, "spatial-audio-off.png"));
        QSignalSpy toggles(checkbox, SIGNAL(toggled()));
        QVERIFY(toggles.isValid());
        checkbox->forceActiveFocus();
        QTRY_VERIFY(checkbox->hasActiveFocus());
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY(prefs->spatialAudio);
        QCOMPARE(toggles.size(), 1);
        QVERIFY(savePreview(window, "spatial-audio-on.png"));

        // Use real keyboard events through the production combo's handlers.
        selector->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Left);
        QTRY_COMPARE(prefs->audioConfig, StreamingPreferences::AC_51_SURROUND);
        QVERIFY(checkbox->isEnabled());
        QVERIFY(checkbox->property("checked").toBool());
        // Six-channel streaming supports the same user opt-in, and toggling
        // here must persist without changing the selected layout.
        checkbox->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY(!prefs->spatialAudio);
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY(prefs->spatialAudio);
        QCOMPARE(prefs->audioConfig, StreamingPreferences::AC_51_SURROUND);
        QCOMPARE(toggles.size(), 3);
        QVERIFY(savePreview(window, "spatial-audio-51-on.png"));
        selector->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Left);
        QTRY_COMPARE(prefs->audioConfig, StreamingPreferences::AC_STEREO);
        QVERIFY(!checkbox->isEnabled());
        QVERIFY(prefs->spatialAudio);
        QCOMPARE(toggles.size(), 3); // Layout changes must not toggle the opt-in.
        QVERIFY(prefs->playAudioOnHost);
        QVERIFY(prefs->muteOnFocusLoss);

        page.reset(); // Executes SettingsView's actual close/save handler.
        QCOMPARE(QSettings().value("spatialaudio").toBool(), true);
        prefs->spatialAudio = false;
        prefs->reload();
        QVERIFY(prefs->spatialAudio);
        page = createPage(window);
        QVERIFY(page);
        checkbox = control(page.get(), "spatialControl");
        selector = control(page.get(), "selector");
        QVERIFY(checkbox && selector);
        QVERIFY(checkbox->property("checked").toBool());
        QVERIFY(!checkbox->isEnabled());
        selector->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Right);
        QTRY_COMPARE(prefs->audioConfig, StreamingPreferences::AC_51_SURROUND);
        QVERIFY(checkbox->isEnabled());
        QVERIFY(checkbox->property("checked").toBool());
        QTest::keyClick(&window, Qt::Key_Right);
        QTRY_COMPARE(prefs->audioConfig, StreamingPreferences::AC_71_SURROUND);
        QVERIFY(checkbox->isEnabled());
        checkbox->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY(!prefs->spatialAudio);
        page.reset();
        QCOMPARE(QSettings().value("spatialaudio").toBool(), false);
        prefs->spatialAudio = true;
        prefs->reload();
        QVERIFY(!prefs->spatialAudio);
        QCOMPARE(prefs->audioConfig, StreamingPreferences::AC_71_SURROUND);
        QVERIFY(prefs->playAudioOnHost);
        QVERIFY(prefs->muteOnFocusLoss);
    }
};

QTEST_MAIN(Spatial71SettingsTests)
#include "tst_spatial71settings.moc"
