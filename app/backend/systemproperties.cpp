#include "systemproperties.h"
#include "utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibraryInfo>

#include "streaming/session.h"
#include "streaming/streamutils.h"
#include "streaming/vrrratepolicy.h"

#ifdef Q_OS_DARWIN
#include "streaming/video/ffmpeg-renderers/macdisplaytiming.h"
#endif

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

class SystemPropertyQueryThread : public QThread
{
public:
    SystemPropertyQueryThread(SystemProperties* properties)
        : QThread(properties), m_Properties(properties)
    {
        setObjectName("System Properties Async Query Thread");
    }

private:
    void run() override
    {
        bool hasHardwareAcceleration;
        bool rendererAlwaysFullScreen;
        bool supportsHdr;
        QSize maximumResolution;

        Session::getDecoderInfo(m_Properties->testWindow, hasHardwareAcceleration, rendererAlwaysFullScreen, supportsHdr, maximumResolution);

        // Propagate the decoder properties to the SystemProperties singleton and emit any change signals on the main thread
        QMetaObject::invokeMethod(m_Properties, "updateDecoderProperties",
                                  Qt::QueuedConnection,
                                  Q_ARG(bool, hasHardwareAcceleration),
                                  Q_ARG(bool, rendererAlwaysFullScreen),
                                  Q_ARG(QSize, maximumResolution),
                                  Q_ARG(bool, supportsHdr));
    }

private:
    SystemProperties* m_Properties;
};

SystemProperties::BenqGr10State SystemProperties::benqGr10State()
{
#ifdef Q_OS_LINUX
    const QDir usb(QStringLiteral("/sys/bus/usb/devices"));
    for (const QString& name : usb.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name.contains(':')) {
            continue; // USB interface, not the device descriptor
        }
        const QDir device(usb.filePath(name));
        QFile vendor(device.filePath(QStringLiteral("idVendor")));
        QFile product(device.filePath(QStringLiteral("idProduct")));
        if (vendor.open(QIODevice::ReadOnly) && vendor.readAll().trimmed() == "04a5" &&
                product.open(QIODevice::ReadOnly) && product.readAll().trimmed() == "b103") {
            return QFileInfo::exists(QString::fromLatin1(Gr10RecoverySocket)) ? Gr10Ready : Gr10NeedsHelper;
        }
    }
#endif
    return Gr10Absent;
}

SystemProperties::SystemProperties()
{
    versionString = QString(VERSION_STR);
    hasDesktopEnvironment = WMUtils::isRunningDesktopEnvironment();
    isRunningWayland = WMUtils::isRunningWayland();
    isRunningXWayland = isRunningWayland && QGuiApplication::platformName() == "xcb";
    usesMaterial3Theme = QLibraryInfo::version() >= QVersionNumber(6, 5, 0);

#ifdef Q_OS_DARWIN
    isDarwin = true;
#else
    isDarwin = false;
#endif

    QString nativeArch = QSysInfo::currentCpuArchitecture();

#ifdef Q_OS_WIN32
    {
        USHORT processArch, machineArch;

        // Use IsWow64Process2() because it doesn't lie on ARM64
        if (IsWow64Process2(GetCurrentProcess(), &processArch, &machineArch)) {
            switch (machineArch) {
            case IMAGE_FILE_MACHINE_I386:
                nativeArch = "i386";
                break;
            case IMAGE_FILE_MACHINE_AMD64:
                nativeArch = "x86_64";
                break;
            case IMAGE_FILE_MACHINE_ARM64:
                nativeArch = "arm64";
                break;
            }
        }

        isWow64 = nativeArch != QSysInfo::buildCpuArchitecture();
    }
#else
    isWow64 = false;
#endif

    if (nativeArch == "i386") {
        friendlyNativeArchName = "x86";
    }
    else if (nativeArch == "x86_64") {
        friendlyNativeArchName = "x64";
    }
    else {
        friendlyNativeArchName = nativeArch.toUpper();
    }

    // Assume we can probably launch a browser if we're in a GUI environment
    hasBrowser = hasDesktopEnvironment;

#ifdef HAVE_DISCORD
    hasDiscordIntegration = true;
#else
    hasDiscordIntegration = false;
#endif

    // PyroWave uses Vulkan decoding with each platform's GPU surface renderer.
#if defined(HAVE_PYROWAVE) && (defined(Q_OS_WIN32) || defined(Q_OS_LINUX) || defined(Q_OS_DARWIN))
    hasPyroWave = true;
#else
    hasPyroWave = false;
#endif

    // These will be queried asynchronously to avoid blocking the UI
    hasHardwareAcceleration = true;
    rendererAlwaysFullScreen = false;
    supportsHdr = true;
    maximumResolution = QSize(0, 0);
}

