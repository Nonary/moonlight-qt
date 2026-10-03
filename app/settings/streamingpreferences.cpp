#include "streamingpreferences.h"
#include "backend/profilemanager.h"
#include "streaming/video/pyrowave/pyrowavebitrate.h"
#include "utils.h"
#include "streaming/vrrratepolicy.h"
#include "diagnostics/diagnosticcapture.h"

#include <QSettings>
#include <QTranslator>
#include <QCoreApplication>
#include <QLocale>
#include <QReadWriteLock>
#include <QMetaProperty>
#include <QVariantMap>
#include <QtMath>
#include <QDesktopServices>
#include <QDir>
#include <QThread>
#include <QUrl>

#include <QtDebug>

#include <vector>

#define SER_STREAMSETTINGS "streamsettings"
#define SER_WIDTH "width"
#define SER_HEIGHT "height"
#define SER_FPS "fps"
#define SER_BITRATE "bitrate"
#define SER_UNLOCK_BITRATE "unlockbitrate"
#define SER_AUTOADJUSTBITRATE "autoadjustbitrate"
#define SER_FULLSCREEN "fullscreen"
#define SER_VSYNC "vsync"
#define SER_ENABLEVRR "enablevrr"
#define SER_VRRLATENCYFIX "vrrlatencyfix"
#define SER_VRRLATENCYMODE "vrrlatencymode"
#define SER_SMOOTHVRRFRAMETIMING "smoothvrrframetiming"
#define SER_TRACEVRRFRAMES "tracevrrframes"
#define SER_GAMEOPTS "gameopts"
#define SER_HOSTAUDIO "hostaudio"
#define SER_MULTICONT "multicontroller"
#define SER_AUDIOCFG "audiocfg"
#define SER_VIDEOCFG "videocfg"
#define SER_HDR "hdr"
#define SER_YUV444 "yuv444"
#define SER_VIDEODEC "videodec"
#define SER_WINDOWMODE "windowmode"
#define SER_MDNS "mdns"
#define SER_QUITAPPAFTER "quitAppAfter"
#define SER_ABSMOUSEMODE "mouseacceleration"
#define SER_ABSTOUCHMODE "abstouchmode"
#define SER_STARTWINDOWED "startwindowed"
#define SER_FRAMEPACING "framepacing"
#define SER_CONNWARNINGS "connwarnings"
#define SER_CONFWARNINGS "confwarnings"
#define SER_UIDISPLAYMODE "uidisplaymode"
#define SER_RICHPRESENCE "richpresence"
#define SER_GAMEPADMOUSE "gamepadmouse"
#define SER_DEFAULTVER "defaultver"
#define SER_PACKETSIZE "packetsize"
#define SER_DETECTNETBLOCKING "detectnetblocking"
#define SER_SHOWPERFOVERLAY "showperfoverlay"
#define SER_SWAPMOUSEBUTTONS "swapmousebuttons"
#define SER_MUTEONFOCUSLOSS "muteonfocusloss"
#define SER_BACKGROUNDGAMEPAD "backgroundgamepad"
#define SER_REVERSESCROLL "reversescroll"
#define SER_SWAPFACEBUTTONS "swapfacebuttons"
#define SER_CAPTURESYSKEYS "capturesyskeys"
#define SER_KEEPAWAKE "keepawake"
#define SER_LANGUAGE "language"
#define SER_RENDERER "renderer"

#define CURRENT_DEFAULT_VER 2

static StreamingPreferences* s_GlobalPrefs;

Q_GLOBAL_STATIC(QReadWriteLock, s_GlobalPrefsLock)

StreamingPreferences::StreamingPreferences(QQmlEngine *qmlEngine, bool transient)
    : m_QmlEngine(qmlEngine), m_Transient(transient)
{
    if (!m_Transient) reload();
}

