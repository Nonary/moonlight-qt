#include "clientdisplaycapabilities_win.h"

#include <QElapsedTimer>
#include <QSettings>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Icm.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "../SDL_compat.h"

#endif

namespace ClientDisplayCapabilitiesWin
{

namespace
{

std::optional<int> normalizePeakNits(double value)
{
    if (!std::isfinite(value) || value < 1.0 || value > 100000.0) {
        return std::nullopt;
    }

    const double rounded = std::floor(value + 0.5);
    if (!std::isfinite(rounded) || rounded < 1.0 || rounded > 100000.0) {
        return std::nullopt;
    }
    return static_cast<int>(rounded);
}

quint32 readBe32(const QByteArray &bytes, qsizetype offset)
{
    const auto *data = reinterpret_cast<const uchar *>(bytes.constData() + offset);
    return (static_cast<quint32>(data[0]) << 24) |
           (static_cast<quint32>(data[1]) << 16) |
           (static_cast<quint32>(data[2]) << 8) |
           static_cast<quint32>(data[3]);
}

bool sameName(const QString &left, const QString &right)
{
    return QString::compare(left, right, Qt::CaseInsensitive) == 0;
}

bool selectedDisplayIsUsable(const SelectedDisplay &selected)
{
    return selected.resolved && selected.unique && selected.hiddenWindowMatches &&
           selected.displayIndex >= 0 && !selected.gdiDeviceName.isEmpty() &&
           selected.geometry.width() > 0 && selected.geometry.height() > 0;
}

bool mappingIsUsable(const SelectedDisplay &selected, const DisplayMapping &mapping)
{
    return mapping.valid && mapping.uniqueActivePath && mapping.identityComplete &&
           mapping.targetAvailable && mapping.displayIndex == selected.displayIndex &&
           sameName(mapping.gdiDeviceName, selected.gdiDeviceName) &&
           mapping.desktopBounds == selected.geometry;
}

bool outputIsUsable(const DisplayMapping &mapping, const DxgiOutputProbe &output)
{
    return output.valid && output.current && output.unique && output.attachedToDesktop &&
           output.adapterIndex == mapping.dxgiAdapterIndex &&
           output.outputIndex == mapping.dxgiOutputIndex &&
           sameName(output.deviceName, mapping.outputDeviceName) &&
           output.desktopBounds == mapping.desktopBounds &&
           output.colorSpace == DxgiColorSpace::RgbFullG2084NoneP2020;
}

template <typename OptionalSource>
void setSource(OptionalSource &destination, const char *source, double peakNits)
{
    using Source = typename OptionalSource::value_type;
    destination = Source { QString::fromLatin1(source), peakNits };
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

std::uint64_t packLuid(const LUID &luid)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) |
           static_cast<std::uint64_t>(luid.LowPart);
}

LUID unpackLuid(std::uint64_t value)
{
    LUID luid = {};
    luid.HighPart = static_cast<LONG>(value >> 32);
    luid.LowPart = static_cast<DWORD>(value & 0xffffffffu);
    return luid;
}

bool sameLuid(const LUID &left, std::uint64_t right)
{
    return packLuid(left) == right;
}

struct DisplayConfigSnapshot
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

bool queryActiveDisplayConfig(DisplayConfigSnapshot &snapshot)
{
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;

    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return false;
        }

        snapshot.paths.assign(pathCount, DISPLAYCONFIG_PATH_INFO {});
        snapshot.modes.assign(modeCount, DISPLAYCONFIG_MODE_INFO {});
        UINT32 actualPathCount = pathCount;
        UINT32 actualModeCount = modeCount;
        const LONG result = QueryDisplayConfig(
            flags,
            &actualPathCount,
            snapshot.paths.empty() ? nullptr : snapshot.paths.data(),
            &actualModeCount,
            snapshot.modes.empty() ? nullptr : snapshot.modes.data(),
            nullptr);
        if (result == ERROR_SUCCESS) {
            snapshot.paths.resize(actualPathCount);
            snapshot.modes.resize(actualModeCount);
            return true;
        }
        if (result != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
    }
    return false;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> findDxgiOutput(
    const DisplayMapping &mapping,
    const SelectedDisplay &selected)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return std::nullopt;
    }

    std::optional<std::pair<std::uint32_t, std::uint32_t>> match;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 adapterDesc = {};
        if (FAILED(adapter->GetDesc1(&adapterDesc)) ||
            !sameLuid(adapterDesc.AdapterLuid, mapping.sourceAdapterLuid)) {
            continue;
        }

        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!output) {
                continue;
            }

            DXGI_OUTPUT_DESC outputDesc = {};
            if (FAILED(output->GetDesc(&outputDesc))) {
                continue;
            }

            const QString outputName = QString::fromWCharArray(outputDesc.DeviceName);
            const QRect bounds(
                outputDesc.DesktopCoordinates.left,
                outputDesc.DesktopCoordinates.top,
                outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left,
                outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);
            if (!sameName(outputName, selected.gdiDeviceName) || bounds != selected.geometry) {
                continue;
            }

            const auto candidate = std::make_pair(
                static_cast<std::uint32_t>(adapterIndex),
                static_cast<std::uint32_t>(outputIndex));
            if (match.has_value()) {
                return std::nullopt;
            }
            match = candidate;
        }
    }
    return match;
}

