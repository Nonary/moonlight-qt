#include "vrrpresenter.h"
#include "highressleep.h"

#include <SDL.h>
#include <Limelight.h>

// Lock re-entry demands a sustained streak of instant blank hits: a single
// hit is not proof (a free-running raster's own trailing blank catches a
// present or chase by luck); see isRasterLockUncertain().
#define ALIGN_LOCK_STREAK 4

VrrPresenter::VrrPresenter() :
#ifdef Q_OS_WIN32
    m_KmtAdapterValid(false),
    m_KmtAdapter(0),
    m_KmtVidPnSourceId(0),
#endif
    m_ActiveScanLines(0),
    m_ScanoutPeriodUs(0),
    m_LastPresentAlignmentWaitUs(0),
    m_PresentTargetUs(0),
    m_PresentAlignBudgetUs(0),
    m_PresentVsyncLatch(false),
    m_LastPresentLatched(false),
    m_PresentNearBuffered(false),
    m_LastPresentBuffered(false),
    m_PresentCatchUp(false),
    m_LastPresentUs(0),
    m_AlignHits(0),
    m_AlignGiveUps(0),
    m_AlignSkips(0),
    m_AlignVsyncLatches(0),
    m_AlignRescueLatches(0),
    m_AlignWaitTotalUs(0),
    m_AlignBudgetTotalUs(0),
    m_AlignStatsStartUs(0),
    m_TearForensicHead(0),
    m_TearForensicCount(0),
    m_AlignInstantStreak(0),
    m_MidScanSinceQuery(0),
    m_PresentBaseCurMinUs(UINT64_MAX),
    m_PresentBasePrevMinUs(UINT64_MAX),
    m_PresentBaseCount(0)
{
}

void VrrPresenter::notePresentDuration(uint64_t presentDurationUs)
{
    // Baseline = the smaller of two ~128-present half-window minimums: the
    // driver's own measured no-block present cost, per device by
    // construction. Excess over it is retire-queue backpressure and joins
    // the alignment-wait report the pacer already excludes from render
    // accounting.
    if (presentDurationUs < m_PresentBaseCurMinUs) {
        m_PresentBaseCurMinUs = presentDurationUs;
    }
    uint64_t baselineUs = m_PresentBaseCurMinUs < m_PresentBasePrevMinUs ?
        m_PresentBaseCurMinUs : m_PresentBasePrevMinUs;
    if (++m_PresentBaseCount >= 128) {
        m_PresentBasePrevMinUs = m_PresentBaseCurMinUs;
        m_PresentBaseCurMinUs = UINT64_MAX;
        m_PresentBaseCount = 0;
    }

    if (presentDurationUs > baselineUs) {
        m_LastPresentAlignmentWaitUs += presentDurationUs - baselineUs;
    }
}

VrrPresenter::~VrrPresenter()
{
    detachDisplay();
}

void VrrPresenter::attachDisplay(const wchar_t* gdiDeviceName,
                                 uint32_t activeScanLines,
                                 uint64_t scanoutPeriodUs)
{
    detachDisplay();

#ifdef Q_OS_WIN32
    // Open the matching GDI/KMT adapter handle so we can query the real scan
    // position via D3DKMTGetScanLine() - there is no equivalent on the
    // graphics APIs' own output objects (IDXGIOutput::GetRasterStatus doesn't
    // exist; that's D3D9-only), and the query is display-level, so it serves
    // any rendering API presenting to this output.
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME openAdapter = {};
    wcsncpy_s(openAdapter.DeviceName, gdiDeviceName, _TRUNCATE);
    if (D3DKMTOpenAdapterFromGdiDisplayName(&openAdapter) == 0 /* STATUS_SUCCESS */) {
        m_KmtAdapter = openAdapter.hAdapter;
        m_KmtVidPnSourceId = openAdapter.VidPnSourceId;
        m_KmtAdapterValid = true;
    }
#else
    Q_UNUSED(gdiDeviceName);
#endif

    m_ActiveScanLines = activeScanLines;
    m_ScanoutPeriodUs = scanoutPeriodUs;
}