namespace {
struct GameField {
    const char* key;
    const char* property;
    const char* label;
};

// Keep storage keys shared with profile serialization. Profile-only fields are
// deliberately absent; this is also the allowlist for untrusted persisted data.
const GameField gameFields[] = {
    {SER_WIDTH, "width", QT_TRANSLATE_NOOP("GameStreamingSettings", "Resolution")},
    {SER_HEIGHT, "height", nullptr},
    {SER_FPS, "fps", QT_TRANSLATE_NOOP("GameStreamingSettings", "Frame rate")},
    {SER_BITRATE, "bitrateKbps", QT_TRANSLATE_NOOP("GameStreamingSettings", "Video bitrate")},
    {SER_AUTOADJUSTBITRATE, "autoAdjustBitrate", nullptr},
    {SER_UNLOCK_BITRATE, "unlockBitrate", QT_TRANSLATE_NOOP("GameStreamingSettings", "Bitrate limit")},
    {SER_VSYNC, "enableVsync", QT_TRANSLATE_NOOP("GameStreamingSettings", "V-Sync")},
    {SER_ENABLEVRR, "enableVrr", QT_TRANSLATE_NOOP("GameStreamingSettings", "Variable refresh rate")},
    {SER_VRRLATENCYMODE, "vrrLatencyMode", QT_TRANSLATE_NOOP("GameStreamingSettings", "VRR latency mode")},
    {SER_SMOOTHVRRFRAMETIMING, "smoothVrrFrameTiming", QT_TRANSLATE_NOOP("GameStreamingSettings", "VRR frame timing")},
    {SER_FRAMEPACING, "framePacing", QT_TRANSLATE_NOOP("GameStreamingSettings", "Frame pacing")},
    {SER_WINDOWMODE, "windowMode", QT_TRANSLATE_NOOP("GameStreamingSettings", "Display mode")},
    {SER_AUDIOCFG, "audioConfig", QT_TRANSLATE_NOOP("GameStreamingSettings", "Audio configuration")},
    {SER_HOSTAUDIO, "playAudioOnHost", QT_TRANSLATE_NOOP("GameStreamingSettings", "Host audio")},
    {SER_MUTEONFOCUSLOSS, "muteOnFocusLoss", QT_TRANSLATE_NOOP("GameStreamingSettings", "Mute on focus loss")},
    {SER_GAMEOPTS, "gameOptimizations", QT_TRANSLATE_NOOP("GameStreamingSettings", "Game optimizations")},
    {SER_QUITAPPAFTER, "quitAppAfter", QT_TRANSLATE_NOOP("GameStreamingSettings", "Quit app after stream")},
    {SER_CONNWARNINGS, "connectionWarnings", QT_TRANSLATE_NOOP("GameStreamingSettings", "Connection warnings")},
    {SER_CONFWARNINGS, "configurationWarnings", QT_TRANSLATE_NOOP("GameStreamingSettings", "Configuration warnings")},
    {SER_KEEPAWAKE, "keepAwake", QT_TRANSLATE_NOOP("GameStreamingSettings", "Keep display awake")},
    {SER_ABSMOUSEMODE, "absoluteMouseMode", QT_TRANSLATE_NOOP("GameStreamingSettings", "Remote desktop mouse")},
    {SER_CAPTURESYSKEYS, "captureSysKeysMode", QT_TRANSLATE_NOOP("GameStreamingSettings", "System keyboard shortcuts")},
    {SER_ABSTOUCHMODE, "absoluteTouchMode", QT_TRANSLATE_NOOP("GameStreamingSettings", "Touchscreen mode")},
    {SER_SWAPMOUSEBUTTONS, "swapMouseButtons", QT_TRANSLATE_NOOP("GameStreamingSettings", "Mouse buttons")},
    {SER_REVERSESCROLL, "reverseScrollDirection", QT_TRANSLATE_NOOP("GameStreamingSettings", "Scroll direction")},
    {SER_SWAPFACEBUTTONS, "swapFaceButtons", QT_TRANSLATE_NOOP("GameStreamingSettings", "Gamepad button layout")},
    {SER_MULTICONT, "multiController", QT_TRANSLATE_NOOP("GameStreamingSettings", "Multiple controllers")},
    {SER_GAMEPADMOUSE, "gamepadMouse", QT_TRANSLATE_NOOP("GameStreamingSettings", "Gamepad mouse control")},
    {SER_BACKGROUNDGAMEPAD, "backgroundGamepad", QT_TRANSLATE_NOOP("GameStreamingSettings", "Background gamepad input")},
    {SER_VIDEODEC, "videoDecoderSelection", QT_TRANSLATE_NOOP("GameStreamingSettings", "Video decoder")},
    {SER_VIDEOCFG, "videoCodecConfig", QT_TRANSLATE_NOOP("GameStreamingSettings", "Video codec")},
    {SER_RENDERER, "rendererSelection", QT_TRANSLATE_NOOP("GameStreamingSettings", "Renderer")},
    {SER_HDR, "enableHdr", QT_TRANSLATE_NOOP("GameStreamingSettings", "HDR")},
    {SER_YUV444, "enableYUV444", QT_TRANSLATE_NOOP("GameStreamingSettings", "YUV 4:4:4")},
    {SER_SHOWPERFOVERLAY, "showPerformanceOverlay", QT_TRANSLATE_NOOP("GameStreamingSettings", "Performance statistics")},
};
}

