// SPDX-License-Identifier: GPL-3.0-or-later
#include "steamcontrollerhost.h"

#include "SDL_compat.h"
#include <Limelight.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace {

const int MAX_STEAM_SLOTS = 16;   // the gamepad mask is 16 bits wide

struct Entry {
    std::shared_ptr<SteamControllerLink> link;
    int generation = 0;
    uint8_t number = 0;
};

struct Registry {
    std::mutex mutex;
    bool alive = false;
    uint16_t mask = 0;
    std::vector<Entry> entries;   // announced links

    // Under mutex: the controller number this link's generation is announced as, or -1.
    int numberFor(const SteamControllerLink& link, int generation) const {
        for (const Entry& e : entries) {
            if (e.link.get() == &link && e.generation == generation) return e.number;
        }
        return -1;
    }
    // Under mutex: the link announced as this controller number (the first in single-controller mode).
    Entry entryFor(uint16_t number) const {
        for (const Entry& e : entries) {
            if (e.number == number) return e;
        }
        return {};
    }
};

Registry& registry() {
    static Registry instance;
    return instance;
}

}

SteamControllerHost::SteamControllerHost()
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.alive = true;
}

SteamControllerHost::~SteamControllerHost()
{
    shutdown();
}

void SteamControllerHost::shutdown()
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.alive = false;
    r.entries.clear();
}

void SteamControllerHost::setActiveGamepadMask(uint16_t mask)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.mask = mask;
}

void SteamControllerHost::announce(uint8_t controllerNumber, const std::shared_ptr<SteamControllerLink>& link, int generation)
{
    if (controllerNumber >= MAX_STEAM_SLOTS || !link) return;
    const uint16_t capabilities = link->capabilities();
    const uint32_t supported = SteamControllerCore::supportedButtonFlags();
    Registry& r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.alive) return;
        r.entries.erase(std::remove_if(r.entries.begin(), r.entries.end(), [&](const Entry& e) { return e.link == link; }), r.entries.end());
        Entry entry;
        entry.link = link;
        entry.generation = generation;
        entry.number = controllerNumber;
        r.entries.push_back(entry);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Sending controller arrival: player %d type %u caps 0x%x buttons 0x%x (Steam Controller %s, mask 0x%x)",
                    controllerNumber, LI_CTYPE_STEAM, capabilities, supported, link->name().c_str(), r.mask);
        LiSendControllerArrivalEvent(controllerNumber, r.mask, LI_CTYPE_STEAM, supported, capabilities);
    }
    // Resend state and pad DOWNs; attributes and battery right after the arrival when known
    // (the link calls back into linkAttributes()/linkBattery(), which look the number up).
    link->requestInputReset();
    link->replayForArrival(generation);
}

void SteamControllerHost::retire(const std::shared_ptr<SteamControllerLink>& link, uint8_t controllerNumber, uint16_t newMask)
{
    if (controllerNumber >= MAX_STEAM_SLOTS) return;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.entries.erase(std::remove_if(r.entries.begin(), r.entries.end(), [&](const Entry& e) { return e.link == link; }), r.entries.end());
    r.mask = newMask;
    if (!r.alive) return;
    LiSendMultiControllerEvent(controllerNumber, newMask, 0, 0, 0, 0, 0, 0, 0);
}

void SteamControllerHost::unpackEvent(const void* data1, const void* data2, int& linkId, int& generation, int& kind)
{
    linkId = (int)(uintptr_t)data1;
    uintptr_t packed = (uintptr_t)data2;
    kind = (int)(packed >> 24);
    generation = (int)(packed & 0xFFFFFF);
}

void SteamControllerHost::pushEvent(const SteamControllerLink& link, int generation, EventKind kind)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_STEAM_CONTROLLER_LINK;
    event.user.data1 = (void*)(uintptr_t)link.id();
    event.user.data2 = (void*)(uintptr_t)(((uintptr_t)kind << 24) | ((uintptr_t)generation & 0xFFFFFF));
    SDL_PushEvent(&event);
}

// ----- host callbacks -----

void SteamControllerHost::clSteamHaptic(uint16_t controllerNumber, uint8_t length, const uint8_t* report)
{
    if (controllerNumber >= MAX_STEAM_SLOTS || report == nullptr || length == 0) return;
    Entry entry;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.alive) return;
        entry = r.entryFor(controllerNumber);
    }
    if (entry.link) entry.link->steamHaptic(report, length, SteamController::nowMs(), entry.generation);
}

