// SPDX-License-Identifier: GPL-3.0-or-later
// Bridge between the Steam Controller (2026) links and the host connection: the process-wide
// registry that maps Moonlight controller numbers to links (so the common-c callbacks, which
// arrive on the control receive thread, reach the driver without the SDL event queue), the
// LiSend* calls made from the reader threads, and the SDL user event that hands link
// arrival/removal to the main thread, which owns the 16 controller slots and the gamepad mask.
//
// Lock order: a link's mutex may be held when the registry mutex is taken (reader thread ->
// listener -> registry); the registry mutex is a leaf. Host callbacks copy the link out of the
// registry and call it after releasing the registry mutex. The main thread never holds the
// registry mutex while calling into a link.
#pragma once

#include "steamcontrollerlink.h"

#include <memory>

// SDL_USEREVENT code for link arrival/removal (the other SDL_CODE_* live in session.cpp).
#define SDL_CODE_STEAM_CONTROLLER_LINK 106

class SteamControllerHost : public SteamControllerLink::Listener {
public:
    enum EventKind { LinkConnected = 1, LinkDisconnected = 2 };

    SteamControllerHost();
    ~SteamControllerHost() override;

    // ----- main thread (SdlInputHandler) -----
    // The host sees one mask for SDL and Steam controllers; keep the registry's copy current.
    static void setActiveGamepadMask(uint16_t mask);
    // The slot was allocated: send the arrival event (and the cached attributes/battery) and let
    // the link's reports through under this controller number.
    void announce(uint8_t controllerNumber, const std::shared_ptr<SteamControllerLink>& link, int generation);
    // The slot was freed: stop forwarding and send the final zeroed state under the new mask.
    void retire(const std::shared_ptr<SteamControllerLink>& link, uint8_t controllerNumber, uint16_t newMask);
    // Session end: nothing is sent to the host from now on (before the runner's threads stop).
    void shutdown();

    static void unpackEvent(const void* data1, const void* data2, int& linkId, int& generation, int& kind);

    // ----- host callbacks (any thread) -----
    static void clSteamHaptic(uint16_t controllerNumber, uint8_t length, const uint8_t* report);
    static void clSteamSettings(uint16_t controllerNumber, uint8_t command, uint8_t count, const uint8_t* triples);
    // True when a Steam slot owns this controller number and took the request.
    static bool rumble(uint16_t controllerNumber, uint16_t lowFreqMotor, uint16_t highFreqMotor);
    static bool setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    // ----- SteamControllerLink::Listener (driver threads) -----
    void linkConnected(SteamControllerLink& link, int generation) override;
    void linkDisconnected(SteamControllerLink& link, int generation) override;
    void linkState(SteamControllerLink& link, int generation, int buttonFlags, uint8_t lt, uint8_t rt,
                   short lsX, short lsY, short rsX, short rsY) override;
    void linkTouch(SteamControllerLink& link, int generation, uint8_t touchpadIndex, uint8_t eventType,
                   uint32_t pointerId, float x, float y, float pressure) override;
    void linkMotion(SteamControllerLink& link, int generation, uint8_t motionType, float x, float y, float z) override;
    void linkBattery(SteamControllerLink& link, int generation, uint8_t state, uint8_t percent) override;
    void linkAttributes(SteamControllerLink& link, int generation, const SteamControllerAttributes& attributes) override;

private:
    void pushEvent(const SteamControllerLink& link, int generation, EventKind kind);
};
