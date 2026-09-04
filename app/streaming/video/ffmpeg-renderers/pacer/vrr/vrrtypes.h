#pragma once

// The VRR pacing path deliberately keeps its data contract separate from
// FFmpeg's PTS/DTS fields.  Those fields are decoder-owned and are still used
// by the legacy pacing path, while these values describe the frame as it
// crossed the decoder/pacer boundary.

#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

struct VrrSessionConfig {
    int displayRefreshHz = 0;
    int streamRateHz = 0;
    bool allowAdditionalQueuedFrame = false;
    // Re-present the last frame inside a gap longer than the panel's
    // adaptive-refresh floor; zero Hz disables it.
    bool gapFillEnabled = false;
    int gapFillMinimumRefreshHz = 0;
    // A session preference resolved into the recorded controller parameters.
    // Both modes retain jitter buffering and display-spacing protection.
    bool smoothFrameTiming = true;
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

    PacedFrame(PacedFrame&&) noexcept = default;
    PacedFrame& operator=(PacedFrame&&) noexcept = default;

    AVFrame* frame() const
    {
        return m_Frame.get();
    }

    AVFrame* release()
    {
        return m_Frame.release();
    }

    // The decoder's GPU work was observed to finish after the CPU reported
    // completion: readiness moves to that later time.
    void noteGpuReadyUs(uint64_t gpuReadyUs)
    {
        if (gpuReadyUs > m_DecodeCompleteUs) {
            m_DecodeCompleteUs = gpuReadyUs;
        }
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

    // Pre-decode timeline of the same frame, all on the LiGetMicroseconds()
    // clock: first packet received from the network, complete frame
    // reassembled (queued for the decoder), and packet handed to the decoder.
    // They exist so a late decode-complete can be attributed to the network,
    // the depacketizer, or the decoder instead of inferred. Zero means the
    // producer did not supply them.
    void setDeliveryTimeline(uint64_t receiveUs,
                             uint64_t reassembledUs,
                             uint64_t decodeSubmitUs)
    {
        m_ReceiveUs = receiveUs;
        m_ReassembledUs = reassembledUs;
        m_DecodeSubmitUs = decodeSubmitUs;
    }

    uint64_t receiveUs() const
    {
        return m_ReceiveUs;
    }

    uint64_t reassembledUs() const
    {
        return m_ReassembledUs;
    }

    uint64_t decodeSubmitUs() const
    {
        return m_DecodeSubmitUs;
    }

    void setDecodeBoundary(uint64_t decodeBoundary)
    {
        m_DecodeBoundary = decodeBoundary;
    }

    uint64_t decodeBoundary() const
    {
        return m_DecodeBoundary;
    }

private:
    struct FrameDeleter {
        void operator()(AVFrame* frame) const
        {
            av_frame_free(&frame);
        }
    };

    std::unique_ptr<AVFrame, FrameDeleter> m_Frame;
    int m_FrameNumber = -1;
    uint32_t m_RtpTimestamp = 0;
    bool m_TimestampValid = false;
    uint64_t m_DecodeCompleteUs = 0;
    uint64_t m_ReceiveUs = 0;
    uint64_t m_ReassembledUs = 0;
    uint64_t m_DecodeSubmitUs = 0;
    uint64_t m_DecodeBoundary = 0;
};
