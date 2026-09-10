#pragma once

#include <cstdint>

// Composition presentation statistics use interrupt time (100 ns), not QPC.
// Sample interrupt time inside a bracket on the worker's monotonic clock.
// Re-correlating each observation also avoids carrying an epoch across sleep.
struct PresentationClockSample {
    uint64_t timeUs = 0;
    uint64_t uncertaintyUs = 0;

    static PresentationClockSample translate(uint64_t displayed100ns,
                                            uint64_t reference100ns,
                                            uint64_t beforeUs,
                                            uint64_t afterUs)
    {
        if (!displayed100ns || displayed100ns > reference100ns ||
                afterUs < beforeUs || afterUs - beforeUs > 500) {
            return {};
        }
        const uint64_t age100ns = reference100ns - displayed100ns;
        // The live feedback matcher only admits events from the last 100 ms.
        if (age100ns > 1000000) {
            return {};
        }
        const uint64_t midpointUs = beforeUs + (afterUs - beforeUs) / 2;
        const uint64_t ageUs = age100ns / 10;
        if (ageUs >= midpointUs) {
            return {};
        }
        return {midpointUs - ageUs, (afterUs - beforeUs + 1) / 2 + 1};
    }
};
