#include "decoderprobecache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSettings>
#include <QStringList>

#include <algorithm>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavcodec/version.h>
#include <libavutil/version.h>
}
#define PROBE_CACHE_LAVC_VERSION LIBAVCODEC_VERSION_INT
#define PROBE_CACHE_LAVU_VERSION LIBAVUTIL_VERSION_INT
#else
// Builds without FFmpeg (e.g. Steam Link) have no decoder versions to key on
#define PROBE_CACHE_LAVC_VERSION 0
#define PROBE_CACHE_LAVU_VERSION 0
#endif

#ifdef HAVE_LIBPLACEBO_VULKAN
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>
#endif

#define PROBE_CACHE_GROUP "decoderprobecache"
#define PROBE_CACHE_SCHEMA 1

bool DecoderProbeCache::enabled()
{
    if (qEnvironmentVariableIsSet("MOONLIGHT_PROBE_CACHE") &&
            qEnvironmentVariableIntValue("MOONLIGHT_PROBE_CACHE") == 0) {
        return false;
    }

    // Without a GPU identity we can't tell whether a cached answer still applies
    static const bool haveIdentity = !gpuIdentity().isEmpty();
    return haveIdentity;
}

QString DecoderProbeCache::gpuIdentity()
{
    static QString identity;
    static bool computed = false;
    if (computed) {
        return identity;
    }
    computed = true;

#ifdef HAVE_LIBPLACEBO_VULKAN
    // Enumerating physical devices needs an instance but no device, which is cheap
    // (tens of milliseconds) compared to the device creations we're avoiding.
    if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
        return identity;
    }

    auto getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    auto createInstance = (PFN_vkCreateInstance)(getInstanceProcAddr ? getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") : nullptr);
    if (createInstance != nullptr) {
        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Moonlight";
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo instanceInfo = {};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

        VkInstance instance = VK_NULL_HANDLE;
        if (createInstance(&instanceInfo, nullptr, &instance) == VK_SUCCESS) {
            auto enumerate = (PFN_vkEnumeratePhysicalDevices)getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
            auto getProps = (PFN_vkGetPhysicalDeviceProperties)getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");
            auto destroyInstance = (PFN_vkDestroyInstance)getInstanceProcAddr(instance, "vkDestroyInstance");

            if (enumerate && getProps) {
                uint32_t count = 0;
                enumerate(instance, &count, nullptr);
                std::vector<VkPhysicalDevice> devices(count);
                enumerate(instance, &count, devices.data());

                QStringList parts;
                for (uint32_t i = 0; i < count; i++) {
                    VkPhysicalDeviceProperties props = {};
                    getProps(devices[i], &props);
                    parts.append(QString("%1:%2:%3:%4:%5")
                                 .arg(props.deviceName)
                                 .arg(props.vendorID, 0, 16)
                                 .arg(props.deviceID, 0, 16)
                                 .arg(props.driverVersion, 0, 16)
                                 .arg(props.apiVersion, 0, 16));
                }
                identity = parts.join(';');
            }

            if (destroyInstance) {
                destroyInstance(instance, nullptr);
            }
        }
    }

    SDL_Vulkan_UnloadLibrary();
#endif

    return identity;
}

QString DecoderProbeCache::environmentKey()
{
    static QString key;
    if (!key.isEmpty()) {
        return key;
    }

    // Environment overrides that steer decoder/renderer selection must invalidate the cache too
    QStringList overrides;
    for (const char* name : { "FORCE_VAAPI", "LIBVA_DRIVER_NAME", "VAAPI_FORCE_DIRECT", "VAAPI_FORCE_INDIRECT",
                              "PLVK_LEAN_DEVICE", "PLVK_ALLOW_SOFTWARE", "PLVK_ALLOW_INTEL", "PLVK_DEBUG", "PLVK_DEBUG_EXTRA",
                              "SDL_VIDEODRIVER", "QT_QPA_PLATFORM", "WAYLAND_DISPLAY", "DISPLAY", "GAMESCOPE_WAYLAND_DISPLAY",
                              "MOONLIGHT_FORCE_SOFTWARE_DECODING", "FORCE_SOFTWARE_DECODING", "MMAL_FORCE_RENDER" }) {
        if (qEnvironmentVariableIsSet(name)) {
            overrides.append(QString("%1=%2").arg(name, qEnvironmentVariable(name)));
        }
    }

    const char* videoDriver = SDL_GetCurrentVideoDriver();
    key = QString("schema%1|%2|sdl:%3|avc:%4|avu:%5|gpu:%6|env:%7")
            .arg(PROBE_CACHE_SCHEMA)
            .arg(VERSION_STR)
            .arg(videoDriver ? videoDriver : "none")
            .arg(PROBE_CACHE_LAVC_VERSION)
            .arg(PROBE_CACHE_LAVU_VERSION)
            .arg(gpuIdentity())
            .arg(overrides.join(','));
    return key;
}

