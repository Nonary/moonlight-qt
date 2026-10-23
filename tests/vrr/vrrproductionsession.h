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
    config.correctInputAccounting = true;
    config.experimentalBoundedCatchup = true;
    const auto options = config.timingOptions.resolved(config.latencyMode);
    config.experimentalInputPercentilePpm = uint64_t(options.targetHundredths) * 100;
    config.experimentalInputPercentileWindowUs = uint64_t(options.historySeconds) * 1000000;
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
    config.readinessHitchFeedback = false;
    config.latencyMode = latencyMode >= 0 && latencyMode <= 2 ? latencyMode : 1;
    config.timingOptions = VrrTimingOptions{}.resolved(config.latencyMode);
    config.streamRateHz = streamRateHz;
    config.displayRefreshHz = displayRefreshHz;
    config.smoothFrameTiming = smoothFrameTiming;
    config.absorbMicrostutter = smoothFrameTiming && absorbMicrostutter;
    return vrrProductionInputConfig(config);
}