SystemProperties::~SystemProperties()
{
    waitForAsyncLoad();
}

void SystemProperties::updateDecoderProperties(bool hasHardwareAcceleration, bool rendererAlwaysFullScreen, QSize maximumResolution, bool supportsHdr)
{
    SDL_assert(testWindow);

    if (hasHardwareAcceleration != this->hasHardwareAcceleration) {
        this->hasHardwareAcceleration = hasHardwareAcceleration;
        emit hasHardwareAccelerationChanged();
    }

    if (rendererAlwaysFullScreen != this->rendererAlwaysFullScreen) {
        this->rendererAlwaysFullScreen = rendererAlwaysFullScreen;
        emit rendererAlwaysFullScreenChanged();
    }

    if (maximumResolution != this->maximumResolution) {
        this->maximumResolution = maximumResolution;
        emit maximumResolutionChanged();
    }

    if (supportsHdr != this->supportsHdr) {
        this->supportsHdr = supportsHdr;
        emit supportsHdrChanged();
    }

    SDL_DestroyWindow(testWindow);
    testWindow = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

QRect SystemProperties::getNativeResolution(int displayIndex)
{
    // Returns default constructed QRect if out of bounds
    return monitorNativeResolutions.value(displayIndex);
}

QRect SystemProperties::getSafeAreaResolution(int displayIndex)
{
    // Returns default constructed QRect if out of bounds
    return monitorSafeAreaResolutions.value(displayIndex);
}

int SystemProperties::getRefreshRate(int displayIndex)
{
    // Returns 0 if out of bounds
    return monitorRefreshRates.value(displayIndex);
}

int SystemProperties::getUltraLowLatencyVrrFpsLimit()
{
    if (ultraLowLatencyVrrFpsLimit >= 0) return ultraLowLatencyVrrFpsLimit;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return 0;

    int limit = 0;
    SDL_Window* probe = StreamUtils::ultraLowLatencyVrrPlatformSupported() ?
        StreamUtils::createTestWindow() : nullptr;
    for (int index = 0; index < SDL_GetNumVideoDisplays(); ++index) {
        SDL_DisplayMode desktopMode = {};
        if (SDL_GetDesktopDisplayMode(index, &desktopMode) != 0) continue;
        int refreshHz = desktopMode.refresh_rate;
#ifdef Q_OS_DARWIN
        const auto nativeTiming = queryMacDisplayTimingForDisplay(index);
        if (nativeTiming.hasValidTiming()) refreshHz = nativeTiming.maximumFramesPerSecond;
#endif
        if (probe) {
            SDL_SetWindowPosition(probe, SDL_WINDOWPOS_CENTERED_DISPLAY(index), SDL_WINDOWPOS_CENTERED_DISPLAY(index));
        }
        const bool supported = probe && SDL_GetWindowDisplayIndex(probe) == index &&
            StreamUtils::supportsUltraLowLatencyVrr(probe, refreshHz);
        limit = qMax(limit, VrrRatePolicy::ultraLowLatencyMaxFps(refreshHz, supported));
    }
    if (probe) SDL_DestroyWindow(probe);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return ultraLowLatencyVrrFpsLimit = limit;
}

void SystemProperties::startAsyncLoad()
{
    if (systemPropertyQueryThread) {
        // Already started/completed
        return;
    }

    // This isn't actually asynchronous (due to the need to synchronize with
    // SdlGamepadKeyNavigation), but we don't query it in the constructor
    // because it's expensive.
    unmappedGamepads = SdlInputHandler::getUnmappedGamepads();
    if (!unmappedGamepads.isEmpty()) {
        emit unmappedGamepadsChanged();
    }

    // We initialize the video subsystem and test window on the main thread
    // because some platforms (macOS) do not support window creation on
    // non-main threads.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return;
    }

    testWindow = StreamUtils::createTestWindow();
    if (!testWindow) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create window for hardware decode test: %s",
                     SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return;
    }

    // Update display related attributes (max FPS, native resolution, etc).
    //
    // NB: SDL3 will forcefully refresh displays when a window is created,
    // so we place this after the window creation to ensure we don't pay
    // the penalty for mode enumeration twice.
    refreshDisplays();

    systemPropertyQueryThread = new SystemPropertyQueryThread(this);
    systemPropertyQueryThread->start();
}

