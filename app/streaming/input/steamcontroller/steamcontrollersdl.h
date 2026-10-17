// SPDX-License-Identifier: GPL-3.0-or-later
// SDL hidapi transport and threads for the Steam Controller (2026) driver. SDL's hidapi API
// (SDL_hid_*) exists from SDL 2.0.18 and covers Windows, macOS and Linux with one code path;
// the Steam Link SDK predates it, so that build compiles the driver out.
#pragma once

#include "SDL_compat.h"

#if SDL_VERSION_ATLEAST(2, 0, 18) && !defined(STEAM_LINK)
#define HAVE_STEAM_CONTROLLER_DRIVER 1
#else
#define HAVE_STEAM_CONTROLLER_DRIVER 0
#endif

#if HAVE_STEAM_CONTROLLER_DRIVER

#include "steamcontrollerlink.h"

#include <condition_variable>
#include <map>
#include <thread>

class SteamControllerRunner {
public:
    SteamControllerRunner(const SteamControllerConfig& config, int rumbleHoldMs, SteamControllerLink::Listener* listener);
    // Stops the threads, runs the stop sequence on every connected slot and closes the devices.
    ~SteamControllerRunner();

    SteamControllerManager& manager() { return *m_Manager; }
    // Hot-plug hint from the main thread (SDL joystick add/remove); the 2 s timer covers the rest.
    void requestRescan();

    static uint64_t now();

private:
    class SdlTransport;

    void serviceLoop();
    void readLoop(std::shared_ptr<SteamControllerLink> link);
    void wake();

    std::unique_ptr<SdlTransport> m_Transport;
    std::unique_ptr<SteamControllerManager> m_Manager;
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    bool m_Wake = false;
    bool m_Stopping = false;
    std::thread m_ServiceThread;
    std::map<int, std::thread> m_Readers;   // link id -> reader thread (service thread / destructor only)
    bool m_HidInitialized = false;
};

#endif