std::optional<DisplayMapping> mapDisplayConfig(const SelectedDisplay &selected)
{
    DisplayConfigSnapshot config;
    if (!queryActiveDisplayConfig(config)) {
        return std::nullopt;
    }

    std::optional<DisplayMapping> match;
    for (const DISPLAYCONFIG_PATH_INFO &path : config.paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
            _wcsicmp(sourceName.viewGdiDeviceName, reinterpret_cast<const wchar_t *>(selected.gdiDeviceName.utf16())) != 0) {
            continue;
        }

        if (match.has_value()) {
            return std::nullopt;
        }

        DisplayMapping mapping;
        mapping.valid = true;
        mapping.uniqueActivePath = true;
        mapping.identityComplete = true;
        mapping.displayIndex = selected.displayIndex;
        mapping.gdiDeviceName = selected.gdiDeviceName;
        mapping.desktopBounds = selected.geometry;
        mapping.sourceAdapterLuid = packLuid(path.sourceInfo.adapterId);
        mapping.sourceId = path.sourceInfo.id;
        mapping.targetAdapterLuid = packLuid(path.targetInfo.adapterId);
        mapping.targetId = path.targetInfo.id;
        mapping.pathFlags = path.flags;
        mapping.targetAvailable = path.targetInfo.targetAvailable != FALSE;
        if (!mapping.targetAvailable) {
            return std::nullopt;
        }

        const auto dxgiOutput = findDxgiOutput(mapping, selected);
        if (!dxgiOutput.has_value()) {
            return std::nullopt;
        }
        mapping.dxgiAdapterIndex = dxgiOutput->first;
        mapping.dxgiOutputIndex = dxgiOutput->second;
        mapping.outputDeviceName = selected.gdiDeviceName;
        match = mapping;
    }
    return match;
}

struct DynamicColorProfileApi
{
    using GetDisplayUserScope = HRESULT(WINAPI *)(
        LUID,
        UINT32,
        WCS_PROFILE_MANAGEMENT_SCOPE *);
    using GetDisplayDefault = HRESULT(WINAPI *)(
        WCS_PROFILE_MANAGEMENT_SCOPE,
        LUID,
        UINT32,
        COLORPROFILETYPE,
        COLORPROFILESUBTYPE,
        LPWSTR *);
    using GetDisplayList = HRESULT(WINAPI *)(
        WCS_PROFILE_MANAGEMENT_SCOPE,
        LUID,
        UINT32,
        LPWSTR **,
        PDWORD);
    using GetColorDirectory = BOOL(WINAPI *)(PCWSTR, PWSTR, PDWORD);

    HMODULE module = nullptr;
    GetDisplayUserScope getDisplayUserScope = nullptr;
    GetDisplayDefault getDisplayDefault = nullptr;
    GetDisplayList getDisplayList = nullptr;
    GetColorDirectory getColorDirectory = nullptr;

    ~DynamicColorProfileApi()
    {
        if (module) {
            FreeLibrary(module);
        }
    }

    bool load()
    {
        module = LoadLibraryExW(L"mscms.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            return false;
        }
        getDisplayUserScope = reinterpret_cast<GetDisplayUserScope>(
            GetProcAddress(module, "ColorProfileGetDisplayUserScope"));
        getDisplayDefault = reinterpret_cast<GetDisplayDefault>(
            GetProcAddress(module, "ColorProfileGetDisplayDefault"));
        getDisplayList = reinterpret_cast<GetDisplayList>(
            GetProcAddress(module, "ColorProfileGetDisplayList"));
        getColorDirectory = reinterpret_cast<GetColorDirectory>(
            GetProcAddress(module, "GetColorDirectoryW"));
        // GetColorDirectoryW is optional on older supported Windows builds;
        // colorDirectory() has a bounded system-directory fallback.
        // ColorProfileGetDisplayList is optional so the default-profile path
        // remains available on older Windows builds.
        return getDisplayUserScope && getDisplayDefault;
    }
};

struct HandleGuard
{
    HANDLE handle = INVALID_HANDLE_VALUE;

    ~HandleGuard()
    {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
        }
    }

    explicit operator bool() const
    {
        return handle != INVALID_HANDLE_VALUE && handle != nullptr;
    }
};

struct FileIdentity
{
    ULONGLONG volumeSerial = 0;
    FILE_ID_128 fileId = {};
};

bool getFileIdentity(HANDLE handle, FileIdentity &identity)
{
    FILE_ID_INFO info = {};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        return false;
    }
    identity.volumeSerial = info.VolumeSerialNumber;
    identity.fileId = info.FileId;
    return true;
}

bool isReparsePoint(HANDLE handle)
{
    FILE_ATTRIBUTE_TAG_INFO info = {};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) &&
           (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::wstring finalPath(HANDLE handle)
{
    DWORD length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (length == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
    const DWORD actual = GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED);
    if (actual == 0 || actual >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), actual);
}

bool startsWithInsensitive(const std::wstring &value, const wchar_t *prefix)
{
    const std::wstring_view prefixView(prefix);
    if (value.size() < prefixView.size()) {
        return false;
    }
    return CompareStringOrdinal(
               value.data(), static_cast<int>(prefixView.size()),
               prefixView.data(), static_cast<int>(prefixView.size()), TRUE) == CSTR_EQUAL;
}

std::wstring normalizeFinalPath(std::wstring path)
{
    if (startsWithInsensitive(path, L"\\\\?\\UNC\\")) {
        path.erase(0, 8);
        path.insert(0, L"\\\\");
    }
    else if (startsWithInsensitive(path, L"\\\\?\\")) {
        path.erase(0, 4);
    }

    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
        // Keep a drive root such as C:\\ intact.
        if (path.size() == 3 && path[1] == L':') {
            break;
        }
        path.pop_back();
    }
    return path;
}