std::unique_ptr<StreamingPreferences> StreamingPreferences::createTransientCopy() const
{
    std::unique_ptr<StreamingPreferences> copy(new StreamingPreferences(nullptr, true));
    for (int i = staticMetaObject.propertyOffset(); i < staticMetaObject.propertyCount(); ++i) {
        const auto property = staticMetaObject.property(i);
        const auto name = QByteArray(property.name());
        if (property.isWritable() && name != "exportingDiagnostics" && name != "diagnosticsStatus") {
            property.write(copy.get(), property.read(this));
        }
    }
    copy->recommendedFullScreenMode = recommendedFullScreenMode;
    copy->packetSize = packetSize;
    return copy;
}

QVariantMap StreamingPreferences::gameValues() const
{
    QVariantMap values;
    for (const auto& field : gameFields) {
        const auto value = property(field.property);
        values.insert(field.key, value.userType() == QMetaType::Bool ? value : QVariant(value.toInt()));
    }
    return values;
}

QVariantList StreamingPreferences::gameSettingGroups()
{
    QVariantList groups;
    for (const auto& field : gameFields) {
        if (field.label) groups.append(QVariantMap{{"key", field.key},
            {"text", QCoreApplication::translate("GameStreamingSettings", field.label)}});
    }
    return groups;
}

QVariantMap StreamingPreferences::validatedGameValues(const QVariantMap& values)
{
    QVariantMap valid;
    for (const auto& field : gameFields) {
        if (!values.contains(field.key)) continue;
        const auto property = staticMetaObject.property(staticMetaObject.indexOfProperty(field.property));
        const auto value = values.value(field.key);
        bool ok = false;
        int number = value.toInt(&ok);
        if (property.userType() == QMetaType::Bool) {
            const auto string = value.toString().toLower();
            ok = string == "true" || string == "false" || string == "0" || string == "1";
            if (ok) valid.insert(field.key, string == "true" || string == "1");
        }
        else {
            if (property.isEnumType()) {
                ok = ok && number >= 0 && property.enumerator().valueToKey(number) != nullptr;
                if (QString::fromLatin1(field.key) == SER_VIDEOCFG && number == VCC_FORCE_HEVC_HDR_DEPRECATED) ok = false;
            }
            else if (QString::fromLatin1(field.key) == SER_VRRLATENCYMODE) ok = ok && number >= VLM_SMOOTH && number <= VLM_LOW_LATENCY;
            else if (QString::fromLatin1(field.key) == SER_BITRATE) ok = ok && number >= 500 && number <= 3000000;
            else ok = ok && number >= (QString::fromLatin1(field.key) == SER_FPS ? 10 : 1) && number <= 9999;
            // Reject fractional numbers rather than silently truncating them.
            ok = ok && value.toDouble() == number;
            if (ok) valid.insert(field.key, number);
        }
        if (!ok) qWarning() << "Ignoring invalid per-game streaming setting:" << field.key;
    }
    if (valid.contains(SER_WIDTH) != valid.contains(SER_HEIGHT)) {
        valid.remove(SER_WIDTH);
        valid.remove(SER_HEIGHT);
    }
    if (valid.value(SER_AUTOADJUSTBITRATE).toBool()) valid.remove(SER_BITRATE);
    else if (valid.contains(SER_AUTOADJUSTBITRATE) && !valid.contains(SER_BITRATE)) valid.remove(SER_AUTOADJUSTBITRATE);
    if (valid.contains(SER_BITRATE)) valid.insert(SER_AUTOADJUSTBITRATE, false);
    return valid;
}

