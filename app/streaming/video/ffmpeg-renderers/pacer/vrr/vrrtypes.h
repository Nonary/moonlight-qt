#pragma once

// The VRR pacing path deliberately keeps its data contract separate from
// FFmpeg's PTS/DTS fields.  Those fields are decoder-owned and are still used
// by the legacy pacing path, while these values describe the frame as it
// crossed the decoder/pacer boundary.

#include <cstdint>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
}

struct VrrSessionConfig {
    int displayRefreshHz = 0;
    int streamRateHz = 0;
};

// A move-only frame record.  Decoder completion is captured while the
// corresponding DECODE_UNIT is still available.  A raw RTP timestamp of zero
// is valid; timestampValid is intentionally separate to make that explicit.
class PacedFrame {
public:
    PacedFrame() = default;

    PacedFrame(AVFrame* frame,
               int frameNumber,
               uint32_t rtpTimestamp,
               bool timestampValid,
               uint64_t decodeCompleteUs) :
        m_Frame(frame),
        m_FrameNumber(frameNumber),
        m_RtpTimestamp(rtpTimestamp),
        m_TimestampValid(timestampValid),
        m_DecodeCompleteUs(decodeCompleteUs)
    {
    }

    ~PacedFrame()
    {
        reset();
    }

    PacedFrame(const PacedFrame&) = delete;
    PacedFrame& operator=(const PacedFrame&) = delete;

    PacedFrame(PacedFrame&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    PacedFrame& operator=(PacedFrame&& other) noexcept
    {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    AVFrame* frame() const
    {
        return m_Frame;
    }

    AVFrame* release()
    {
        AVFrame* frame = m_Frame;
        m_Frame = nullptr;
        return frame;
    }

    explicit operator bool() const
    {
        return m_Frame != nullptr;
    }

    int frameNumber() const
    {
        return m_FrameNumber;
    }

    uint32_t rtpTimestamp() const
    {
        return m_RtpTimestamp;
    }

    bool timestampValid() const
    {
        return m_TimestampValid;
    }

    uint64_t decodeCompleteUs() const
    {
        return m_DecodeCompleteUs;
    }

    void reset()
    {
        if (m_Frame != nullptr) {
            av_frame_free(&m_Frame);
        }
    }

private:
    void moveFrom(PacedFrame&& other)
    {
        m_Frame = other.m_Frame;
        m_FrameNumber = other.m_FrameNumber;
        m_RtpTimestamp = other.m_RtpTimestamp;
        m_TimestampValid = other.m_TimestampValid;
        m_DecodeCompleteUs = other.m_DecodeCompleteUs;
        other.m_Frame = nullptr;
    }

    AVFrame* m_Frame = nullptr;
    int m_FrameNumber = -1;
    uint32_t m_RtpTimestamp = 0;
    bool m_TimestampValid = false;
    uint64_t m_DecodeCompleteUs = 0;
};
