#include "settings/streamingpreferences.h"
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

// Automatic mouse mode defaults on for new and existing profiles, and never changes
// the regular mouse setting it falls back to.
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class AutoMousePreferencesTest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
private slots:
    void initTestCase()
    {
        QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("MoonlightAutoMouseSettingsTest");
        QCoreApplication::setApplicationName("IsolatedPreferences");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory.path());
    }
    void init() { QSettings().clear(); }

    void newProfileDefaultsOn()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        QVERIFY(prefs->automaticMouseMode);
        QVERIFY(!prefs->absoluteMouseMode);
    }

    void existingProfilesKeepTheirFallback()
    {
        auto* prefs = StreamingPreferences::get();
        for (bool remoteDesktop : {false, true}) {
            // A profile saved before the setting existed: an old defaultver and the
            // legacy "mouseacceleration" key for the remote-desktop option.
            QSettings().clear();
            QSettings().setValue("defaultver", 1);
            QSettings().setValue("mouseacceleration", remoteDesktop);
            prefs->reload();
            QVERIFY(prefs->automaticMouseMode);
            QCOMPARE(prefs->absoluteMouseMode, remoteDesktop);

            prefs->save();
            QCOMPARE(QSettings().value("automaticmousemode").toBool(), true);
            QCOMPARE(QSettings().value("mouseacceleration").toBool(), remoteDesktop);
            prefs->reload();
            QVERIFY(prefs->automaticMouseMode);
            QCOMPARE(prefs->absoluteMouseMode, remoteDesktop);
        }
    }

    void turningItOffPersists()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        prefs->automaticMouseMode = false;
        prefs->absoluteMouseMode = true;
        prefs->save();
        prefs->reload();
        QVERIFY(!prefs->automaticMouseMode);
        QVERIFY(prefs->absoluteMouseMode);
    }
};

QTEST_GUILESS_MAIN(AutoMousePreferencesTest)
#include "tst_automousepreferences.moc"