void StreamingPreferences::applyGameValues(const QVariantMap& values)
{
    for (const auto& field : gameFields) {
        if (values.contains(field.key)) setProperty(field.property, values.value(field.key));
    }
}

StreamingPreferences* StreamingPreferences::get(QQmlEngine *qmlEngine)
{
    {
        QReadLocker readGuard(s_GlobalPrefsLock);

        // If we have a preference object and it's associated with a QML engine or
        // if the caller didn't specify a QML engine, return the existing object.
        if (s_GlobalPrefs && (s_GlobalPrefs->m_QmlEngine || !qmlEngine)) {
            // The lifetime logic here relies on the QML engine also being a singleton.
            Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            return s_GlobalPrefs;
        }
    }

    {
        QWriteLocker writeGuard(s_GlobalPrefsLock);

        // If we already have an preference object but the QML engine is now available,
        // associate the QML engine with the preferences.
        if (s_GlobalPrefs) {
            if (!s_GlobalPrefs->m_QmlEngine) {
                s_GlobalPrefs->m_QmlEngine = qmlEngine;
            }
            else {
                // We could reach this codepath if another thread raced with us
                // and created the object while we were outside the pref lock.
                Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            }
        }
        else {
            s_GlobalPrefs = new StreamingPreferences(qmlEngine);
        }

        return s_GlobalPrefs;
    }
}

