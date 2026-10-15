#pragma once

#include <cstddef>
#include <cstdint>

namespace GamepadStickMerge {

struct Stick {
    short x = 0;
    short y = 0;
};

struct Sticks {
    Stick left;
    Stick right;
};

inline std::int64_t squaredMagnitude(Stick stick)
{
    // Widen before multiplying: (-32768, -32768) exceeds signed 32-bit.
    const std::int64_t x = stick.x;
    const std::int64_t y = stick.y;
    return x * x + y * y;
}

template<typename State>
Sticks select(const State* states, std::size_t count, short playerIndex)
{
    Sticks selected;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& state = states[i];
        if (state.controller == nullptr || state.index != playerIndex) {
            continue;
        }

        const Stick left {state.lsX, state.lsY};
        const Stick right {state.rsX, state.rsY};
        // Keep each vector intact. A greater component alone must not let
        // another controller's drift replace a strongly deflected stick.
        // Strict comparisons give equal magnitudes a stable lowest-slot tie.
        if (squaredMagnitude(left) > squaredMagnitude(selected.left)) {
            selected.left = left;
        }
        if (squaredMagnitude(right) > squaredMagnitude(selected.right)) {
            selected.right = right;
        }
    }
    return selected;
}

} // namespace GamepadStickMerge
