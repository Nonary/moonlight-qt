#pragma once

// Test-only fixtures for the greenfield VRR worker.  They deliberately model
// just the IVrrFramePresenter boundary: no window, decoder, network, or
// graphics device is created by these tests.

#include "../../app/streaming/video/ffmpeg-renderers/ivrrframepresenter.h"

#include <Limelight.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/mem.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class FakeVrrFramePresenter final : public IVrrFramePresenter {
public:
    VrrFallbackReason checkSupport() const override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Support;
    }

    VrrPrepareResult prepareFrame(AVFrame* frame) override
    {
        VrrPrepareResult result;
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_PreparedFrame = frame;
        const int frameNumber = frame == nullptr ? -1 :
            static_cast<int>(reinterpret_cast<intptr_t>(frame->opaque));
        m_PreparedFrames.push_back(frameNumber);
        ++m_PrepareCount;
        m_Condition.notify_all();

        while (m_BlockPreparation && !m_ReleasePreparation) {
            m_Condition.wait(lock);
        }

        result.prepared = m_PreparationSucceeds && frame != nullptr;
        result.cancellationMaySubmit =
            m_CancellationMaySubmit && frame != nullptr;
        if (!result.prepared && !result.cancellationMaySubmit) {
            m_PreparedFrame = nullptr;
        }
        return result;
    }

    VrrPresentFeedback presentAdaptive(
        const VrrPresentRequest& request) override
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_PresentRequests.push_back(request);
        }
        VrrPresentFeedback feedback;
        int frameNumber = -1;
        uint64_t preSubmissionDelayUs = 0;
        uint64_t postSubmissionDelayUs = 0;
        bool presentCancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_PreparedFrame == nullptr) {
                feedback.cancelled = true;
                return feedback;
            }

            frameNumber = static_cast<int>(
                reinterpret_cast<intptr_t>(m_PreparedFrame->opaque));
            m_PreparedFrame = nullptr;
            preSubmissionDelayUs = m_PreSubmissionDelayUs;
            postSubmissionDelayUs = m_PresentDelayUs;
            presentCancelled = m_PresentCancelled;
        }

        // Model native work before and after the actual submission boundary.
        if (preSubmissionDelayUs != 0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(preSubmissionDelayUs));
        }
        const uint64_t submissionTimeUs = LiGetMicroseconds();

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_PresentCallTimesUs.push_back(submissionTimeUs);
        }

        if (postSubmissionDelayUs != 0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(postSubmissionDelayUs));
        }

        feedback.presented = true;
        feedback.cancelled = presentCancelled;
        feedback.submissionTimeValid = true;
        feedback.submissionTimeUs = submissionTimeUs;

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_PresentedFrames.push_back(frameNumber);
            m_PresentReturnTimesUs.push_back(LiGetMicroseconds());
            ++m_PresentCount;
            m_Condition.notify_all();
        }
        return feedback;
    }

    VrrPresentFeedback cancelFrame() override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        VrrPresentFeedback feedback;
        feedback.cancelled = true;
        if (m_CancelSubmits && m_PreparedFrame != nullptr) {
            const int frameNumber = static_cast<int>(
                reinterpret_cast<intptr_t>(m_PreparedFrame->opaque));
            const uint64_t callTimeUs = LiGetMicroseconds();
            m_PresentedFrames.push_back(frameNumber);
            m_PresentCallTimesUs.push_back(callTimeUs);
            m_PresentReturnTimesUs.push_back(callTimeUs);
            ++m_PresentCount;
            feedback.presented = true;
            feedback.submissionTimeValid = true;
            feedback.submissionTimeUs = callTimeUs;
        }
        m_PreparedFrame = nullptr;
        ++m_CancelCount;
        m_Condition.notify_all();
        return feedback;
    }

    void setSuspended(bool suspended) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        suspended ? ++m_SuspendedCount : ++m_ResumedCount;
        m_Condition.notify_all();
    }

    void setSupport(VrrFallbackReason support)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Support = support;
    }

    void setCancelSubmits(bool enabled)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_CancelSubmits = enabled;
    }

    void setPreparationSucceeds(bool succeeds)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PreparationSucceeds = succeeds;
    }

    void setCancellationMaySubmit(bool maySubmit)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_CancellationMaySubmit = maySubmit;
    }

    void setPresentDelayUs(uint64_t delayUs)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PresentDelayUs = delayUs;
    }

    void setPreSubmissionDelayUs(uint64_t delayUs)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PreSubmissionDelayUs = delayUs;
    }

    void setPresentCancelled(bool cancelled)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PresentCancelled = cancelled;
    }

    void blockPreparation()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_BlockPreparation = true;
        m_ReleasePreparation = false;
    }

    void releasePreparation()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_BlockPreparation = false;
        m_ReleasePreparation = true;
        m_Condition.notify_all();
    }

    bool waitForPrepareCount(size_t count,
                             std::chrono::milliseconds timeout =
                                 std::chrono::milliseconds(2000))
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Condition.wait_for(lock, timeout, [&] {
            return m_PrepareCount >= count;
        });
    }

    bool waitForPresentCount(size_t count,
                             std::chrono::milliseconds timeout =
                                 std::chrono::milliseconds(2000))
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Condition.wait_for(lock, timeout, [&] {
            return m_PresentCount >= count;
        });
    }

    bool waitForCancelCount(size_t count,
                            std::chrono::milliseconds timeout =
                                std::chrono::milliseconds(2000))
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Condition.wait_for(lock, timeout, [&] {
            return m_CancelCount >= count;
        });
    }

    size_t prepareCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_PrepareCount;
    }

    size_t presentCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_PresentCount;
    }

    size_t cancelCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_CancelCount;
    }

    size_t resumedCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_ResumedCount;
    }

    size_t suspendedCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_SuspendedCount;
    }

    std::vector<int> presentedFrames() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_PresentedFrames;
    }

    std::vector<uint64_t> presentCallTimesUs() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_PresentCallTimesUs;
    }

    std::vector<uint64_t> presentReturnTimesUs() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_PresentReturnTimesUs;
    }

    std::vector<VrrPresentRequest> presentRequests() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_PresentRequests;
    }

