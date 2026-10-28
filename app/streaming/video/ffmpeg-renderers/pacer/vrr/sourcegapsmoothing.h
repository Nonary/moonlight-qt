#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Vrr13 {

// Correct only source-gap error beyond T, by at most another T. Retain the
// prior phase on clean gaps so their intervals stay unchanged, and bound the
// entire retained phase to T so consecutive corrections cannot accumulate.
inline int64_t sourceGapSmoothingUs(uint64_t gapUs, uint64_t referenceUs,
                                  uint64_t toleranceUs, int64_t previousUs)
{
    const auto limit = static_cast<int64_t>(std::min<uint64_t>(
        toleranceUs, uint64_t(std::numeric_limits<int64_t>::max() / 2)));
    previousUs = std::clamp(previousUs, -limit, limit);
    const auto error = gapUs > referenceUs ? gapUs - referenceUs : referenceUs - gapUs;
    const auto excess = static_cast<int64_t>(std::min<uint64_t>(
        uint64_t(limit), error > toleranceUs ? error - toleranceUs : 0));
    return std::clamp(previousUs + (gapUs > referenceUs ? -excess : excess), -limit, limit);
}

} // namespace Vrr13
