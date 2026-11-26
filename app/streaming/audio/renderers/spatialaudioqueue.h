#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Platform-neutral frame storage and channel routing shared with the tests.
// The caller serializes queue operations; no operation allocates memory.
namespace Spatial71 {
constexpr std::size_t ChannelCount = 8; // Preserve the default 7.1 layout
constexpr std::size_t MaxChannelCount = 12; // Storage only; never a PCM stride
constexpr std::size_t SampleRate = 48000;
constexpr std::size_t QueueCapacityFrames = 1440; // 30 ms, independent of video FPS
constexpr std::size_t StartupFrames = 480;       // 10 ms
constexpr std::size_t MaxDecodeFrames = 5760;    // Opus maximum packet duration: 120 ms
constexpr std::size_t BytesPerFrame = ChannelCount * sizeof(float);

// Decoded PCM order: FL FR FC LFE BL BR [SL SR [TFL TFR TBL TBR]]. Moonlight's 5.1 channel
// mask is 0x3F, so the six-channel layout uses the BACK pair, not the SIDE
// pair. These are AudioObjectType
// values, NOT WAVEFORMATEXTENSIBLE speaker-mask values. The Windows renderer
// statically verifies the values against the SDK enums.
constexpr std::array<std::uint32_t, MaxChannelCount> StaticObjectTypes = {
    0x2, 0x4, 0x8, 0x10, 0x80, 0x100, 0x20, 0x40, 0x200, 0x400, 0x800, 0x1000
};
constexpr std::uint32_t StaticObjectMask = 0x1FE; // Legacy 7.1
constexpr std::uint32_t StaticObjectMask714 = 0x1FFE;
// These are separate identifier spaces. The wire value is never a native mask.
constexpr std::uint32_t WaveSpeakerMask714 = 0x2D63F;
constexpr std::uint16_t FoundationWireMask714 = 0xF63F;
constexpr std::array<const char*, MaxChannelCount> ChannelLabels = {
    "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR", "TFL", "TFR", "TBL", "TBR"
};

constexpr bool supportedChannelCount(std::size_t channels)
{
    return channels == 6 || channels == ChannelCount || channels == MaxChannelCount;
}

constexpr std::uint32_t staticObjectMask(std::size_t channels)
{
    return channels == 6 ? 0x19E : channels == ChannelCount ? StaticObjectMask :
           channels == MaxChannelCount ? StaticObjectMask714 : 0;
}

inline bool bytesToFrames(int bytes, std::size_t capacityFrames, std::size_t& frames,
                          std::size_t channels = ChannelCount)
{
    frames = 0;
    if (!supportedChannelCount(channels) || bytes < 0) {
        return false;
    }
    const auto bytesPerFrame = channels * sizeof(float);
    if (static_cast<std::size_t>(bytes) % bytesPerFrame != 0) return false;
    const auto count = static_cast<std::size_t>(bytes) / bytesPerFrame;
    if (count > capacityFrames) {
        return false;
    }
    frames = count;
    return true;
}

inline bool deinterleave(const float* source, std::size_t frames,
                        std::size_t channel, float* destination,
                        std::size_t channels = ChannelCount)
{
    if (!supportedChannelCount(channels) || channel >= channels ||
            frames > std::numeric_limits<std::size_t>::max() / channels ||
            (frames && (!source || !destination))) {
        return false;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        destination[frame] = source[frame * channels + channel];
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
    // Configure only before streaming, while the caller owns the queue. A
    // successful layout change starts a fresh data/statistics epoch. Invalid
    // layouts leave the current layout, data, and statistics untouched.
    bool setChannelCount(std::size_t channels)
    {
        if (!supportedChannelCount(channels)) return false;
        clear();
        m_ChannelCount = channels;
        return true;
    }

    std::size_t channelCount() const { return m_ChannelCount; }

    // source must contain frames * channelCount() samples (or frames must be 0).
    // Returns dropped old frames, including an oversized input's old prefix.
    std::size_t push(const float* source, std::size_t frames)
    {
        m_Statistics.submittedFrames += frames;
        std::size_t dropped = 0;
        if (frames > QueueCapacityFrames) {
            dropped = frames - QueueCapacityFrames;
            source += dropped * m_ChannelCount;
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
            std::copy_n(source + frame * m_ChannelCount, m_ChannelCount,
                        m_Data.data() + destination * m_ChannelCount);
        }
        m_Size += frames;
        m_Statistics.droppedFrames += dropped;
        m_Statistics.highWaterFrames = (std::max)(m_Statistics.highWaterFrames, m_Size);
        return dropped;
    }

    // destination must hold frames * channelCount() samples. Always zero-fills
    // missing frames, so an underrun cannot replay stale decoded audio.
    std::size_t pop(float* destination, std::size_t frames)
    {
        const auto copied = (std::min)(frames, m_Size);
        for (std::size_t frame = 0; frame < copied; ++frame) {
            const auto source = (m_Head + frame) % QueueCapacityFrames;
            std::copy_n(m_Data.data() + source * m_ChannelCount, m_ChannelCount,
                        destination + frame * m_ChannelCount);
        }
        if (frames > copied) {
            std::fill_n(destination + copied * m_ChannelCount,
                        (frames - copied) * m_ChannelCount, 0.0f);
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
    std::array<float, QueueCapacityFrames * MaxChannelCount> m_Data{};
    std::size_t m_ChannelCount = ChannelCount;
    std::size_t m_Head = 0;
    std::size_t m_Size = 0;
    QueueStatistics m_Statistics;
};

// Used while holding the frame queue's mutex. The deadline starts at the
// first packet of an unprimed interval; time is in the caller's QPC ticks.
class PlaybackGate {
    bool primed = false;
    std::int64_t firstQueued = 0;
public:
    bool waitingForStartup() const { return !primed; }
    void onFirstPacket(std::int64_t now) { firstQueued = now; }

    std::size_t read(FrameQueue& queue, float* output, std::size_t frames,
                     std::int64_t now, std::int64_t startupTicks) {
        if (!frames) return 0;
        if (!primed && queue.size() &&
            (queue.size() >= StartupFrames || now - firstQueued >= startupTicks))
            primed = true;
        if (!primed) {
            std::fill_n(output, frames * queue.channelCount(), 0.f);
            return 0;
        }
        const auto copied = queue.pop(output, frames);
        // An exact drain is healthy. The next packet may still arrive before
        // the next render event. Re-prime only after an actual underrun.
        if (copied < frames) primed = false;
        return copied;
    }
};
} // namespace Spatial71
