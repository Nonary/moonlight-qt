#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

// Event notifications wake the CPU; only the fence value proves that a decoder
// surface/back buffer is safe to reuse. Keep the original 50 ms total budget,
// but check completion between short blocking waits so a delayed notification
// cannot manufacture a 50 ms stall followed by a decoder reset.
namespace D3D11FenceWait {
enum class Status { Complete, Timeout, WaitFailed, DeviceRemoved };
struct Result {
    Status status;
    uint64_t completedValue;
};

template<class Clock, class Poll, class Wait>
Result wait(uint64_t target, Clock&& clock, Poll&& poll, Wait&& waitEvent)
{
    const auto start = clock();
    uint64_t completed = 0;
    // The iteration bound also handles a stalled clock or stale signalled
    // event. Normally each unsuccessful event wait blocks for one millisecond.
    for (unsigned attempts = 0; attempts <= 100; ++attempts) {
        completed = poll();
        if (completed == (std::numeric_limits<uint64_t>::max)())
            return {Status::DeviceRemoved, completed};
        if (completed >= target) return {Status::Complete, completed};
        const auto now = clock();
        if (now < start || now - start >= 50000 || attempts == 100)
            return {Status::Timeout, completed};
        if (!waitEvent(1)) return {Status::WaitFailed, completed};
    }
    return {Status::Timeout, completed};
}
}
