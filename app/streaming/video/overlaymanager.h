#pragma once

#include <QString>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;

    struct UpdateTiming {
        OverlayType type;
        uint64_t revision;
        int64_t queueNs, rasterNs, dispatchNs;
    };
    void recordOverlayTiming(UpdateTiming timing) {
        std::lock_guard<std::mutex> lock(m_TimingLock);
        m_Timing[timing.type] = timing;
        m_HaveTiming[timing.type] = true;
    }
    bool takeOverlayTiming(UpdateTiming& timing) {
        std::lock_guard<std::mutex> lock(m_TimingLock);
        for (int i = 0; i < OverlayMax; ++i) if (m_HaveTiming[i]) {
            timing = m_Timing[i]; m_HaveTiming[i] = false; return true;
        }
        return false;
    }
private:
    std::mutex m_TimingLock;
    UpdateTiming m_Timing[OverlayMax] = {};
    bool m_HaveTiming[OverlayMax] = {};
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    std::string getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void run();
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);

    struct {
        bool enabled = false;
        bool dirty = false;
        uint64_t revision = 0;
        std::chrono::steady_clock::time_point queued;
        int fontSize = 0;
        SDL_Color color = {};
        char text[1024] = {};

        TTF_Font* font = nullptr; // Owned exclusively by the overlay worker.
        SDL_Surface* surface = nullptr; // Atomic ownership transfer to renderer.
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
    std::mutex m_StateLock;
    std::condition_variable m_WorkReady;
    // Only renderer attachment and callbacks take this lock. Producers never
    // wait for rasterization, texture upload or renderer destruction.
    std::mutex m_RendererLock;
    bool m_HaveRenderer = false, m_Stopping = false, m_TtfInitialized = false;
    std::thread m_Worker;
};

}
