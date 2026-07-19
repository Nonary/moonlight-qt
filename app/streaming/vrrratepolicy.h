#pragma once

#include <vector>

// VRR rate selection and deterministic stream/display qualification live in
// one small policy object. It has no dependency on SDL, QSettings, or a
// renderer, so the arithmetic can be tested independently.
enum class VrrFpsChoiceKind {
    Baseline,
    Native,
    Vrr,
    LowLatencyVrr,
    Custom,
};

struct VrrFpsChoice {
    int fps;
    VrrFpsChoiceKind kind;
};

class VrrRatePolicy
{
public:
    // floor(refresh - refresh^2 / 3600)
    static int vrrRateForRefresh(int refreshHz);

    // floor((refresh * 5 / 6) / 5) * 5
    static int lowLatencyRateForRefresh(int refreshHz);

    static bool isNativeRefreshRate(int fps, const std::vector<int>& refreshRates);

    // Adaptive presentation needs enough time between stream frames for one
    // display period and the pacer's baseline safety guard. This is a
    // deterministic session qualification, not a per-frame latching policy.
    static bool hasAdaptiveHeadroom(int streamRateHz, int displayRefreshHz);

    // Build the complete FPS list for the settings UI.  With VRR enabled,
    // exact native refresh choices are intentionally left out, while the two
    // baseline choices and calculated rates remain available.
    static std::vector<VrrFpsChoice> buildChoices(const std::vector<int>& refreshRates,
                                                   int savedFps,
                                                   bool vrrEnabled);

};
