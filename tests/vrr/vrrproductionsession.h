#pragma once

// The session configuration a live VRR stream actually runs.
//
// VrrSessionConfig's member defaults are historical: they leave bounded
// catch-up, the receive clock, profile-scaled recovery credit, layer seeding
// and learning trim switched off. The live path enables them in Pacer and
// VrrPacingWorker::resolvedInputConfig(), which tests outside the worker
// cannot link. Controller tests that build a default config therefore test a
// policy no user runs. Build sessions through this header instead.
//
// tst_vrrpacingworker compares every resolved timing parameter produced from
// this helper with the worker's own resolution, so the two cannot drift apart.

#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

// Mirrors VrrPacingWorker::resolvedInputConfig() with no diagnostic
// MOONLIGHT_VRR_* environment overrides.
inline VrrSessionConfig vrrProductionInputConfig(VrrSessionConfig config)
{
    return vrrProductionSessionConfig(std::move(config));
}

// Historical goal-solver fixtures keep their original session parameters.
inline VrrSessionConfig vrrHistoricalGoalConfig(VrrSessionConfig config)
{
    const auto defaults = config.latencyMode == 2 ? VrrTimingOptions{500, 9900, 60, 500} :
        config.latencyMode == 0 ? VrrTimingOptions{2000, 9995, 300, 500} :
                                 VrrTimingOptions{1000, 9950, 120, 500};
    auto& options = config.timingOptions;
    if (!options.bufferPerMille) options.bufferPerMille = defaults.bufferPerMille;
    if (!options.targetHundredths) options.targetHundredths = defaults.targetHundredths;
    if (!options.historySeconds) options.historySeconds = defaults.historySeconds;
    if (!options.toleranceUs) options.toleranceUs = defaults.toleranceUs;
    config = vrrProductionSessionConfig(config);
    config.vrr18IntervalFeedback = false;
    return config;
}

// Mirrors Pacer::initialize() for a preset (latencyMode 0 Smooth, 1 Balanced,
// 2 Low Latency) and then applies the worker's resolution.
inline VrrSessionConfig vrrProductionSession(int latencyMode, int streamRateHz,
                                             int displayRefreshHz,
                                             bool smoothFrameTiming = true,
                                             bool absorbMicrostutter = false)
{
    VrrSessionConfig config;
    config.latencyMode = latencyMode >= 0 && latencyMode <= 2 ? latencyMode : 1;
    config.timingOptions = VrrTimingOptions{}.resolved(config.latencyMode);
    config.streamRateHz = streamRateHz;
    config.displayRefreshHz = displayRefreshHz;
    config.smoothFrameTiming = smoothFrameTiming;
    config.absorbMicrostutter = smoothFrameTiming && absorbMicrostutter;
    return vrrProductionInputConfig(config);
}
