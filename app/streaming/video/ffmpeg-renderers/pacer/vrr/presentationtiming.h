#pragma once

#include <cstdint>

namespace Vrr13 {

// A successful native query does not establish the meaning of its timestamp.
// In particular, a DXGI refresh reference can precede the associated Present.
enum class PresentationTimeKind : uint8_t {
    Unavailable = 0,
    RefreshReference = 1,
    DisplayEvent = 2,
};

}
