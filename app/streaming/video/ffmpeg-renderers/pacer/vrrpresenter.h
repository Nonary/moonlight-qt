#pragma once

#include <QtGlobal>
#include <stdint.h>

#ifdef Q_OS_WIN32
// Deliberately NOT WIN32_LEAN_AND_MEAN: d3dkmthk.h needs types the lean
// windows.h omits (this header may be the translation unit's first
// windows.h inclusion, which decides the mode for the whole TU).
#include <Windows.h>
#include <d3dkmthk.h>
#endif

// The renderer-agnostic present-side half of VRR cadence pacing. The Pacer's
// cadence thread decides WHEN each frame should flip and with what policy
// (target instant, blank-alignment budget, vsync-latch, near-ceiling buffer);
// this class executes that intent at present time and reports the outcomes
// the pacer's feedback loops consume (alignment waits, raster-lock streak,
// visible-zone tear counts, per-tear forensics).
//
// Nothing here depends on the rendering API. The scan-position source is
// D3DKMTGetScanLine, which is a DISPLAY-level query opened from the GDI
// display name - it works identically under a D3D11, Vulkan, or GL
// swapchain on Windows. What a renderer must supply to adopt VRR cadence
// pacing:
//  - a tearing-capable present path (DXGI_PRESENT_ALLOW_TEARING,
//    VK_PRESENT_MODE_IMMEDIATE_KHR, ...) plus a non-tearing present for
//    vsync-latched frames,
//  - GPU-completion fencing before prepareToPresent(), so the present call
//    is the true flip instant rather than a queue submission,
//  - display attach/detach notifications with the output's GDI name and
//    scanout geometry.
// See D3D11VARenderer for the reference integration.
class VrrPresenter
{
public:
    VrrPresenter();
    ~VrrPresenter();

    // Attach the display the swapchain presents to. gdiDeviceName is the
    // DXGI/GDI display name (e.g. \\.\DISPLAY1) used to open the kernel
    // adapter for scanline queries; activeScanLines is the output height in
    // the panel's native orientation; scanoutPeriodUs is the max-refresh
    // period (under VRR the line clock stays at max-refresh speed and only
    // the blanking gap stretches).
    void attachDisplay(const wchar_t* gdiDeviceName,
                       uint32_t activeScanLines,
                       uint64_t scanoutPeriodUs);
    void detachDisplay();

    // Backing for the IFFmpegRenderer pacing interface - renderers forward
    // these directly. See renderer.h for semantics.
    void setPresentTarget(uint64_t targetUs, bool catchUp,
                          uint64_t alignBudgetUs, bool vsyncLatch,
                          bool nearBuffered);
    uint64_t popAlignmentWaitUs();
    uint32_t popMidScanTearCount();
    bool isRasterLockUncertain() const;

    uint64_t lastPresentUs() const
    {
        return m_LastPresentUs;
    }

    // Live sub-state for the overlay's presentation-mode line.
    bool lastPresentLatched() const
    {
        return m_LastPresentLatched;
    }

    bool lastPresentBuffered() const
    {
        return m_LastPresentBuffered;
    }

    // Executes the pacer's present intent for the next present: holds the
    // (already GPU-fenced) frame until the target instant, then either
    // records a vsync-latched present or aligns the flip to the panel's
    // blanking gap within the pacer's budget. Returns true if this present
    // must go out WITHOUT the tearing flag (vsync-latched).
    bool prepareToPresent();

    // MOONLIGHT_VRR_CLASSIC_PRESENT fallback: the original fixed-3ms
    // scanline alignment with no target hold. Clears per-present state.
    void prepareToPresentClassic();

    // Record the instant of the actual present call (the true flip time for
    // renderers that fence GPU completion beforehand).
    void notePresent(uint64_t presentUs)
    {
        m_LastPresentUs = presentUs;
    }

    // Record how long the present call itself took. A tearing present
    // returns in the driver's fixed submission overhead; wall time beyond
    // that is the swapchain blocking on a flip retire (queue-full
    // BACKPRESSURE), which no amount of earlier render start can avoid.
    // The excess over a self-calibrating baseline (rolling minimum, i.e.
    // this driver's own measured no-block cost) is folded into the
    // alignment-wait report so the pacer's render estimate, adaptive lead
    // margin, and render-tail handoff never schedule around it - measured
    // 2026-07-06 at near-ceiling rates as the whole "render" estimate
    // riding at the flip retire interval and inflating the standing
    // buffer toward its clamp.
    void notePresentDuration(uint64_t presentDurationUs);

private:
    uint64_t holdUntilPresentTarget();
    bool waitForVBlankBeforeTearingPresent(uint64_t alignBudgetUs,
                                           uint64_t latePastTargetUs = 0,
                                           bool catchUpPresent = false,
                                           bool rescueOnMiss = false);
    void logAlignStatsIfDue(uint64_t nowUs);

#ifdef Q_OS_WIN32
    bool m_KmtAdapterValid;
    D3DKMT_HANDLE m_KmtAdapter;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID m_KmtVidPnSourceId;
#endif
    uint32_t m_ActiveScanLines;
    uint64_t m_ScanoutPeriodUs;
    uint64_t m_LastPresentAlignmentWaitUs;
    uint64_t m_PresentTargetUs;
    uint64_t m_PresentAlignBudgetUs;
    bool m_PresentVsyncLatch;
    bool m_LastPresentLatched;
    bool m_PresentNearBuffered;
    bool m_LastPresentBuffered;
    bool m_PresentCatchUp;
    uint64_t m_LastPresentUs;
    uint32_t m_AlignHits;
    uint32_t m_AlignGiveUps;
    uint32_t m_AlignSkips;
    uint32_t m_AlignQueryFailures;
    uint32_t m_AlignVsyncLatches;
    uint32_t m_AlignRescueLatches;
    uint64_t m_AlignWaitTotalUs;
    uint64_t m_AlignBudgetTotalUs;
    uint64_t m_AlignStatsStartUs;

    // Per-tear forensics: one sample per mid-scan give-up, dumped alongside
    // the 10s scanline-align stats to attribute residual tears to a cause
    // (late render vs rush catch-up vs phase compression vs lost VRR
    // flip-following).
    struct TearForensicSample {
        uint32_t lateUs;        // how far past the pacer's target the hold ended
        uint32_t gapUs;         // spacing from the previous present call
        uint32_t entryScanPct;  // beam position at align entry, % of active scanout (255 = unknown)
        uint32_t budgetUs;      // align budget this present carried
        bool catchUp;           // pacer flagged it as a rush/catch-up present
        bool spunOut;           // burned the full budget (vs fast predicted-unreachable give-up)
    };
    TearForensicSample m_TearForensics[16];
    int m_TearForensicHead;
    int m_TearForensicCount;
    int m_AlignInstantStreak;
    uint32_t m_MidScanSinceQuery;

    // Present-call baseline for notePresentDuration(): two half-windows of
    // rolling minimum so the baseline tracks the driver's real submission
    // overhead (and re-learns it after driver/mode changes) without a
    // single lucky fast call pinning it forever.
    uint64_t m_PresentBaseCurMinUs;
    uint64_t m_PresentBasePrevMinUs;
    uint32_t m_PresentBaseCount;
};
