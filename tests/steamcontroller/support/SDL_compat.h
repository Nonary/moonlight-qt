// Minimal SDL boundary for the production SteamControllerHost bridge test.
// No event loop, HID device or network connection is started.
#pragma once
#include <cstdint>

constexpr uint32_t SDL_USEREVENT = 0x8000;
constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
struct SDL_UserEvent {
    int code = 0;
    void* data1 = nullptr;
    void* data2 = nullptr;
};
struct SDL_Event {
    uint32_t type = 0;
    SDL_UserEvent user;
};
inline int SDL_PushEvent(SDL_Event*) { return 1; }
inline void SDL_LogInfo(int, const char*, ...) {}