void VrrPresenter::detachDisplay()
{
#ifdef Q_OS_WIN32
    if (m_KmtAdapterValid) {
        D3DKMT_CLOSEADAPTER closeAdapter = {};
        closeAdapter.hAdapter = m_KmtAdapter;
        D3DKMTCloseAdapter(&closeAdapter);
        m_KmtAdapterValid = false;
    }
#endif

    m_ActiveScanLines = 0;
    m_ScanoutPeriodUs = 0;
}

void VrrPresenter::setPresentTarget(uint64_t targetUs, bool catchUp,
                                    uint64_t alignBudgetUs, bool vsyncLatch,
                                    bool nearBuffered)
{
    // The catch-up policy is fully encoded in the pacer's alignment budget,
    // so the flag doesn't steer presentation - it's recorded purely so tear
    // forensics can attribute a mid-scan give-up to the catch-up path. The
    // near-buffered flag likewise only labels the overlay sub-state.
    m_PresentTargetUs = targetUs;
    m_PresentAlignBudgetUs = alignBudgetUs;
    m_PresentVsyncLatch = vsyncLatch;
    m_PresentNearBuffered = nearBuffered;
    m_PresentCatchUp = catchUp;
}

uint64_t VrrPresenter::popAlignmentWaitUs()
{
    uint64_t waitUs = m_LastPresentAlignmentWaitUs;
    m_LastPresentAlignmentWaitUs = 0;

    // In classic present mode, keep Pacer's render-lead estimate including
    // the alignment spin, bit-identical to the pre-target-hold tuning.
    if (qEnvironmentVariableIntValue("MOONLIGHT_VRR_CLASSIC_PRESENT") != 0) {
        return 0;
    }

    return waitUs;
}

uint32_t VrrPresenter::popMidScanTearCount()
{
    // Only read from the pacer's cadence thread, which is also the render
    // thread for VrrCadence - no synchronization needed.
    uint32_t count = m_MidScanSinceQuery;
    m_MidScanSinceQuery = 0;
    return count;
}

bool VrrPresenter::isRasterLockUncertain() const
{
#ifdef Q_OS_WIN32
    if (m_KmtAdapterValid) {
        return m_AlignInstantStreak < ALIGN_LOCK_STREAK;
    }
#endif

    // Without a scan-position source there is nothing to measure and no
    // alignment wait to spend - presents latch tear-free at the platform's
    // discretion instead - so never report the uncertainty that makes the
    // pacer pay for re-lock rituals.
    return false;
}

bool VrrPresenter::prepareToPresent()
{
    m_LastPresentAlignmentWaitUs = 0;

    // Three-step phase-corrected present, executed with the frame's GPU work
    // already fenced by the caller (so the present call is the true flip
    // instant):
    // 1. Hold until the pacer's intended present time.
    // 2. Align the last few ms to the panel's blanking gap - unless the
    //    pacer requested a vsync-latched present (measured content cadence
    //    at/above the panel's usable ceiling): those present without the
    //    tearing flag so the flip latches tear-free at the display's next
    //    vblank, making scan-position alignment moot.
    uint64_t alignBudgetUs = m_PresentAlignBudgetUs;
    m_PresentAlignBudgetUs = 0;
    bool vsyncLatch = m_PresentVsyncLatch;
    m_PresentVsyncLatch = false;
    bool nearBuffered = m_PresentNearBuffered;
    m_LastPresentLatched = vsyncLatch;
    m_LastPresentBuffered = nearBuffered && !vsyncLatch;
    m_PresentNearBuffered = false;
    bool catchUpPresent = m_PresentCatchUp;
    m_PresentCatchUp = false;

    uint64_t latePastTargetUs = holdUntilPresentTarget();
    if (vsyncLatch) {
        // Latched presents run classic fixed vsync - by the time we
        // return to tearing presents the panel is certainly not
        // flip-following, so the re-anchor must start re-armed.
        m_AlignInstantStreak = 0;
        m_AlignVsyncLatches++;
        logAlignStatsIfDue(LiGetMicroseconds());
    }
    else {
        // When true VRR has real headroom, an isolated on-time blank miss is
        // cheaper to latch once than to emit a visible tear and lose raster
        // lock. Do not rescue late/catch-up or near-ceiling presents: latching
        // those would turn overload into repeated fixed-vsync judder. The
        // pacer's tear-rate fallback handles a chronic near-ceiling failure.
        uint64_t rescueLateLimitUs = m_ScanoutPeriodUs != 0 ?
            m_ScanoutPeriodUs * 30 / 1000 : 250;
        bool rescueOnMiss = !nearBuffered && !catchUpPresent &&
            latePastTargetUs <= rescueLateLimitUs;
        bool aligned = waitForVBlankBeforeTearingPresent(
            alignBudgetUs, latePastTargetUs, catchUpPresent, rescueOnMiss);
        if (!aligned && rescueOnMiss) {
            vsyncLatch = true;
            m_LastPresentLatched = true;
            m_LastPresentBuffered = false;
        }
    }

    return vsyncLatch;
}

