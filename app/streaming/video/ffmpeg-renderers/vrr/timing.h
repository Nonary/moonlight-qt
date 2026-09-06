#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>
#include "reserve.h"

// The controller has no OS, decoder, Qt, or wall-clock dependencies. All times
// are signed nanoseconds in the client's monotonic clock unless named RTP.
namespace Vrr {
using Ns = int64_t;
constexpr Ns Millisecond = 1000000;
constexpr Ns Second = 1000000000;
constexpr size_t QueueFrames = 3; // Fixed decoder-surface/worker queue budget.

Ns now();
bool waitUntil(Ns deadline, const std::atomic<bool>& stopping, Ns yieldTail = 50000);

enum class Quality : uint64_t { Unavailable, Compositor, Hardware };
enum class Outcome : uint64_t { Presented, Discarded, Unavailable, Reset };
enum class Mode : uint64_t { Acquiring, Tracking, Stale };
enum Recovery : uint64_t {
    NewEpoch = 1, SourceDiscontinuity = 2, ArrivalStall = 4,
    BurstExcluded = 8, RateChanged = 16, PhaseRestarted = 32, ClockRestarted = 64,
    UncertainCompletion = 128, FramesSkipped = 256
};

struct Config {
    Ns minInterval = Second / 120; // Set from the active mode, never inferred from content FPS.
    Ns maxInterval = 0;            // Unknown VRR floor; zero is NOT an invented 48 Hz limit.
    Ns maxBuffer = 100 * Millisecond;
    Ns guard = 50000;
    Ns feedbackTimeout = 100 * Millisecond;
    Ns minBuffer = 500000;
    Ns bufferAttack = 250000;
    Ns bufferRelease = 20000; // Per display interval; scaled by elapsed arrival time.
    Ns initialRender = Millisecond;
    unsigned jitterPercentile = 95;
    unsigned renderPercentile = 95;
    bool smoothCadence = false;
    Ns cadenceSlew = Millisecond; // Maximum interval correction per source sample.
    Ns smoothingDelay = 6 * Millisecond; // Maximum cadence phase correction, not an added wait.
    bool adaptiveReserve = true;
    Ns reserveMax = 100 * Millisecond;
    Ns reserveBoost = 100 * Millisecond;
    Ns frameInterval = Second / 120; // Negotiated stream FPS; independent of display ceiling.
};

struct Frame {
    uint64_t id = 0;
    uint32_t rtp = 0;
    Ns received = 0;
    Ns assembled = 0;
    Ns decoded = 0;
    Ns source = 0;
    Ns playout = 0;
};

struct RenderProbe {
    Ns decoded = 0, residual = 0, applied = 0, typical = 0, period = 0;
    uint64_t recovery = 0;
};

struct Preparation {
    uint64_t token = 0; // Nonzero: completion will arrive asynchronously.
    Ns acquired = 0;
    Ns commandsSubmitted = 0;
    Ns gpuNotReady = 0;
    Ns gpuReady = 0; // Completion observed in [gpuNotReady, gpuReady], not a GPU timestamp.
};

inline Ns preparationWork(Ns started, const Preparation& p, Ns precedingReady)
{
    // Waiting for a reusable swapchain image is presentation backpressure;
    // extra playout delay cannot create another image or shorten that wait.
    started = std::max(started, p.acquired);
    // A queued image's elapsed span includes work already charged to its
    // predecessor. Remove that overlap before modeling FIFO service demand.
    const Ns overlap = p.token ? std::max<Ns>(0,
        std::min(p.gpuReady, precedingReady) - std::max(started, p.commandsSubmitted)) : 0;
    return std::max<Ns>(0, p.gpuReady - started - overlap);
}

struct Feedback {
    uint64_t id = 0;         // Local submission identity; 0 means an uncorrelated sync sample.
    uint64_t sequence = 0;   // Native output refresh counter, not the frame ID.
    Ns presented = 0;
    Ns observed = 0;         // Time the application learned about this event.
    Ns uncertainty = 0;     // Clock conversion + timestamp measurement bound.
    Ns refresh = 0;         // Compositor prediction; NEVER interpreted as the VRR range.
    uint64_t output = 0;
    uint64_t flags = 0;      // Native quality/composition flags, preserved in captures.
    Quality quality = Quality::Unavailable;
    Outcome outcome = Outcome::Unavailable;
};

struct Plan {
    Ns deadline = 0; // Immutable intended scanout; execution may be later.
    Ns prepare = 0;
    Ns submit = 0;
    Ns target = 0;           // Predicted first scanout, never a measured timestamp.
    Ns earliest = 0;
    Ns latest = 0;           // 0 when no usable lower refresh limit/anchor exists.
    Ns uncertainty = 0;
    Ns buffer = 0;
    Ns renderBudget = 0;
    Ns compositorLead = 0;
    Mode mode = Mode::Acquiring;
    bool belowRange = false;
};

template<size_t N> struct Samples {
    std::array<Ns, N> values{};
    size_t count = 0, next = 0;
    void add(Ns value) { values[next] = value; next = (next + 1) % N; count = std::min(count + 1, N); }
    Ns percentile(unsigned percent, Ns fallback = 0) const {
        if (!count) return fallback;
        auto copy = values;
        auto index = (count - 1) * std::min(percent, 100u) / 100;
        std::nth_element(copy.begin(), copy.begin() + index, copy.begin() + count);
        return copy[index];
    }
};

class Controller {
public:
    explicit Controller(Config config);
    void reset();
    void arrive(Frame& frame);
    Plan plan(const Frame& frame, Ns at) const;
    // Replan after CPU preparation and after polling new presentation feedback.
    // This preserves the original target; it does not charge a second render reserve.
    Plan prepared(Plan original, Ns at) const;
    // schedulingDelay is dispatch/oversleep for this frame, excluding queue and pacing waits.
    RenderProbe renderProbe() const;
    void renderCost(const RenderProbe& probe, Ns cost, Ns observed, Ns schedulingDelay = 0);
    void renderCost(Ns cost, Ns observed = 0, Ns schedulingDelay = 0);
    void wakeError(Ns error);
    void submitted(uint64_t id, Ns at, Ns target, Ns gpuReady = -1);
    void gpuReady(uint64_t id, Ns at);
    bool feedback(const Feedback& sample);
    const Config& config() const { return m_Config; }
    std::vector<int64_t> checkpoint() const;
    bool restore(const std::vector<int64_t>& state);
    uint64_t recoveryFlags() const { return m_RecoveryFlags; }
    Ns bufferLimit() const;
    // Unused time before the next source frame, with enough time left for
    // another physical refresh and normal preparation. This is recovery room,
    // not a claim that the current frame was presented on its original deadline.
    Ns schedulingHeadroom(Ns period, Ns typical) const {
        return std::max<Ns>(0, period - std::max(m_Config.minInterval, typical));
    }
    Ns bufferTarget() const {
        const Ns startup = m_Config.frameInterval -
            schedulingHeadroom(m_Config.frameInterval, m_Config.initialRender);
        const Ns period = m_SmoothedPeriod ? m_SmoothedPeriod : m_Config.frameInterval;
        const Ns typical = m_Render.percentile(50, m_Config.initialRender);
        return m_Reserve.target(m_Config.reserveBoost, startup, schedulingHeadroom(period, typical));
    }
    Ns sourcePeriod() const { return m_SmoothedPeriod; }
    unsigned excludedFrames() const { return m_BurstRemaining; }
    const Reserve& reserve() const { return m_Reserve; }
    bool workloadOverloaded() const { return m_WorkloadOverloaded; }
    bool loadReserve(const std::vector<int64_t>& profile) { return m_Reserve.loadProfile(profile); }

private:
    Plan constrain(Plan result, Ns at, Ns render) const;
    void clearWorkEpisode();
    void finishWorkEpisode();
    struct WorkSample { Ns required = 0, applied = 0, observed = 0, headroom = 0; };
    // Qualify a transient busy episode before caching its delay. An indefinitely
    // saturated pipeline cannot be fixed by accumulating playout latency.
    std::array<WorkSample, 128> m_WorkSamples{};
    size_t m_WorkCount = 0;
    bool m_WorkloadOverloaded = false;
    struct Pending { uint64_t id = 0; Ns submitted = 0, target = 0, ready = 0, presented = 0; };
    void learnLatency(Pending& pending);
    Config m_Config;
    Samples<128> m_Transit, m_Render, m_Latency, m_Wake;
    Ns m_UnpacedReady = 0; // FIFO work completion with intentional pacing removed.
    Ns m_ExpectedReady = 0; // Same source cadence with typical work and no receiver jitter.
    std::array<Pending, 32> m_Pending{};
    size_t m_NextPending = 0;
    uint32_t m_LastRtp = 0;
    Ns m_Source = 0, m_LastDecoded = 0, m_Buffer = Millisecond;
    Ns m_SmoothedSource = 0, m_SmoothedPeriod = 0, m_CadencePeriod = 0;
    Samples<16> m_Periods;
    uint64_t m_LastFrameId = 0, m_RecoveryFlags = 0;
    Ns m_RateCandidate = 0;
    unsigned m_RateCandidateCount = 0, m_BurstRemaining = 0, m_ClockLateFrames = 0;
    Reserve m_Reserve;
    Ns m_TransitResidual = 0;
    Ns m_LastSubmit = 0, m_LastScanout = 0, m_LastObserved = 0;
    Ns m_ClockUncertainty = 0;
    uint64_t m_Output = 0, m_LastSequence = 0;
    Quality m_Quality = Quality::Unavailable;
};
}