void StreamingPreferences::reload()
{
    if (m_Transient) return;
    // The profile picker can create this singleton before a profile is chosen.
    // An empty snapshot initializes the same defaults without touching storage.
    QVariantMap settings;
    if (ProfileManager::hasActiveProfile()) {
        QSettings storedSettings;
        ProfileManager::beginProfileSettings(storedSettings);

        // Native VRR always permits tearing. Remove retired timing selectors
        // only from the selected profile.
        storedSettings.remove(QStringLiteral("allowvrrtearing"));
        storedSettings.remove(QStringLiteral("vrrdiagnosticmode"));

        for (const QString& key : storedSettings.childKeys()) {
            settings.insert(key, storedSettings.value(key));
        }
    }

    int defaultVer = settings.value(SER_DEFAULTVER, 0).toInt();

#ifdef Q_OS_DARWIN
    recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
#else
    // Wayland doesn't support modesetting, so use fullscreen desktop mode
    // unless we have a slow GPU (which can take advantage of wp_viewporter
    // to reduce GPU load with lower resolution video streams).
    if (WMUtils::isRunningWayland() && !WMUtils::isGpuSlow()) {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
    }
    else {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN;
    }
#endif

    width = settings.value(SER_WIDTH, 1280).toInt();
    height = settings.value(SER_HEIGHT, 720).toInt();
    fps = settings.value(SER_FPS, 60).toInt();
    enableYUV444 = settings.value(SER_YUV444, false).toBool();
    bitrateKbps = settings.value(SER_BITRATE, getDefaultBitrate(width, height, fps, enableYUV444)).toInt();
    unlockBitrate = settings.value(SER_UNLOCK_BITRATE, false).toBool();
    autoAdjustBitrate = settings.value(SER_AUTOADJUSTBITRATE, true).toBool();
    enableVsync = settings.value(SER_VSYNC, true).toBool();
    enableVrr = settings.value(SER_ENABLEVRR, false).toBool();
    vrrLatencyMode = VLM_BALANCED;
    if (settings.contains(SER_VRRLATENCYMODE)) {
        bool validMode = false;
        const int savedMode = settings.value(SER_VRRLATENCYMODE).toInt(&validMode);
        if (validMode && savedMode >= VLM_SMOOTH && savedMode <= VLM_LOW_LATENCY) {
            vrrLatencyMode = savedMode;
        }
    }
    else if (settings.contains(SER_VRRLATENCYFIX)) {
        // Preserve the old checkbox choice while new users start on Balanced Target.
        vrrLatencyMode = settings.value(SER_VRRLATENCYFIX).toBool() ? VLM_BALANCED_TARGET : VLM_SMOOTH;
    }
    smoothVrrFrameTiming = settings.value(SER_SMOOTHVRRFRAMETIMING, true).toBool();
    traceVrrFrames = settings.value(SER_TRACEVRRFRAMES, false).toBool();
    gameOptimizations = settings.value(SER_GAMEOPTS, true).toBool();
    playAudioOnHost = settings.value(SER_HOSTAUDIO, false).toBool();
    multiController = settings.value(SER_MULTICONT, true).toBool();
    enableMdns = settings.value(SER_MDNS, true).toBool();
    quitAppAfter = settings.value(SER_QUITAPPAFTER, false).toBool();
    absoluteMouseMode = settings.value(SER_ABSMOUSEMODE, false).toBool();
    absoluteTouchMode = settings.value(SER_ABSTOUCHMODE, true).toBool();
    framePacing = settings.value(SER_FRAMEPACING, false).toBool();
    connectionWarnings = settings.value(SER_CONNWARNINGS, true).toBool();
    configurationWarnings = settings.value(SER_CONFWARNINGS, true).toBool();
    richPresence = settings.value(SER_RICHPRESENCE, true).toBool();
    gamepadMouse = settings.value(SER_GAMEPADMOUSE, true).toBool();
    detectNetworkBlocking = settings.value(SER_DETECTNETBLOCKING, true).toBool();
    showPerformanceOverlay = settings.value(SER_SHOWPERFOVERLAY, false).toBool();
    packetSize = settings.value(SER_PACKETSIZE, 0).toInt();
    swapMouseButtons = settings.value(SER_SWAPMOUSEBUTTONS, false).toBool();
    muteOnFocusLoss = settings.value(SER_MUTEONFOCUSLOSS, false).toBool();
    backgroundGamepad = settings.value(SER_BACKGROUNDGAMEPAD, false).toBool();
    reverseScrollDirection = settings.value(SER_REVERSESCROLL, false).toBool();
    swapFaceButtons = settings.value(SER_SWAPFACEBUTTONS, false).toBool();
    keepAwake = settings.value(SER_KEEPAWAKE, true).toBool();
    enableHdr = settings.value(SER_HDR, false).toBool();
    captureSysKeysMode = static_cast<CaptureSysKeysMode>(settings.value(SER_CAPTURESYSKEYS,
                                                         static_cast<int>(CaptureSysKeysMode::CSK_OFF)).toInt());
    audioConfig = static_cast<AudioConfig>(settings.value(SER_AUDIOCFG,
                                                  static_cast<int>(AudioConfig::AC_STEREO)).toInt());
    videoCodecConfig = static_cast<VideoCodecConfig>(settings.value(SER_VIDEOCFG,
                                                  static_cast<int>(VideoCodecConfig::VCC_AUTO)).toInt());
    videoDecoderSelection = static_cast<VideoDecoderSelection>(settings.value(SER_VIDEODEC,
                                                  static_cast<int>(VideoDecoderSelection::VDS_AUTO)).toInt());
    rendererSelection = static_cast<RendererSelection>(settings.value(SER_RENDERER,
                                                  static_cast<int>(RendererSelection::RS_AUTO)).toInt());
    windowMode = static_cast<WindowMode>(settings.value(SER_WINDOWMODE,
                                                        // Try to load from the old preference value too
                                                        static_cast<int>(settings.value(SER_FULLSCREEN, true).toBool() ?
                                                                             recommendedFullScreenMode : WindowMode::WM_WINDOWED)).toInt());
    uiDisplayMode = static_cast<UIDisplayMode>(settings.value(SER_UIDISPLAYMODE,
                                               static_cast<int>(settings.value(SER_STARTWINDOWED, true).toBool() ? UIDisplayMode::UI_WINDOWED
                                                                                                                 : UIDisplayMode::UI_MAXIMIZED)).toInt());
    language = static_cast<Language>(settings.value(SER_LANGUAGE,
                                                    static_cast<int>(Language::LANG_AUTO)).toInt());


    // Perform default settings updates as required based on last default version
    if (defaultVer < 1) {
#ifdef Q_OS_DARWIN
        // Update window mode setting on macOS from full-screen (old default) to borderless windowed (new default)
        if (windowMode == WindowMode::WM_FULLSCREEN) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
#endif
    }
    if (defaultVer < 2) {
        if (windowMode == WindowMode::WM_FULLSCREEN && WMUtils::isRunningWayland()) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
    }

    // Fixup VCC value to the new settings format with codec and HDR separate
    if (videoCodecConfig == VCC_FORCE_HEVC_HDR_DEPRECATED) {
        videoCodecConfig = VCC_AUTO;
        enableHdr = true;
    }
}

