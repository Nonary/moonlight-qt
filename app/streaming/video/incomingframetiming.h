#pragma once

#include <cstdint>

// Decoder-owned source cadence measurement. Only raw host RTP timestamps enter
// this metric; receive, decode, and presentation timing cannot affect it.
class IncomingFrameTiming
{
public:
    enum class Sample { Unavailable, Smooth, Uneven };

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
            return Sample::Unavailable;
        }

        const uint32_t previousInterval = m_PreviousInterval;
        m_PreviousInterval = interval;
        if (!m_HaveInterval) {
            m_HaveInterval = true;
            return Sample::Unavailable;
        }

        const uint32_t change = interval > previousInterval ?
            interval - previousInterval : previousInterval - interval;
        // 270 ticks at 90 kHz is exactly 3 ms. A stable low frame rate is
        // smooth; long source stalls are included, without a 25 ms exclusion.
        return change > 270 ? Sample::Uneven : Sample::Smooth;
    }

private:
    uint32_t m_PreviousFrame = 0;
    uint32_t m_PreviousTimestamp = 0;
    uint32_t m_PreviousInterval = 0;
    bool m_HaveFrame = false;
    bool m_HaveInterval = false;
};
