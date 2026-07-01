#pragma once

#include "../../decoder.h"
#include "../renderer.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

#include <array>

// The maximum number of frames pacer will ever hold is:
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
#define PACER_MAX_OUTSTANDING_FRAMES (3 + 1 + 1)
#define PACER_FRAME_DIAGNOSTIC_RING_SIZE 180

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual void waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing);

    void signalVsync();

    void renderOnMainThread();

private:
    struct FrameDiagnosticSample {
        uint32_t intervalUs;
        uint32_t queueDelayUs;
        uint32_t renderUs;
    };

    struct RenderQueueEntry {
        AVFrame* frame;
        uint64_t targetPresentUs;
    };

    static int vsyncThread(void* context);

    static int cadenceThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame, uint64_t targetPresentUs = 0);

    void renderFrame(AVFrame* frame);

    bool waitUntil(uint64_t targetUs);

    void recordFrameInterval(uint64_t beforeRenderUs, uint64_t afterRenderUs, uint64_t queueDelayUs);

    void maybeLogFrameDiagnostics(const char* reason, uint32_t intervalUs);

    void logFrameDiagnostics(const char* reason, uint32_t triggerIntervalUs);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    void dropFrameForEnqueue(QQueue<RenderQueueEntry>& queue);

    QQueue<RenderQueueEntry> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;
    uint64_t m_LastRenderTimeUs;
    uint64_t m_EstimatedRenderTimeUs;
    uint64_t m_LastFrameDiagnosticDumpUs;
    std::array<FrameDiagnosticSample, PACER_FRAME_DIAGNOSTIC_RING_SIZE> m_FrameDiagnosticRing;
    uint32_t m_FrameDiagnosticRingIndex;
    uint32_t m_FrameDiagnosticRingCount;
    IFFmpegRenderer::PresentationMode m_PresentationMode;
};