private:
    mutable std::mutex m_Mutex;
    std::condition_variable m_Condition;
    VrrFallbackReason m_Support = VrrFallbackReason::NoFallback;
    bool m_PreparationSucceeds = true;
    bool m_CancellationMaySubmit = false;
    bool m_CancelSubmits = false;
    bool m_BlockPreparation = false;
    bool m_ReleasePreparation = false;
    bool m_PresentCancelled = false;
    uint64_t m_PreSubmissionDelayUs = 0;
    uint64_t m_PresentDelayUs = 0;
    size_t m_PrepareCount = 0;
    size_t m_PresentCount = 0;
    size_t m_CancelCount = 0;
    size_t m_SuspendedCount = 0;
    size_t m_ResumedCount = 0;
    AVFrame* m_PreparedFrame = nullptr;
    std::vector<int> m_PreparedFrames;
    std::vector<int> m_PresentedFrames;
    std::vector<VrrPresentRequest> m_PresentRequests;
    std::vector<uint64_t> m_PresentCallTimesUs;
    std::vector<uint64_t> m_PresentReturnTimesUs;
};

struct TrackedFrameLifetime {
    std::atomic_uint releases { 0 };
};

inline void releaseTrackedFrameBuffer(void* opaque, uint8_t* data)
{
    static_cast<TrackedFrameLifetime*>(opaque)->releases.fetch_add(1);
    av_free(data);
}

inline PacedFrame makeTrackedPacedFrame(int frameNumber,
                                        uint32_t rtpTimestamp,
                                        uint64_t decodeCompleteUs,
                                        TrackedFrameLifetime& lifetime)
{
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        return {};
    }

    uint8_t* data = static_cast<uint8_t*>(av_malloc(1));
    if (data == nullptr) {
        av_frame_free(&frame);
        return {};
    }

    frame->buf[0] = av_buffer_create(data, 1, releaseTrackedFrameBuffer,
                                     &lifetime, 0);
    if (frame->buf[0] == nullptr) {
        av_free(data);
        av_frame_free(&frame);
        return {};
    }

    frame->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(frameNumber));

    return PacedFrame(frame, frameNumber, rtpTimestamp, true, decodeCompleteUs);
}

// Portable contract probes. These are deliberately not substitutes for the
// native D3D11/libplacebo integration tests; they state the immutable request
// mapping used by test fakes when those APIs are unavailable on the host.
constexpr unsigned int kFakeDxgiPresentAllowTearing = 0x00000200U;

inline unsigned int expectedDxgiVrrPresentFlags()
{
    return kFakeDxgiPresentAllowTearing;
}

enum class FakeLinuxPresentationBackend {
    Wayland,
    X11,
    Gamescope,
    KmsDrm,
    Other,
};

enum class FakeLinuxPresentationMode {
    Mailbox,
    Immediate,
    Fifo,
};

inline FakeLinuxPresentationMode expectedLinuxPresentationMode(
    FakeLinuxPresentationBackend backend,
    bool requestedModeIsSupported)
{
    if (!requestedModeIsSupported) {
        return FakeLinuxPresentationMode::Fifo;
    }

    switch (backend) {
    case FakeLinuxPresentationBackend::Wayland:
        return FakeLinuxPresentationMode::Mailbox;
    case FakeLinuxPresentationBackend::X11:
    case FakeLinuxPresentationBackend::Gamescope:
    case FakeLinuxPresentationBackend::KmsDrm:
        return FakeLinuxPresentationMode::Immediate;
    case FakeLinuxPresentationBackend::Other:
        return FakeLinuxPresentationMode::Fifo;
    }

    return FakeLinuxPresentationMode::Fifo;
}
