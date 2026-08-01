#pragma once

#include <cstdint>

// Timing feedback for a frame that the platform has confirmed reached the
// display scanout. Both timestamps use LiGetMicroseconds()'s local monotonic
// epoch, so their difference is safe without any host/client clock sync.
struct DisplayedFrameTiming {
    uint64_t frameReadyUs = 0;
    uint64_t displayTimeUs = 0;
};
