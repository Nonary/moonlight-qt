// SPDX-License-Identifier: GPL-3.0-or-later
#include "steamcontrollersdl.h"

#if HAVE_STEAM_CONTROLLER_DRIVER

#include <SDL_hidapi.h>

#include <chrono>

namespace {

void sdlLog(int level, const char* message)
{
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, level >= SteamController::LogWarn ? SDL_LOG_PRIORITY_WARN : SDL_LOG_PRIORITY_INFO,
                   "%s", message);
}

class SdlDevice final : public SteamHidDevice {
public:
    explicit SdlDevice(SDL_hid_device* device) : m_Device(device) {}
    ~SdlDevice() override { SDL_hid_close(m_Device); }
    int read(uint8_t* buffer, int length, int timeoutMs) override { return SDL_hid_read_timeout(m_Device, buffer, length, timeoutMs); }
    int write(const uint8_t* data, int length) override { return SDL_hid_write(m_Device, data, length); }
    int sendFeatureReport(const uint8_t* data, int length) override { return SDL_hid_send_feature_report(m_Device, data, length); }
    int getFeatureReport(uint8_t* buffer, int length) override { return SDL_hid_get_feature_report(m_Device, buffer, length); }
private:
    SDL_hid_device* m_Device;
};

}

class SteamControllerRunner::SdlTransport final : public SteamHidTransport {
public:
    std::vector<SteamHidDeviceInfo> enumerate() override
    {
        std::vector<SteamHidDeviceInfo> out;
        SDL_hid_device_info* list = SDL_hid_enumerate(SteamController::VENDOR_VALVE, 0);
        for (SDL_hid_device_info* d = list; d != nullptr; d = d->next) {
            if (!d->path) continue;
            SteamHidDeviceInfo info;
            info.path = d->path;
            info.vendorId = d->vendor_id;
            info.productId = d->product_id;
            info.interfaceNumber = d->interface_number;
            info.usagePage = d->usage_page;
            info.usage = d->usage;
#if SDL_VERSION_ATLEAST(3, 0, 0)
            // SDL3's hidapi names the bus; SDL2 (and sdl2-compat) does not, and the product id
            // (0x1303) identifies the Bluetooth LE controller either way.
            info.bus = d->bus_type == SDL_HID_API_BUS_BLUETOOTH ? SteamHidDeviceInfo::BusBluetooth :
                       d->bus_type == SDL_HID_API_BUS_USB ? SteamHidDeviceInfo::BusUsb : SteamHidDeviceInfo::BusUnknown;
#endif
            out.push_back(info);
        }
        SDL_hid_free_enumeration(list);
        return out;
    }

    std::unique_ptr<SteamHidDevice> open(const SteamHidDeviceInfo& info) override
    {
        SDL_hid_device* device = SDL_hid_open_path(info.path.c_str(), 0);
        if (!device) return nullptr;
        return std::unique_ptr<SteamHidDevice>(new SdlDevice(device));
    }
};

uint64_t SteamControllerRunner::now()
{
    return SteamController::nowMs();
}

SteamControllerRunner::SteamControllerRunner(const SteamControllerConfig& config, int rumbleHoldMs, SteamControllerLink::Listener* listener)
{
    SteamController::setLogFunction(sdlLog);
    if (SDL_hid_init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Steam Controller: SDL_hid_init() failed: %s; the driver is off for this session", SDL_GetError());
        return;
    }
    m_HidInitialized = true;
    m_Transport.reset(new SdlTransport());
    m_Manager.reset(new SteamControllerManager(*m_Transport, config, rumbleHoldMs, listener));
    m_Manager->onLinkOpened = [this](const std::shared_ptr<SteamControllerLink>& link) {
        link->wake = [this] { wake(); };
        m_Readers[link->id()] = std::thread([this, link] { readLoop(link); });
    };
    m_Manager->onLinkClosing = [this](const std::shared_ptr<SteamControllerLink>& link) {
        link->requestClose();
        auto it = m_Readers.find(link->id());
        if (it != m_Readers.end()) {
            if (it->second.joinable()) it->second.join();
            m_Readers.erase(it);
        }
    };
    m_ServiceThread = std::thread([this] { serviceLoop(); });
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Steam Controller (2026) driver started: motion %s, split pads %s, grips %d, rumble hold %d ms, stick stretch %s, "
                "Bluetooth LE haptics: boost +%d dB, pulse on-time %d%%, master gain %d dB",
                config.motion ? "on" : "off", config.splitPads ? "on" : "off", config.grips, rumbleHoldMs,
                config.stickStretch ? "on" : "off", config.hapticBoostDb, config.hapticPulseScalePercent, config.hapticMasterGainDb);
}

SteamControllerRunner::~SteamControllerRunner()
{
    if (!m_HidInitialized) return;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stopping = true;
    }
    m_Cv.notify_all();
    if (m_ServiceThread.joinable()) m_ServiceThread.join();
    // Stop sequence per connected slot (motors off, lizard mode back on), then close.
    m_Manager->stop(now());
    for (auto& reader : m_Readers) {
        if (reader.second.joinable()) reader.second.join();
    }
    m_Readers.clear();
    m_Manager.reset();
    m_Transport.reset();
    SDL_hid_exit();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Steam Controller (2026) driver stopped");
}

void SteamControllerRunner::requestRescan()
{
    if (!m_Manager) return;
    m_Manager->requestRescan();
    wake();
}

void SteamControllerRunner::wake()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Wake = true;
    }
    m_Cv.notify_all();
}

void SteamControllerRunner::serviceLoop()
{
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_Stopping) return;
            m_Wake = false;
        }
        uint64_t next = m_Manager->service(now());
        std::unique_lock<std::mutex> lock(m_Mutex);
        if (m_Stopping) return;
        if (m_Wake) continue;
        if (next == UINT64_MAX) {
            m_Cv.wait(lock, [this] { return m_Stopping || m_Wake; });
        }
        else {
            uint64_t current = now();
            if (next > current) {
                m_Cv.wait_for(lock, std::chrono::milliseconds(next - current), [this] { return m_Stopping || m_Wake; });
            }
        }
    }
}

void SteamControllerRunner::readLoop(std::shared_ptr<SteamControllerLink> link)
{
    uint8_t report[SteamController::FEATURE_REPORT_SIZE + 1];
    int consecutiveErrors = 0;
    while (!link->closing() && !link->stopped()) {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_Stopping) return;
        }
        const uint64_t before = now();
        int size = link->readReport(report, sizeof(report), SteamControllerLink::READ_TIMEOUT_MS);
        const uint64_t after = now();
        if (size > 0) {
            consecutiveErrors = 0;
            link->handleReport(report, size, after);
        }
        else if (size < 0) {
            link->noteReadError();
            if (++consecutiveErrors >= 3 && !link->lost()) {
                // Three failures in a row (at least 750 ms) is a gone device, not a hiccup.
                link->markLost();
                wake();
            }
        }
        link->checkStale(after);
        // hidapi reports a detached device as an immediate error; do not spin on it.
        if (size <= 0 && after - before < (uint64_t)SteamControllerLink::READ_TIMEOUT_MS / 2) {
            SDL_Delay(SteamControllerLink::READ_TIMEOUT_MS);
        }
    }
}

#endif
