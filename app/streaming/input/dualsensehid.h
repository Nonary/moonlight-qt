// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "SDL_compat.h"
#include <cstddef>
#include <cstdint>
#include <memory>

// Output-only handle for the exact controller opened by SDL. Input and ordinary
// effects stay with SDL. Implementations must not leave asynchronous writes
// referencing the caller's buffer after write() returns.
class DualSenseHidOutput {
public:
    virtual ~DualSenseHidOutput() = default;
    virtual bool write(const uint8_t* report, size_t size) = 0;
};

std::unique_ptr<DualSenseHidOutput> openDualSenseBluetoothOutput(SDL_GameController* controller);
