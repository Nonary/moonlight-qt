// Minimal SDL boundary for the production SteamControllerHost bridge test.
// No event loop, HID device or network connection is started.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

constexpr uint32_t SDL_USEREVENT = 0x8000;
constexpr uint32_t SDL_QUIT = 0x100;
constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
struct SDL_UserEvent {
    int code = 0;
    void* data1 = nullptr;
    void* data2 = nullptr;
};
struct SDL_QuitEvent {
    uint32_t timestamp = 0;
};
struct SDL_Event {
    uint32_t type = 0;
    SDL_UserEvent user;
    SDL_QuitEvent quit;
};
void recordSdlEvent(const SDL_Event& event);
inline int SDL_PushEvent(SDL_Event* event) { recordSdlEvent(*event); return 1; }
inline uint32_t SDL_GetTicks() { return 123; }
inline const char* SDL_getenv(const char* name) { return std::getenv(name); }
inline int SDL_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }
inline void SDL_LogInfo(int, const char*, ...) {}
