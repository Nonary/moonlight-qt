#include "vrrratepolicy.h"

#include <algorithm>
#include <map>

namespace {

bool isUsableRefreshRate(int refreshHz)
{
    // SDL reports zero for an unknown refresh rate.  Values at or below one
    // cannot produce a meaningful stream-rate recommendation either.
    return refreshHz > 1;
}

void addChoice(std::map<int, VrrFpsChoiceKind>& choices,
               int fps,
               VrrFpsChoiceKind kind)
{
    if (fps > 0) {
        // Keep the first semantic role for duplicate rates.  Baseline choices
        // intentionally win over a coincident calculated/native rate.
        choices.emplace(fps, kind);
    }
}

} // namespace

int VrrRatePolicy::vrrRateForRefresh(int refreshHz)
{
    if (!isUsableRefreshRate(refreshHz)) {
        return 0;
    }

    // Use the original below-refresh recommendation and integer rounding.
    return protectedRateForRefresh(refreshHz);
}

int VrrRatePolicy::ultraLowLatencyMaxFps(int refreshHz, bool nativeSupport)
{
    const int nearRefresh = nativeSupport ? vrrRateForRefresh(refreshHz) : 0;
    return nearRefresh > 0 ? nearRefresh : lowLatencyRateForRefresh(refreshHz);
}

int VrrRatePolicy::rateAfterVrrToggle(int selectedFps, int refreshHz, bool enabled)
{
    const int vrrFps = vrrRateForRefresh(refreshHz);
    if (refreshHz < 60 || vrrFps == 0) {
        return selectedFps;
    }
    const int from = enabled ? refreshHz : vrrFps;
    const int to = enabled ? vrrFps : refreshHz;
    return selectedFps == from ? to : selectedFps;
}

bool VrrRatePolicy::hasAdaptiveHeadroom(int streamRateHz, int displayRefreshHz)
{
    if (streamRateHz <= 0 || displayRefreshHz <= 0) {
        return false;
    }

    return streamRateHz <= displayRefreshHz;
}

std::vector<VrrFpsChoice> VrrRatePolicy::buildChoices(const std::vector<int>& refreshRates,
                                                    int savedFps,
                                                    bool windowsVrrEnabled,
                                                    int fpsLimit)
{
    std::map<int, VrrFpsChoiceKind> choices;

    // These are always useful streaming rates, including on a 60 Hz display
    // where 60 is also the exact native rate.
    addChoice(choices, 30, VrrFpsChoiceKind::Fixed);
    addChoice(choices, 60, VrrFpsChoiceKind::Fixed);

    // Native refresh remains a normal choice with VRR enabled. Insert all
    // native rates first so another display's recommendation cannot relabel one.
    for (const int refreshHz : refreshRates) {
        if (isUsableRefreshRate(refreshHz)) {
            addChoice(choices, refreshHz, VrrFpsChoiceKind::Fixed);
        }
    }

    for (const int refreshHz : refreshRates) {
        if (!isUsableRefreshRate(refreshHz)) {
            continue;
        }

        addChoice(choices, vrrRateForRefresh(refreshHz), VrrFpsChoiceKind::Fixed);
    }

    if (windowsVrrEnabled) {
        for (const int refreshHz : refreshRates) {
            addChoice(choices, lowLatencyRateForRefresh(refreshHz), VrrFpsChoiceKind::LowLatencyVrr);
        }
    }

    if (fpsLimit > 0) {
        choices.erase(choices.upper_bound(fpsLimit), choices.end());
        addChoice(choices, fpsLimit, VrrFpsChoiceKind::Fixed);
        savedFps = std::min(savedFps, fpsLimit);
    }

    // Preserve manually saved values within the available range.
    if (savedFps > 0) {
        addChoice(choices, savedFps, VrrFpsChoiceKind::Custom);
    }

    std::vector<VrrFpsChoice> result;
    result.reserve(choices.size());
    for (const auto& choice : choices) {
        result.push_back({choice.first, choice.second});
    }

    return result;
}
