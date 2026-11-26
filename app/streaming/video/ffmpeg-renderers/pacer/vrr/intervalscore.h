#pragma once

#include <algorithm>
#include <cstdint>

namespace Vrr13 {
constexpr uint64_t IntervalScoreBucketUs = 100000;

inline double intervalLossFraction(uint64_t errorUs, uint64_t intendedUs, uint64_t toleranceUs)
{
    const double excessUs = errorUs > toleranceUs ? double(errorUs - toleranceUs) : 0.0;
    return std::min(1.0, excessUs / intendedUs);
}
}