void SystemProperties::waitForAsyncLoad()
{
    if (systemPropertyQueryThread) {
        systemPropertyQueryThread->wait();
    }
}

void SystemProperties::refreshDisplays()
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return;
    }

    monitorNativeResolutions.clear();
    monitorSafeAreaResolutions.clear();
    monitorRefreshRates.clear();
    ultraLowLatencyVrrFpsLimit = -1;

    SDL_DisplayMode bestMode;
    for (int displayIndex = 0; displayIndex < SDL_GetNumVideoDisplays(); displayIndex++) {
        SDL_DisplayMode desktopMode;
        SDL_Rect safeArea;

        if (StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea)) {
            if (desktopMode.w <= 8192 && desktopMode.h <= 8192) {
                // Keep these lists compact because their QML consumers iterate until
                // the first empty entry. Inserting by SDL display index is invalid if
                // an earlier display was skipped (for example, a >8K virtual display).
                monitorNativeResolutions.append(QRect(0, 0, desktopMode.w, desktopMode.h));
                monitorSafeAreaResolutions.append(QRect(0, 0, safeArea.w, safeArea.h));
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Skipping resolution over 8K: %dx%d",
                            desktopMode.w, desktopMode.h);
            }

            // Start at desktop mode and work our way up
            bestMode = desktopMode;
            int numDisplayModes = SDL_GetNumDisplayModes(displayIndex);
            for (int i = 0; i < numDisplayModes; i++) {
                SDL_DisplayMode mode;
                if (SDL_GetDisplayMode(displayIndex, i, &mode) == 0) {
                    if (mode.w == desktopMode.w && mode.h == desktopMode.h) {
                        if (mode.refresh_rate > bestMode.refresh_rate) {
                            bestMode = mode;
                        }
                    }
                }
            }

#ifdef Q_OS_DARWIN
            const auto nativeTiming = queryMacDisplayTimingForDisplay(displayIndex);
            if (nativeTiming.hasValidTiming()) {
                bestMode.refresh_rate = nativeTiming.maximumFramesPerSecond;
            }
#endif

            // Try to normalize values around our our standard refresh rates.
            // Some displays/OSes report values that are slightly off.
            if (bestMode.refresh_rate >= 58 && bestMode.refresh_rate <= 62) {
                monitorRefreshRates.append(60);
            }
            else if (bestMode.refresh_rate >= 28 && bestMode.refresh_rate <= 32) {
                monitorRefreshRates.append(30);
            }
            else {
                monitorRefreshRates.append(bestMode.refresh_rate);
            }
        }
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