QString DecoderProbeCache::settingsGroup()
{
    QByteArray hash = QCryptographicHash::hash(environmentKey().toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString(PROBE_CACHE_GROUP "/") + QString::fromLatin1(hash.left(16));
}

bool DecoderProbeCache::lookupDecoderInfo(DecoderInfo& info)
{
    if (!enabled()) {
        return false;
    }

    QSettings settings;
    settings.beginGroup(settingsGroup());
    bool hit = settings.contains("decoderinfo/hw");
    if (hit) {
        info.hardwareAccelerated = settings.value("decoderinfo/hw").toBool();
        info.fullScreenOnly = settings.value("decoderinfo/fullscreenonly").toBool();
        info.hdrSupported = settings.value("decoderinfo/hdr").toBool();
        info.maxResolution = QSize(settings.value("decoderinfo/maxw").toInt(),
                                   settings.value("decoderinfo/maxh").toInt());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Decoder probe cache hit: hw=%d fullscreenonly=%d hdr=%d max=%dx%d",
                    info.hardwareAccelerated, info.fullScreenOnly, info.hdrSupported,
                    info.maxResolution.width(), info.maxResolution.height());
    }
    settings.endGroup();
    return hit;
}

void DecoderProbeCache::storeDecoderInfo(const DecoderInfo& info)
{
    if (!enabled()) {
        return;
    }

    QSettings settings;

    // Several environments legitimately coexist (e.g. Wayland and X11 sessions, or a headless
    // test run), so only trim the cache once it holds more than a handful of them, evicting the
    // least recently stored.
    QString group = settingsGroup();
    settings.beginGroup(PROBE_CACHE_GROUP);
    QStringList groups = settings.childGroups();
    const int maxGroups = 8;
    if (groups.size() > maxGroups) {
        std::sort(groups.begin(), groups.end(), [&settings](const QString& a, const QString& b) {
            return settings.value(a + "/touched").toLongLong() < settings.value(b + "/touched").toLongLong();
        });
        for (int i = 0; i < groups.size() - maxGroups; i++) {
            if (QString(PROBE_CACHE_GROUP "/") + groups[i] != group) {
                settings.remove(groups[i]);
            }
        }
    }
    settings.endGroup();

    settings.beginGroup(group);
    settings.setValue("touched", QDateTime::currentSecsSinceEpoch());
    settings.setValue("decoderinfo/hw", info.hardwareAccelerated);
    settings.setValue("decoderinfo/fullscreenonly", info.fullScreenOnly);
    settings.setValue("decoderinfo/hdr", info.hdrSupported);
    settings.setValue("decoderinfo/maxw", info.maxResolution.width());
    settings.setValue("decoderinfo/maxh", info.maxResolution.height());
    settings.endGroup();
}

static QString availabilityKey(StreamingPreferences::VideoDecoderSelection vds,
                               int videoFormat, int width, int height, int frameRate)
{
    return QString("availability/vds%1/fmt%2/%3x%4@%5")
            .arg((int)vds).arg(videoFormat, 0, 16).arg(width).arg(height).arg(frameRate);
}

