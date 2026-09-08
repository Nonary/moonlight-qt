#pragma once

#include <vector>

// VRR rate selection and deterministic stream/display qualification live in
// one small policy object. It has no dependency on SDL, QSettings, or a
// renderer, so the arithmetic can be tested independently.
enum class VrrFpsChoiceKind {
    Fixed,
    Vrr,
    Custom,
};

struct VrrFpsChoice {
    int fps;
    VrrFpsChoiceKind kind;
};

class VrrRatePolicy
{
public:
    // Native maximum; no artificial below-refresh cap.
    static int vrrRateForRefresh(int refreshHz);

    // Accept sources through native refresh. Per-frame presentation enforces
    // scanout safety; session admission no longer reserves a fixed FPS margin.
    static bool hasAdaptiveHeadroom(int streamRateHz, int displayRefreshHz);

    // Build baseline, native maximum, and saved custom choices.
    static std::vector<VrrFpsChoice> buildChoices(const std::vector<int>& refreshRates,
                                                   int savedFps,
                                                   bool vrrEnabled);

};