bool StreamingPreferences::retranslate()
{
    if (m_Transient) return false;
    static QTranslator* translator = nullptr;

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
    if (m_QmlEngine != nullptr) {
        // Dynamic retranslation is not supported until Qt 5.10
        return false;
    }
#endif

    QTranslator* newTranslator = new QTranslator();
    QString languageSuffix = getSuffixFromLanguage(language);

    // Remove the old translator, even if we can't load a new one.
    // Otherwise we'll be stuck with the old translated values instead
    // of defaulting to English.
    if (translator != nullptr) {
        QCoreApplication::removeTranslator(translator);
        delete translator;
        translator = nullptr;
    }

    if (newTranslator->load(QString(":/languages/qml_") + languageSuffix)) {
        qInfo() << "Successfully loaded translation for" << languageSuffix;

        translator = newTranslator;
        QCoreApplication::installTranslator(translator);
    }
    else {
        qInfo() << "No translation available for" << languageSuffix;
        delete newTranslator;
    }

    if (m_QmlEngine != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // This is a dynamic retranslation from the settings page.
        // We have to kick the QML engine into reloading our text.
        m_QmlEngine->retranslate();
#else
        // Unreachable below Qt 5.10 due to the check above
        Q_ASSERT(false);
#endif
    }
    else {
        // This is a translation from a non-QML context, which means
        // it is probably app startup. There's nothing to refresh.
    }

    return true;
}

QString StreamingPreferences::getSuffixFromLanguage(StreamingPreferences::Language lang)
{
    switch (lang)
    {
    case LANG_DE:
        return "de";
    case LANG_EN:
        return "en";
    case LANG_FR:
        return "fr";
    case LANG_ZH_CN:
        return "zh_CN";
    case LANG_NB_NO:
        return "nb_NO";
    case LANG_RU:
        return "ru";
    case LANG_ES:
        return "es";
    case LANG_JA:
        return "ja";
    case LANG_VI:
        return "vi";
    case LANG_TH:
        return "th";
    case LANG_KO:
        return "ko";
    case LANG_HU:
        return "hu";
    case LANG_NL:
        return "nl";
    case LANG_SV:
        return "sv";
    case LANG_TR:
        return "tr";
    case LANG_UK:
        return "uk";
    case LANG_ZH_TW:
        return "zh_TW";
    case LANG_PT:
        return "pt";
    case LANG_PT_BR:
        return "pt_BR";
    case LANG_EL:
        return "el";
    case LANG_IT:
        return "it";
    case LANG_HI:
        return "hi";
    case LANG_PL:
        return "pl";
    case LANG_CS:
        return "cs";
    case LANG_HE:
        return "he";
    case LANG_CKB:
        return "ckb";
    case LANG_LT:
        return "lt";
    case LANG_ET:
        return "et";
    case LANG_BG:
        return "bg";
    case LANG_EO:
        return "eo";
    case LANG_TA:
        return "ta";
    case LANG_AUTO:
    default:
        return QLocale::system().name();
    }
}