void VrrPresenter::prepareToPresentClassic()
{
    // MOONLIGHT_VRR_CLASSIC_PRESENT=1: the original scanline-align-only
    // behavior with its fixed 3ms bound, kept as an instant fallback for A/B
    // testing.
    m_LastPresentAlignmentWaitUs = 0;
    m_PresentTargetUs = 0;
    m_PresentAlignBudgetUs = 0;
    m_PresentVsyncLatch = false;
    m_LastPresentLatched = false;
    m_PresentNearBuffered = false;
    m_LastPresentBuffered = false;
    waitForVBlankBeforeTearingPresent(3000);
}

uint64_t VrrPresenter::holdUntilPresentTarget()
{
    // Hold the flip until the cadence pacer's intended instant. Without this,
    // a present is released as soon as rendering finishes - up to the whole
    // render-lead early - which re-scatters the present phase the pacer just
    // computed and can space flips tighter than the panel's max refresh
    // (observed as 5.5-7.4ms minimum intervals against an 8.33ms floor).
    //
    // Returns how far past the target we already were on entry (0 if we
    // held), so the caller knows the schedule is running behind.
    uint64_t targetUs = m_PresentTargetUs;
    m_PresentTargetUs = 0;

    if (targetUs == 0) {
        return 0;
    }

    uint64_t startUs = LiGetMicroseconds();
    if (startUs > targetUs) {
        return startUs - targetUs;
    }

    uint64_t nowUs = startUs;
    while (nowUs < targetUs) {
        if (targetUs - nowUs > 1500) {
            HighResSleep::sleepUntilUs(targetUs - 1000);
        }
        nowUs = LiGetMicroseconds();
    }

    m_LastPresentAlignmentWaitUs += nowUs - startUs;
    return 0;
}

