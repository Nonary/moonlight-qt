#include "config.h"
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace Vrr {
bool loadConfig(const std::string& path, Config& config, std::string& error)
{
    std::ifstream input(path);
    if (!input) { error = "cannot open configuration"; return false; }
    Config result = config;
    std::unordered_set<std::string> keys;
    std::string line;
    unsigned lines = 0;
    while (std::getline(input, line)) {
        if (++lines > 64 || line.size() > 256) { error = "configuration too large"; return false; }
        if (line.empty() || line[0] == '#') continue;
        const auto separator = line.find('=');
        const auto key = line.substr(0, separator);
        if (separator == std::string::npos || !keys.insert(key).second) { error = "invalid or duplicate key: " + key; return false; }
        double value;
        std::istringstream number(line.substr(separator + 1));
        if (!(number >> value) || !std::isfinite(value) || !(number >> std::ws).eof() || value < 0 || value > 1000000) {
            error = "invalid value: " + key; return false;
        }
        const auto us = Ns(std::llround(value * 1000));
        if (key == "min_interval_us") result.minInterval = us;
        else if (key == "frame_interval_us") result.frameInterval = us;
        else if (key == "max_interval_us") result.maxInterval = us;
        else if (key == "buffer_max_us") result.maxBuffer = us;
        else if (key == "guard_us") result.guard = us;
        else if (key == "feedback_timeout_us") result.feedbackTimeout = us;
        else if (key == "buffer_min_us") result.minBuffer = us;
        else if (key == "buffer_attack_us") result.bufferAttack = us;
        else if (key == "buffer_release_us") result.bufferRelease = us;
        else if (key == "initial_render_us") result.initialRender = us;
        else if (key == "jitter_percentile" && std::floor(value) == value) result.jitterPercentile = unsigned(value);
        else if (key == "render_percentile" && std::floor(value) == value) result.renderPercentile = unsigned(value);
        else if (key == "smooth_cadence" && (value == 0 || value == 1)) result.smoothCadence = value != 0;
        else if (key == "cadence_slew_us") result.cadenceSlew = us;
        else if (key == "smoothing_delay_us") result.smoothingDelay = us;
        else if (key == "adaptive_reserve" && (value == 0 || value == 1)) result.adaptiveReserve = value != 0;
        else if (key == "reserve_max_us") result.reserveMax = us;
        else if (key == "reserve_boost_us") result.reserveBoost = us;
        else { error = "unknown key: " + key; return false; }
    }
    // New optional limits inherit a smaller explicitly configured parent cap.
    // Explicit values are still validated exactly, preserving sweep manifests.
    if (!keys.count("reserve_max_us")) result.reserveMax = std::min(result.reserveMax, result.maxBuffer);
    if (!keys.count("reserve_boost_us")) result.reserveBoost = std::min(result.reserveBoost, result.reserveMax);
    if (!keys.count("smoothing_delay_us")) result.smoothingDelay = std::min(result.smoothingDelay, result.maxBuffer);
    // Reject rather than silently clamping: a sweep must test exactly what its
    // manifest says, and applying its winning configuration must be identical.
    const auto valid = Controller(result).config();
    if (result.minInterval != valid.minInterval || result.maxInterval != valid.maxInterval ||
        result.maxBuffer != valid.maxBuffer || result.guard != valid.guard ||
        result.feedbackTimeout != valid.feedbackTimeout || result.minBuffer != valid.minBuffer ||
        result.bufferAttack != valid.bufferAttack || result.bufferRelease != valid.bufferRelease ||
        result.initialRender != valid.initialRender || result.jitterPercentile != valid.jitterPercentile ||
        result.renderPercentile != valid.renderPercentile || result.cadenceSlew != valid.cadenceSlew ||
        result.smoothingDelay != valid.smoothingDelay || result.reserveMax != valid.reserveMax ||
        result.reserveBoost != valid.reserveBoost || result.frameInterval != valid.frameInterval) { error = "configuration value outside supported range"; return false; }
    config = result;
    return true;
}
}