void StreamingPreferences::save()
{
    if (m_Transient || !ProfileManager::hasActiveProfile()) {
        return;
    }

    QSettings settings;
    ProfileManager::beginProfileSettings(settings);

    settings.setValue(SER_WIDTH, width);
    settings.setValue(SER_HEIGHT, height);
    settings.setValue(SER_FPS, fps);
    settings.setValue(SER_BITRATE, bitrateKbps);
    settings.setValue(SER_UNLOCK_BITRATE, unlockBitrate);
    settings.setValue(SER_AUTOADJUSTBITRATE, autoAdjustBitrate);
    settings.setValue(SER_VSYNC, enableVsync);
    settings.setValue(SER_ENABLEVRR, enableVrr);
    settings.setValue(SER_VRRLATENCYMODE, vrrLatencyMode);
    settings.remove("vrrlatencyoscillation");
    settings.remove("gamescopemailbox"); // Retired Mailbox A/B experiment.
    settings.remove("gamescoperepaint");
    settings.remove("gamescopeforcecomposition");
    settings.setValue(SER_SMOOTHVRRFRAMETIMING, smoothVrrFrameTiming);
    settings.setValue(SER_TRACEVRRFRAMES, traceVrrFrames);
    settings.remove("v2queue"); // The interval queue is now the production policy.
    settings.setValue(SER_GAMEOPTS, gameOptimizations);
    settings.setValue(SER_HOSTAUDIO, playAudioOnHost);
    settings.setValue(SER_MULTICONT, multiController);
    settings.setValue(SER_MDNS, enableMdns);
    settings.setValue(SER_QUITAPPAFTER, quitAppAfter);
    settings.setValue(SER_ABSMOUSEMODE, absoluteMouseMode);
    settings.setValue(SER_ABSTOUCHMODE, absoluteTouchMode);
    settings.setValue(SER_FRAMEPACING, framePacing);
    settings.setValue(SER_CONNWARNINGS, connectionWarnings);
    settings.setValue(SER_CONFWARNINGS, configurationWarnings);
    settings.setValue(SER_RICHPRESENCE, richPresence);
    settings.setValue(SER_GAMEPADMOUSE, gamepadMouse);
    settings.setValue(SER_PACKETSIZE, packetSize);
    settings.setValue(SER_DETECTNETBLOCKING, detectNetworkBlocking);
    settings.setValue(SER_SHOWPERFOVERLAY, showPerformanceOverlay);
    settings.setValue(SER_AUDIOCFG, static_cast<int>(audioConfig));
    settings.setValue(SER_HDR, enableHdr);
    settings.setValue(SER_YUV444, enableYUV444);
    settings.setValue(SER_VIDEOCFG, static_cast<int>(videoCodecConfig));
    settings.setValue(SER_VIDEODEC, static_cast<int>(videoDecoderSelection));
    settings.setValue(SER_RENDERER, static_cast<int>(rendererSelection));
    settings.setValue(SER_WINDOWMODE, static_cast<int>(windowMode));
    settings.setValue(SER_UIDISPLAYMODE, static_cast<int>(uiDisplayMode));
    settings.setValue(SER_LANGUAGE, static_cast<int>(language));
    settings.setValue(SER_DEFAULTVER, CURRENT_DEFAULT_VER);
    settings.setValue(SER_SWAPMOUSEBUTTONS, swapMouseButtons);
    settings.setValue(SER_MUTEONFOCUSLOSS, muteOnFocusLoss);
    settings.setValue(SER_BACKGROUNDGAMEPAD, backgroundGamepad);
    settings.setValue(SER_REVERSESCROLL, reverseScrollDirection);
    settings.setValue(SER_SWAPFACEBUTTONS, swapFaceButtons);
    settings.setValue(SER_CAPTURESYSKEYS, captureSysKeysMode);
    settings.setValue(SER_KEEPAWAKE, keepAwake);
}

void StreamingPreferences::setDiagnosticsStatus(const QString& message)
{
    m_DiagnosticsStatus = message;
    emit diagnosticsChanged();
}

void StreamingPreferences::openDiagnosticsFolder()
{
    const auto root = DiagnosticCapture::rootDirectory();
    if (root.isEmpty() || !QDir().mkpath(root) || !QDesktopServices::openUrl(QUrl::fromLocalFile(root)))
        setDiagnosticsStatus(tr("Unable to open the diagnostics folder: %1").arg(root));
    else
        setDiagnosticsStatus(tr("Diagnostics folder: %1").arg(QDir::toNativeSeparators(root)));
}