void SteamControllerHost::clSteamSettings(uint16_t controllerNumber, uint8_t command, uint8_t count, const uint8_t* triples)
{
    // A Bluetooth LE link advertises LI_CCAP_STEAM_SETTINGS and relays the write to its firmware
    // (minus the reserved ids). USB and puck links do not take host settings writes, as in the
    // Android client; a host that sends them anyway is ignored.
    if (controllerNumber >= MAX_STEAM_SLOTS) return;
    Entry entry;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.alive) return;
        entry = r.entryFor(controllerNumber);
    }
    if (entry.link && entry.link->hostSettings(command, count, triples, SteamController::nowMs(), entry.generation)) {
        return;
    }
    static bool logged;
    if (!logged) {
        logged = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Steam Controller: ignoring host settings 0x%02x (%u triples) for player %u; only a Bluetooth LE link takes them (USB/puck links keep their own link settings)",
                    command, count, controllerNumber);
    }
}

bool SteamControllerHost::rumble(uint16_t controllerNumber, uint16_t lowFreqMotor, uint16_t highFreqMotor)
{
    if (controllerNumber >= MAX_STEAM_SLOTS) return false;
    Entry entry;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.alive) return false;
        entry = r.entryFor(controllerNumber);
    }
    return entry.link && entry.link->rumble(lowFreqMotor, highFreqMotor, SteamController::nowMs(), entry.generation);
}

bool SteamControllerHost::setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz)
{
    if (controllerNumber >= MAX_STEAM_SLOTS) return false;
    Entry entry;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.alive) return false;
        entry = r.entryFor(controllerNumber);
    }
    if (!entry.link || !entry.link->setMotionRate(motionType, reportRateHz, entry.generation)) return false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Steam Controller %s: motion type %u at %u Hz (host request)",
                entry.link->name().c_str(), motionType, reportRateHz);
    return true;
}

// ----- listener (driver threads, link mutex held) -----

void SteamControllerHost::linkConnected(SteamControllerLink& link, int generation)
{
    pushEvent(link, generation, LinkConnected);
}

void SteamControllerHost::linkDisconnected(SteamControllerLink& link, int generation)
{
    pushEvent(link, generation, LinkDisconnected);
}

void SteamControllerHost::linkState(SteamControllerLink& link, int generation, int buttonFlags, uint8_t lt, uint8_t rt,
                                    short lsX, short lsY, short rsX, short rsY)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    int number = r.alive ? r.numberFor(link, generation) : -1;
    if (number < 0) return;
    LiSendMultiControllerEvent((short)number, (short)r.mask, buttonFlags, lt, rt, lsX, lsY, rsX, rsY);
}

void SteamControllerHost::linkTouch(SteamControllerLink& link, int generation, uint8_t touchpadIndex, uint8_t eventType,
                                    uint32_t pointerId, float x, float y, float pressure)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    int number = r.alive ? r.numberFor(link, generation) : -1;
    if (number < 0) return;
    LiSendControllerTouchEvent2((uint8_t)number, eventType, touchpadIndex, pointerId, x, y, pressure);
}

void SteamControllerHost::linkMotion(SteamControllerLink& link, int generation, uint8_t motionType, float x, float y, float z)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    int number = r.alive ? r.numberFor(link, generation) : -1;
    if (number < 0) return;
    LiSendControllerMotionEvent((uint8_t)number, motionType, x, y, z);
}

void SteamControllerHost::linkBattery(SteamControllerLink& link, int generation, uint8_t state, uint8_t percent)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    int number = r.alive ? r.numberFor(link, generation) : -1;
    if (number < 0) return;
    LiSendControllerBatteryEvent((uint8_t)number, state, percent);
}

void SteamControllerHost::linkAttributes(SteamControllerLink& link, int generation, const SteamControllerAttributes& attributes)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    int number = r.alive ? r.numberFor(link, generation) : -1;
    if (number < 0) return;
#if STEAM_CONTROLLER_LI_EXTENSIONS
    LI_STEAM_CONTROLLER_ATTRIBUTES li = attributes.toLi();
    int err = LiSendSteamControllerAttributesEvent((uint8_t)number, &li);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Steam Controller %s: firmware attributes sent for player %d (valid mask 0x%02x, rc %d)",
                link.name().c_str(), number, attributes.validMask, err);
#else
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Steam Controller %s: firmware attributes read (valid mask 0x%02x) but this build's moonlight-common-c cannot send them",
                link.name().c_str(), attributes.validMask);
#endif
}
