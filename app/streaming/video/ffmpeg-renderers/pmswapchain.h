#pragma once

// Deliberately NOT WIN32_LEAN_AND_MEAN for consistency with vrrpresenter.h,
// which may share a translation unit and needs the full windows.h.
#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <presentation.h>

#include <wrl/client.h>

#include <stdint.h>

// The "Smoothest VRR (OS-scheduled presentation)" present path: presents
// through the Windows 11 composition swapchain API (IPresentationManager)
// instead of a DXGI swapchain. Each present carries a target time - the
// instant the cadence pacer wants the flip - and the OS schedules the
// flip itself (a present becomes "ready" once GPU work completes AND the
// target time arrives). This replaces the VrrPresenter's app-side target
// hold + scanline alignment with kernel-scheduled, tear-free flips at any
// content rate, at the cost of the kernel's fixed ~2-vsync present
// pipeline (roughly one extra frame of display latency that app-side
// stats cannot observe).
//
// IIndependentFlipFramePresentStatistics reports the true displayed time
// of every independent-flip present; the 10s "PM present stats" log lines
// diff those against the pacer's wanted instants (glass-interval jitter =
// the judder metric). If presents come back as composition frames instead
// of iflip frames, DWM is re-quantizing timing - logged explicitly, and
// init fails over to the DXGI path when independent flip isn't available.
class PmSwapchain
{
public:
    PmSwapchain();
    ~PmSwapchain();

    // Creates the presentation factory/manager/surface, the displayable
    // buffer ring, and a DirectComposition tree over the window. Returns
    // false with logging on any failure; the caller falls back to DXGI.
    // Requires independent-flip support unless MOONLIGHT_PM_ALLOW_COMPOSED=1.
    bool initialize(ID3D11Device* device, HWND window,
                    int width, int height, DXGI_FORMAT format);

    bool initialized() const
    {
        return m_Manager != nullptr;
    }

    // Waits for a presentation buffer to leave the OS's outstanding-present
    // set and returns its render target view (owned by this class). Returns
    // nullptr on timeout or device loss - check isLost() to distinguish.
    // Safe to call without the renderer's context lock; the wait time is
    // reported through popAcquireWaitUs() so the pacer's render-time
    // estimate doesn't absorb it.
    ID3D11RenderTargetView* acquireBuffer();

    bool isLost() const
    {
        return m_Lost;
    }

    void setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace, bool isHdr);

    // Queues the last-acquired buffer for display at targetUs in the
    // LiGetMicroseconds() clock domain (0 or past = as soon as possible).
    // The caller must Flush() the D3D context first: without a DXGI
    // Present() to implicitly flush, queued GPU work can sit unsubmitted
    // and the present would never become ready. Returns false on
    // PRESENTATION_ERROR_LOST or other fatal errors.
    bool present(uint64_t targetUs);

    // The instant the last present should hit the screen (the target, or
    // the present call time for ASAP presents). Fed to
    // VrrPresenter::notePresent() so the pacer's flip-spacing floor tracks
    // expected flips rather than our (deliberately early) present calls.
    uint64_t lastExpectedFlipUs() const
    {
        return m_LastExpectedFlipUs;
    }

    // Cumulative time acquireBuffer() spent blocked since last read.
    uint64_t popAcquireWaitUs();

private:
    struct PendingPresent;
    void drainStatistics();
    void updateServo(int64_t netErrorUs);
    void recordTargetError(double errorMs);
    void logStatsIfDue(uint64_t nowUs);
    const PendingPresent* lookupPending(UINT64 presentId);

    static constexpr int NUM_BUFFERS = 6; // allocation max; runtime count below
    static constexpr int PENDING_RING = 128;

    // MOONLIGHT_PM_BUFFERS (2-6, default 3): the OS can hold at most
    // ~count-1 presents in flight. Measured: buffer count has no effect on
    // the kernel's fixed present pipeline, and 2-3 buffers show zero
    // acquire waits - 3 keeps one frame of hitch headroom over the minimum.
    int m_BufferCount;

    HMODULE m_DcompModule;
    HRESULT (WINAPI* m_pCreatePresentationFactory)(IUnknown*, REFIID, void**);
    void (WINAPI* m_pQueryInterruptTimePrecise)(PULONGLONG);

    Microsoft::WRL::ComPtr<IPresentationManager> m_Manager;
    Microsoft::WRL::ComPtr<IPresentationSurface> m_Surface;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Textures[NUM_BUFFERS];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_Rtvs[NUM_BUFFERS];
    Microsoft::WRL::ComPtr<IPresentationBuffer> m_Buffers[NUM_BUFFERS];
    HANDLE m_AvailableEvents[NUM_BUFFERS];
    HANDLE m_LostEvent;
    HANDLE m_StatsEvent;
    HANDLE m_CompSurfaceHandle;

    Microsoft::WRL::ComPtr<IDCompositionDevice> m_DcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_DcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_DcompVisual;
    Microsoft::WRL::ComPtr<IUnknown> m_DcompContent;

    int m_CurrentBuffer;
    bool m_Lost;
    uint64_t m_LastExpectedFlipUs;
    uint64_t m_AcquireWaitUs;
    uint64_t m_StatAcquireWaitUs; // per-stats-window, for the 10s log line

    // Present-id -> requested target interrupt-times, for diffing against
    // the displayed time the iflip statistics report. "wanted" is the
    // pacer's target; "sent" is wanted minus the servo's compensation.
    struct PendingPresent {
        UINT64 presentId;
        UINT64 sentTargetInterruptTime;   // 0 = ASAP present
        UINT64 wantedTargetInterruptTime;
        UINT64 presentCallInterruptTime;  // for ASAP display-latency stats
    };
    PendingPresent m_Pending[PENDING_RING];
    int m_PendingHead;

    // Offset servo: the OS's target-time executor displays a near-constant
    // interval AFTER the requested target (~20ms measured on AMD). Since
    // iflip statistics report true displayed times, integrate the net
    // error (displayed vs the pacer's wanted instant) into a compensation
    // subtracted from future targets. MOONLIGHT_PM_NO_OFFSET_SERVO=1
    // disables; the raw executor offset is then fully visible in stats.
    bool m_ServoEnabled;
    bool m_ForceAsap;
    uint64_t m_CompensationUs;
    uint32_t m_ServoSamples;
    uint64_t m_LastPresentCallUs;

    // 10s stats window
    uint64_t m_StatsStartUs;
    uint32_t m_StatPresents;
    uint32_t m_StatIflip;
    uint32_t m_StatIflipAsap;
    uint32_t m_StatMpoScanout;
    uint32_t m_StatComposed;
    uint32_t m_StatSkipped;
    uint32_t m_StatCanceled;
    uint32_t m_StatErrSamples;
    uint32_t m_StatErrLateOverMs;
    double m_StatErrSum;
    double m_StatErrSumSq;
    double m_StatErrMin;
    double m_StatErrMax;

    // ASAP presents: displayed - present call = true OS pipeline latency.
    uint32_t m_StatAsapLatSamples;
    double m_StatAsapLatSum;
    double m_StatAsapLatSumSq;

    // Consecutive displayed-time deltas across ALL iflip presents: the
    // direct glass-side judder metric (sd of intervals), valid in both
    // scheduled and ASAP modes.
    UINT64 m_LastDisplayedTime;
    uint32_t m_StatGlassSamples;
    double m_StatGlassSum;
    double m_StatGlassSumSq;

    bool m_LoggedFirstIflip;
    bool m_LoggedFirstComposed;
};
