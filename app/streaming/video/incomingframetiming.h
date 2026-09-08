#pragma once

#include <cstdint>

// Decoder-owned source cadence measurement. Only raw host RTP timestamps enter
// this metric; receive, decode, and presentation timing cannot affect it.
class IncomingFrameTiming
{
public:
    struct Sample {
        uint32_t changeTicks = 0;
        uint32_t referenceTicks = 0; // Zero means unavailable.
    };

    // Ratio of shared interval duration to the longer duration, accumulated
    // across the window. Every change contributes its magnitude, without a
    // hitch threshold or per-sample rounding. Longer stalls carry more weight.
    static double smoothnessPercent(uint64_t changeTicks, uint64_t referenceTicks)
    {
        return referenceTicks == 0 ? 0.0 :
            100.0 * (1.0 - double(changeTicks) / double(referenceTicks));
    }

    Sample observe(uint32_t frameNumber, uint32_t rtpTimestamp)
    {
        const uint32_t interval = rtpTimestamp - m_PreviousTimestamp;
        const bool adjacent = m_HaveFrame && frameNumber - m_PreviousFrame == 1;
        m_PreviousFrame = frameNumber;
        m_PreviousTimestamp = rtpTimestamp;
        m_HaveFrame = true;

        // Unsigned subtraction handles RTP/frame-number wrap. Missing frames,
        // absent/repeated timestamps, and backwards timestamps break the chain.
        if (!adjacent || interval == 0 || interval >= 0x80000000U) {
            m_HaveInterval = false;
            return {};
        }

        const uint32_t previousInterval = m_PreviousInterval;
        m_PreviousInterval = interval;
        if (!m_HaveInterval) {
            m_HaveInterval = true;
            return {};
        }

        const uint32_t change = interval > previousInterval ?
            interval - previousInterval : previousInterval - interval;
        // Comparing adjacent intervals preserves steady low FPS. Normalizing
        // by their maximum keeps the aggregate score within 0..100% naturally.
        return {change, interval > previousInterval ? interval : previousInterval};
    }

private:
    uint32_t m_PreviousFrame = 0;
    uint32_t m_PreviousTimestamp = 0;
    uint32_t m_PreviousInterval = 0;
    bool m_HaveFrame = false;
    bool m_HaveInterval = false;
};
