#pragma once

#include <cstdint>

struct AVFrame;

// Renderer-facing VRR contract. Pacing timestamps and sender metadata never
// cross this boundary: the presenter only prepares, adaptively presents, or
// abandons a decoded image.
enum class VrrFallbackReason : uint8_t {
    None,
    IneffectiveVsync,
    InvalidRefresh,
    UnsupportedRenderer,
    MainThreadRenderer,
    WindowsVulkan,
    AdaptivePresentationUnavailable,
    InsufficientHeadroom,
    InitializationFailed,
};

inline const char* vrrFallbackReasonName(VrrFallbackReason reason)
{
    switch (reason) {
    case VrrFallbackReason::None:
        return "none";
    case VrrFallbackReason::IneffectiveVsync:
        return "ineffective V-sync";
    case VrrFallbackReason::InvalidRefresh:
        return "invalid display refresh";
    case VrrFallbackReason::UnsupportedRenderer:
        return "unsupported renderer";
    case VrrFallbackReason::MainThreadRenderer:
        return "main-thread renderer";
    case VrrFallbackReason::WindowsVulkan:
        return "Windows Vulkan is not supported";
    case VrrFallbackReason::AdaptivePresentationUnavailable:
        return "adaptive presentation is unavailable";
    case VrrFallbackReason::InsufficientHeadroom:
        return "stream rate leaves insufficient adaptive-refresh headroom";
    case VrrFallbackReason::InitializationFailed:
        return "VRR initialization failed";
    }

    return "unknown fallback";
}

// presented means the native API accepted a display transition. cancelled
// describes why the operation ended, not whether the platform submitted: some
// APIs must submit an acquired image even while abandoning it.
struct VrrPresentFeedback {
    bool presented = false;
    bool cancelled = false;
    bool submissionTimeValid = false;
    uint64_t submissionTimeUs = 0;

    // Optional display-latch observation. A backend that can query when
    // submitted frames actually reached the display (e.g. DXGI frame
    // statistics, Wayland presentation-time, Metal presented handlers)
    // reports the most recent known latch here, translated onto the shared
    // pacing clock. The latch may describe an earlier submission than this
    // one; submissionId/latchSubmissionId associate the two. This is
    // observation only: it must never gate, delay, or fail a submission.
    bool submissionIdValid = false;
    uint64_t submissionId = 0;
    bool latchSampleValid = false;
    uint64_t latchSubmissionId = 0;
    uint64_t latchTimeUs = 0;
    uint64_t latchRefreshSequence = 0;

    // Opt-in, observation-only diagnostics captured immediately around the
    // native Present call. These fields must never affect presentation policy.
    bool nativePresentTimingValid = false;
    uint64_t nativePresentStartUs = 0;
    uint64_t nativePresentEndUs = 0;
    bool presentCountBeforeValid = false;
    uint64_t presentCountBefore = 0;
    bool frameStatsBeforeValid = false;
    uint64_t frameStatsBeforePresentCount = 0;
    uint64_t frameStatsBeforeTimeUs = 0;
    uint64_t frameStatsBeforeRefreshSequence = 0;
    // Optional renderer-readiness timing. A backend that queues GPU work in
    // prepareFrame() reports when that work became complete, proving the
    // later target wait operated on a displayable buffer rather than merely
    // on an accepted CPU Present call.
    bool gpuReadyTimingValid = false;
    uint64_t gpuReadyWaitStartUs = 0;
    uint64_t gpuReadyTimeUs = 0;
};

// Per-present request from the platform-neutral controller. When the learned
// source cadence leaves too little adaptive-refresh headroom for safe
// immediate flips, the controller asks for a latched (non-tearing) present:
// at near-refresh rates the cadence cost of latching is a few repeated frames
// per second while immediate flips tear. Backends whose presentation mode is
// immutable after swapchain creation may ignore the preference.
struct VrrPresentRequest {
    bool latchedPresentation = false;
    bool collectDiagnostics = false;
};

struct VrrPrepareResult {
    bool prepared = false;
    // Some acquired images (notably Vulkan swapchain frames) can only be
    // abandoned by submitting them. The worker owns any required wait.
    bool cancellationMaySubmit = false;
    VrrPresentFeedback feedback;
};

class IVrrFramePresenter {
public:
    virtual ~IVrrFramePresenter() = default;

    // Startup eligibility only. None means the presenter supports a worker-
    // thread split prepare/present path using its adaptive presentation mode.
    virtual VrrFallbackReason checkSupport() const = 0;

    // May acquire a swapchain image and submit rendering work, but must not
    // intentionally pace or wait for the worker's presentation target.
    virtual VrrPrepareResult prepareFrame(AVFrame* frame) = 0;

    // Presents the prepared image using the backend's adaptive presentation
    // path without intentionally waiting.
    virtual VrrPresentFeedback presentAdaptive(
        const VrrPresentRequest& request) = 0;

    // Releases a prepared image. The result must report a native submission
    // when the platform cannot abandon an acquired image without submitting.
    virtual VrrPresentFeedback cancelFrame()
    {
        VrrPresentFeedback feedback;
        feedback.cancelled = true;
        return feedback;
    }

    virtual void setSuspended(bool)
    {
    }

    // Called only when creating the VRR worker failed, before any frame was
    // handed to it or a legacy render worker was started.
    virtual bool restoreFixedPresentation(VrrFallbackReason)
    {
        return false;
    }
};
