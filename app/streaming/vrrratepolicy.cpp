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

    // Keep this integer-only so the documented floor behavior is stable on
    // every supported compiler.  floor(r - r^2 / 3600) is not the same as
    // r - floor(r^2 / 3600) for rates such as 144 Hz.
    const long long numerator = static_cast<long long>(refreshHz) *
                                (3600LL - refreshHz);
    if (numerator <= 0) {
        return 0;
    }

    return static_cast<int>(numerator / 3600LL);
}

int VrrRatePolicy::lowLatencyRateForRefresh(int refreshHz)
{
    if (!isUsableRefreshRate(refreshHz)) {
        return 0;
    }

    const long long fiveSixths = static_cast<long long>(refreshHz) * 5LL / 6LL;
    return static_cast<int>((fiveSixths / 5LL) * 5LL);
}

bool VrrRatePolicy::isNativeRefreshRate(int fps, const std::vector<int>& refreshRates)
{
    return std::find(refreshRates.cbegin(), refreshRates.cend(), fps) != refreshRates.cend();
}

bool VrrRatePolicy::hasAdaptiveHeadroom(int streamRateHz, int displayRefreshHz)
{
    if (streamRateHz <= 0 || displayRefreshHz <= 0) {
        return false;
    }

    constexpr long long microsecondsPerSecond = 1000000LL;
    const auto periodForRate = [](int rateHz) {
        const long long rate = static_cast<long long>(rateHz);
        return std::max(1LL,
                        (microsecondsPerSecond + rate / 2) / rate);
    };

    const long long displayPeriodUs = periodForRate(displayRefreshHz);
    const long long streamPeriodUs = periodForRate(streamRateHz);
    const long long guardUs = std::max(100LL,
                                      std::min(displayPeriodUs / 64, 250LL));
    return streamPeriodUs > displayPeriodUs + guardUs;
}

std::vector<VrrFpsChoice> VrrRatePolicy::buildChoices(const std::vector<int>& refreshRates,
                                                       int savedFps,
                                                       bool vrrEnabled)
{
    std::map<int, VrrFpsChoiceKind> choices;

    // These are always useful streaming rates, including on a 60 Hz display
    // where 60 is also the exact native rate.
    addChoice(choices, 30, VrrFpsChoiceKind::Baseline);
    addChoice(choices, 60, VrrFpsChoiceKind::Baseline);

    for (const int refreshHz : refreshRates) {
        if (!isUsableRefreshRate(refreshHz)) {
            continue;
        }

        if (vrrEnabled) {
            addChoice(choices, vrrRateForRefresh(refreshHz), VrrFpsChoiceKind::Vrr);
            addChoice(choices, lowLatencyRateForRefresh(refreshHz), VrrFpsChoiceKind::LowLatencyVrr);
        }
        else {
            addChoice(choices, refreshHz, VrrFpsChoiceKind::Native);
        }
    }

    // A manually saved custom choice must remain visible.  Native FPS values
    // are deliberately not reintroduced while VRR is enabled, because that
    // would undermine the exact-native omission rule.
    if (savedFps > 0 && (!vrrEnabled || !isNativeRefreshRate(savedFps, refreshRates))) {
        addChoice(choices, savedFps, VrrFpsChoiceKind::Custom);
    }

    std::vector<VrrFpsChoice> result;
    result.reserve(choices.size());
    for (const auto& choice : choices) {
        result.push_back({choice.first, choice.second});
    }

    return result;
}
