#pragma once

#include "../renderer.h"
#include "trace.h"
#include "smoothness.h"
#include "deadlines.h"
#include <condition_variable>
#include <deque>
#include <thread>

namespace Vrr {
class Worker {
public:
    explicit Worker(IFFmpegRenderer* renderer, Config config, QString profileKey = {});
    ~Worker();
    bool start();
    void stop();
    void submit(AVFrame* frame, uint64_t receivedUs, uint64_t assembledUs);
    void collectStats(VIDEO_STATS& stats); // Cumulative deltas, consumed only by decoder thread.
private:
    struct Queued { AVFrame* frame; Frame timing; };
    struct InFlight {
        Queued q;
        Preparation preparation;
        RenderProbe probe;
        Plan plan;
        Ns started = 0, cpuCompleted = 0, dispatch = 0, submitted = 0;
        bool presented = false;
    };
    void drainPreparation();
    void finishFrame(InFlight& frame);
    void run();
    void poll();
    void drop(Queued& frame, Drop reason);
    void recordPlan(Event event, const Frame& frame, const Plan& plan, Ns at);
    IFFmpegRenderer* m_Renderer;
    Controller m_Controller;
    Smoothness m_Smoothness;
    Deadlines m_Deadlines;
    std::deque<InFlight> m_InFlight;
    SmoothnessReport m_SmoothnessReport;
    Trace m_Trace;
    std::atomic<bool> m_Stopping{false};
    std::thread m_Thread;
    std::mutex m_Lock;
    std::condition_variable m_Ready;
    std::deque<Queued> m_Queue;
    VIDEO_STATS m_Stats{};
    uint64_t m_NextId = 0;
    Ns m_LastGpuReady = 0;
    Ns m_LastDeliveryFailure = 0;
    uint64_t m_Presented = 0, m_Discarded = 0, m_Unavailable = 0, m_BelowRange = 0;
    uint64_t m_Tracking = 0, m_Predicted = 0;
    QString m_ProfileKey, m_ProfilePath;
};
}
