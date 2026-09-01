#include "clientdisplaycapabilities.h"

#include <cmath>

namespace
{
constexpr double kMinPeakLuminanceNits = 1.0;
constexpr double kMaxPeakLuminanceNits = 100000.0;
}

std::optional<int> ClientDisplayCapabilities::normalizePeakLuminance(const double nits)
{
    if (!std::isfinite(nits) || nits < kMinPeakLuminanceNits ||
            nits > kMaxPeakLuminanceNits) {
        return std::nullopt;
    }

    return static_cast<int>(std::round(nits));
}