bool isDirectChildPath(
    const std::wstring &directoryPath,
    const std::wstring &candidatePath,
    const std::wstring &profileName)
{
    const std::wstring directory = normalizeFinalPath(directoryPath);
    const std::wstring candidate = normalizeFinalPath(candidatePath);
    if (directory.empty() || candidate.size() <= directory.size()) {
        return false;
    }
    if (CompareStringOrdinal(
            candidate.data(), static_cast<int>(directory.size()),
            directory.data(), static_cast<int>(directory.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    const wchar_t separator = candidate[directory.size()];
    if (separator != L'\\' && separator != L'/') {
        return false;
    }
    const std::wstring suffix = candidate.substr(directory.size() + 1);
    return CompareStringOrdinal(
               suffix.data(), static_cast<int>(suffix.size()),
               profileName.data(), static_cast<int>(profileName.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::wstring> colorDirectory(DynamicColorProfileApi &api)
{
    if (!api.getColorDirectory) {
        wchar_t windowsDirectory[MAX_PATH] = {};
        const UINT length = GetSystemWindowsDirectoryW(
            windowsDirectory,
            ARRAYSIZE(windowsDirectory)
        );
        if (length == 0 || length >= ARRAYSIZE(windowsDirectory)) {
            return std::nullopt;
        }
        return std::wstring(windowsDirectory, length) +
               L"\\System32\\spool\\drivers\\color";
    }

    DWORD size = MAX_PATH;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<wchar_t> buffer(size + 1, L'\0');
        DWORD requested = size;
        if (api.getColorDirectory(nullptr, buffer.data(), &requested)) {
            return std::wstring(buffer.data());
        }
        if (requested <= size) {
            return std::nullopt;
        }
        size = requested + 1;
    }
    return std::nullopt;
}

std::optional<FILE_ID_128> findDirectChildId(HANDLE directory, const std::wstring &profileName)
{
    std::vector<std::byte> buffer(64 * 1024);
    if (!GetFileInformationByHandleEx(
            directory,
            FileIdExtdDirectoryRestartInfo,
            buffer.data(),
            static_cast<DWORD>(buffer.size()))) {
        return std::nullopt;
    }

    while (true) {
        auto *entry = reinterpret_cast<FILE_ID_EXTD_DIR_INFO *>(buffer.data());
        while (true) {
            const std::wstring name(
                entry->FileName,
                entry->FileNameLength / sizeof(wchar_t));
            if (CompareStringOrdinal(
                    name.data(), static_cast<int>(name.size()),
                    profileName.data(), static_cast<int>(profileName.size()), TRUE) == CSTR_EQUAL) {
                return entry->FileId;
            }
            if (entry->NextEntryOffset == 0) {
                break;
            }
            entry = reinterpret_cast<FILE_ID_EXTD_DIR_INFO *>(
                reinterpret_cast<std::byte *>(entry) + entry->NextEntryOffset);
        }

        // The directory API is stateful: subsequent calls continue from the
        // retained directory handle.
        if (!GetFileInformationByHandleEx(
                directory,
                FileIdExtdDirectoryInfo,
                buffer.data(),
                static_cast<DWORD>(buffer.size()))) {
            break;
        }
    }
    return std::nullopt;
}

bool sameFileId(const FILE_ID_128 &left, const FILE_ID_128 &right)
{
    return std::memcmp(&left, &right, sizeof(FILE_ID_128)) == 0;
}

struct ProfileReadResult
{
    bool completed = false;
    QByteArray bytes;
};

struct AsyncProfileReadState
{
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped = {};
    QByteArray bytes;

    ~AsyncProfileReadState()
    {
        if (event != nullptr) {
            CloseHandle(event);
        }
        if (file != INVALID_HANDLE_VALUE && file != nullptr) {
            CloseHandle(file);
        }
    }
};

void detachProfileReadCleanup(const std::shared_ptr<AsyncProfileReadState> &state)
{
    try {
        std::thread([state] {
            WaitForSingleObject(state->event, INFINITE);
            DWORD bytesRead = 0;
            (void) GetOverlappedResult(state->file, &state->overlapped, &bytesRead, FALSE);
        }).detach();
    }
    catch (...) {
        // A cleanup thread is normally available. If the process cannot
        // create one, keep the operation's OVERLAPPED and buffer alive until
        // cancellation completes rather than returning dangling I/O state.
        WaitForSingleObject(state->event, INFINITE);
    }
}

ProfileReadResult readProfileBytes(HANDLE file, LARGE_INTEGER fileSize, DWORD waitBudgetMs)
{
    ProfileReadResult result;
    if (fileSize.QuadPart < 0 || fileSize.QuadPart > kMaxMhc2ProfileBytes) {
        return result;
    }

    auto state = std::make_shared<AsyncProfileReadState>();
    state->bytes.resize(static_cast<qsizetype>(fileSize.QuadPart));
    if (!DuplicateHandle(
            GetCurrentProcess(),
            file,
            GetCurrentProcess(),
            &state->file,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS)) {
        state->bytes.clear();
        return result;
    }
    state->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state->event == nullptr) {
        result.bytes.clear();
        return result;
    }

    state->overlapped.hEvent = state->event;
    DWORD bytesRead = 0;
    const BOOL started = ReadFile(
        state->file,
        state->bytes.data(),
        static_cast<DWORD>(state->bytes.size()),
        &bytesRead,
        &state->overlapped);
    if (!started && GetLastError() != ERROR_IO_PENDING) {
        return result;
    }

    if (started) {
        result.completed = bytesRead == static_cast<DWORD>(state->bytes.size());
    }
    else {
        const DWORD waitResult = WaitForSingleObject(
            state->event,
            waitBudgetMs
        );
        if (waitResult == WAIT_TIMEOUT) {
            CancelIoEx(state->file, &state->overlapped);
            detachProfileReadCleanup(state);
            return result;
        }
        if (waitResult != WAIT_OBJECT_0 ||
            !GetOverlappedResult(state->file, &state->overlapped, &bytesRead, FALSE)) {
            if (waitResult != WAIT_OBJECT_0) {
                CancelIoEx(state->file, &state->overlapped);
                detachProfileReadCleanup(state);
            }
            return result;
        }
        result.completed = bytesRead == static_cast<DWORD>(state->bytes.size());
    }

    if (result.completed) {
        result.bytes = state->bytes;
    }
    return result;
}

ProfileProbe readProfileFile(
    DynamicColorProfileApi &api,
    const QString &profileName,
    QElapsedTimer &timer,
    qint64 budgetMs)
{
    ProfileProbe result;
    result.profileType = ProfileType::Icc;
    result.displayColorMode = DisplayColorMode::Extended;
    result.profileName = profileName;

    if (!isSafeProfileBasename(result.profileName) || timer.elapsed() >= budgetMs) {
        return result;
    }

    const auto directoryPath = colorDirectory(api);
    if (!directoryPath.has_value()) {
        return result;
    }
    const std::wstring profileNameWide = result.profileName.toStdWString();
    const std::wstring directoryWide = *directoryPath;
    const std::wstring candidatePath = directoryWide + L"\\" + profileNameWide;

    HandleGuard directory;
    directory.handle = CreateFileW(
        directoryWide.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (!directory || isReparsePoint(directory.handle)) {
        return result;
    }

    FileIdentity directoryIdentity;
    if (!getFileIdentity(directory.handle, directoryIdentity)) {
        return result;
    }
    const std::wstring directoryFinalPath = finalPath(directory.handle);
    if (directoryFinalPath.empty()) {
        return result;
    }

    const auto childId = findDirectChildId(directory.handle, profileNameWide);
    if (!childId.has_value()) {
        return result;
    }

    HandleGuard file;
    file.handle = CreateFileW(
        candidatePath.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (!file || isReparsePoint(file.handle)) {
        return result;
    }

    FileIdentity candidateIdentity;
    if (!getFileIdentity(file.handle, candidateIdentity) ||
        candidateIdentity.volumeSerial != directoryIdentity.volumeSerial ||
        !sameFileId(candidateIdentity.fileId, *childId) ||
        !isDirectChildPath(directoryFinalPath, finalPath(file.handle), profileNameWide)) {
        return result;
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file.handle, &fileSize)) {
        return result;
    }

    const qint64 remainingBudgetMs = budgetMs - timer.elapsed();
    if (remainingBudgetMs <= 0) {
        return result;
    }
    const ProfileReadResult read = readProfileBytes(
        file.handle,
        fileSize,
        static_cast<DWORD>(remainingBudgetMs));
    result.elapsedMs = timer.elapsed();
    result.readCompleted = read.completed && result.elapsedMs <= budgetMs;
    if (result.readCompleted) {
        result.bytes = read.bytes;
    }
    return result;
}

bool isNoExtendedDefaultResult(HRESULT result)
{
    return result == S_OK || result == S_FALSE ||
           result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

QStringList readAdvancedColorProfileNames(
    const DisplayMapping &mapping,
    WCS_PROFILE_MANAGEMENT_SCOPE scope)
{
    const QString sourceKey = QString::number(mapping.sourceId).rightJustified(
        4, QChar('0'));
    const QString keyPath = scope == WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER
        ? QStringLiteral(
              "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion\\"
              "ICM\\ProfileAssociations\\Display\\{4d36e96e-e325-11ce-bfc1-08002be10318}\\")
        : QStringLiteral(
              "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Class\\"
              "{4d36e96e-e325-11ce-bfc1-08002be10318}\\");
    const QSettings settings(keyPath + sourceKey, QSettings::NativeFormat);
    const QVariant value = settings.value(QStringLiteral("ICMProfileAC"));

    QStringList names = value.toStringList();
    if (names.isEmpty()) {
        const QString scalar = value.toString();
        if (!scalar.isEmpty()) {
            names.push_back(scalar);
        }
    }
    names.removeAll(QString());
    return names;
}

ProfileProbe readColorProfile(const DisplayMapping &mapping, qint64 budgetMs)
{
    ProfileProbe result;
    result.lookupStatus = ProfileLookupStatus::Failed;
    DynamicColorProfileApi api;
    result.exportsAvailable = api.load();
    result.osSupported = result.exportsAvailable;
    if (!result.exportsAvailable) {
        return result;
    }

    const LUID targetLuid = unpackLuid(mapping.targetAdapterLuid);
    WCS_PROFILE_MANAGEMENT_SCOPE scope = WCS_PROFILE_MANAGEMENT_SCOPE_SYSTEM_WIDE;
    if (FAILED(api.getDisplayUserScope(targetLuid, mapping.sourceId, &scope))) {
        return result;
    }
    result.scopeSupported = true;

    LPWSTR profileName = nullptr;
    QElapsedTimer timer;
    timer.start();
    const HRESULT profileResult = api.getDisplayDefault(
        scope,
        targetLuid,
        mapping.sourceId,
        CPT_ICC,
        CPST_EXTENDED_DISPLAY_COLOR_MODE,
        &profileName);
    const QString profileNameValue = profileName
        ? QString::fromWCharArray(profileName)
        : QString();
    if (profileName) {
        LocalFree(profileName);
    }

    if (profileNameValue.isEmpty()) {
        if (isNoExtendedDefaultResult(profileResult)) {
            result.lookupStatus = ProfileLookupStatus::NoExtendedDefault;
        }
        return result;
    }
    if (FAILED(profileResult)) {
        return result;
    }

    result = readProfileFile(api, profileNameValue, timer, budgetMs);
    result.exportsAvailable = true;
    result.osSupported = true;
    result.scopeSupported = true;
    result.lookupStatus = ProfileLookupStatus::ExtendedDefaultAvailable;
    return result;
}

std::vector<ProfileProbe> readAssociatedColorProfiles(
    const DisplayMapping &mapping,
    qint64 budgetMs)
{
    constexpr DWORD kMaxAssociatedProfiles = 8;

    std::vector<ProfileProbe> result;
    QElapsedTimer timer;
    timer.start();
    if (budgetMs <= 0) {
        return result;
    }

    DynamicColorProfileApi api;
    const bool exportsAvailable = api.load();
    if (!exportsAvailable || !api.getDisplayList) {
        return result;
    }

    const LUID targetLuid = unpackLuid(mapping.targetAdapterLuid);
    WCS_PROFILE_MANAGEMENT_SCOPE scope = WCS_PROFILE_MANAGEMENT_SCOPE_SYSTEM_WIDE;
    if (FAILED(api.getDisplayUserScope(targetLuid, mapping.sourceId, &scope))) {
        return result;
    }

    QStringList activeNames = readAdvancedColorProfileNames(mapping, scope);
    QStringList uniqueActiveNames;
    for (const QString &activeName : activeNames) {
        if (!activeName.isEmpty() && !uniqueActiveNames.contains(activeName, Qt::CaseInsensitive)) {
            uniqueActiveNames.push_back(activeName);
        }
    }
    if (uniqueActiveNames.isEmpty() || uniqueActiveNames.size() > kMaxAssociatedProfiles) {
        return result;
    }

    LPWSTR *profileList = nullptr;
    DWORD profileCount = 0;
    const HRESULT listResult = api.getDisplayList(
            scope,
            targetLuid,
            mapping.sourceId,
            &profileList,
            &profileCount);
    if (FAILED(listResult)) {
        if (profileList) {
            LocalFree(profileList);
        }
        return result;
    }

    if (profileList == nullptr || profileCount == 0 ||
        profileCount > kMaxAssociatedProfiles) {
        if (profileList) {
            LocalFree(profileList);
        }
        return result;
    }

    if (timer.elapsed() >= budgetMs) {
        LocalFree(profileList);
        return result;
    }

    QStringList matchedNames;
    bool complete = true;
    for (DWORD index = 0; index < profileCount; ++index) {
        if (profileList[index] == nullptr || timer.elapsed() >= budgetMs) {
            complete = false;
            break;
        }

        const QString profileName = QString::fromWCharArray(profileList[index]);
        if (!uniqueActiveNames.contains(profileName, Qt::CaseInsensitive) ||
            matchedNames.contains(profileName, Qt::CaseInsensitive)) {
            continue;
        }
        matchedNames.push_back(profileName);

        ProfileProbe profile = readProfileFile(api, profileName, timer, budgetMs);
        profile.exportsAvailable = true;
        profile.osSupported = true;
        profile.scopeSupported = true;
        profile.displayColorMode = DisplayColorMode::Extended;

        if (!profile.readCompleted || profile.elapsedMs < 0 ||
            profile.elapsedMs > budgetMs || !isSafeProfileBasename(profile.profileName)) {
            continue;
        }
        const auto peak = parseMhc2PeakNits(profile.bytes);
        if (!peak.has_value()) {
            continue;
        }

        profile.parsedMhc2PeakNits = *peak;
        profile.bytes.clear();
        if (!result.empty()) {
            // More than one active Advanced Color profile is ambiguous.
            result.clear();
            complete = false;
            break;
        }
        result.push_back(std::move(profile));
    }

    LocalFree(profileList);
    if (!complete || matchedNames.size() != uniqueActiveNames.size()) {
        result.clear();
    }
    return result;
}

DxgiOutputProbe readDxgiOutput(const DisplayMapping &mapping)
{
    DxgiOutputProbe result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return result;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(mapping.dxgiAdapterIndex, &adapter) != S_OK || !adapter) {
        return result;
    }
    ComPtr<IDXGIOutput> output;
    if (adapter->EnumOutputs(mapping.dxgiOutputIndex, &output) != S_OK || !output) {
        return result;
    }

    DXGI_OUTPUT_DESC outputDesc = {};
    if (FAILED(output->GetDesc(&outputDesc))) {
        return result;
    }

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(output.As(&output6)) || !output6) {
        return result;
    }
    DXGI_OUTPUT_DESC1 descriptor = {};
    if (FAILED(output6->GetDesc1(&descriptor))) {
        return result;
    }

    result.valid = true;
    result.current = factory->IsCurrent() != FALSE;
    result.unique = true;
    result.attachedToDesktop = outputDesc.AttachedToDesktop != FALSE;
    result.deviceName = QString::fromWCharArray(outputDesc.DeviceName);
    result.desktopBounds = QRect(
        outputDesc.DesktopCoordinates.left,
        outputDesc.DesktopCoordinates.top,
        outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left,
        outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);
    result.adapterIndex = mapping.dxgiAdapterIndex;
    result.outputIndex = mapping.dxgiOutputIndex;
    result.colorSpace = descriptor.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        ? DxgiColorSpace::RgbFullG2084NoneP2020
        : DxgiColorSpace::Other;
    result.maxLuminance = descriptor.MaxLuminance;
    return result;
}

SelectedDisplay resolveSelectedDisplay(const QScreen *screen, quintptr hiddenWindow)
{
    SelectedDisplay result;
    if (!screen || hiddenWindow == 0) {
        return result;
    }

    const QRect screenGeometry = screen->geometry();
    const int hiddenDisplayIndex = SDL_GetWindowDisplayIndex(
        reinterpret_cast<SDL_Window *>(hiddenWindow));
    std::vector<QRect> displayBounds;
    for (int index = 0; index < SDL_GetNumVideoDisplays(); ++index) {
        SDL_Rect bounds = {};
        if (SDL_GetDisplayBounds(index, &bounds) != 0) {
            displayBounds.emplace_back();
            continue;
        }
        const QRect displayGeometry(bounds.x, bounds.y, bounds.w, bounds.h);
        displayBounds.push_back(displayGeometry);
    }

    const auto resolvedDisplayIndex = resolveDisplayIndex(
        screenGeometry,
        hiddenDisplayIndex,
        displayBounds);
    if (!resolvedDisplayIndex.has_value()) {
        return result;
    }
    const int displayIndex = *resolvedDisplayIndex;
    const QRect displayGeometry = displayBounds[displayIndex];
    result.resolved = true;
    result.unique = true;
    result.displayIndex = displayIndex;
    result.geometry = displayGeometry;
    int hiddenWindowX = 0;
    int hiddenWindowY = 0;
    SDL_GetWindowPosition(
        reinterpret_cast<SDL_Window *>(hiddenWindow),
        &hiddenWindowX,
        &hiddenWindowY);
    result.hiddenWindowMatches = hiddenDisplayIndex == displayIndex ||
        (hiddenDisplayIndex < 0 && displayGeometry.contains(QPoint(hiddenWindowX, hiddenWindowY)));

    RECT monitorRect = {
        displayGeometry.left(),
        displayGeometry.top(),
        displayGeometry.right() + 1,
        displayGeometry.bottom() + 1,
    };
    const HMONITOR monitor = MonitorFromRect(&monitorRect, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        result.resolved = false;
        return result;
    }
    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        result.resolved = false;
        return result;
    }
    result.gdiDeviceName = QString::fromWCharArray(monitorInfo.szDevice);
    return result;
}

#endif

}

std::optional<int> resolveDisplayIndex(
    const QRect &screenGeometry,
    int hiddenDisplayIndex,
    const std::vector<QRect> &displayBounds)
{
    if (hiddenDisplayIndex >= 0 &&
        hiddenDisplayIndex < static_cast<int>(displayBounds.size()) &&
        displayBounds[hiddenDisplayIndex].width() > 0 &&
        displayBounds[hiddenDisplayIndex].height() > 0) {
        return hiddenDisplayIndex;
    }

    std::optional<int> topLeftMatch;
    for (int index = 0; index < static_cast<int>(displayBounds.size()); ++index) {
        if (displayBounds[index].width() <= 0 || displayBounds[index].height() <= 0 ||
            displayBounds[index].topLeft() != screenGeometry.topLeft()) {
            continue;
        }
        if (topLeftMatch.has_value()) {
            return std::nullopt;
        }
        topLeftMatch = index;
    }
    return topLeftMatch;
}

std::optional<int> parseMhc2PeakNits(const QByteArray &profileBytes)
{
    if (profileBytes.size() < 132 || profileBytes.size() > kMaxMhc2ProfileBytes) {
        return std::nullopt;
    }

    const quint32 tagCount = readBe32(profileBytes, 128);
    const qsizetype tableBytes = profileBytes.size() - 132;
    if (tagCount > static_cast<quint32>(tableBytes / 12)) {
        return std::nullopt;
    }

    for (quint32 index = 0; index < tagCount; ++index) {
        const qsizetype entry = 132 + static_cast<qsizetype>(index) * 12;
        if (profileBytes[entry] != 'M' || profileBytes[entry + 1] != 'H' ||
            profileBytes[entry + 2] != 'C' || profileBytes[entry + 3] != '2') {
            continue;
        }

        const quint32 offset = readBe32(profileBytes, entry + 4);
        const quint32 size = readBe32(profileBytes, entry + 8);
        if (size < 20 || offset > static_cast<quint32>(profileBytes.size()) ||
            size > static_cast<quint32>(profileBytes.size()) - offset) {
            return std::nullopt;
        }
        const qsizetype tagOffset = static_cast<qsizetype>(offset);
        if (profileBytes[tagOffset] != 'M' || profileBytes[tagOffset + 1] != 'H' ||
            profileBytes[tagOffset + 2] != 'C' || profileBytes[tagOffset + 3] != '2') {
            return std::nullopt;
        }

        const qint32 fixedPoint = static_cast<qint32>(readBe32(profileBytes, tagOffset + 16));
        const double peakNits = static_cast<double>(fixedPoint) / 65536.0;
        return normalizePeakNits(peakNits);
    }
    return std::nullopt;
}

bool isSafeProfileBasename(const QString &profileName)
{
    if (profileName.isEmpty() || profileName == QStringLiteral(".") ||
        profileName == QStringLiteral("..") || profileName.endsWith(QChar('.')) ||
        profileName.endsWith(QChar(' '))) {
        return false;
    }

    if (profileName.contains(QChar('\0')) || profileName.contains(QChar('/')) ||
        profileName.contains(QChar('\\')) || profileName.contains(QChar(':'))) {
        return false;
    }

    if (profileName.size() >= 2 && profileName.at(1) == QChar(':')) {
        return false;
    }
    if (profileName.startsWith(QStringLiteral("\\\\")) ||
        profileName.startsWith(QStringLiteral("//")) ||
        profileName.startsWith(QStringLiteral("\\\\?\\")) ||
        profileName.startsWith(QStringLiteral("\\\\.\\"))) {
        return false;
    }
    return true;
}

bool isUsableProfileProbe(const ProfileProbe &profile, qint64 measuredElapsedMs)
{
    return profile.exportsAvailable && profile.osSupported && profile.scopeSupported &&
           profile.profileType == ProfileType::Icc &&
           profile.displayColorMode == DisplayColorMode::Extended &&
           profile.readCompleted && profile.elapsedMs >= 0 &&
           profile.elapsedMs <= kProfileReadBudgetMs && measuredElapsedMs >= 0 &&
           measuredElapsedMs <= kProfileReadBudgetMs &&
           isSafeProfileBasename(profile.profileName);
}

std::optional<int> profilePeakNits(const ProfileProbe &profile)
{
    if (profile.parsedMhc2PeakNits.has_value()) {
        return profile.parsedMhc2PeakNits;
    }
    return parseMhc2PeakNits(profile.bytes);
}

std::optional<ClientDisplayPreparation> collectClientDisplayPreparation(
    const SelectedDisplay &selectedDisplay,
    const CollectorProvider &provider)
{
    if (!selectedDisplayIsUsable(selectedDisplay) || !provider.mapDisplayConfig ||
        !provider.readColorProfile || !provider.readDxgiOutput || !provider.revalidate) {
        return std::nullopt;
    }

    const DisplayMapping mapping = provider.mapDisplayConfig(selectedDisplay);
    if (!mappingIsUsable(selectedDisplay, mapping)) {
        return std::nullopt;
    }

    QElapsedTimer profileTimer;
    profileTimer.start();
    const ProfileProbe profile = provider.readColorProfile(mapping, kProfileReadBudgetMs);
    const qint64 measuredProfileMs = profileTimer.elapsed();
    const DxgiOutputProbe output = provider.readDxgiOutput(mapping);

    ClientDisplayCapabilities capabilities;
    bool calibratedProfileFound = false;
    if (isUsableProfileProbe(profile, measuredProfileMs)) {
        if (const auto peak = profilePeakNits(profile)) {
            setSource(capabilities.calibrated, "windows-icc-mhc2", *peak);
            calibratedProfileFound = true;
        }
    }

    if (!calibratedProfileFound &&
        profile.lookupStatus == ProfileLookupStatus::NoExtendedDefault &&
        provider.readAssociatedColorProfiles) {
        const qint64 remainingBudgetMs = kProfileReadBudgetMs - profileTimer.elapsed();
        if (remainingBudgetMs > 0) {
            const std::vector<ProfileProbe> associatedProfiles =
                provider.readAssociatedColorProfiles(mapping, remainingBudgetMs);
            const qint64 measuredAssociatedProfileMs = profileTimer.elapsed();

            std::vector<QString> validProfileNames;
            std::optional<int> associatedPeakNits;
            for (const ProfileProbe &associated : associatedProfiles) {
                if (!isUsableProfileProbe(associated, measuredAssociatedProfileMs)) {
                    continue;
                }
                const auto peak = profilePeakNits(associated);
                if (!peak.has_value()) {
                    continue;
                }

                bool duplicate = false;
                for (const QString &validProfileName : validProfileNames) {
                    if (sameName(validProfileName, associated.profileName)) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    continue;
                }

                validProfileNames.push_back(associated.profileName);
                associatedPeakNits = *peak;
            }

            // Advanced Color can make the default-profile API return no
            // profile. Only inherit an active associated profile when there
            // is exactly one valid MHC2 candidate; otherwise leave the source
            // unset so DXGI remains the next deterministic fallback.
            if (validProfileNames.size() == 1 && associatedPeakNits.has_value()) {
                setSource(capabilities.calibrated, "windows-icc-mhc2", *associatedPeakNits);
            }
        }
    }

    if (outputIsUsable(mapping, output)) {
        if (const auto peak = normalizePeakNits(output.maxLuminance)) {
            setSource(capabilities.edid, "dxgi-output", *peak);
        }
    }

    if (!provider.revalidate(mapping, output)) {
        return std::nullopt;
    }
    ClientDisplayPreparation preparation;
    preparation.selectedDisplay = selectedDisplay;
    preparation.mapping = mapping;
    preparation.dxgiOutput = output;
    preparation.capabilities = capabilities;
    return preparation;
}

std::optional<CollectedCapabilities> collectCapabilities(
    const SelectedDisplay &selectedDisplay,
    const CollectorProvider &provider)
{
    const auto preparation = collectClientDisplayPreparation(selectedDisplay, provider);
    if (!preparation.has_value()) {
        return std::nullopt;
    }

    CollectedCapabilities result;
    if (preparation->capabilities.calibrated.has_value()) {
        result.calibratedPeakNits = ClientDisplayCapabilities::normalizePeakLuminance(
            preparation->capabilities.calibrated->peakLuminanceNits);
    }
    if (preparation->capabilities.edid.has_value()) {
        result.edidPeakNits = ClientDisplayCapabilities::normalizePeakLuminance(
            preparation->capabilities.edid->peakLuminanceNits);
    }
    return result;
}

std::optional<CollectedCapabilities> collectCapabilities(
    const QScreen *selectedScreen,
    quintptr hiddenWindow,
    const CollectorProvider &provider)
{
    const auto preparation = collectClientDisplayPreparation(
        selectedScreen, hiddenWindow, provider);
    if (!preparation.has_value()) {
        return std::nullopt;
    }

    CollectedCapabilities result;
    if (preparation->capabilities.calibrated.has_value()) {
        result.calibratedPeakNits = ClientDisplayCapabilities::normalizePeakLuminance(
            preparation->capabilities.calibrated->peakLuminanceNits);
    }
    if (preparation->capabilities.edid.has_value()) {
        result.edidPeakNits = ClientDisplayCapabilities::normalizePeakLuminance(
            preparation->capabilities.edid->peakLuminanceNits);
    }
    return result;
}

std::optional<ClientDisplayPreparation> collectClientDisplayPreparation(
    const QScreen *selectedScreen,
    quintptr hiddenWindow,
    const CollectorProvider &provider)
{
    if (!selectedScreen || !provider.resolveSelectedDisplay) {
        return std::nullopt;
    }
    const SelectedDisplay selected = provider.resolveSelectedDisplay(selectedScreen, hiddenWindow);
    const auto preparation = collectClientDisplayPreparation(selected, provider);
    if (!preparation.has_value()) {
        return std::nullopt;
    }
    if (provider.revalidateSelected && !provider.revalidateSelected(
            selectedScreen,
            hiddenWindow,
            preparation->selectedDisplay,
            preparation->mapping,
            preparation->dxgiOutput)) {
        return std::nullopt;
    }
    return preparation;
}

std::optional<ClientDisplayCapabilities> collectClientDisplayCapabilities(
    const QScreen *selectedScreen,
    quintptr hiddenWindow,
    const CollectorProvider &provider)
{
    const auto preparation = collectClientDisplayPreparation(
        selectedScreen, hiddenWindow, provider);
    if (!preparation.has_value()) {
        return std::nullopt;
    }
    return preparation->capabilities;
}

std::optional<ClientDisplayCapabilities> collectClientDisplayCapabilities(
    const QScreen *selectedScreen,
    quintptr hiddenWindow)
{
    return collectClientDisplayCapabilities(
        selectedScreen,
        hiddenWindow,
        makeWindowsCollectorProvider());
}

#if defined(_WIN32)

CollectorProvider makeWindowsCollectorProvider()
{
    CollectorProvider provider;
    provider.resolveSelectedDisplay = resolveSelectedDisplay;
    provider.mapDisplayConfig = [](const SelectedDisplay &selected) {
        const auto mapping = mapDisplayConfig(selected);
        return mapping.value_or(DisplayMapping {});
    };
    provider.readColorProfile = readColorProfile;
    provider.readAssociatedColorProfiles = readAssociatedColorProfiles;
    provider.readDxgiOutput = readDxgiOutput;
    provider.revalidate = [](const DisplayMapping &mapping, const DxgiOutputProbe &captured) {
        const SelectedDisplay selected {
            true,
            true,
            true,
            mapping.displayIndex,
            mapping.gdiDeviceName,
            mapping.desktopBounds,
        };
        const auto current = mapDisplayConfig(selected);
        if (!current.has_value() ||
            current->displayIndex != mapping.displayIndex ||
            current->gdiDeviceName.compare(mapping.gdiDeviceName, Qt::CaseInsensitive) != 0 ||
            current->outputDeviceName.compare(mapping.outputDeviceName, Qt::CaseInsensitive) != 0 ||
            current->desktopBounds != mapping.desktopBounds ||
            current->sourceAdapterLuid != mapping.sourceAdapterLuid ||
            current->sourceId != mapping.sourceId ||
            current->targetAdapterLuid != mapping.targetAdapterLuid ||
            current->targetId != mapping.targetId ||
            current->pathFlags != mapping.pathFlags ||
            current->targetAvailable != mapping.targetAvailable ||
            current->dxgiAdapterIndex != mapping.dxgiAdapterIndex ||
            current->dxgiOutputIndex != mapping.dxgiOutputIndex) {
            return false;
        }

        const DxgiOutputProbe currentOutput = readDxgiOutput(*current);
        return currentOutput.valid == captured.valid &&
               currentOutput.current == captured.current &&
               currentOutput.unique == captured.unique &&
               currentOutput.attachedToDesktop == captured.attachedToDesktop &&
               currentOutput.deviceName.compare(captured.deviceName, Qt::CaseInsensitive) == 0 &&
               currentOutput.desktopBounds == captured.desktopBounds &&
               currentOutput.adapterIndex == captured.adapterIndex &&
               currentOutput.outputIndex == captured.outputIndex &&
               currentOutput.colorSpace == captured.colorSpace &&
               currentOutput.maxLuminance == captured.maxLuminance;
    };
    provider.revalidateSelected = [](const QScreen *screen,
                                     quintptr hiddenWindow,
                                     const SelectedDisplay &captured,
                                     const DisplayMapping &,
                                     const DxgiOutputProbe &) {
        const SelectedDisplay current = resolveSelectedDisplay(screen, hiddenWindow);
        return current.resolved == captured.resolved &&
               current.unique == captured.unique &&
               current.hiddenWindowMatches == captured.hiddenWindowMatches &&
               current.displayIndex == captured.displayIndex &&
               current.gdiDeviceName.compare(captured.gdiDeviceName, Qt::CaseInsensitive) == 0 &&
               current.geometry == captured.geometry;
    };
    return provider;
}

#endif

}
