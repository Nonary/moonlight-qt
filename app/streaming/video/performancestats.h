#pragma once

#include "ffmpeg-renderers/pacer/vrr/inputbuffer.h"
#include "ffmpeg-renderers/pacer/vrr/presenttiming.h"
#include <algorithm>
#include <cstdio>
#include <string>

// Presentation only: these values never become controller feedback.
// The normal overlay keeps Moonlight's original stats and appends this short
// VRR summary; the full breakdown remains in the session log.
namespace PerformanceStats {
struct Snapshot {
    bool vrr = false, adaptive = false, atLimit = false;
    uint64_t bufferUs = 0;
    // Scheduled beyond the input buffer by cadence smoothing and late-frame
    // recovery. Show it and the total separately from the input buffer so the
    // value labeled "VRR buffer" matches the timing graph's Buffer line.
    // Label the combined positive correction "extra delay": display-spacing
    // and recovery holds can exceed the cadence smoother's tolerance budget.
    uint64_t smoothingUs = 0;
    uint64_t targetPpm = 0, toleranceUs = 0;
    uint64_t nowUs = 0;
    Vrr13::InputGoalBuffer::Stats input;
    // Interval-policy CPU submission score, independent of input coverage.
    bool clientTimingValid = false;
    double clientTimingPercent = 0;
    // Display error added after submission, judged against the plan. Source
    // timestamp jitter is the host's cadence, not a display error.
    Vrr13::PresentTiming::Stats present;
};

inline bool fresh(uint64_t atUs, uint64_t nowUs)
{
    return atUs && nowUs >= atUs && nowUs - atUs <= 1000000;
}

// Counters share the rendered-frame population and window. The residual is
// the bounded GPU decode synchronization removed from queue/render accounting;
// it is not inferred from the latest buffer budget or from dropped frames.
inline std::string formatClientTiming(uint64_t totalUs, uint64_t queueUs,
                                      uint64_t renderUs, uint64_t frames)
{
    if (!frames) return "";
    // Never turn inconsistent/reset counters into an unsigned latency spike.
    if (queueUs > totalUs || renderUs > totalUs - queueUs)
        return "Average post-output client time: N/A (timing counters unavailable)\n";
    const uint64_t decodeWaitUs = totalUs - queueUs - renderUs;
    const double divisor = frames * 1000.0;
    char text[320];
    std::snprintf(text, sizeof(text),
        "Average post-output client time: %.2f ms (to Present return)\n"
        "  Includes queue/pacing %.2f ms | GPU decode wait %.2f ms | CPU render/submit %.2f ms\n",
        totalUs / divisor, queueUs / divisor, decodeWaitUs / divisor, renderUs / divisor);
    return text;
}

inline std::string formatVrr(const Snapshot& s)
{
    if (!s.vrr) return "VRR pacing: inactive\n";

    char buffer[96], coverage[192] = {}, timing[128], present[128], text[768];
    const double totalMs = (s.bufferUs + s.smoothingUs) / 1000.0;
    if (s.adaptive && s.input.samples)
        std::snprintf(buffer, sizeof(buffer), "%.2f ms%s (target %.2f%s)",
            s.bufferUs / 1000.0, s.atLimit ? " at limit" : "",
            s.input.targetUs / 1000.0, s.input.tailUncertain ? " learning" : "");
    else
        std::snprintf(buffer, sizeof(buffer), "%.2f ms%s",
            s.bufferUs / 1000.0, s.atLimit ? " at limit" : "");

    // The goal belongs to the severity-weighted interval score. Binary input
    // coverage is independent: recovery may still present an uncovered input smoothly.
    const double target = s.targetPpm / 10000.0;
    if (s.adaptive) {
        if (s.input.recentEligible) {
            const auto covered = s.input.recentEligible - std::min(s.input.recentMissed, s.input.recentEligible);
            std::snprintf(coverage, sizeof(coverage), "Input coverage: %.2f%%\n",
                100.0 * double(covered) / double(s.input.recentEligible));
        }
        else std::snprintf(coverage, sizeof(coverage), "Input coverage: N/A\n");
        if (s.input.reportingWindowUs) {
            char recent[32], history[32];
            const auto percent=[](char* out, uint64_t count, uint64_t misses) {
                if (count) std::snprintf(out, 32, "%.2f%%", 100.0 * double(count-std::min(count,misses))/double(count));
                else std::snprintf(out, 32, "N/A");
            };
            percent(recent, s.input.reportingEligible, s.input.reportingMissed);
            percent(history, s.input.recentEligible, s.input.recentMissed);
            std::snprintf(coverage, sizeof(coverage), "Input coverage (%llus): %s | %llus %s\n",
                static_cast<unsigned long long>(s.input.reportingWindowUs/1000000), recent,
                static_cast<unsigned long long>(s.input.windowUs/1000000), history);
        }
    }
    if (s.clientTimingValid)
        std::snprintf(timing, sizeof(timing), "Client timing (CPU): %.2f%% / %.2f%% target",
            s.clientTimingPercent, target);
    else std::snprintf(timing, sizeof(timing), "Client timing (CPU): collecting / %.2f%% target", target);

    if (s.present.intervals && fresh(s.present.lastObservedUs, s.nowUs))
        std::snprintf(present, sizeof(present), "%.2f%% >%.2f ms | hitches %llu | worst +%.1f ms",
            s.present.issuePercent(), s.toleranceUs / 1000.0,
            static_cast<unsigned long long>(s.present.hitches), s.present.worstAddedUs / 1000.0);
    else if (fresh(s.present.lastPausedUs, s.nowUs))
        std::snprintf(present, sizeof(present), "paused below VRR range");
    else std::snprintf(present, sizeof(present), "N/A (no recent display feedback)");

    std::snprintf(text, sizeof(text),
        "VRR buffer: %s | %s\n"
        "%s"
        "Extra delay: %.2f ms (smoothing/recovery) | Planned total: %.2f ms\n"
        "Present timing issues (30s): %s\n",
        buffer, timing, coverage, s.smoothingUs / 1000.0, totalMs, present);
    return text;
}
}
