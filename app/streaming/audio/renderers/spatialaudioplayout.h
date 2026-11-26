#pragma once

#include "spatialaudioqueue.h"
#include <algorithm>
#include <cstdint>

namespace Spatial71 {
// Called under the renderer queue lock, by both the decode callback and the
// device worker. Contains the actual startup/rebuffer decision, not a model.
struct PlayoutState {
    bool primed = false;
    bool everPlayed = false;
    std::int64_t firstQueued = 0;
    std::uint64_t startupSilenceFrames = 0;
    std::uint64_t underrunFrames = 0;

    void queued(std::size_t previousSize, std::int64_t now) noexcept {
        if (!primed && !previousSize) firstQueued = now;
    }

    void render(FrameQueue& queue, float* output, std::size_t frames,
                std::int64_t now, std::int64_t frequency) noexcept {
        if (!frames) return;
        if (!primed && queue.size() &&
            (queue.size() >= StartupFrames ||
             (firstQueued && now - firstQueued >= frequency / 100))) primed = true;
        if (primed) {
            const auto copied = queue.pop(output, frames);
            everPlayed = everPlayed || copied != 0;
            underrunFrames += frames - copied;
            // An exactly served quantum is not starvation. The next packet
            // may arrive before the next device event without rebuffering.
            if (copied < frames) {
                primed = false;
                firstQueued = 0;
            }
        }
        else {
            std::fill_n(output, frames * queue.channelCount(), 0.0f);
            if (everPlayed) underrunFrames += frames;
            else startupSilenceFrames += frames;
        }
    }
};
}