bool VrrPresenter::waitForVBlankBeforeTearingPresent(uint64_t alignBudgetUs,
                                                     uint64_t latePastTargetUs,
                                                     bool catchUpPresent,
                                                     bool rescueOnMiss)
{
#ifdef Q_OS_WIN32
    // VRR only changes how long the panel is willing to extend its blanking
    // gap while waiting for us - it does not make a tearing-allowed present
    // synchronize to scan position on its own. A tearing present that lands
    // while the panel is actively mid-scan still tears, no matter how well
    // the cadence clock has paced the interval between presents.
    //
    // Public DXGI has no scanline/vblank query at all (IDXGIOutput::GetRasterStatus
    // is D3D9-only and doesn't exist here; IDXGIOutput::WaitForVBlank() is an
    // unbounded blocking call with unverified behavior under VRR's extended
    // blanking - risked adding up to a full extra refresh interval of latency
    // if it doesn't return immediately when already inside the blanking gap).
    //
    // D3DKMTGetScanLine() is the same low-level scan-position query used by
    // Special K's "Latent Sync" and proposed for RetroArch's beam-racing work
    // (with WinUAE as a working tear-free-VSYNC-OFF-under-VRR precedent) - and
    // unlike WaitForVBlank, it's a plain non-blocking query, so we can poll it
    // in a tightly bounded loop instead of risking an open-ended block.
    if (!m_KmtAdapterValid || qEnvironmentVariableIntValue("MOONLIGHT_DISABLE_SCANLINE_ALIGN")) {
        return true;
    }

    // The wait bound is the pacer's per-present budget, sized from the
    // measured content cadence so waiting can never starve the next frame
    // (see Pacer::cadenceThread). This replaces the old fixed 3ms spin,
    // whose cap was exactly what made the free-running-raster state sticky:
    // once torn flips knock the driver out of VRR flip-following, presents
    // land at random scan phase and a 3ms window only catches the blank
    // ~40% of the time, so the tearing regime self-sustains even at content
    // rates with milliseconds of idle headroom per frame. One scanout cycle
    // plus slack is always enough waiting: even a free-running raster
    // passes through its blanking gap within a cycle.
    //
    // MOONLIGHT_VRR_SCANLINE_SLEEP=1 forces the widest bound on every
    // present regardless of the pacer's budget, for A/B testing.
    // Scale-free bounds: the over-scanout slack and the instant-hit
    // discriminator below are fractions of this display's scanout period
    // (anchored to their validated values on the 8.33ms/120Hz reference),
    // not fixed microseconds - see the pacing-geometry block in
    // Pacer::cadenceThread for the rationale.
    uint64_t scanoutSlackUs = m_ScanoutPeriodUs * 240 / 1000;  // 2000us on the reference
    if (qEnvironmentVariableIntValue("MOONLIGHT_VRR_SCANLINE_SLEEP") != 0 && m_ScanoutPeriodUs != 0) {
        alignBudgetUs = m_ScanoutPeriodUs + scanoutSlackUs;
    }

    uint64_t maxWaitUs = m_ScanoutPeriodUs != 0 ?
        qMin(alignBudgetUs, m_ScanoutPeriodUs + scanoutSlackUs) :
        qMin(alignBudgetUs, (uint64_t)3000);

    uint64_t startUs = LiGetMicroseconds();
    D3DKMT_GETSCANLINE getScanLine = {};
    getScanLine.hAdapter = m_KmtAdapter;
    getScanLine.VidPnSourceId = m_KmtVidPnSourceId;

    // Query at least once even with a zero budget: a rush present that
    // happens to arrive inside the blanking gap still flips clean for the
    // cost of one syscall.
    const bool classicPresent =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_CLASSIC_PRESENT") != 0;
    bool reachedBlank = false;
    bool spunOut = false;
    uint32_t entryScanPct = 255;
    for (;;) {
        if (D3DKMTGetScanLine(&getScanLine) != 0 /* STATUS_SUCCESS */ || getScanLine.InVerticalBlank) {
            reachedBlank = true;
            break;
        }

        if (entryScanPct == 255 && m_ActiveScanLines != 0) {
            entryScanPct = qMin((uint32_t)((uint64_t)getScanLine.ScanLine * 100 /
                                           m_ActiveScanLines), 199u);
        }

        uint64_t elapsedUs = LiGetMicroseconds() - startUs;
        if (elapsedUs >= maxWaitUs) {
            spunOut = true;
            break;
        }

        if (m_ActiveScanLines != 0 && m_ScanoutPeriodUs != 0 &&
                getScanLine.ScanLine < m_ActiveScanLines) {
            uint64_t remainingUs = (uint64_t)(m_ActiveScanLines - getScanLine.ScanLine) *
                    m_ScanoutPeriodUs / (m_ActiveScanLines + m_ActiveScanLines / 20);

            // Give up immediately when the blanking gap is out of reach
            // within the remaining budget: waiting toward a guaranteed
            // mid-scan present doesn't move the tear, it only adds latency
            // and flip-phase noise. This is what turns a small budget into
            // a phase SNAP - presents already near the blank align to it,
            // everything else goes out untouched at pure content cadence,
            // which above the panel's flip ceiling keeps the beat's tear
            // line herded toward the screen edge instead of crawling
            // mid-frame. The line-time estimate deliberately errs low
            // (total lines padded ~5% over active for VBI overhead), so
            // borderline frames still wait and try. Classic mode keeps the
            // historical spin-out-the-full-bound behavior for faithful A/B.
            if (!classicPresent && remainingUs > maxWaitUs - elapsedUs) {
                break;
            }

            // While the beam is still far from the blanking gap, sleep off
            // most of the remaining scanout instead of hard-spinning a
            // TIME_CRITICAL thread for up to a whole refresh period; only
            // the final approach spins. Clamped to the remaining budget so
            // the beam estimate can't carry the wait past a tighter bound.
            remainingUs = qMin(remainingUs, maxWaitUs - elapsedUs);
            if (remainingUs > 1500) {
                HighResSleep::sleepUs(remainingUs - 1000);
            }
        }
    }

    uint64_t waitedUs = LiGetMicroseconds() - startUs;
    m_LastPresentAlignmentWaitUs += waitedUs;

    // Outcome telemetry for the residual-tear hunt: every give-up below is a
    // present that went out mid-scan, i.e. one visible tear, while a skip is
    // a present that never meaningfully tried (a rush with no cadence slack).
    // This is the ground truth to A/B the budget policy against.
    // Raster-lock tracking for the pacer's re-anchor. A mid-scan give-up
    // proves the raster is free-running, and a present that only FOUND the
    // blank after a chase implies it (a flip-following panel sits in
    // extended blanking waiting for us; measured waits ~0.1ms). But one
    // instant hit is NOT proof of re-lock: torn presents' beam positions
    // measure exactly where a fixed 120Hz raster restarted at the last
    // flip would put them, and such a raster's own trailing blank catches
    // a present by luck every few frames - a single-hit disarm measurably
    // leaked (tear, armed catch, lucky instant hit, disarm, tear again).
    // Require a sustained streak of instant hits before declaring the
    // panel locked; staying armed while actually locked costs nothing,
    // since the wide budget is only spent when a chase is needed.
    uint64_t instantHitUs = m_ScanoutPeriodUs != 0 ?
        m_ScanoutPeriodUs * 60 / 1000 : 500;  // 500us on the reference panel
    if (!reachedBlank || waitedUs > instantHitUs) {
        m_AlignInstantStreak = 0;
    }
    else if (m_AlignInstantStreak < ALIGN_LOCK_STREAK) {
        m_AlignInstantStreak++;
    }

    if (reachedBlank) {
        m_AlignHits++;
    }
    else if (rescueOnMiss) {
        // This present will go out without ALLOW_TEARING and latch at the
        // next vblank. It is a measured avoided tear, not a tear sample.
        m_AlignRescueLatches++;
    }
    else {
        // Tear-position-aware count for the pacer's tear-rate probe: a
        // tear landing within a few percent of the frame's top or bottom
        // edge reads as invisible in practice (the user-validated "edge
        // tearing" regime), so it doesn't count against free-run pacing -
        // only tears in the visible middle do. A crawling tear line sweeps
        // the middle and still fails the probe; an edge-pinned one passes.
        // The align stats line keeps counting every mid-scan present.
        uint32_t exitScanPct = m_ActiveScanLines != 0 ?
            (uint32_t)((uint64_t)getScanLine.ScanLine * 100 / m_ActiveScanLines) :
            50;
        if (exitScanPct >= 6 && exitScanPct <= 90) {
            m_MidScanSinceQuery++;
        }

        if (maxWaitUs < 500) {
            m_AlignSkips++;
        }
        else {
            m_AlignGiveUps++;
        }

        // Either way this present goes out mid-scan - one visible tear.
        // Record what state it was in so the periodic forensics line can
        // attribute the residual tears to a cause: late render (lateUs
        // beyond the pacer's margin), rush catch-up (catchUp), phase
        // compression (gapUs under the panel's ~9.1ms flip ceiling), or
        // lost VRR flip-following (on-time, well-spaced presents that
        // still find the beam mid-scan).
        TearForensicSample& s = m_TearForensics[m_TearForensicHead];
        s.lateUs = (uint32_t)qMin(latePastTargetUs, (uint64_t)9999999);
        s.gapUs = (m_LastPresentUs != 0 && startUs > m_LastPresentUs) ?
            (uint32_t)qMin(startUs - m_LastPresentUs, (uint64_t)9999999) : 0;
        s.entryScanPct = entryScanPct;
        s.budgetUs = (uint32_t)maxWaitUs;
        s.catchUp = catchUpPresent;
        s.spunOut = spunOut;
        m_TearForensicHead = (m_TearForensicHead + 1) % 16;
        if (m_TearForensicCount < 16) {
            m_TearForensicCount++;
        }
    }
    m_AlignWaitTotalUs += waitedUs;
    m_AlignBudgetTotalUs += maxWaitUs;

    logAlignStatsIfDue(startUs + waitedUs);
    return reachedBlank;
#else
    Q_UNUSED(alignBudgetUs);
    Q_UNUSED(latePastTargetUs);
    Q_UNUSED(catchUpPresent);
    Q_UNUSED(rescueOnMiss);
    return true;
#endif
}