bool DecoderProbeCache::lookupAvailability(StreamingPreferences::VideoDecoderSelection vds,
                                           int videoFormat, int width, int height, int frameRate,
                                           int& availability)
{
    if (!enabled()) {
        return false;
    }

    QSettings settings;
    settings.beginGroup(settingsGroup());
    QString key = availabilityKey(vds, videoFormat, width, height, frameRate);
    bool hit = settings.contains(key);
    if (hit) {
        availability = settings.value(key).toInt();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Decoder probe cache hit: format 0x%x %dx%dx%d vds %d -> availability %d",
                    videoFormat, width, height, frameRate, (int)vds, availability);
    }
    settings.endGroup();
    return hit;
}

void DecoderProbeCache::storeAvailability(StreamingPreferences::VideoDecoderSelection vds,
                                          int videoFormat, int width, int height, int frameRate,
                                          int availability)
{
    if (!enabled()) {
        return;
    }

    QSettings settings;
    settings.beginGroup(settingsGroup());
    settings.setValue(availabilityKey(vds, videoFormat, width, height, frameRate), availability);
    settings.endGroup();
}

static QString streamPropertiesKey(StreamingPreferences::VideoDecoderSelection vds,
                                   StreamingPreferences::RendererSelection renderer,
                                   int videoFormat, int width, int height, int frameRate,
                                   bool enableVrr)
{
    return QString("streamprops/vds%1/rs%2/fmt%3/%4x%5@%6/vrr%7")
            .arg((int)vds).arg((int)renderer).arg(videoFormat, 0, 16)
            .arg(width).arg(height).arg(frameRate).arg(enableVrr ? 1 : 0);
}

bool DecoderProbeCache::lookupStreamProperties(StreamingPreferences::VideoDecoderSelection vds,
                                               StreamingPreferences::RendererSelection renderer,
                                               int videoFormat, int width, int height, int frameRate,
                                               bool enableVrr, StreamProperties& props)
{
    if (!enabled()) {
        return false;
    }

    QSettings settings;
    settings.beginGroup(settingsGroup());
    QString key = streamPropertiesKey(vds, renderer, videoFormat, width, height, frameRate, enableVrr);
    bool hit = settings.contains(key + "/caps");
    if (hit) {
        props.capabilities = settings.value(key + "/caps").toInt();
        props.colorSpace = settings.value(key + "/colorspace").toInt();
        props.colorRange = settings.value(key + "/colorrange").toInt();
        props.alwaysFullScreen = settings.value(key + "/fullscreen").toBool();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Decoder probe cache hit: stream properties for format 0x%x %dx%dx%d vds %d rs %d vrr %d -> caps 0x%x colorspace %d range %d",
                    videoFormat, width, height, frameRate, (int)vds, (int)renderer, enableVrr,
                    props.capabilities, props.colorSpace, props.colorRange);
    }
    settings.endGroup();
    return hit;
}

void DecoderProbeCache::storeStreamProperties(StreamingPreferences::VideoDecoderSelection vds,
                                              StreamingPreferences::RendererSelection renderer,
                                              int videoFormat, int width, int height, int frameRate,
                                              bool enableVrr, const StreamProperties& props)
{
    if (!enabled()) {
        return;
    }

    QSettings settings;
    settings.beginGroup(settingsGroup());
    QString key = streamPropertiesKey(vds, renderer, videoFormat, width, height, frameRate, enableVrr);
    settings.setValue(key + "/caps", props.capabilities);
    settings.setValue(key + "/colorspace", props.colorSpace);
    settings.setValue(key + "/colorrange", props.colorRange);
    settings.setValue(key + "/fullscreen", props.alwaysFullScreen);
    settings.endGroup();
}

bool DecoderProbeCache::lookupFlag(const QString& name)
{
    if (!enabled()) {
        return false;
    }

    QSettings settings;
    settings.beginGroup(settingsGroup());
    bool value = settings.value("flags/" + name, false).toBool();
    settings.endGroup();
    if (value) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Decoder probe cache hit: flag %s",
                    name.toUtf8().constData());
    }
    return value;
}

void DecoderProbeCache::storeFlag(const QString& name, bool value)
{
    if (!enabled()) {
        return;
    }

    QSettings settings;
    settings.beginGroup(settingsGroup());
    settings.setValue("flags/" + name, value);
    settings.endGroup();
}
