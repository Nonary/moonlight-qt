#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Platform-neutral frame storage and channel routing shared with the tests.
// The caller serializes queue operations; no operation allocates memory.
namespace Spatial71 {
constexpr std::size_t ChannelCount = 8;
constexpr std::size_t SampleRate = 48000;
constexpr std::size_t QueueCapacityFrames = 1440; // 30 ms, independent of video FPS
constexpr std::size_t StartupFrames = 480;       // 10 ms
constexpr std::size_t MaxDecodeFrames = 5760;    // Opus maximum packet duration: 120 ms
constexpr std::size_t BytesPerFrame = ChannelCount * sizeof(float);

// Decoded PCM order: FL FR FC LFE BL BR SL SR. These are AudioObjectType
// values, NOT WAVEFORMATEXTENSIBLE speaker-mask values. The Windows renderer
// statically verifies the values against the SDK enums.
constexpr std::array<std::uint32_t, ChannelCount> StaticObjectTypes = {
    0x2, 0x4, 0x8, 0x10, 0x80, 0x100, 0x20, 0x40
};
constexpr std::uint32_t StaticObjectMask = 0x1FE;

inline bool bytesToFrames(int bytes, std::size_t capacityFrames, std::size_t& frames)
{
    frames = 0;
    if (bytes < 0 || static_cast<std::size_t>(bytes) % BytesPerFrame != 0) {
        return false;
    }
    const auto count = static_cast<std::size_t>(bytes) / BytesPerFrame;
    if (count > capacityFrames) {
        return false;
    }
    frames = count;
    return true;
}

inline bool deinterleave(const float* source, std::size_t frames,
                        std::size_t channel, float* destination)
{
    if (channel >= ChannelCount || frames > std::numeric_limits<std::size_t>::max() / ChannelCount ||
            (frames && (!source || !destination))) {
        return false;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        destination[frame] = source[frame * ChannelCount + channel];
    }
    return true;
}

struct QueueStatistics {
    std::uint64_t submittedFrames = 0;
    std::uint64_t consumedFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t missingFrames = 0;
    std::size_t highWaterFrames = 0;
};

class FrameQueue
{
public:
    // source must contain frames * ChannelCount samples (or frames must be 0).
    // Returns dropped old frames, including an oversized input's old prefix.
    std::size_t push(const float* source, std::size_t frames)
    {
        m_Statistics.submittedFrames += frames;
        std::size_t dropped = 0;
        if (frames > QueueCapacityFrames) {
            dropped = frames - QueueCapacityFrames;
            source += dropped * ChannelCount;
            frames = QueueCapacityFrames;
        }
        if (frames > QueueCapacityFrames - m_Size) {
            const auto discard = frames - (QueueCapacityFrames - m_Size);
            m_Head = (m_Head + discard) % QueueCapacityFrames;
            m_Size -= discard;
            dropped += discard;
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto destination = (m_Head + m_Size + frame) % QueueCapacityFrames;
            std::copy_n(source + frame * ChannelCount, ChannelCount,
                        m_Data.data() + destination * ChannelCount);
        }
        m_Size += frames;
        m_Statistics.droppedFrames += dropped;
        m_Statistics.highWaterFrames = (std::max)(m_Statistics.highWaterFrames, m_Size);
        return dropped;
    }

    // destination must hold frames * ChannelCount samples. Always zero-fills
    // missing frames, so an underrun cannot replay stale decoded audio.
    std::size_t pop(float* destination, std::size_t frames)
    {
        const auto copied = (std::min)(frames, m_Size);
        for (std::size_t frame = 0; frame < copied; ++frame) {
            const auto source = (m_Head + frame) % QueueCapacityFrames;
            std::copy_n(m_Data.data() + source * ChannelCount, ChannelCount,
                        destination + frame * ChannelCount);
        }
        if (frames > copied) {
            std::fill_n(destination + copied * ChannelCount,
                        (frames - copied) * ChannelCount, 0.0f);
        }
        m_Head = (m_Head + copied) % QueueCapacityFrames;
        m_Size -= copied;
        m_Statistics.consumedFrames += copied;
        m_Statistics.missingFrames += frames - copied;
        return copied;
    }

    std::size_t size() const { return m_Size; }
    QueueStatistics statistics() const { return m_Statistics; }

    // Starts a fresh queue and statistics epoch; storage need not be erased
    // because pop() never exposes unqueued samples.
    void clear() { m_Head = m_Size = 0; m_Statistics = {}; }

private:
    std::array<float, QueueCapacityFrames * ChannelCount> m_Data{};
    std::size_t m_Head = 0;
    std::size_t m_Size = 0;
    QueueStatistics m_Statistics;
};
} // namespace Spatial71