void VrrPresenter::logAlignStatsIfDue(uint64_t nowUs)
{
    if (m_AlignStatsStartUs == 0) {
        m_AlignStatsStartUs = nowUs;
    }
    else if (nowUs - m_AlignStatsStartUs >= 10000000) {
        uint32_t attempted = m_AlignHits + m_AlignGiveUps +
            m_AlignRescueLatches;
        uint32_t total = attempted + m_AlignSkips;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR scanline align: %u in-blank, %u mid-scan tears (%.1f%%), %u misses rescued tear-free, avg wait %.2f ms, avg budget %.2f ms, %u skipped-late, %u policy-latched",
                    m_AlignHits,
                    m_AlignGiveUps,
                    attempted != 0 ? m_AlignGiveUps * 100.0 / attempted : 0.0,
                    m_AlignRescueLatches,
                    attempted != 0 ? (m_AlignWaitTotalUs / 1000.0) / attempted : 0.0,
                    total != 0 ? (m_AlignBudgetTotalUs / 1000.0) / total : 0.0,
                    m_AlignSkips,
                    m_AlignVsyncLatches);

        // Per-tear attribution for the window's mid-scan presents. Sample
        // format: late/gap/scan%/budget in ms, then flags - R = rush
        // catch-up, S = spun out the full budget (no S = fast give-up, the
        // beam said the blank was out of reach). Reading it: big late =
        // render overshoot beat the lead margin; gap under ~9.1 = phase
        // compression; late ~0 with gap ~content-interval and mid-scan
        // entry = the driver is not in VRR flip-following.
        if (m_TearForensicCount > 0) {
            char forensics[512];
            int off = 0;
            for (int i = 0; i < m_TearForensicCount; i++) {
                const TearForensicSample& s = m_TearForensics[
                    (m_TearForensicHead + 16 - m_TearForensicCount + i) % 16];
                int len = SDL_snprintf(forensics + off, sizeof(forensics) - off,
                                       "%s%.1f/%.1f/%u%%/%.1f%s%s",
                                       i ? ", " : "",
                                       s.lateUs / 1000.0,
                                       s.gapUs / 1000.0,
                                       s.entryScanPct,
                                       s.budgetUs / 1000.0,
                                       s.catchUp ? "R" : "",
                                       s.spunOut ? "S" : "");
                if (len < 0 || off + len >= (int)sizeof(forensics) - 1) {
                    break;
                }
                off += len;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "VRR tear forensics (last %d of %u): late/gap/scan/budget ms = %s",
                        m_TearForensicCount,
                        m_AlignGiveUps + m_AlignSkips,
                        forensics);
            m_TearForensicCount = 0;
        }

        m_AlignHits = 0;
        m_AlignGiveUps = 0;
        m_AlignSkips = 0;
        m_AlignVsyncLatches = 0;
        m_AlignRescueLatches = 0;
        m_AlignWaitTotalUs = 0;
        m_AlignBudgetTotalUs = 0;
        m_AlignStatsStartUs = nowUs;
    }
}
