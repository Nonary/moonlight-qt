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

    // The playout buffer absorbs receiver jitter. The presenter decides whether
    // each frame can flip immediately or must wait for the next scanout.
    return refreshHz;
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
                                                       bool vrrEnabled)
{
    std::map<int, VrrFpsChoiceKind> choices;

    // These are always useful streaming rates, including on a 60 Hz display
    // where 60 is also the exact native rate.
    addChoice(choices, 30, VrrFpsChoiceKind::Fixed);
    addChoice(choices, 60, VrrFpsChoiceKind::Fixed);

    for (const int refreshHz : refreshRates) {
        if (!isUsableRefreshRate(refreshHz)) {
            continue;
        }

        if (vrrEnabled) {
            addChoice(choices, vrrRateForRefresh(refreshHz), VrrFpsChoiceKind::Vrr);
        }
        else {
            addChoice(choices, refreshHz, VrrFpsChoiceKind::Fixed);
        }
    }

    // Preserve manually saved values, including exact native refresh.
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
