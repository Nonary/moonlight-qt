#pragma once

#include <QByteArray>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QString>

#include "clientdisplaycapabilities.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ClientDisplayCapabilitiesWin
{

constexpr qsizetype kMaxMhc2ProfileBytes = 32 * 1024 * 1024;
constexpr qint64 kProfileReadBudgetMs = 250;

enum class ProfileType
{
    Icc,
    Other,
};

enum class DisplayColorMode
{
    Extended,
    Standard,
};

enum class ProfileLookupStatus
{
    Unknown,
    ExtendedDefaultAvailable,
    NoExtendedDefault,
    Failed,
};

enum class DxgiColorSpace
{
    RgbFullG2084NoneP2020,
    Srgb,
    ScRgb,
    StudioPq,
    Other,
};

// This is the value-only result used by the collector and its fixtures.  It
// intentionally has no display, profile, adapter, or OS handle identity.
struct CollectedCapabilities
{
    std::optional<int> calibratedPeakNits;
    std::optional<int> edidPeakNits;
};

struct SelectedDisplay
{
    bool resolved = false;
    bool unique = false;
    bool hiddenWindowMatches = false;
    int displayIndex = -1;
    QString gdiDeviceName;
    QRect geometry;
};

struct DisplayMapping
{
    bool valid = false;
    bool uniqueActivePath = false;
    bool identityComplete = false;
    int displayIndex = -1;
    QString gdiDeviceName;
    QString outputDeviceName;
    QRect desktopBounds;
    std::uint64_t sourceAdapterLuid = 0;
    std::uint32_t sourceId = 0;
    std::uint64_t targetAdapterLuid = 0;
    std::uint32_t targetId = 0;
    std::uint32_t pathFlags = 0;
    bool targetAvailable = false;
    std::uint32_t dxgiAdapterIndex = 0;
    std::uint32_t dxgiOutputIndex = 0;
};

// ProfileProbe is provider-owned input.  profileName and bytes are consumed
// locally and are never copied into CollectedCapabilities or the launch args.
struct ProfileProbe
{
    bool exportsAvailable = false;
    bool osSupported = false;
    bool scopeSupported = false;
    ProfileLookupStatus lookupStatus = ProfileLookupStatus::Unknown;
    ProfileType profileType = ProfileType::Other;
    DisplayColorMode displayColorMode = DisplayColorMode::Standard;
    QString profileName;
    QByteArray bytes;
    // Windows collectors parse associated profiles before returning and clear
    // bytes. Test providers may leave bytes populated for the collector to
    // parse, so raw profile data remains provider-owned in either case.
    std::optional<int> parsedMhc2PeakNits;
    bool readCompleted = false;
    qint64 elapsedMs = 0;
};

struct DxgiOutputProbe
{
    bool valid = false;
    bool current = false;
    bool unique = false;
    bool attachedToDesktop = false;
    QString deviceName;
    QRect desktopBounds;
    std::uint32_t adapterIndex = 0;
    std::uint32_t outputIndex = 0;
    DxgiColorSpace colorSpace = DxgiColorSpace::Other;
    double maxLuminance = 0.0;
};

// A completed GUI-thread collection.  Every member is a value copy: no
// QScreen, SDL window, Windows API object, or mutable OS handle crosses into
// the connection worker.  The identity fields let Session perform one final
// collection immediately before handoff instead of trusting an initialization
// time display snapshot.
struct ClientDisplayPreparation
{
    SelectedDisplay selectedDisplay;
    DisplayMapping mapping;
    DxgiOutputProbe dxgiOutput;
    ClientDisplayCapabilities capabilities;
};

struct CollectorProvider
{
    std::function<SelectedDisplay(const QScreen *, quintptr)> resolveSelectedDisplay;
    std::function<DisplayMapping(const SelectedDisplay &)> mapDisplayConfig;
    std::function<ProfileProbe(const DisplayMapping &, qint64 budgetMs)> readColorProfile;
    std::function<std::vector<ProfileProbe>(const DisplayMapping &, qint64 budgetMs)>
        readAssociatedColorProfiles;
    std::function<DxgiOutputProbe(const DisplayMapping &)> readDxgiOutput;
    std::function<bool(const DisplayMapping &, const DxgiOutputProbe &)> revalidate;
    std::function<bool(
        const QScreen *,
        quintptr,
        const SelectedDisplay &,
        const DisplayMapping &,
        const DxgiOutputProbe &)> revalidateSelected;
};

// Resolve a Qt-selected display against SDL's native desktop bounds. The
// hidden SDL probe identity is authoritative when available because Qt and
// SDL may report different rectangle sizes under Windows DPI scaling.
std::optional<int> resolveDisplayIndex(
    const QRect &screenGeometry,
    int hiddenDisplayIndex,
    const std::vector<QRect> &displayBounds);

std::optional<int> parseMhc2PeakNits(const QByteArray &profileBytes);
bool isSafeProfileBasename(const QString &profileName);

std::optional<CollectedCapabilities> collectCapabilities(
    const SelectedDisplay &selectedDisplay,
    const CollectorProvider &provider);

std::optional<CollectedCapabilities> collectCapabilities(
    const QScreen *selectedScreen,
    quintptr hiddenWindow,
    const CollectorProvider &provider);

std::optional<ClientDisplayPreparation> collectClientDisplayPreparation(
    const SelectedDisplay &selectedDisplay,
    const CollectorProvider &provider);

std::optional<ClientDisplayPreparation> collectClientDisplayPreparation(
    const QScreen *selectedScreen,
    quintptr hiddenWindow,
    const CollectorProvider &provider);

std::optional<ClientDisplayCapabilities> collectClientDisplayCapabilities(
    const QScreen *selectedScreen,
    quintptr hiddenWindow,
    const CollectorProvider &provider);

std::optional<ClientDisplayCapabilities> collectClientDisplayCapabilities(
    const QScreen *selectedScreen,
    quintptr hiddenWindow);

#if defined(Q_OS_WIN) || defined(Q_OS_WIN32)
CollectorProvider makeWindowsCollectorProvider();
#else
inline CollectorProvider makeWindowsCollectorProvider()
{
    return {};
}
#endif

}