void StreamingPreferences::exportLatestDiagnostics()
{
    if (m_ExportingDiagnostics) return;
    m_ExportingDiagnostics = true;
    setDiagnosticsStatus(tr("Exporting the latest diagnostic recording..."));
    const auto root = DiagnosticCapture::rootDirectory();
    // Trace ZIPs can be large. Keep export I/O out of both the GUI and pacing.
    struct ExportResult { QString destination, error; };
    const auto result = std::make_shared<ExportResult>();
    auto thread = QThread::create([root, result] {
        result->destination = DiagnosticCapture::exportLatest(root, result->error);
    });
    connect(thread, &QThread::finished, this, [this, root, result] {
        m_ExportingDiagnostics = false;
        setDiagnosticsStatus(result->destination.isEmpty() ? result->error :
            tr("Exported: %1").arg(QDir::toNativeSeparators(result->destination)));
        if (!result->destination.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(root));
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    // Finish the atomic export on normal application exit. The worker never
    // dereferences preferences; Qt discards the UI callback if they are gone.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, thread, [thread] { thread->wait(); });
    thread->start();
}

QVariantList StreamingPreferences::getFpsChoices(const QVariantList& refreshRates) const
{
    std::vector<int> rates;
    rates.reserve(refreshRates.size());
    for (const QVariant& value : refreshRates) {
        bool ok = false;
        const int refreshHz = value.toInt(&ok);
        if (ok && refreshHz > 0) {
            rates.push_back(refreshHz);
        }
    }

    const std::vector<VrrFpsChoice> choices = VrrRatePolicy::buildChoices(
        rates, fps, enableVsync && enableVrr);
    QVariantList result;
    for (const VrrFpsChoice& choice : choices) {
        QVariantMap item;
        item.insert("video_fps", QString::number(choice.fps));
        item.insert("is_custom", choice.kind == VrrFpsChoiceKind::Custom);

        switch (choice.kind) {
        case VrrFpsChoiceKind::Fixed:
            item.insert("kind", "fixed");
            break;
        case VrrFpsChoiceKind::Vrr:
            item.insert("kind", "vrr");
            break;
        case VrrFpsChoiceKind::LowLatencyVrr:
            item.insert("kind", "low-latency-vrr");
            break;
        case VrrFpsChoiceKind::Custom:
            item.insert("kind", "custom");
            break;
        }

        result.append(item);
    }

    return result;
}

int StreamingPreferences::getDefaultBitrate(int width, int height, int fps, bool yuv444)
{
    // Don't scale bitrate linearly beyond 60 FPS. It's definitely not a linear
    // bitrate increase for frame rate once we get to values that high.
    float frameRateFactor = (fps <= 60 ? fps : (qSqrt(fps / 60.f) * 60.f)) / 30.f;

    // TODO: Collect some empirical data to see if these defaults make sense.
    // We're just using the values that the Shield used, as we have for years.
    static const struct resTable {
        int pixels;
        int factor;
    } resTable[] {
        { 640 * 360, 1 },
        { 854 * 480, 2 },
        { 1280 * 720, 5 },
        { 1920 * 1080, 10 },
        { 2560 * 1440, 20 },
        { 3840 * 2160, 40 },
        { -1, -1 },
    };

    // Calculate the resolution factor by linear interpolation of the resolution table
    float resolutionFactor;
    int pixels = width * height;
    for (int i = 0;; i++) {
        if (pixels == resTable[i].pixels) {
            // We can bail immediately for exact matches
            resolutionFactor = resTable[i].factor;
            break;
        }
        else if (pixels < resTable[i].pixels) {
            if (i == 0) {
                // Never go below the lowest resolution entry
                resolutionFactor = resTable[i].factor;
            }
            else {
                // Interpolate between the entry greater than the chosen resolution (i) and the entry less than the chosen resolution (i-1)
                resolutionFactor = ((float)(pixels - resTable[i-1].pixels) / (resTable[i].pixels - resTable[i-1].pixels)) * (resTable[i].factor - resTable[i-1].factor) + resTable[i-1].factor;
            }
            break;
        }
        else if (resTable[i].pixels == -1) {
            // Never go above the highest resolution entry
            resolutionFactor = resTable[i-1].factor;
            break;
        }
    }

    if (yuv444) {
        // This is rough estimation based on the fact that 4:4:4 doubles the amount of raw YUV data compared to 4:2:0
        resolutionFactor *= 2;
    }

    return qRound(resolutionFactor * frameRateFactor) * 1000;
}

int StreamingPreferences::getDefaultPyroWaveBitrate(int width, int height, int fps, bool yuv444, bool hdr)
{
    // Match the author recommendation shown by calibration without running it.
    return pyroWaveRecommendedKbps(width, height, fps, yuv444, hdr);
}

int StreamingPreferences::getEffectiveDefaultBitrate() const
{
    return videoCodecConfig == VCC_FORCE_PYROWAVE
        ? getDefaultPyroWaveBitrate(width, height, fps, enableYUV444, enableHdr)
        : getDefaultBitrate(width, height, fps, enableYUV444);
}
