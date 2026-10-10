#include "settings/streamingpreferences.h"
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

// These tests exercise settings persistence without inspecting the display.
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class VrrPreferencesTest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    static void saveTimingOptions(int mode, const VrrTimingOptions& options)
    {
        QSettings saved;
        saved.setValue("vrrlatencymode", mode);
        saved.setValue("vrrbufferpermille", options.bufferPerMille);
        saved.setValue("vrrtargethundredths", options.targetHundredths);
        saved.setValue("vrrhistoryseconds", options.historySeconds);
        saved.setValue("vrrtoleranceus", options.toleranceUs);
    }
private slots:
    void initTestCase()
    {
        QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("MoonlightVrrSettingsTest");
        QCoreApplication::setApplicationName("IsolatedPreferences");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory.path());
    }
    void init() { QSettings().clear(); }
    void retiredDxgiWaitablePreference()
    {
        auto* prefs = StreamingPreferences::get();
        for (bool oldValue : {false, true}) {
            QSettings().setValue("experimentaldxgiwaitable", oldValue);
            prefs->reload();
            const auto renderer = prefs->rendererSelection;
            QVERIFY(!QSettings().contains("experimentaldxgiwaitable"));
            prefs->save();
            prefs->reload();
            QVERIFY(!QSettings().contains("experimentaldxgiwaitable"));
            QCOMPARE(prefs->rendererSelection, renderer);
        }
    }
    void migration()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        QCOMPARE(prefs->vrrBufferPerMille(), 1000);
        QCOMPARE(prefs->vrrTargetHundredths(), 9950);
        QCOMPARE(prefs->vrrHistorySeconds(), 120);
        QCOMPARE(prefs->vrrToleranceUs(), 500);
        for (int mode : {0, 1, 2}) {
            QSettings().setValue("vrrlatencymode", mode);
            prefs->reload();
            const auto expected = VrrTimingOptions::preset(mode);
            QCOMPARE(prefs->vrrBufferPerMille(), expected.bufferPerMille);
            QCOMPARE(prefs->vrrTargetHundredths(), expected.targetHundredths);
            QCOMPARE(prefs->vrrHistorySeconds(), expected.historySeconds);
            QCOMPARE(prefs->vrrToleranceUs(), expected.toleranceUs);
        }
    }
    void presetTargets()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        for (int mode : {0, 1, 2}) {
            prefs->applyVrrPreset(mode);
            QCOMPARE(prefs->vrrTargetHundredths(), mode == 2 ? 9900 : mode == 1 ? 9950 : 9995);
            prefs->save();
            prefs->reload();
            QCOMPARE(prefs->vrrTargetHundredths(), mode == 2 ? 9900 : mode == 1 ? 9950 : 9995);
        }
    }
    void oldSavedPresetsMigrateOnce()
    {
        auto* prefs = StreamingPreferences::get();
        for (int revision : {0, 1}) for (int mode : {0, 1, 2}) {
            QSettings().clear();
            const VrrTimingOptions oldPreset = revision == 1 ?
                (mode == 2 ? VrrTimingOptions{500, 9500, 60, 500} : mode == 0 ?
                 VrrTimingOptions{4000, 9900, 300, 250} : VrrTimingOptions{1000, 9750, 120, 500}) :
                (mode == 2 ? VrrTimingOptions{500, 9900, 60, 500} : mode == 0 ?
                 VrrTimingOptions{4000, 9999, 300, 250} : VrrTimingOptions{1000, 9950, 120, 500});
            saveTimingOptions(mode, oldPreset);
            QSettings().setValue("vrrtimingpresetrevision", revision);
            prefs->reload();
            const auto expected = VrrTimingOptions::preset(mode);
            QCOMPARE(prefs->vrrBufferPerMille(), expected.bufferPerMille);
            QCOMPARE(prefs->vrrTargetHundredths(), expected.targetHundredths);
            QCOMPARE(prefs->vrrHistorySeconds(), expected.historySeconds);
            QCOMPARE(prefs->vrrToleranceUs(), expected.toleranceUs);
            QCOMPARE(QSettings().value("vrrtargethundredths").toInt(), expected.targetHundredths);
            QCOMPARE(QSettings().value("vrrtimingpresetrevision").toInt(), 2);

            // Explicitly choosing the former target after migration is custom.
            prefs->setVrrTargetHundredths(oldPreset.targetHundredths);
            prefs->save();
            prefs->reload();
            QCOMPARE(prefs->vrrTargetHundredths(), oldPreset.targetHundredths);
        }
    }
    void customizedLegacyPresetsStayUnchanged()
    {
        auto* prefs = StreamingPreferences::get();
        for (int revision : {0, 1}) for (int mode : {0, 1, 2}) {
            const VrrTimingOptions oldPreset = revision == 1 ?
                (mode == 2 ? VrrTimingOptions{500, 9500, 60, 500} : mode == 0 ?
                 VrrTimingOptions{4000, 9900, 300, 250} : VrrTimingOptions{1000, 9750, 120, 500}) :
                (mode == 2 ? VrrTimingOptions{500, 9900, 60, 500} : mode == 0 ?
                 VrrTimingOptions{4000, 9999, 300, 250} : VrrTimingOptions{1000, 9950, 120, 500});
            for (int field = 0; field < 4; ++field) {
                QSettings().clear();
                auto custom = oldPreset;
                if (field == 0) custom.bufferPerMille = 750;
                else if (field == 1) custom.targetHundredths = 9650;
                else if (field == 2) custom.historySeconds = 35;
                else custom.toleranceUs = 1000;
                saveTimingOptions(mode, custom);
                QSettings().setValue("vrrtimingpresetrevision", revision);
                prefs->reload();
                QCOMPARE(prefs->vrrBufferPerMille(), custom.bufferPerMille);
                QCOMPARE(prefs->vrrTargetHundredths(), custom.targetHundredths);
                QCOMPARE(prefs->vrrHistorySeconds(), custom.historySeconds);
                QCOMPARE(prefs->vrrToleranceUs(), custom.toleranceUs);
            }
        }
    }
    void savedTupleWithoutMatchingModeIsCustom()
    {
        auto* prefs = StreamingPreferences::get();
        const VrrTimingOptions oldBalanced{1000, 9950, 120, 500};
        saveTimingOptions(2, oldBalanced);
        prefs->reload();
        QCOMPARE(prefs->vrrTargetHundredths(), 9950);
        QSettings().clear();
        saveTimingOptions(1, oldBalanced);
        QSettings().remove("vrrlatencymode");
        prefs->reload();
        QCOMPARE(prefs->vrrTargetHundredths(), 9950);
    }
    void customRoundTripAndPresetReset()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        prefs->setVrrBufferPerMille(750);
        prefs->setVrrTargetHundredths(9725);
        prefs->setVrrHistorySeconds(30);
        prefs->setVrrToleranceUs(1500);
        prefs->save();
        prefs->applyVrrPreset(0);
        prefs->reload();
        QCOMPARE(prefs->vrrBufferPerMille(), 750);
        QCOMPARE(prefs->vrrTargetHundredths(), 9725);
        QCOMPARE(prefs->vrrHistorySeconds(), 30);
        QCOMPARE(prefs->vrrToleranceUs(), 1500);
        prefs->applyVrrPreset(2);
        prefs->save();
        prefs->reload();
        QCOMPARE(prefs->vrrBufferPerMille(), 500);
        QCOMPARE(prefs->vrrTargetHundredths(), 9900);
        QCOMPARE(prefs->vrrHistorySeconds(), 60);
        QCOMPARE(prefs->vrrToleranceUs(), 500);
    }
    void invalidSavedValues()
    {
        QSettings saved;
        saved.setValue("vrrbufferpermille", -9);
        saved.setValue("vrrtargethundredths", 100000);
        saved.setValue("vrrhistoryseconds", "bad");
        saved.setValue("vrrtoleranceus", 1600);
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        QCOMPARE(prefs->vrrBufferPerMille(), 250);
        QCOMPARE(prefs->vrrTargetHundredths(), 9999);
        QCOMPARE(prefs->vrrHistorySeconds(), 120);
        QCOMPARE(prefs->vrrToleranceUs(), 1500);
        prefs->setVrrToleranceUs(-1);
        prefs->setVrrTargetHundredths(12);
        prefs->setVrrHistorySeconds(99999);
        prefs->setVrrBufferPerMille(99999);
        QCOMPARE(prefs->vrrToleranceUs(), 250);
        QCOMPARE(prefs->vrrTargetHundredths(), 9000);
        QCOMPARE(prefs->vrrHistorySeconds(), 300);
        QCOMPARE(prefs->vrrBufferPerMille(), 4000);
    }
};
QTEST_GUILESS_MAIN(VrrPreferencesTest)
#include "tst_vrrpreferences.moc"
