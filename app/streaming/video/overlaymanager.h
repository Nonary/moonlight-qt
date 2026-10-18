#pragma once

#include <QString>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <string>
#include <thread>

#include "SDL_compat.h"
#include <SDL_ttf.h>
#include "timinggraph.h"

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayMax
};

enum class StatusSource { Network, PacketLoss, ClientPacing, Mouse, HostLaunch, Count };

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

    // For OverlayDebug, true while the stats text or the timing graph shows.
    bool isOverlayEnabled(OverlayType type);
    bool isStatsEnabled();
    bool isTimingGraphEnabled();
    void setTimingGraphState(bool enabled);
    std::string getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text, TimingGraphSnapshot graph = {});
    // Refreshes the stats graph between text updates.
    void updateTimingGraph(TimingGraphSnapshot graph);
    int getOverlayMaxTextLength();
    // For OverlayDebug, controls the stats text independently of the graph.
    void setOverlayState(OverlayType type, bool enabled);
    void setStatusMessage(StatusSource source, const std::string& text);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    void setOutputSize(int width, int height);
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
        int outputWidth = 0, outputHeight = 0;
        SDL_Color color = {};
        char text[1024] = {};

        TTF_Font* font = nullptr; // Owned exclusively by the overlay worker.
        SDL_Surface* surface = nullptr; // Atomic ownership transfer to renderer.
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
    std::shared_ptr<const TimingGraphSnapshot> m_TimingGraph;
    bool m_StatsEnabled = false, m_TimingGraphEnabled = false;
    std::mutex m_StateLock;
    std::string m_StatusMessages[static_cast<int>(StatusSource::Count)];
    std::condition_variable m_WorkReady;
    // Only renderer attachment and callbacks take this lock. Producers never
    // wait for rasterization, texture upload or renderer destruction.
    std::mutex m_RendererLock;
    bool m_HaveRenderer = false, m_Stopping = false, m_TtfInitialized = false;
    std::thread m_Worker;
};

}
