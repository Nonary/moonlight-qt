#include "vrrcatchup.h"
#include "vrrtimingcontroller.h"
#include "sourcegapsmoothing.h"
#include "../../../../vrrratepolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr uint64_t kMicrosecondsPerSecond = 1000000ULL;
constexpr uint64_t kRtpClockRate = 90000ULL;
constexpr uint64_t kQ16One = 1ULL << 16;
constexpr uint64_t kQ16Half = kQ16One >> 1;
// These base values also serve historical replay policies. Live sessions
// resolve the interval-quality controller and preset caps below; the rolling
// quality target, rather than a calibration percentile, owns reserve changes.
constexpr uint64_t kFixedPlayoutDelayUs = 3000;
constexpr uint64_t kPlayoutStartUs = 6000;
constexpr uint64_t kPlayoutMinimumUs = 1000;
constexpr uint64_t kPlayoutMaximumUs = 8000;
// Profile buffer allowances are measured in fitted source frames, independent
// of display refresh: half for Low Latency, one for Balanced, four for Smooth.
// Every profile waits in the same four-frame queue and differs only in how
// much of it its delay ceiling uses. With three frames, Balanced's 16 ms
// ceiling filled the queue budget near 120 FPS, so capacity rather than the
// profile clipped its delay.
constexpr uint64_t kPlayoutQueueFrames = VrrLargestQueuedFrames;
// Historical Smooth tolerance, used only when a capture has no explicit value.
constexpr uint64_t kSmoothIntervalToleranceUs = 200;
// The whole reservoir tail: the delay covers the largest lateness seen in
// the last thousand admitted frames plus the margin, so a late present is
// something the exclusions below deliberately left out (a host stall and
// the frames bunched behind it), never ordinary jitter.
constexpr uint64_t kPlayoutPercentilePerMille = 1000;
constexpr uint64_t kPlayoutBurstExclusionPerMille = 750;
// Reduce judder keeps 85% of the predicted slot and 15% of the raw mapped
// timestamp. Track gradual source-rate changes with a 2.5-percent period EMA
// plus 2-percent phase-error feedback, so a drifting game rate does not leave
// the smoothed slot trailing its stamps. Positive retiming may reach 6 ms: a
// host-refresh-quantized game (for example 90 FPS captured at 120 Hz) needs
// several milliseconds to even out, and the old 2 ms cap left most of that
// judder in place while biasing the schedule early. The cap is not a bound on
// total client latency. Unchecked sessions retain timestamp-following playout.
// Schema defaults and explicit captured parameters preserve historical replay.
constexpr uint64_t kPlayoutSmoothingGainPerMille = 150;
constexpr uint64_t kPlayoutSmoothingPeriodAlphaPerMille = 25;
constexpr uint64_t kPlayoutSmoothingMaxLagUs = 6000;
constexpr uint64_t kPlayoutSmoothingPeriodFeedbackPerMillion = 20000;
// A smoothed slot earlier than the frame's raw slot is only useful if the frame
// can be ready by then; otherwise the readiness clamp restores the judder. Move
// the smoothed schedule later by the p98 of recent smoother-caused shortfall,
// leaving 0.5 ms uncovered (a sub-millisecond miss stays under a 2 ms step),
// at most 3 ms, released at 0.5 ms per second once the shortfall subsides.
constexpr uint64_t kPlayoutSmoothingReserveMaxUs = 3000;
constexpr uint64_t kPlayoutSmoothingReserveToleranceUs = 500;
constexpr uint64_t kPlayoutSmoothingReservePercentilePerMille = 980;
constexpr uint64_t kPlayoutSmoothingReserveReleaseUsPerSecond = 500;
// Native future-target sessions in the 2026-10-10 capture trade away tail
// latency for little cadence benefit above 2 ms. Retain the wider worker-paced
// policy for host-quantized sources; explicit capture parameters remain exact.
constexpr uint64_t kNativeScheduledSmoothingMaxLagUs = 2000;
// A cadence reset (a few slow game frames, a burst, a stall) used to drop the
// accumulated retiming in one frame: up to a 6 ms step on screen, the most
// common client-made snap at 116/120 in capture 20260922-193211. Ease it back
// to the raw slot instead; 1 ms per frame keeps each step under 2 ms of jerk.
constexpr uint64_t kPlayoutSmoothingResetSlewUs = 1000;
// Retired metronome playout, kept reachable for replay. It advances the
// presented slot by the fitted source period, corrects phase toward the mapped
// sender clock by a bounded step, and moves a frame that cannot make its tick
// to the next tick rather than presenting it early.
constexpr uint64_t kPlayoutStartPeriodPerMille = 950;
constexpr uint64_t kPlayoutMaximumPeriodPerMille = 950;
constexpr uint64_t kPlayoutMetronomeSnapPerMille = 3000;
// A provisional cadence segment after a major departure must span this much
// sender time before its fit replaces the source rate. Three long stamps
// used to be enough, so a four-frame host hitch became a 30 Hz source and
// every rate-dependent term followed it there and back.
constexpr uint64_t kRateCandidateMinimumUs = 200000;
// Absorb retains the wider local cadence predictor. Revision 7 gives it a
// fixed 2T reserve and subjects intentional extra timing to the shared slew
// limit; historical revisions retain their recorded reserve parameters.
// Bounded catch-up holds a late frame's successors at most this long behind
// their slots, and at most half the source period: the same bound the VRR17
// recovery floor used. Beyond it a successor accepts compression to the panel
// floor instead of carrying up to a whole period of extra latency.
constexpr uint64_t kCatchupCarryLimitUs = 4000;
constexpr uint64_t kCatchupCarryLimitPeriodPerMille = 500;
// A never-seen rate layer is seeded from the previous layer only when that
// layer ran at most 1.5x slower; a loading screen's lateness does not carry
// into gameplay. While a layer is learning, its single largest requirement
// is ignored, so one scene-load outlier cannot set its protection.
constexpr uint64_t kInputLayerSeedRatioPerMille = 1500;
constexpr uint64_t kInputRateSettlingUs = 10000000;
constexpr uint64_t kInputLearningTrim = 1;
// Historical post-present acquisition spacing retains this minimum lead.
// Production uses native acquisition backpressure without software spacing.
constexpr uint64_t kRenderStartMinimumLeadUs = 2500;
// With the decoder's GPU work synced before preparation, the learned lead
// collapses to the 0.6 ms render and no longer covers the sporadic 2 to 3 ms
// renders; the floor keeps that headroom.
constexpr uint64_t kRenderLeadFloorUs = 3000;
// Consecutive frames that must map more than a period into the future before
// the sender clock is considered to have jumped. One early outlier used to
// re-seed the mapping on itself and make every following frame late.
constexpr uint64_t kPlayoutOffsetReseedFrames = 3;

uint64_t clampUnsigned(uint64_t value, uint64_t low, uint64_t high)
{
    if (high < low) {
        high = low;
    }
    return std::max(low, std::min(value, high));
}

template<typename T>
T percentile(const std::deque<T>& values, unsigned int requestedPercentile)
{
    if (values.empty()) {
        return 0;
    }
    std::vector<T> ordered(values.begin(), values.end());
    const unsigned int percentileValue = std::min(100U, requestedPercentile);
    const size_t rank = std::max<size_t>(
        1, (ordered.size() * percentileValue + 99) / 100);
    std::nth_element(ordered.begin(), ordered.begin() + rank - 1,
                     ordered.end());
    return ordered[rank - 1];
}

template<typename T>
void appendBounded(std::deque<T>& values, T value, size_t limit)
{
    while (values.size() >= limit) {
        values.pop_front();
    }
    values.push_back(value);
}

uint64_t intervalQualityWindowUs(const VrrTimingParameters& parameters)
{
    // Live revision 11 honors the explicit history for presets and custom
    // settings. Preserve the tuple-based histories of recorded revisions.
    if (parameters.playoutResponsiveBuffer >= 11) {
        return parameters.playoutReadinessWindowUs;
    }
    // Revision 7 originally captured playoutReadinessWindowUs for the
    // readiness estimator, while IntervalBuffer used a fixed 30-second score
    // history. Identify the new preset tuples here so old revision-7 traces
    // keep their exact replay behavior without adding a trace-schema field.
    if (parameters.playoutOnTimeTargetPerMillion == 990000 &&
        parameters.playoutReadinessWindowUs == 60000000) {
        return 60000000;
    }
    if (parameters.playoutOnTimeTargetPerMillion == 995000 &&
        parameters.playoutReadinessWindowUs == 120000000) {
        return 120000000;
    }
    if (parameters.playoutOnTimeTargetPerMillion == 999900 &&
        parameters.playoutReadinessWindowUs == 300000000) {
        return 300000000;
    }
    return 30000000;
}

uint64_t intervalQualityToleranceUs(const VrrTimingParameters& parameters)
{
    if (parameters.playoutIntervalToleranceUs != 0) {
        return parameters.playoutIntervalToleranceUs;
    }
    // Revision 7 originally used the shared 500 us default. Identify only
    // the current Smooth tuple so existing revision-7 traces replay exactly;
    // explicit revision 8 retains its historical 250 us tolerance.
    if (parameters.playoutResponsiveBuffer == 8) {
        return 250;
    }
    if (parameters.playoutResponsiveBuffer >= 7 &&
        parameters.playoutOnTimeTargetPerMillion == 999900 &&
        parameters.playoutReadinessWindowUs == 300000000) {
        return kSmoothIntervalToleranceUs;
    }
    return Vrr13::IntervalBuffer::ToleranceUs;
}

} // namespace

VrrTimingParameters vrrTimingParametersForSession(
    const VrrSessionConfig& config, bool nativeSynchronizedPresentation,
    bool nativeScheduledPresentation)
{
    // Present on a tracked source cadence plus a learned delay. The readiness
    // reserve, its per-frame slewing, and every phase re-anchor are off on this
    // path: they each moved the target between frames the source had spaced
    // evenly. Preparation readiness is a separate, bounded head start: it
    // moves renderStart earlier without moving the presentation deadline.
    // Explicit parameters keep older policies replayable.
    VrrTimingParameters parameters;
    parameters.nativeSynchronizedPresentation = nativeSynchronizedPresentation ? 1 : 0;
    parameters.nativeScheduledPresentation = nativeScheduledPresentation ? 1 : 0;
    // Mode zero is the new Smooth profile. Captured parameters retain their
    // own defaults for exact replay.
    const int latencyMode = config.latencyMode >= 0 && config.latencyMode <= 2 ?
        config.latencyMode : 1;
    const auto options = config.timingOptions.resolved(latencyMode);
    parameters.playoutIntervalToleranceUs = options.toleranceUs;
    // Presets change only the four user-visible values. Admission, release,
    // and late-frame recovery use the same policy for every combination.
    parameters.latencyFixEnabled = 1;
    parameters.latencyFixAllRates = config.latencyFix ? 0 : 1;
    parameters.latencyFixDelayPeriodPerMille = 500;
    parameters.playoutDelayCapSourcePeriodPerMille = config.latencyFix ? 0 : options.bufferPerMille;
    // The nominal 116 Hz period was shorter than the measured ~99 Hz source
    // in the deep capture, so it clipped the queue exactly when GPU stalls
    // needed more room. New live sessions use the fitted source period;
    // captured policies retain the old nominal-period behavior by default.
    parameters.playoutDelayCapUsesObservedPeriod = 1;
    parameters.playoutQueueFrames = kPlayoutQueueFrames;
    parameters.playoutCapacityTelemetry = 1;
    // Experimental release guard remains opt-in for replay comparisons.
    // Direct empirical controllers are evaluated separately before UI semantics change.
    parameters.playoutAbsorbedJitterRelease = 0;
    parameters.playoutInputPercentileEnabled = config.experimentalInputPercentilePpm != 0;
    parameters.playoutInputHitchMissesOnly = parameters.playoutInputPercentileEnabled ? 10 : 0;
    parameters.playoutInputStableAdaptation = parameters.playoutInputPercentileEnabled ? 2 : 0;
    // Record lifecycle semantics explicitly; missing fields in older captures
    // retain their historical eviction and reacquisition behavior.
    parameters.playoutInputLayerLifecycle = parameters.playoutInputPercentileEnabled ? 2 : 0;
    parameters.boundedCatchupEnabled = config.experimentalBoundedCatchup;
    parameters.normalizedSourceMapping = config.experimentalBoundedCatchup;
    if (parameters.boundedCatchupEnabled) {
        parameters.playoutCatchupCarryLimitUs = kCatchupCarryLimitUs;
        parameters.playoutCatchupCarryLimitPeriodPerMille = kCatchupCarryLimitPeriodPerMille;
    }
    if (parameters.playoutInputPercentileEnabled) {
        parameters.playoutInputClockRelativeHistory = config.correctInputAccounting && config.experimentalBoundedCatchup ? 4 : 0;
        // Lateness within the physical VRR room is credited, scaled by the
        // profile's miss allowance: a credited late frame is shown late, an
        // interval stretch, so Low Latency (1%) takes the whole room, Balanced
        // half, Smooth a twentieth.
        parameters.playoutInputBoundedRecoveryCredit = 0;
        parameters.playoutInputRateDropSnap = config.correctInputAccounting && config.experimentalBoundedCatchup;
        if (config.correctInputAccounting && config.experimentalBoundedCatchup) {
            parameters.playoutInputRecoveryCreditRoomPerMille = std::clamp<uint64_t>(
                (10000 - uint64_t(std::min(options.targetHundredths, 10000))) * 10, 1, 1000);
            parameters.playoutInputLayerSeedRatioPerMille = kInputLayerSeedRatioPerMille;
            parameters.playoutInputLearningTrim = kInputLearningTrim;
        }
        parameters.playoutInputPercentileTargetPerMillion = uint64_t(options.targetHundredths) * 100;
        // Explicit zero default preserves strict input coverage in old traces.
        parameters.playoutInputPercentileToleranceUs = options.toleranceUs;
        parameters.playoutInputPercentileWindowUs = std::clamp<uint64_t>(
            uint64_t(options.historySeconds) * 1000000, 10000000, 300000000);
        parameters.playoutInputPercentileRiseUsPerSecond = std::clamp<uint64_t>(
            config.experimentalInputPercentileRiseUsPerSecond, 250, 1000);
    }
    // Late-frame recovery is independent of optional source-cadence smoothing.
    parameters.playoutCatchupPerMille = 20;
    parameters.playoutLateRecovery = 1;
    parameters.playoutGpuReadinessAdaptation = 1;
    parameters.playoutPredictionOnly = 1;
    // Every normal VRR session uses the interval-quality queue. Historical
    // policies remain selectable only through explicit diagnostic parameters.
    // Apply tolerance to each interval before averaging its loss. Isolated
    // late/catch-up pairs must remain visible to both reporting and control.
    // Revision 11 also honors custom history; recorded 7/9/10 stay replayable.
    parameters.playoutResponsiveBuffer = config.readinessHitchFeedback &&
        !parameters.playoutInputPercentileEnabled ? 0 : 12;
    // Timeline mapping anchors to decode completion, absorbing hardware decode
    // duration into the sender offset instead of inflating client buffer delay.
    parameters.playoutSourceMappingDecoderOutput = 0;
    // Buffer transient work when measured service fits within intended time
    // over the qualified window. Include decoder waits and raw preparation
    // even when readiness-lead learning excludes them from generic render cost.
    parameters.playoutSerialServiceGate = parameters.playoutResponsiveBuffer ? 2 : 0;
    // Keep the preset's long quality history for reporting and future attack.
    // Only absorbable readiness misses renew the hold; revision 3 preserves
    // qualified clean recovery across short frame or phase breaks.
    parameters.playoutRecentPressureRelease = parameters.playoutResponsiveBuffer ? 3 : 0;
    // Qualify initial learning sooner with enough observations, without
    // increasing attack speed or rearming fast calibration on FPS changes.
    parameters.playoutIntervalInitialWarmupUs = 500000;
    parameters.playoutIntervalInitialMinimumSamples = 32;
    // Retain earned protection between bursts instead of repeatedly shedding
    // it and reacquiring it. Explicit captured values preserve older release.
    parameters.playoutMeanMissHoldUs = 8000000;
    // Balanced's 250 us/s recovery is the measured latency/smoothness knee:
    // faster recovery saved little additional latency and raised presented
    // jerk. All custom settings use this shared release policy.
    parameters.playoutMeanMissReleaseUsPerSecond = 250;
    // Above-target score changes neither renew the hold nor pause recovery.
    // Captured revisions 0/1/2 retain their historical pressure behavior.
    parameters.playoutHoldRenewBelowTarget = parameters.playoutResponsiveBuffer >= 7 ? 3 : 0;
    // No profile floors its delay at a recent ready-offset percentile. Such a
    // floor counts every late frame, including a decoder that has fallen
    // behind, and held Balanced at its ceiling after an ordinary startup
    // backlog. The parameter remains for captures that recorded it.
    parameters.playoutOnTimeTargetPerMillion = uint64_t(options.targetHundredths) * 100;
    parameters.playoutReadinessWindowUs = uint64_t(options.historySeconds) * 1000000;
    parameters.playoutReadinessHitchThresholdUs = config.readinessHitchFeedback &&
        !parameters.playoutInputPercentileEnabled ? 2000 : 0;
    parameters.playoutNativeHitchAdaptation = 0;
    // Display observations remain diagnostics; the queue uses interval quality.
    parameters.playoutRequireDisplayEvents = 1;
    parameters.playoutSubmissionEstimateFallback = 1;
    parameters.playoutReadinessDrivenAdaptation = 1;
    parameters.playoutStableSmoothnessReference = 1;
    parameters.renderStartPreserveLearnedLead = 1;
    parameters.playoutPredictionEnabled = 1;
    parameters.playoutSmoothnessFeedbackEnabled = 1;
    parameters.playoutDelayMarginUs = parameters.playoutResponsiveBuffer ? 500 : 3000;
    parameters.playoutDelayAttackUs = 500;
    parameters.playoutAdaptiveOnly = 0;
    // Match vrr14's planned-slot protection. Keep revision 2 available for
    // exact replay; native-rate/tight slots still request synchronized output.
    // Revision 2 latches every 116/120 frame; latchedFlipAnchor instead
    // closes the latched-then-tearing gap that its headroom papered over.
    parameters.playoutPerFrameLatch = 1;
    parameters.playoutRateProtectionEnabled = 0;
    parameters.playoutHistoryEnabled = 1;
    parameters.timestampPlayoutEnabled = 1;
    // Do not let an ineligible desktop/cadence transition poison the next
    // game's clock floor. Keep the applied phase and recover gradually from
    // fresh cadence evidence, without reseeding the retained playout buffer.
    parameters.playoutOffsetCadenceGate = 1;
    // Match the former 20 us/frame at 120 FPS in elapsed time. At 60 FPS the
    // mapper may now correct 40 us/frame instead of taking twice as long.
    // A per-observation cap prevents gaps from buying a large phase jump.
    parameters.playoutOffsetSlewUsPerSecond = 2400;
    // Local GPU and worker backlog must not age the sender-clock model or buy
    // a larger correction. The unwrapped RTP timeline supplies elapsed time;
    // decode completion supplies the source offset being corrected.
    parameters.playoutOffsetSourceClock = 1;
    parameters.playoutOffsetMaximumStepUs = 100;
    parameters.playoutDelayAdaptive = 1;
    parameters.sourcePlayoutDelayUs = kFixedPlayoutDelayUs;
    parameters.playoutDelayStartUs = kPlayoutStartUs;
    parameters.playoutDelayMinimumUs = kPlayoutMinimumUs;
    // Use a source-frame budget for every preset. Fixed millisecond ceilings
    // made Balanced and Smooth collapse to roughly one frame on slower streams.
    parameters.playoutDelayMaximumUs = kPlayoutMinimumUs;
    parameters.playoutDelayPercentilePerMille = kPlayoutPercentilePerMille;
    parameters.playoutBurstExclusionPerMille = kPlayoutBurstExclusionPerMille;
    // The independent smoothing preference trades timestamp fidelity for
    // steadier cadence. The timing profile still bounds adaptive buffering.
    parameters.playoutSmoothingGainPerMille =
        kPlayoutSmoothingGainPerMille;
    parameters.playoutSmoothingPeriodAlphaPerMille =
        kPlayoutSmoothingPeriodAlphaPerMille;
    parameters.playoutSmoothingMaxLagUs = nativeScheduledPresentation ?
        kNativeScheduledSmoothingMaxLagUs : kPlayoutSmoothingMaxLagUs;
    parameters.playoutSmoothingPeriodFeedbackPerMillion =
        kPlayoutSmoothingPeriodFeedbackPerMillion;
    parameters.playoutSmoothingReserveMaxUs = std::min(
        kPlayoutSmoothingReserveMaxUs, parameters.playoutSmoothingMaxLagUs);
    parameters.playoutSmoothingReserveToleranceUs =
        kPlayoutSmoothingReserveToleranceUs;
    parameters.playoutSmoothingReservePercentilePerMille =
        kPlayoutSmoothingReservePercentilePerMille;
    parameters.playoutSmoothingReserveReleaseUsPerSecond =
        kPlayoutSmoothingReserveReleaseUsPerSecond;
    parameters.playoutSmoothingResetSlewUs = kPlayoutSmoothingResetSlewUs;
    // Reduce judder may spend available readiness slack, but cannot buy a
    // larger interval buffer by advancing an otherwise on-time deadline.
    // Zero preserves both the old smoother and its attribution for replay.
    parameters.playoutSmoothingReadinessBound =
        parameters.playoutResponsiveBuffer >= 6 && config.smoothFrameTiming ? 1 : 0;
    // Absorb revision 7 retains wider phase-locked smoothing, qualifying and
    // reseeding uniform local source runs before the longer rate fit catches
    // up. Ordinary Reduce judder uses its tolerance-bounded source-gap policy;
    // historical parameter snapshots retain their explicit revisions.
    parameters.playoutSmoothingWindowedCadence = 2;
    if (config.smoothFrameTiming && config.absorbMicrostutter) {
        parameters.playoutSmoothingWindowedCadence = 7;
        parameters.playoutSmoothingAbsorbReserveUs = uint64_t(options.toleranceUs) * 2;
        parameters.playoutSmoothingAbsorbReservePeriodPerMille = 0;
        parameters.playoutSmoothingResetSlewUs = std::min<uint64_t>(
            kPlayoutSmoothingResetSlewUs, std::max<uint64_t>(1, options.toleranceUs / 2));
        parameters.playoutSmoothingMaxLagUs = uint64_t(options.toleranceUs) * 2;
        // Absorb has a fixed 2T pad. It cannot acquire a separate learned
        // reserve on top of that pad, outside the shared slew budget.
        parameters.playoutSmoothingReserveMaxUs = 0;
        parameters.playoutSmoothingReservePercentilePerMille = 0;
        parameters.playoutSmoothingReserveReleaseUsPerSecond = 0;
    }
    else if (config.smoothFrameTiming) {
        // T is both the untouched source-gap band and the entire retained
        // phase budget. Correct only excess beyond T, by at most T per gap.
        // Clean gaps retain the phase: recentering or a learned reserve would
        // change their spacing despite their being within tolerance.
        parameters.playoutSmoothingWindowedCadence = 4;
        parameters.playoutSmoothingMaxLagUs = options.toleranceUs;
        parameters.playoutSmoothingPeriodFeedbackPerMillion = 0;
        parameters.playoutSmoothingReserveMaxUs = 0;
        parameters.playoutSmoothingReserveToleranceUs = 0;
        parameters.playoutSmoothingReservePercentilePerMille = 0;
        parameters.playoutSmoothingReserveReleaseUsPerSecond = 0;
        parameters.playoutSmoothingResetSlewUs = 0;
    }
    // Four consecutive intervals qualify the new window. Source-rate changes
    // already have their own confirmation gate; another 200 ms without
    // smoothing after a recovered gap needlessly reproduces source jitter.
    parameters.playoutSmoothingRecoveryUs = 0;
    parameters.playoutMetronomeEnabled = 0;
    parameters.playoutDelayStartPeriodPerMille = kPlayoutStartPeriodPerMille;
    // The maximum and cap both express the selected total buffer in source
    // frames: half for Low Latency, one for Balanced, four for Smooth. Use the
    // fitted source period, never the display refresh, for this allowance.
    parameters.playoutDelayMaximumPeriodPerMille =
        options.bufferPerMille;
    parameters.playoutSmoothingSnapPerMille = kPlayoutMetronomeSnapPerMille;
    parameters.playoutOffsetReseedFrames = kPlayoutOffsetReseedFrames;
    parameters.playoutDelaySlewAcrossBands = 1;
    // Spend the existing playout interval on preparation on both platforms.
    // Vulkan can hand off a pending GPU render using its present semaphore;
    // D3D11 completes its verified render fence before the target hold.
    // Delaying preparation until just before Present would remove that overlap,
    // especially when no synchronous GPU wait is available to train a lead.
    // Acquisition still enforces native backpressure; it does not justify an
    // additional six-millisecond software delay after every submission.
    parameters.playoutPrepareOnArrival = 1;
    parameters.renderStartAfterSubmissionUs = 0;
    parameters.renderStartMinimumLeadUs = kRenderStartMinimumLeadUs;
    parameters.renderLeadFloorUs = kRenderLeadFloorUs;
    parameters.preparationInitialSampleExcluded = 1;
    // A source-rate epoch of at least 1.5x returns to the delay previously
    // sustained at that rate; decreases slew so a restore is not a jump.
    parameters.playoutEpochRateRatioPerMille = 1500;
    parameters.playoutEpochSustainUs = 10000000;
    parameters.playoutDelayDecreaseSlewUs = 250;
    parameters.playoutEpochConfirmUs = 1000000;
    parameters.rateCandidateMinimumUs = kRateCandidateMinimumUs;
    // A provisional source rate is cancelled only by an interval near the
    // old fitted period. The captured 2:1 default treats the 16.7 ms half of
    // a 16.7/33.3 ms pattern as a return to 120 FPS and never accepts 40 FPS.
    parameters.candidateCadenceRatioNumerator = 5;
    parameters.candidateCadenceRatioDenominator = 4;
    parameters.playoutStallBurstExclusion = 1;
    parameters.latchedFloorDisabled = 1;
    // A latched present flips no earlier than one display period after the
    // previous flip, not at its call. Anchoring the next tearing present to
    // the call let it flip inside the panel's minimum period after a late or
    // compressed frame.
    parameters.latchedFlipAnchor = 1;
    // Beyond ~50 Hz the panel may be repeating the last frame (LFC); an
    // immediate tearing present can then land mid-repeat.
    parameters.vrrFloorLatchGapUs = 20000;
    // The flip anchor assumes a tearing present flips at its call, but DXGI
    // can take several milliseconds, so a latched successor flips later than
    // anchored and the next tearing present landed inside its scanout. Let the
    // presenter latch that present from live frame statistics instead; the
    // flip queue then shows it at the earliest untorn refresh.
    parameters.nativeFlipProtection = 1;
    parameters.pacingLatencyQueueModeExtra = 0;
    if (!config.smoothFrameTiming) {
        // Preserve the mapped RTP intervals instead of regularizing the
        // source cadence. Keep the adaptive delay, readiness constraints,
        // and display-spacing floor in effect. Disable both the production
        // gain smoother and the replay-compatible metronome.
        parameters.playoutMetronomeEnabled = 0;
        parameters.playoutSmoothingGainPerMille = 0;
        parameters.playoutSmoothingPeriodFeedbackPerMillion = 0;
        parameters.playoutSmoothingReserveMaxUs = 0;
        parameters.playoutSmoothingReserveToleranceUs = 0;
        parameters.playoutSmoothingReservePercentilePerMille = 0;
        parameters.playoutSmoothingReserveReleaseUsPerSecond = 0;
        parameters.playoutSmoothingResetSlewUs = 0;
    }
    return parameters;
}

VrrTimingController::VrrTimingController(const VrrSessionConfig& config,
                                         bool canLatchPresentation) :
    VrrTimingController(config, canLatchPresentation, VrrTimingParameters {})
{
}

VrrTimingController::VrrTimingController(const VrrSessionConfig& config,
                                         bool canLatchPresentation,
                                         const VrrTimingParameters& parameters) :
    m_Config(config),
    m_Parameters(parameters),
    m_CanLatchPresentation(canLatchPresentation)
{
    reset();
}

void VrrTimingController::reset()
{
    m_PhaseTrajectory.reset();
    m_HaveMappingBasis = false;
    m_CorrectedMappingBasis = false;
    m_DisplayPeriodUs = periodForRate(m_Config.displayRefreshHz, 16667);
    m_ConfiguredStreamPeriodQ16 = periodForRateQ16(
        m_Config.streamRateHz, m_DisplayPeriodUs * kQ16One);
    m_ConfiguredStreamPeriodUs = std::max<uint64_t>(
        1, roundedQ16(m_ConfiguredStreamPeriodQ16));
    m_BaseGuardUs = clampUnsigned(
        m_DisplayPeriodUs / m_Parameters.baseGuardDivisor,
        m_Parameters.minimumGuardUs,
        m_Parameters.maximumBaseGuardUs);

    m_HaveLastSubmission = false;
    m_CatchupActive = false;
    m_LastSubmissionUs = 0;
    m_SpacingAnchorUs = 0;
    m_NativeLatchPending = false;
    m_NativeLatchFlipUs = 0;
    m_ReadinessFloorSamples.clear();
    m_ReadinessFloorUs = 0;
    m_ReadinessFloorUpdatedUs = 0;
    m_CleanSpacingFrames = 0;
    m_PhaseErrorFrames = 0;
    clearTimeline(false);
}

void VrrTimingController::rebase()
{
    m_PhaseTrajectory.breakSequence();
    clearTimeline(true);
}

void VrrTimingController::clearTimeline(bool retainLearnedBudgets)
{
    const uint64_t previousReadinessDemandUs = m_ReadinessDemandUs;
    const uint64_t previousAppliedReadinessReserveUs =
        m_AppliedReadinessReserveUs;
    const bool previousReadinessModelValid = m_ReadinessModelValid;
    const uint64_t previousRenderBaselineUs = m_RenderBaselineUs;
    const uint64_t previousRenderLeadUs = m_RenderLeadUs;
    const uint64_t previousRenderWakeLeadUs = m_RenderWakeLeadUs;
    const uint64_t previousTargetWakeLeadUs = m_TargetWakeLeadUs;
    const uint64_t previousGpuReadinessLeadUs = m_GpuReadinessLeadUs;
    const uint64_t previousGuardUs = m_GuardUs;

    m_SourcePeriodUsQ16 = m_ConfiguredStreamPeriodQ16;
    m_SourcePeriodUs = std::max<uint64_t>(
        1, roundedQ16(m_SourcePeriodUsQ16));
    m_LatencyFixActive = false;
    updateLatencyFixState();
    m_MetronomePeriodUsQ16 = m_ConfiguredStreamPeriodQ16;
    m_LatchedPresentation = m_CanLatchPresentation && m_Parameters.nativeSynchronizedPresentation ? true :
        m_Parameters.playoutAdaptiveOnly ? false :
        m_Parameters.playoutRateProtectionEnabled ?
        rateProtectedPresentation() : m_CanLatchPresentation && m_SourcePeriodUs <
        saturatingAdd(m_DisplayPeriodUs,
                      latchedPresentationHeadroomUs());
    m_ReadinessBudgetUs = 0;
    m_ReadinessPhaseUs = 0;
    m_ReadinessDemandUs = retainLearnedBudgets ?
        previousReadinessDemandUs : m_Parameters.coldStartReadinessDemandUs;
    m_AppliedReadinessReserveUs = retainLearnedBudgets &&
            m_Parameters.retainReadinessOnPhaseReset != 0 ?
        previousAppliedReadinessReserveUs :
        m_Parameters.coldStartReadinessDemandUs;
    m_ReadinessModelValid = retainLearnedBudgets &&
        previousReadinessModelValid;
    if (timestampPlayoutEnabled()) {
        // The fixed playout delay is the whole buffer. No learned reserve is
        // applied or reported on top of it.
        m_ReadinessDemandUs = 0;
        m_AppliedReadinessReserveUs = 0;
        m_ReadinessModelValid = false;
    }
    resetPlayoutOffsets();
    m_WorkloadEpisode.reset();
    m_ReadinessPrediction.reset();
    m_CadenceStableSinceUs = 0;
    m_PreviousSmoothingIntervalUs = 0;
    m_SmoothingCadenceCount = 0;
    m_SmoothingCadenceIndex = 0;
    m_ReadinessFeedback.reset();
    m_MeanMissBuffer.breakSequence();
    m_IntervalBuffer.breakSequence();
    m_InputPercentileBuffer.breakSequence();
    m_InputLayerCadence.reset();
    // A rebase moves the slot basis for every rate layer's evidence alike.
    for (auto& layer : m_InputLayers) layer.buffer.breakSequence();
    m_PresentationPrediction.reset();
    m_PresentTiming.breakSequence();
    m_SubmissionCadence.breakSequence();
    m_DisplayCadence.breakSequence();
    m_SubmissionSmoothness.breakSequence();
    m_NativeSmoothness.breakSequence();
    m_FeedbackModeValid = false;
    // Learned per-band delays survive a source-phase rebase like the other
    // learned budgets; only a full reset discards them.
    if (!retainLearnedBudgets) {
        m_MeanMissBuffer.reset();
        m_IntervalBuffer.reset();
        m_InputPercentileBuffer = Vrr13::InputPercentileBuffer{};
        m_InputCoverageHistory = {};
        m_ReceiveDriftClock = {};
        m_ReceiveDriftPacketValid = false;
        m_InputHeadroomUs = 0;
        m_InputLayers.clear();
        m_InputLayerPeriodUs = 0;
        m_InputLayerEligibleAtStart = 0;
        m_InputSettledLayerPeriodUs = 0;
        m_InputRateDropAtUs = 0;
        m_InputRateRiseAtUs = 0;
        m_PresentTiming.reset();
        m_SubmissionCadence.reset();
        m_DisplayCadence.reset();
        m_SubmissionSmoothness.reset();
        m_NativeSmoothness.reset();
        m_RequestedPlayoutDelayUs = 0;
        m_UnclampedRequestedPlayoutDelayUs = 0;
        m_GpuReadinessLeadUs = 0;
        m_RecentReadiness = m_Parameters.playoutResponsiveBuffer >= 3 ?
            Vrr13::RecentReadiness(m_Parameters.playoutReadinessWindowUs,
                                  m_Parameters.playoutOnTimeTargetPerMillion,
                                  m_Parameters.playoutResponsiveBuffer >= 4) :
            Vrr13::RecentReadiness{};
        m_PlayoutHistory = Vrr13::Reserve(m_Parameters.playoutResponsiveBuffer ? 20 :
            m_Parameters.playoutReadinessHitchThresholdUs ? 19 :
            m_Parameters.playoutPredictionOnly ? 18 :
            m_Parameters.playoutNativeHitchAdaptation ? 17 :
            m_Parameters.playoutReadinessDrivenAdaptation ? 16 :
            m_Parameters.playoutSmoothnessFeedbackEnabled ? 15 : m_Parameters.playoutPredictionEnabled ? 14 : 13);
        m_LastHistoryArrivalUs = 0;
        m_PlayoutBands.clear();
        m_PlayoutBandValid = false;
        m_AppliedPlayoutDelayUs = 0;
        m_AppliedPlayoutDelayValid = false;
        m_SmoothingReserveUs = 0;
        m_SmoothingReserveReleaseRemainder = 0;
        m_LastSmoothingReserveUpdateUs = 0;
        m_SmoothingShortfallCount = 0;
        m_SmoothingShortfallIndex = 0;
        m_ExtraBufferSlew.reset();
        m_BufferLayerDrain = false;
    }
    m_SmoothingEngaged = false;
    m_LastSmoothingRetimingUs = 0;
    m_HaveLastDecodeComplete = false;
    m_LastDecodeCompleteUs = 0;
    resetCadenceSmoothing();
    m_SmoothedPeriodUs = 0;
    m_SmoothedPeriodRemainder = 0;
    m_SmoothedPeriodFeedbackRemainder = 0;
    m_MotionResiduals.clear();
    m_FutureProjectionFrames = 0;
    m_BurstExclusionFrames = 0;
    m_HaveTimeline = false;
    m_SourceTimeUs = 0;
    m_SourceTimeUsQ16 = 0;
    m_SourceFrameOrdinal = 0;
    m_UnwrappedRtpTicks = 0;
    m_LastFrameNumber = -1;
    m_HaveLastFrameNumber = false;
    m_LastRtpTimestamp = 0;
    m_LastTimestampValid = false;
    m_RtpConversionRemainder = 0;
    m_FrameConversionRemainder = 0;
    m_LastCadenceUsedRtp = false;
    m_PhaseErrorFrames = 0;
    m_CadenceStabilityLatchFramesRemaining = 0;

    m_CadenceSamples.clear();
    m_RateCandidateSamples.clear();
    m_ReadyOffsets.clear();
    m_PreparationDurations.clear();
    m_PreparationInitialSampleSeen = false;
    m_RenderSchedulerDelays.clear();
    m_TargetSchedulerDelays.clear();
    // A phase rebase invalidates sample timestamps, but the bounded reserve
    // itself is still useful immediately in the new source epoch.
    m_GpuReadinessSamples.clear();
    m_GpuReadinessSortedWaits.clear();
    m_LastGpuReadinessUpdateUs = 0;
    m_Pending = PendingFrame {};

    if (retainLearnedBudgets) {
        m_RenderBaselineUs = std::min(previousRenderBaselineUs,
                                      m_SourcePeriodUs);
        m_RenderLeadUs = clampUnsigned(previousRenderLeadUs,
                                       renderLeadFloorUs(),
                                       renderLeadCeilingUs());
        m_RenderWakeLeadUs = std::min(previousRenderWakeLeadUs,
                                      m_Parameters.maximumRenderWakeLeadUs);
        m_TargetWakeLeadUs = std::min(previousTargetWakeLeadUs,
                                      m_Parameters.maximumTargetWakeLeadUs);
        m_GpuReadinessLeadUs = std::min(previousGpuReadinessLeadUs,
                                        gpuReadinessCeilingUs());
        m_GuardUs = clampUnsigned(previousGuardUs,
                                  m_BaseGuardUs,
                                  guardCeilingUs());
    }
    else {
        // Until measurements arrive, treat the historical 1 ms lead as
        // unavoidable render work rather than pacing latency insurance.
        m_RenderBaselineUs = std::min(m_Parameters.renderLeadFloorUs,
                                      m_SourcePeriodUs);
        m_RenderLeadUs = clampUnsigned(m_Parameters.renderLeadFloorUs,
                                       renderLeadFloorUs(),
                                       renderLeadCeilingUs());
        m_RenderWakeLeadUs = 0;
        m_TargetWakeLeadUs = 0;
        m_GpuReadinessLeadUs = 0;
        m_GuardUs = m_BaseGuardUs;
    }
    clampReadinessReserveToPolicy();
}

void VrrTimingController::initializeTimeline(const PacedFrame& frame)
{
    m_HaveTimeline = true;
    anchorSourceTime(sourceMappingUs(frame));
    m_SourceFrameOrdinal = 0;
    m_UnwrappedRtpTicks = 0;
    m_LastFrameNumber = frame.frameNumber();
    m_HaveLastFrameNumber = frame.frameNumber() >= 0;
    m_LastRtpTimestamp = frame.rtpTimestamp();
    m_LastTimestampValid = frame.timestampValid();
    m_CadenceSamples.clear();
    m_RateCandidateSamples.clear();
    if (frame.timestampValid()) {
        m_CadenceSamples.push_back(CadenceSample {});
    }
}

uint64_t VrrTimingController::estimatedDecodeCompleteUs(const PacedFrame& frame) const
{
    return estimateDecodeCompleteUs(frame.decoderOutputUs(), frame.decodeCompleteUs(),
                                    frame.decodeSyncWaitUs(), m_DecodeGpuTypicalUs);
}

void VrrTimingController::learnDecodeGpu(const PacedFrame& frame)
{
    const uint64_t output = frame.decoderOutputUs();
    const uint64_t complete = frame.decodeCompleteUs();
    if (!output || complete <= output || frame.decodeSyncWaitUs() < PreciseCompletionWaitUs) return;
    m_DecodeGpuSamples[m_DecodeGpuNext] = complete - output;
    m_DecodeGpuNext = (m_DecodeGpuNext + 1) % m_DecodeGpuSamples.size();
    m_DecodeGpuCount = std::min(m_DecodeGpuCount + 1, m_DecodeGpuSamples.size());
    auto sorted = m_DecodeGpuSamples;
    const auto middle = sorted.begin() + m_DecodeGpuCount / 2;
    std::nth_element(sorted.begin(), middle, sorted.begin() + m_DecodeGpuCount);
    m_DecodeGpuTypicalUs = *middle;
}

uint64_t VrrTimingController::sourceMappingUs(const PacedFrame& frame) const
{
    const uint64_t raw = m_Parameters.playoutSourceMappingDecoderOutput != 0 ?
        frame.decoderOutputUs() : frame.decodeCompleteUs();
    if (m_Parameters.normalizedSourceMapping && m_CorrectedMappingBasis) {
        // Virtual producer clock: remove explicitly measured queue/hold,
        // never the decoder service cost or GPU/compositor completion.
        // PyroWave's decoder output is GPU submission, not completion, so the
        // service cost ends at the (estimated) decode completion. Using output
        // planned frames ~4 ms before they could be decoded (2026-10-10).
        return frame.reassembledUs() + (estimatedDecodeCompleteUs(frame) - frame.decodeSubmitUs());
    }
    return raw;
}

VrrTimingDecision VrrTimingController::schedule(const PacedFrame& frame,
                                                 uint64_t nowUs)
{
    m_Pending = PendingFrame {};
    m_ReceiveDriftPacketValid = false;
    learnDecodeGpu(frame);
    m_SmoothingEngaged = false;
    if (m_Parameters.normalizedSourceMapping) {
        const bool corrected = frame.reassembledUs() != 0 &&
            frame.decodeSubmitUs() >= frame.reassembledUs() &&
            frame.decodeHoldUs() <= frame.decodeSubmitUs() - frame.reassembledUs() &&
            frame.decoderOutputUs() >= frame.decodeSubmitUs();
        if (m_HaveMappingBasis && corrected != m_CorrectedMappingBasis) rebase();
        m_HaveMappingBasis = true;
        m_CorrectedMappingBasis = corrected;
    }

    CadenceObservation cadence;
    m_BufferLayerChanged = false;
    bool rebased = false;
    if (!m_HaveTimeline) {
        initializeTimeline(frame);
        rebased = true;
    }
    else {
        const bool frameNumberReset =
            m_HaveLastFrameNumber && frame.frameNumber() >= 0 &&
            frame.frameNumber() <= m_LastFrameNumber;

        if (frameNumberReset) {
            rebase();
            initializeTimeline(frame);
            rebased = true;
        }
        else {
            cadence = observeCadence(frame);
            if (cadence.needsRebase) {
                rebase();
                initializeTimeline(frame);
                rebased = true;
            }
            else {
                const uint64_t maximum = std::numeric_limits<uint64_t>::max();
                const uint64_t projectedMovementQ16 =
                    cadence.frameDelta != 0 &&
                    m_SourcePeriodUsQ16 > maximum / cadence.frameDelta ?
                        maximum : m_SourcePeriodUsQ16 * cadence.frameDelta;
                m_SourceTimeUsQ16 = saturatingAdd(m_SourceTimeUsQ16,
                                                   projectedMovementQ16);
                m_SourceTimeUs = roundedQ16(m_SourceTimeUsQ16);

                if (cadence.phaseDiscontinuity) {
                    // A large cadence transition or isolated source gap is a
                    // local phase event, not a reason to forget the learned
                    // rate. Anchor the live one-slot path to the ready frame
                    // while the cumulative estimator confirms or abandons its
                    // provisional segment.
                    anchorSourceTime(sourceMappingUs(frame));
                }

                m_LastFrameNumber = frame.frameNumber();
                m_HaveLastFrameNumber = frame.frameNumber() >= 0;
                m_LastRtpTimestamp = frame.rtpTimestamp();
                m_LastTimestampValid = frame.timestampValid();
            }
        }
    }

    updateLatencyFixState();

    // Timestamp playout: the target is the sender timestamp mapped into the
    // local clock plus one constant delay. The mapping offset is the windowed
    // minimum of the selected mapping clock minus RTP time. Production uses
    // immutable decoder output; the trajectory experiment removes measured
    // queue/hold from this virtual clock. Historical captures can select decode complete. Live sessions age samples
    // and bound correction by elapsed observation time, independent of FPS;
    // historical captures retain per-frame slewing. Steady-state corrections
    // only move adjacent targets by the bounded step. Nothing below re-anchors on
    // a late or early frame: a late frame simply clamps to "now" and the next
    // frame returns to its own slot.
    const bool timestampPlayout = timestampPlayoutEnabled() &&
        frame.timestampValid() && (rebased || cadence.usedRtpTimestamp);
    m_TimestampPlayoutActive = timestampPlayout;
    const uint64_t rtpUs = timestampPlayout ?
        rtpTicksToUs(m_UnwrappedRtpTicks) : 0;
    int64_t readyOffsetUs = 0;
    int64_t smoothingUs = 0;
    uint64_t smoothingReserveUs = 0;
    int64_t desiredRetimingUs = 0;
    uint64_t smoothingReadyPushUs = 0;
    uint64_t missedTicks = 0;
    uint64_t delayBeforeUs = 0;
    const bool readinessBoundedSmoothing = timestampPlayout &&
        !metronomeEnabled() && m_Parameters.playoutResponsiveBuffer >= 6 &&
        m_Parameters.playoutSmoothingReadinessBound != 0;
    const uint64_t leadUs = saturatingAdd(m_Parameters.playoutPredictionEnabled ? typicalRenderUs() : m_RenderLeadUs,
                                          m_Parameters.presentationSafetyUs);
    if (timestampPlayout) {
        const uint64_t mappingUs = sourceMappingUs(frame);
        const int64_t offsetUs = signedDifference(mappingUs,
                                                  rtpUs);
        const bool timeBasedOffset =
            m_Parameters.playoutOffsetSlewUsPerSecond != 0;
        const uint64_t offsetClockUs = timeBasedOffset ?
            (m_Parameters.playoutOffsetSourceClock != 0 ? rtpUs : nowUs) :
            mappingUs;
        // Mode 4 observes a physical receive/RTP clock before decoder service
        // and adaptive timing are added. Invalid packets do not teach drift.
        m_ReceiveDriftPacketValid = frame.timestampValid() && frame.receiveUs() != 0 &&
            cadence.eligible && frame.lostPackets() == 0;
        m_ReceiveDriftPacketOffsetUs = signedDifference(frame.receiveUs(), rtpUs);
        const int64_t appliedOffsetUs = observePlayoutOffset(
            offsetClockUs,
            offsetUs, rebased || cadence.eligible, cadence.phaseDiscontinuity);
        anchorSourceTime(addSigned(rtpUs, appliedOffsetUs));
        readyOffsetUs = signedDifference(sourceMappingUs(frame),
                                         m_SourceTimeUs);
        m_ReadinessBudgetUs = 0;
        m_ReadinessPhaseUs = 0;
        m_PhaseErrorFrames = 0;
        // The smoothed slot is decided against the delay in force before this
        // frame's lateness is admitted. A proven miss may retain more
        // protection immediately; ordinary growth remains rate-limited. The
        // calibrator then sees lateness against the slot actually used, so
        // a schedule that runs ahead of a late-stamped frame is paid for by
        // the delay rather than by a late present.
        delayBeforeUs = effectivePlayoutDelayUs();
        const uint64_t rawBasisUs = saturatingAdd(m_SourceTimeUs,
                                                  delayBeforeUs);
        int64_t remainingDebtUs = 0;
        if (metronomeEnabled()) {
            smoothingUs = metronomeAdjustUs(
                cadence, rebased, rawBasisUs, delayBeforeUs, nowUs,
                missedTicks, remainingDebtUs);
        }
        else {
            // The reserve delays the whole smoothed schedule while Reduce
            // judder is enabled, including frames it cannot smooth, so the
            // schedule does not step by the reserve at every cadence reset.
            smoothingReserveUs = m_Parameters.playoutSmoothingWindowedCadence == 7 ?
                absorbReserveUs() : smoothingReserveEnabled() ? m_SmoothingReserveUs : 0;
            int64_t retimingUs = cadenceSmoothingAdjustUs(
                cadence, rebased, saturatingAdd(rawBasisUs, smoothingReserveUs),
                delayBeforeUs, smoothingReserveUs);
            const int64_t slewUs = static_cast<int64_t>(
                m_Parameters.playoutSmoothingResetSlewUs);
            if (slewUs != 0 && !m_SmoothingEngaged && !rebased &&
                    m_Parameters.playoutSmoothingGainPerMille != 0) {
                // The smoother reset this frame onto its raw slot. Ease the
                // retiming the previous frame carried back to it instead of
                // stepping the whole difference into one presented interval.
                // A new clock epoch has no comparable raw slot and still jumps.
                retimingUs = m_LastSmoothingRetimingUs > slewUs ?
                    m_LastSmoothingRetimingUs - slewUs :
                    m_LastSmoothingRetimingUs < -slewUs ?
                    m_LastSmoothingRetimingUs + slewUs : 0;
                retimingUs = std::min(retimingUs, static_cast<int64_t>(
                    m_Parameters.playoutSmoothingMaxLagUs -
                    std::min(m_Parameters.playoutSmoothingMaxLagUs, smoothingReserveUs)));
                retimingUs = std::max(retimingUs, -static_cast<int64_t>(
                    saturatingAdd(delayBeforeUs, smoothingReserveUs)));
            }
            desiredRetimingUs = retimingUs;
            if (readinessBoundedSmoothing && (m_Parameters.playoutSmoothingWindowedCadence < 4 ||
                    m_Parameters.playoutSmoothingWindowedCadence == 6 ||
                    m_Parameters.playoutSmoothingWindowedCadence == 7)) {
                // Decode readiness is already known. Spend only the time left
                // before the raw slot, retaining its typical render allowance.
                // A raw-late frame keeps that deadline for real buffer feedback;
                // it must not advance it even further to satisfy the smoother.
                const int64_t readyFloorUs = std::min<int64_t>(0,
                    readyOffsetUs - static_cast<int64_t>(delayBeforeUs));
                retimingUs = std::max(retimingUs, readyFloorUs -
                    static_cast<int64_t>(smoothingReserveUs));
                smoothingReadyPushUs = static_cast<uint64_t>(retimingUs - desiredRetimingUs);
            }
            // Source-gap correction is a retained source phase. Clipping it
            // to this frame's readiness removes/reapplies that phase on later
            // clean gaps, magnifying them. Revision 4 keeps the phase intact;
            // targetUs still applies the independent known-readiness floor.
            if (m_Parameters.playoutSmoothingWindowedCadence == 7) {
                // Timing can spend the fixed pad, but cannot create an
                // additional positive reserve or borrow the input buffer.
                retimingUs = std::clamp<int64_t>(retimingUs, -int64_t(smoothingReserveUs), 0);
            }
            m_LastSmoothingRetimingUs = retimingUs;
            smoothingUs = retimingUs + static_cast<int64_t>(smoothingReserveUs);
        }
        // The calibrator sees lateness against the slot the schedule is
        // trying to reach, not the slot it currently occupies: while the
        // tick still carries lag from a late frame, on-time arrivals would
        // otherwise look early and the cushion would release until frames
        // were late all the time.
        updatePlayoutDelay(frame, cadence, rebased,
                           readyOffsetUs - (bufferedSmoothingUs(smoothingUs) - remainingDebtUs),
                           nowUs);
    }
    else {
        resetCadenceSmoothing();
        m_LastSmoothingRetimingUs = 0;
        if (m_Parameters.playoutSmoothingWindowedCadence) {
            m_SmoothingCadenceCount = 0;
            m_SmoothingCadenceIndex = 0;
            m_CadenceStableSinceUs = 0;
        }
        readyOffsetUs = signedDifference(sourceMappingUs(frame),
                                         m_SourceTimeUs);
        if (!rebased && !cadence.phaseDiscontinuity && cadence.eligible) {
            const int64_t ceilingUs =
                static_cast<int64_t>(readinessCeilingUs());
            if (readyOffsetUs < -ceilingUs) {
                // A frame that is ready well before the old slower clock must
                // not wait behind an obsolete cutscene cadence. The display
                // floor and latched near-refresh mode still bound how quickly
                // it can submit.
                anchorSourceTime(sourceMappingUs(frame));
                readyOffsetUs = 0;
                cadence.phaseDiscontinuity = true;
                cadence.eligible = false;
                m_PhaseErrorFrames = 0;
            }
            else if (readyOffsetUs > ceilingUs) {
                ++m_PhaseErrorFrames;
            }
            else {
                m_PhaseErrorFrames = 0;
            }

            if (!cadence.phaseDiscontinuity &&
                    m_PhaseErrorFrames >= m_Parameters.phaseErrorFrames) {
                // A bounded readiness reserve cannot repay a sustained source
                // phase error. Re-anchor locally while retaining the
                // cumulative cadence fit, rather than repeatedly rebasing the
                // whole model.
                anchorSourceTime(sourceMappingUs(frame));
                readyOffsetUs = 0;
                cadence.phaseDiscontinuity = true;
                cadence.eligible = false;
                m_PhaseErrorFrames = 0;
            }
        }
        else {
            m_PhaseErrorFrames = 0;
        }
        if (rebased || cadence.sourceRateChanged ||
            cadence.phaseDiscontinuity) {
            // A new source epoch or local phase recovery is anchored by the
            // first directly observed ready offset. Cadence history is
            // retained for the phase-only cases above.
            m_ReadyOffsets.clear();
            const int64_t ceilingUs =
                static_cast<int64_t>(readinessCeilingUs());
            m_ReadinessPhaseUs = std::max(
                -ceilingUs, std::min(readyOffsetUs, ceilingUs));
            // A source-phase reset must not acquire a standing reserve in one
            // cadence-breaking jump. Start on the observed phase and let
            // clean arrival evidence build or release the reserve smoothly.
            const bool retainReserve =
                m_Parameters.retainReadinessOnPhaseReset != 0;
            applyReadinessBudget(retainReserve, retainReserve);
        }
    }

    // The metronome tick was placed against the delay in force when the raw
    // slot was mapped; applying the calibrator's newer value here would put
    // its slew on the presented interval. It reaches the schedule through
    // the next frame's raw slot instead.
    const uint64_t proposedPlayoutDelayUs = timestampPlayout ?
        (metronomeEnabled() ? delayBeforeUs : effectivePlayoutDelayUs()) :
        m_Parameters.sourcePlayoutDelayUs;
    const uint64_t playoutDelayUs = m_Parameters.playoutInputHitchMissesOnly >= 9 ?
        proposedPlayoutDelayUs : std::min(proposedPlayoutDelayUs, playoutDelayCapUs());
    const bool limitedBuffers = timestampPlayout &&
        (m_Parameters.playoutInputHitchMissesOnly >= 9 || m_Parameters.playoutSmoothingWindowedCadence == 7);
    const int64_t desiredSmoothingUs = smoothingUs;
    if (limitedBuffers) {
        m_ExtraBufferSlew.begin(uint64_t(frame.frameNumber()), nowUs, playoutDelayUs,
            intervalQualityToleranceUs(m_Parameters), int64_t(absorbReserveUs()),
            m_BufferLayerChanged, m_BufferLayerDrain && m_Parameters.playoutInputHitchMissesOnly < 10,
            m_Parameters.playoutInputHitchMissesOnly < 10);
        smoothingUs = m_ExtraBufferSlew.smoothing(smoothingUs);
        m_LastSmoothingRetimingUs = smoothingUs - int64_t(smoothingReserveUs);
    }
    const uint64_t renderOffsetUs = m_Parameters.playoutPredictionEnabled ? typicalRenderUs() : m_RenderLeadUs;
    const uint64_t compositorLeadUs = m_Parameters.playoutPredictionEnabled &&
        !m_Parameters.playoutPredictionOnly ? m_PresentationPrediction.lead(nowUs) : 0;
    uint64_t targetUs = saturatingAdd(
        addSigned(addSigned(m_SourceTimeUs, m_ReadinessBudgetUs),
                  smoothingUs),
        saturatingAdd(
            playoutDelayUs,
            saturatingAdd(renderOffsetUs,
                          m_Parameters.presentationSafetyUs)));
    const uint64_t originalTargetUs = targetUs;
    targetUs = std::max(
        targetUs,
        saturatingAdd(nowUs,
                      saturatingAdd(renderOffsetUs,
                                    m_Parameters.presentationSafetyUs)));

    // This is a live, one-slot path. An unconfirmed RTP/frame jump may
    // describe already-skipped content, never hundreds of milliseconds that
    // the client should wait again. Reseed poisoned playout phase without
    // discarding the cumulative cadence model.
    const uint64_t maximumDirectTargetUs = saturatingAdd(
        saturatingAdd(nowUs,
                      static_cast<uint64_t>(std::max<int64_t>(smoothingUs, 0))),
        saturatingAdd(
            std::max(m_ConfiguredStreamPeriodUs, m_SourcePeriodUs),
            saturatingAdd(
                playoutDelayUs,
                saturatingAdd(m_RenderLeadUs,
                              m_Parameters.presentationSafetyUs))));
    bool reseedPhase = targetUs > maximumDirectTargetUs;
    if (reseedPhase && timestampPlayout) {
        // Under timestamp playout one early outlier is not a clock jump.
        // The frame simply waits for its slot; only a run of frames that
        // all map into the future re-seeds the mapping.
        const uint64_t requiredFrames = std::max<uint64_t>(
            1, m_Parameters.playoutOffsetReseedFrames);
        ++m_FutureProjectionFrames;
        reseedPhase = m_FutureProjectionFrames >= requiredFrames;
    }
    else {
        m_FutureProjectionFrames = 0;
    }
    if (reseedPhase) {
        m_FutureProjectionFrames = 0;
        smoothingUs = 0;
        desiredRetimingUs = 0;
        smoothingReadyPushUs = 0;
        m_LastSmoothingRetimingUs = 0;
        missedTicks = 0;
        resetCadenceSmoothing();
        // The metronome restarts on the re-seeded slot; the grid tick this
        // frame was given is not a basis to owe the jump against.
        m_Pending.hasSmoothedBasis = false;
        // Do not clear cadence history when a faster source makes the old
        // playout phase point into the future. Reseed phase from this already
        // decoded frame and let the cumulative fit heal the rate.
        const uint64_t mappingUs = sourceMappingUs(frame);
        anchorSourceTime(mappingUs);
        m_ReadyOffsets.clear();
        m_ReadinessBudgetUs = 0;
        m_ReadinessPhaseUs = 0;
        if (timestampPlayout) {
            // The frame is more than a source period earlier than the mapped
            // clock predicts: the sender clock jumped. Re-seed the offset on
            // this frame rather than making it wait out a stale mapping.
            resetPlayoutOffsets();
            const bool timeBasedOffset =
                m_Parameters.playoutOffsetSlewUsPerSecond != 0;
            const uint64_t offsetClockUs = timeBasedOffset ?
                (m_Parameters.playoutOffsetSourceClock != 0 ? rtpUs : nowUs) :
                mappingUs;
            observePlayoutOffset(
                offsetClockUs,
                signedDifference(mappingUs, rtpUs));
        }
        readyOffsetUs = 0;
        cadence.phaseDiscontinuity = true;
        cadence.eligible = false;
        m_PhaseErrorFrames = 0;
        targetUs = saturatingAdd(
            std::max(mappingUs, nowUs),
            saturatingAdd(
                playoutDelayUs,
                saturatingAdd(m_RenderLeadUs,
                              m_Parameters.presentationSafetyUs)));
    }
    if (timestampPlayout && !metronomeEnabled() && smoothingReserveEnabled()) {
        // This frame has already arrived, so its own readiness against the
        // raw slot is known. Charge the reserve only for lateness the smoother
        // caused by moving the frame before that slot. Delivery that misses
        // the raw slot is the playout buffer's evidence. Worker backlog is not
        // measured here: it follows the previous (already reserved) target,
        // shifts with the reserve, and would otherwise feed it back into itself.
        const int64_t rawLatenessUs = readyOffsetUs -
            static_cast<int64_t>(playoutDelayUs);
        // Learn from the requested retiming, before its readiness bound. The
        // bounded smoothing reserve can then cover future early slots without
        // asking the independent interval buffer to grow or hold for them.
        const int64_t retimingUs = readinessBoundedSmoothing ? desiredRetimingUs :
            smoothingUs - static_cast<int64_t>(smoothingReserveUs);
        observeSmoothingReserve(m_SmoothingEngaged && !reseedPhase,
                                std::min<int64_t>(rawLatenessUs, 0) - retimingUs,
                                nowUs);
    }

    if (m_CanLatchPresentation && m_Parameters.nativeSynchronizedPresentation != 0) {
        m_LatchedPresentation = true;
    }
    else if (m_Parameters.playoutAdaptiveOnly != 0) {
        m_LatchedPresentation = false;
    }
    else if (m_Parameters.playoutRateProtectionEnabled != 0) {
        // Stay protected throughout the near-refresh source-rate range,
        // including the first frame. Late CPU/GPU work must not briefly
        // switch this range back to an immediate tearing present.
        m_LatchedPresentation = rateProtectedPresentation();
    }
    else if (m_Parameters.playoutPerFrameLatch != 0) {
        // Judge this frame's planned submission. Revision 1 omitted the
        // headroom that VRR12 required beyond the display period and guard:
        // at 116/120 its 188 us remaining slack was incorrectly enough to
        // permit tearing. Restore that margin and full exit hysteresis without
        // adding it to the playout buffer or imposing a source-rate band.
        // Explicit revision 1 remains unchanged for historical exact replay.
        const uint64_t safetyHeadroomUs = m_Parameters.playoutPerFrameLatch >= 2 ?
            (m_LatchedPresentation ? latchedPresentationExitHeadroomUs() :
                                     latchedPresentationHeadroomUs()) : 0;
        const uint64_t safeAdaptiveUs = saturatingAdd(spacingAnchorUs(),
            saturatingAdd(saturatingAdd(m_DisplayPeriodUs, m_GuardUs),
                          safetyHeadroomUs));
        // After a gap past the VRR range the driver may be mid-repeat of the
        // previous frame; let the flip queue place this one.
        const bool beyondVrrFloor = m_Parameters.vrrFloorLatchGapUs != 0 &&
            m_HaveLastSubmission &&
            targetUs >= saturatingAdd(spacingAnchorUs(), m_Parameters.vrrFloorLatchGapUs);
        m_LatchedPresentation = m_CanLatchPresentation && m_HaveLastSubmission &&
                                (targetUs < safeAdaptiveUs || beyondVrrFloor);
    }
    const uint64_t unflooredTargetUs = targetUs;
    uint64_t requestedPhaseTargetUs = originalTargetUs;
    if (m_Parameters.playoutPredictionEnabled && !m_Parameters.playoutPredictionOnly &&
        !m_LatchedPresentation) {
        const auto scanoutFloor = m_PresentationPrediction.floor(nowUs, m_DisplayPeriodUs, m_GuardUs);
        const uint64_t requestedFloorUs = scanoutFloor > compositorLeadUs ? scanoutFloor - compositorLeadUs : 0;
        targetUs = std::max(targetUs, requestedFloorUs);
        requestedPhaseTargetUs = std::max(requestedPhaseTargetUs, requestedFloorUs);
    }
    const uint64_t earliestUs = earliestSubmissionUs();
    targetUs = std::max(targetUs, earliestUs);
    requestedPhaseTargetUs = std::max(requestedPhaseTargetUs, earliestUs);
    const uint64_t presentationFloorPushUs = targetUs - unflooredTargetUs;
    const uint64_t physicalTargetUs = targetUs;
    const uint64_t physicalRequestedPhaseTargetUs = requestedPhaseTargetUs;
    uint64_t recoveryAddedUs = 0;
    // Recovery is not display-floor backlog: the drop policy must not discard
    // this frame just because we intentionally spread out its catch-up.
    // Recover actual backlog only, after the native protection decision. A
    // late source slot may already require a latched present; recovery must
    // never remove that protection. Never hold past the stale horizon.
    const uint64_t elapsed = nowUs > frame.decoderOutputUs() ? nowUs - frame.decoderOutputUs() : 0;
    const uint64_t queueAge = elapsed - std::min(elapsed, frame.decodeSyncWaitUs());
    // Bounded catch-up (PhaseTrajectory) owns recovery when enabled. This
    // VRR17 floor spaces each frame from the previous actual submission and
    // re-arms on any submission past the original plan, so with a trajectory
    // carrying it stayed armed and re-imposed every wake-up overshoot: carry
    // sat at its 4 ms extra limit (2026-10-10 23:38, 81% of frames pushed).
    const bool recoveryEligible = !m_Parameters.boundedCatchupEnabled &&
        timestampPlayout && !rebased && !reseedPhase &&
        cadence.eligible && !cadence.phaseDiscontinuity && !cadence.sourceRateChanged &&
        m_HaveLastSubmission &&
        m_Parameters.playoutCatchupPerMille != 0 &&
        m_SourcePeriodUs <= 1000000 && cadence.intervalUs >= m_SourcePeriodUs * 9 / 10 &&
        cadence.intervalUs <= m_SourcePeriodUs * 11 / 10;
    if (recoveryEligible && (m_CatchupActive || queueAge > m_SourcePeriodUs)) {
        const bool lateRecovery = m_Parameters.playoutLateRecovery != 0;
        // Intentional playout residence is not overload pressure. Rescue a
        // late image without immediately accelerating its successors merely
        // because the selected jitter buffer occupies more than one period.
        const uint64_t pressureAge = lateRecovery ?
            queueAge - std::min(queueAge, playoutDelayUs) : queueAge;
        const uint64_t floor = VrrCatchUp::floorUs(m_LastSubmissionUs, m_SourcePeriodUs,
            saturatingAdd(m_DisplayPeriodUs, m_GuardUs), pressureAge,
            m_Parameters.playoutCatchupPerMille);
        const uint64_t horizon = lateRecovery ?
            std::max(m_SourcePeriodUs * 2, saturatingAdd(playoutDelayUs, m_SourcePeriodUs)) :
            m_SourcePeriodUs * 2;
        const uint64_t hardDeadline = saturatingAdd(frame.decoderOutputUs(),
            saturatingAdd(horizon, frame.decodeSyncWaitUs()));
        m_CatchupActive = floor != 0 && (floor > targetUs || queueAge > m_SourcePeriodUs) &&
            queueAge < horizon && targetUs < hardDeadline;
        // Bound temporary recovery separately from the adaptive jitter buffer.
        // The older 1 ms bound could still produce a sharp short interval
        // immediately after a small render stall. Allow at most half a source
        // period, capped at 4 ms, without moving the underlying source clock.
        const uint64_t extraLimit = lateRecovery ?
            std::min<uint64_t>(4000, m_SourcePeriodUs / 2) : 1000;
        if (m_CatchupActive) {
            requestedPhaseTargetUs = std::max(requestedPhaseTargetUs,
                std::min({floor, hardDeadline, saturatingAdd(requestedPhaseTargetUs, extraLimit)}));
            const uint64_t recoveredUs = std::max(targetUs,
                std::min({floor, hardDeadline, saturatingAdd(targetUs, extraLimit)}));
            recoveryAddedUs = recoveredUs - targetUs;
            targetUs = recoveredUs;
        }
    }
    else {
        m_CatchupActive = false;
    }
    Vrr13::PhaseTrajectory::Stats trajectory;
    if (m_Parameters.boundedCatchupEnabled) {
        const bool valid = timestampPlayout && !rebased && !cadence.phaseDiscontinuity &&
            !cadence.sourceRateChanged;
        m_Pending.trajectoryValid = valid;
        // The previous actual submission includes its wake-up overshoot, and
        // this frame will overshoot too. Counting it twice left 8 us/frame of
        // catch-up room at 116 FPS on 120 Hz, so carry lingered; remove the
        // smallest recent overshoot, which the next submission also incurs.
        const uint64_t anchorUs = spacingAnchorUs();
        const uint64_t ownershipFloor = m_HaveLastSubmission ?
            saturatingAdd(anchorUs - std::min(anchorUs, m_PhaseTrajectory.minimumRecentLatenessUs()),
                          m_DisplayPeriodUs) : 0;
        trajectory = m_PhaseTrajectory.plan(uint64_t(frame.frameNumber()),
            addSigned(m_SourceTimeUs, smoothingUs), originalTargetUs, targetUs,
            saturatingAdd(nowUs, saturatingAdd(renderOffsetUs, m_Parameters.presentationSafetyUs)),
            m_DisplayPeriodUs, ownershipFloor, intervalQualityToleranceUs(m_Parameters),
            m_SourcePeriodUs, valid, catchupCarryLimitUs());
        targetUs = trajectory.targetUs;
        requestedPhaseTargetUs = std::max(requestedPhaseTargetUs, trajectory.requestedTargetUs);
        recoveryAddedUs = saturatingAdd(recoveryAddedUs, trajectory.addedUs);
    }
    if (limitedBuffers) {
        const uint64_t desiredRecoveryUs = targetUs - std::min(targetUs, physicalTargetUs);
        const uint64_t recoveryUs = m_ExtraBufferSlew.recovery(desiredRecoveryUs);
        targetUs = std::max(physicalTargetUs, saturatingAdd(originalTargetUs, recoveryUs));
        // Keep known input readiness out of the requested phase. Otherwise a
        // late input would move its own scoring deadline and erase the miss.
        requestedPhaseTargetUs = std::max(physicalRequestedPhaseTargetUs,
            saturatingAdd(originalTargetUs, recoveryUs));
        recoveryAddedUs = targetUs - physicalTargetUs;
        if (m_Parameters.boundedCatchupEnabled) {
            m_PhaseTrajectory.applyTargetLimit(targetUs, requestedPhaseTargetUs);
            trajectory = m_PhaseTrajectory.stats();
        }
        if (!m_InputPercentileBuffer.layerTransitionActive() &&
            !m_ExtraBufferSlew.draining(desiredSmoothingUs, desiredRecoveryUs)) m_BufferLayerDrain = false;
    }
    // GPU readiness is learned independently from CPU/render preparation.
    // Keep it out of targetUs: it is an earlier start opportunity, not extra
    // presentation latency. The source-period bound prevents the worker from
    // preparing so early that one frame can occupy the whole pacing window.
    const uint64_t gpuReadinessLeadUs = this->gpuReadinessLeadUs();
    const uint64_t totalLeadUs = std::min(
        m_SourcePeriodUs,
        saturatingAdd(saturatingAdd(m_RenderLeadUs,
                                    m_RenderWakeLeadUs),
                      gpuReadinessLeadUs));
    uint64_t renderStartUs = targetUs > totalLeadUs ?
        targetUs - totalLeadUs : 0;
    if (timestampPlayout && m_Parameters.playoutPrepareOnArrival != 0) {
        // Preparation may use the existing playout interval. Native image
        // acquisition can still apply backpressure; the presentation target
        // remains unchanged regardless of when that acquisition completes.
        const uint64_t arrivalLeadUs = saturatingAdd(totalLeadUs,
                                                     playoutDelayUs);
        renderStartUs = targetUs > arrivalLeadUs ?
            targetUs - arrivalLeadUs : 0;
    }
    if (m_Parameters.renderStartAfterSubmissionUs != 0 && m_HaveLastSubmission) {
        const uint64_t earliestStartUs = saturatingAdd(
            m_LastSubmissionUs, m_Parameters.renderStartAfterSubmissionUs);
        // Acquisition spacing may use spare lead, but must not squeeze the
        // learned preparation budget back to a fixed 2.5 ms. Increasing the
        // playout buffer cannot repair that loss of rendering opportunity.
        const uint64_t minimumLeadUs = m_Parameters.renderStartPreserveLearnedLead ?
            std::max(m_Parameters.renderStartMinimumLeadUs, totalLeadUs) :
            m_Parameters.renderStartMinimumLeadUs;
        const uint64_t latestStartUs =
            targetUs > minimumLeadUs ? targetUs - minimumLeadUs : 0;
        if (earliestStartUs > renderStartUs) {
            renderStartUs = std::min(earliestStartUs,
                                     std::max(renderStartUs, latestStartUs));
        }
    }

    VrrTimingDecision decision;
    decision.trajectory = trajectory;
    decision.frameNumber = uint64_t(frame.frameNumber());
    decision.smoothnessProtectionUs = m_Parameters.playoutPredictionOnly ?
        m_SubmissionSmoothness.protectionUs() :
        m_Parameters.playoutNativeHitchAdaptation ?
            activeSmoothnessFeedback(nowUs).protectionUs() :
            std::max(m_SubmissionSmoothness.protectionUs(),
                     m_NativeSmoothness.protectionUs());
    decision.requestedPlayoutDelayUs =
        m_Parameters.playoutCapacityTelemetry != 0 ?
            m_UnclampedRequestedPlayoutDelayUs : m_RequestedPlayoutDelayUs;
    decision.playoutCapacityLimited =
        m_Parameters.playoutCapacityTelemetry != 0 &&
        m_UnclampedRequestedPlayoutDelayUs > playoutDelayMaximumUs();
    decision.submissionSmoothnessSamples = m_SubmissionSmoothness.samples();
    decision.submissionSmoothnessMisses = m_SubmissionSmoothness.misses();
    decision.nativeSmoothnessSamples = m_NativeSmoothness.samples();
    decision.nativeSmoothnessMisses = m_NativeSmoothness.misses();
    decision.originalScanoutUs = saturatingAdd(originalTargetUs, compositorLeadUs);
    decision.predictedScanoutUs = saturatingAdd(targetUs, compositorLeadUs);
    decision.compositorLeadUs = compositorLeadUs;
    decision.recoveryHeadroomUs = m_Parameters.playoutPredictionEnabled ? recoveryHeadroomUs() : 0;
    decision.originalTargetUs = originalTargetUs;
    decision.sourceTimeUs = m_SourceTimeUs;
    decision.sourceIntervalUs = cadence.intervalUs;
    decision.sourcePeriodUs = m_SourcePeriodUs;
    decision.readyOffsetUs = readyOffsetUs;
    decision.readinessBudgetUs = m_ReadinessBudgetUs;
    decision.playoutDelayUs = playoutDelayUs;
    decision.cadenceSmoothingUs = smoothingUs;
    decision.missedTicks = missedTicks;
    decision.renderStartUs = renderStartUs;
    decision.targetUs = targetUs;
    decision.presentationFloorPushUs = presentationFloorPushUs;
    decision.requestedPhaseTargetUs = requestedPhaseTargetUs;
    decision.recoveryAddedUs = recoveryAddedUs;
    decision.playoutDelayMaximumUs = playoutDelayMaximumUs();
    decision.playoutQueueLimitUs = playoutQueueLimitUs();
    decision.playoutPresetCapUs = playoutDelayCapUs();
    decision.playoutOffsetUs = playoutOffsetUs();
    decision.guardUs = m_GuardUs;
    decision.headroomUs = headroomUs();
    decision.timingBudgetUs = timingBudgetUs();
    decision.renderLeadUs = m_RenderLeadUs;
    decision.renderWakeLeadUs = m_RenderWakeLeadUs;
    decision.gpuReadinessLeadUs = gpuReadinessLeadUs;
    decision.targetWakeLeadUs = m_TargetWakeLeadUs;
    const uint64_t learnedHeadroomUs = decision.headroomUs;
    const bool cadenceUnstable = rebased || !cadence.eligible ||
        cadence.sourceRateChanged || cadence.phaseDiscontinuity;
    bool cadenceLatchActive = false;
    if (m_Parameters.cadenceStabilityLatchFrames != 0) {
        if (cadenceUnstable) {
            // Immediate tearing presents are only safe after the source phase
            // has remained coherent. A source hitch followed by a decoder
            // burst can otherwise queue several adaptive presents into one
            // scanout interval, overwriting frames and producing a visible
            // fluidity break even though the display has ample rate headroom.
            m_CadenceStabilityLatchFramesRemaining =
                m_Parameters.cadenceStabilityLatchFrames;
            cadenceLatchActive = true;
        }
        else if (m_CadenceStabilityLatchFramesRemaining != 0) {
            cadenceLatchActive = true;
            --m_CadenceStabilityLatchFramesRemaining;
        }
    }
    if (m_CanLatchPresentation && m_Parameters.nativeSynchronizedPresentation != 0) {
        m_LatchedPresentation = true;
    }
    else if (m_Parameters.playoutAdaptiveOnly != 0) {
        m_LatchedPresentation = false;
    }
    else if (m_Parameters.playoutRateProtectionEnabled != 0 ||
            m_Parameters.playoutPerFrameLatch != 0) {
        // Selected before applying the software floor above. Native latching
        // carries the frame to the next scanout only when this slot needs it.
    }
    else if (!m_CanLatchPresentation) {
        m_LatchedPresentation = false;
    }
    else if (cadenceLatchActive) {
        m_LatchedPresentation = true;
    }
    else if (m_LatchedPresentation) {
        // Production requires the full exit threshold so small guard or
        // cadence fluctuations cannot bounce a borderline stream between
        // adaptive and latched presentation. The base-guard shortcut remains
        // parameterized only to reproduce captures made under the legacy
        // absolute/scaled latch policies.
        if (learnedHeadroomUs >=
                latchedPresentationExitHeadroomUs() ||
            (m_Parameters.latchedPresentationBaseGuardExit != 0 &&
             m_GuardUs == m_BaseGuardUs &&
             learnedHeadroomUs >=
                latchedPresentationHeadroomUs())) {
            m_LatchedPresentation = false;
        }
    }
    else if (learnedHeadroomUs <
             latchedPresentationHeadroomUs()) {
        m_LatchedPresentation = true;
    }
    decision.latchedPresentation = m_LatchedPresentation;
    decision.usedRtpTimestamp = cadence.usedRtpTimestamp;
    decision.cadenceEligible = !rebased && cadence.eligible;
    decision.sourceRateChanged = !rebased && cadence.sourceRateChanged;
    decision.phaseDiscontinuity = !rebased && cadence.phaseDiscontinuity;
    decision.rebased = rebased;
    decision.diagnosticSourceTimestampUs = rtpUs;
    decision.diagnosticSourceTimestampValid = timestampPlayout && !rebased && !cadence.phaseDiscontinuity;

    m_Pending.valid = true;
    m_Pending.smoothness = smoothnessSample(decision);
    m_Pending.smoothness.intended = decision.originalTargetUs;
    // Follow the host and deliberate judder correction. Buffer/render-budget
    // changes do not redefine the intended motion or conceal client errors.
    m_Pending.intervalIntendedUs = addSigned(decision.sourceTimeUs, decision.cadenceSmoothingUs);
    // Reuse the chart's input-slot basis, but use the producer's decoder
    // output timestamp. GPU completion sampled after a pacing-queue wait is
    // only an upper bound and would count our own delay as fresh jitter.
    m_Pending.inputObservationUs = nowUs;
    // A standing reserve or a later retiming is not delivery slack the input
    // buffer may release; only an early retiming the reserve does not cover
    // must be buffered.
    m_Pending.inputSlotUs = addSigned(addSigned(decision.sourceTimeUs,
        bufferedSmoothingUs(decision.cadenceSmoothingUs)), decision.readinessBudgetUs);
    m_Pending.inputReadinessBudgetUs = decision.readinessBudgetUs;
    m_Pending.inputClockOffsetUs = inputClockOffsetUs(nowUs);
    // Readiness is the (estimated) decode completion: PyroWave's decoder
    // output is only GPU submission, and GPU decode variation is jitter the
    // buffer must cover. Completions observed after a queue wait are bounded
    // by output + typical GPU time so our own queueing is not counted.
    const auto inputBacklog = saturatingAdd(frame.decoderQueueUs(), frame.decodeHoldUs());
    const auto decodeReadyUs = estimatedDecodeCompleteUs(frame);
    const auto inputReadyUs = decodeReadyUs - std::min(decodeReadyUs, inputBacklog);
    m_Pending.inputRequiredUs = signedDifference(inputReadyUs, m_Pending.inputSlotUs);
    m_Pending.inputValid = frame.reassembledUs() != 0 &&
        frame.decodeSubmitUs() >= frame.reassembledUs() &&
        frame.decoderOutputUs() >= frame.decodeSubmitUs() &&
        !decision.sourceRateChanged &&
        (!m_Parameters.normalizedSourceMapping || m_CorrectedMappingBasis);
    m_Pending.intervalValid = decision.usedRtpTimestamp && !decision.rebased && !decision.phaseDiscontinuity;
    // Timestamp playout never feeds the learned readiness reserve.
    m_Pending.cadenceEligible = decision.cadenceEligible && !timestampPlayout;
    m_Pending.readyOffsetUs = readyOffsetUs;
    m_Pending.decodeSyncWaitUs = frame.decodeSyncWaitUs();
    if (m_Parameters.playoutPredictionEnabled) {
        const uint64_t stall = std::max(m_Parameters.playoutStallExclusionUs, scaledPerMille(m_SourcePeriodUs, 1500));
        // Padding must cover readiness relative to the cadence we actually
        // schedule, with the padding itself removed. Comparing only with raw
        // RTP time misses the extra readiness requirement of an earlier
        // smoothed slot. Intentional worker waits are excluded by the FIFO
        // readiness model; late native presentation is not readiness work.
        const uint64_t unpaddedSlotUs = m_Parameters.playoutReadinessDrivenAdaptation &&
            !m_Parameters.playoutResponsiveBuffer ?
            addSigned(m_SourceTimeUs, smoothingUs) : m_SourceTimeUs;
        m_Pending.prediction = {sourceMappingUs(frame), unpaddedSlotUs, m_SourcePeriodUs,
            typicalRenderUs(), playoutDelayUs,
            m_Parameters.playoutReadinessDrivenAdaptation ? 0 : recoveryHeadroomUs(),
            m_Parameters.playoutDelayMarginUs,
            frame.decoderQueueUs(),
            timestampPlayout && !rebased && cadence.eligible && !cadence.phaseDiscontinuity &&
            cadence.frameDelta == 1 && cadence.intervalUs <= stall &&
            cadence.intervalUs >= scaledPerMille(m_SourcePeriodUs, m_Parameters.playoutBurstExclusionPerMille)};
        if (m_Parameters.playoutResponsiveBuffer) {
            // RTP spacing owns source timing even while the rate fit catches up.
            // Only actual delivery/FIFO lateness enters the raw predictor.
            auto& probe = m_Pending.prediction;
            probe.period = std::max(m_ConfiguredStreamPeriodUs, cadence.intervalUs);
            probe.eligible = timestampPlayout && !rebased && cadence.usedRtpTimestamp &&
                cadence.frameDelta == 1 && !cadence.phaseDiscontinuity;
            probe.smoothingAdvance = smoothingUs < 0 ? uint64_t(-smoothingUs) : 0;
            probe.accountReadinessSlack = m_Parameters.playoutResponsiveBuffer >= 2;
            if (m_Parameters.playoutResponsiveBuffer >= 3) {
                // A host stall or catch-up burst is not steady delivery jitter.
                // Its real output effect is still counted by readiness telemetry.
                probe.eligible = probe.eligible && cadence.intervalUs <=
                    std::max<uint64_t>(25000, m_SourcePeriodUs * 3 / 2) &&
                    cadence.intervalUs >= m_SourcePeriodUs / 2;
            }
        }
    }
    // PyroWave delivers a frame with zero-filled detail rather than waiting
    // for lost packets. Its lateness is loss, not delivery jitter or slow
    // work, and more playout delay cannot recover it; it only moves every
    // later frame. Present it as scheduled, but keep it out of the evidence
    // that grows the readiness, interval, mean-miss and smoothness buffers.
    if (frame.lostPackets() != 0) {
        m_Pending.smoothness.eligible = false;
        m_Pending.prediction.eligible = false;
    }
    m_LastDecodeCompleteUs = sourceMappingUs(frame);
    m_HaveLastDecodeComplete = true;
    if (timestampPlayout && metronomeEnabled()) {
        // The slot this frame occupies becomes the schedule basis only when
        // the frame is presented (noteSubmission), so a dropped or cancelled
        // frame frees its tick for the successor. A floor wait that pushed
        // the target past the tick is the slot actually used.
        const uint64_t basisUs = targetUs > leadUs ? targetUs - leadUs : 0;
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        const uint64_t basisUsQ16 = basisUs > maximum / kQ16One ?
            maximum : basisUs * kQ16One;
        if (!m_Pending.hasSmoothedBasis) {
            m_Pending.smoothedBasisUsQ16 = basisUsQ16;
            m_Pending.phaseDebtUs = 0;
            m_Pending.phaseResidualEmaUs = 0;
            m_Pending.basisMappingUs = m_AppliedPlayoutOffsetUs +
                static_cast<int64_t>(delayBeforeUs);
        }
        else if (basisUs > roundedQ16(m_Pending.smoothedBasisUsQ16)) {
            // A late arrival or a floor wait pushed the slot past the tick.
            // The schedule continues from the slot used and owes the
            // difference, which it pays back one bounded step at a time.
            m_Pending.phaseDebtUs += static_cast<int64_t>(
                basisUs - roundedQ16(m_Pending.smoothedBasisUsQ16));
            m_Pending.smoothedBasisUsQ16 = basisUsQ16;
        }
        m_Pending.hasSmoothedBasis = true;
        m_Pending.smoothedBasisOrdinal = m_SourceFrameOrdinal;
    }
    else if (timestampPlayout &&
            m_Parameters.playoutSmoothingGainPerMille != 0) {
        // Keep the source schedule independent of readiness. Feeding a late
        // execution time or a readiness-bounded slot back into this clock
        // carries delay into later frames and undoes host-quantized smoothing.
        // Old captures retain their original/execution-anchored policy.
        const uint64_t basis = m_Parameters.playoutPredictionEnabled ?
            originalTargetUs - std::min(originalTargetUs, smoothingReadyPushUs) : targetUs;
        m_LastSmoothedBasisUs = basis > leadUs ? basis - leadUs : 0;
        m_HaveSmoothedBasis = true;
    }
    else {
        resetCadenceSmoothing();
    }
    return decision;
}

void VrrTimingController::resetCadenceSmoothing()
{
    m_SourceGapSmoothingUs = 0;
    m_HaveSmoothedBasis = false;
    m_LastSmoothedBasisUs = 0;
    m_LastSmoothedBasisUsQ16 = 0;
    m_LastSmoothedBasisOrdinal = 0;
    m_PhaseDebtUs = 0;
    m_PhaseResidualEmaUs = 0;
    m_LastBasisMappingUs = 0;
}

bool VrrTimingController::metronomeEnabled() const
{
    return m_Parameters.playoutMetronomeEnabled != 0;
}

uint64_t VrrTimingController::motionThresholdUs(uint64_t periodUs) const
{
    const uint64_t floorUs = m_Parameters.playoutMotionFloorUs;
    const uint64_t ceilingUs = std::max(
        floorUs,
        scaledPerMille(periodUs,
                       m_Parameters.playoutMotionCeilingPeriodPerMille));
    if (m_MotionResiduals.size() < m_Parameters.playoutMotionMinimumSamples) {
        // Until the jitter bound is known, treat every deviation as noise
        // rather than re-anchoring the grid on a first-frame guess.
        return ceilingUs;
    }
    const uint64_t observedUs = percentile(
        m_MotionResiduals, m_Parameters.playoutMotionPercentile);
    return clampUnsigned(
        scaledPerMille(observedUs, m_Parameters.playoutMotionGainPerMille),
        floorUs, ceilingUs);
}

int64_t VrrTimingController::metronomeAdjustUs(
    const CadenceObservation& cadence, bool rebased, uint64_t rawBasisUs,
    uint64_t playoutDelayUs, uint64_t earliestBasisUs,
    uint64_t& missedTicks, int64_t& remainingDebtUs)
{
    missedTicks = 0;
    remainingDebtUs = 0;
    if (rebased || cadence.sourceRateChanged || cadence.phaseDiscontinuity ||
            !cadence.eligible || cadence.intervalUs == 0) {
        // A new epoch, a confirmed rate change, or too little cadence
        // history: present on the raw slot and restart the metronome there.
        resetCadenceSmoothing();
        return 0;
    }
    const uint64_t periodUs = std::max<uint64_t>(1, m_SourcePeriodUs);
    if (cadence.intervalUs > periodUs * 5 / 2) {
        // The content itself stalled. Show the stall rather than smear it,
        // and restart the metronome on this frame's slot.
        resetCadenceSmoothing();
        return 0;
    }
    if (!m_HaveSmoothedBasis) {
        return 0;
    }

    // Advance from the last presented slot by one fitted period per source
    // frame since it, so a locally skipped or dropped frame keeps the grid
    // anchored to the content.
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    const uint64_t periodQ16 = std::max(
        std::max<uint64_t>(kQ16One, m_ConfiguredStreamPeriodQ16),
        m_MetronomePeriodUsQ16);
    const uint64_t frames = m_SourceFrameOrdinal > m_LastSmoothedBasisOrdinal ?
        m_SourceFrameOrdinal - m_LastSmoothedBasisOrdinal : 1;
    const uint64_t advanceQ16 = frames > maximum / periodQ16 ?
        maximum : periodQ16 * frames;
    uint64_t tickQ16 = saturatingAdd(m_LastSmoothedBasisUsQ16, advanceQ16);

    const int64_t errorUs = signedDifference(roundedQ16(tickQ16), rawBasisUs);
    const uint64_t magnitudeUs = static_cast<uint64_t>(
        errorUs < 0 ? -errorUs : errorUs);
    const uint64_t snapUs = scaledPerMille(
        periodUs, m_Parameters.playoutSmoothingSnapPerMille);
    if (magnitudeUs > snapUs) {
        // The mapping moved by several periods (a confirmed clock re-seed or
        // a stall the drop policy did not fully shed). Re-phase here.
        resetCadenceSmoothing();
        return 0;
    }

    if (m_Parameters.playoutMotionDeadbandEnabled != 0) {
        // The stamp's deviation from the grid, excluding lag the schedule
        // already knows it owes. Beyond the learned jitter bound it is the
        // host reporting a change in frame timing: present on the stamp and
        // restart the grid there so the next frames are spaced from it.
        const int64_t deviationUs = errorUs - m_PhaseDebtUs;
        const uint64_t deviationMagnitudeUs = static_cast<uint64_t>(
            deviationUs < 0 ? -deviationUs : deviationUs);
        const uint64_t thresholdUs = motionThresholdUs(periodUs);
        if (deviationMagnitudeUs > thresholdUs) {
            // Not admitted to the jitter bound: a run of real timing changes
            // (a rate change the fit has yet to absorb) must not widen the
            // band until the grid treats them as noise. The grid's period
            // was wrong too, so it restarts on the current fit rather than
            // on the long filter that was still averaging the old rate.
            resetCadenceSmoothing();
            m_MetronomePeriodUsQ16 = m_SourcePeriodUsQ16;
            const uint64_t rawQ16 = rawBasisUs > maximum / kQ16One ?
                maximum : rawBasisUs * kQ16One;
            const uint64_t earliestRawQ16 =
                earliestBasisUs > maximum / kQ16One ?
                    maximum : earliestBasisUs * kQ16One;
            if (rawQ16 < earliestRawQ16) {
                missedTicks = (earliestRawQ16 - rawQ16) / periodQ16;
            }
            return 0;
        }
        appendBounded(m_MotionResiduals, deviationMagnitudeUs,
                      std::max<size_t>(1, m_Parameters.playoutMotionWindowFrames));
    }

    const uint64_t stepCeilingUs = std::max(
        m_Parameters.playoutPhaseStepMinimumUs,
        scaledPerMille(periodUs, m_Parameters.playoutPhaseStepPeriodPerMille));
    const uint64_t divisor = std::max<uint64_t>(
        1, m_Parameters.playoutPhaseStepDivisor);

    // Known movement of the clock mapping since the basis was committed
    // (offset tracking, delay attack or release) is owed to the schedule
    // exactly, in the opposite sense to lag the schedule took on itself.
    const int64_t mappingUs = m_AppliedPlayoutOffsetUs >
            std::numeric_limits<int64_t>::max() -
                static_cast<int64_t>(playoutDelayUs) ?
        std::numeric_limits<int64_t>::max() :
        m_AppliedPlayoutOffsetUs + static_cast<int64_t>(playoutDelayUs);
    int64_t debtUs = m_PhaseDebtUs - (mappingUs - m_LastBasisMappingUs);

    // Pay the debt first: exact, noise-free, and bounded per frame.
    const int64_t ceilingUs = static_cast<int64_t>(stepCeilingUs);
    const int64_t paidUs = std::max(-ceilingUs, std::min(debtUs, ceilingUs));
    debtUs -= paidUs;
    int64_t correctionUs = -paidUs;

    // Then track the slow residual with a long filter and a deadband, so
    // per-frame stamp wobble never becomes a step while clock drift and
    // fit error still do.
    const int64_t residualUs = errorUs - m_PhaseDebtUs;
    const int64_t windowFrames = static_cast<int64_t>(std::max<uint64_t>(
        1, m_Parameters.playoutPhaseResidualWindowFrames));
    int64_t emaUs = m_PhaseResidualEmaUs +
        (residualUs - m_PhaseResidualEmaUs) / windowFrames;
    const int64_t deadbandUs = static_cast<int64_t>(
        m_Parameters.playoutPhaseDeadbandUs);
    if (emaUs > deadbandUs || emaUs < -deadbandUs) {
        const uint64_t emaMagnitudeUs = static_cast<uint64_t>(
            emaUs < 0 ? -emaUs : emaUs);
        const uint64_t residualStepUs = std::min(
            clampUnsigned(emaMagnitudeUs / divisor,
                          m_Parameters.playoutPhaseStepMinimumUs,
                          stepCeilingUs),
            emaMagnitudeUs);
        // The step is applied to the schedule, so the filter must see it
        // too or it would keep asking for the same correction.
        if (emaUs > 0) {
            correctionUs -= static_cast<int64_t>(residualStepUs);
            emaUs -= static_cast<int64_t>(residualStepUs);
        }
        else {
            correctionUs += static_cast<int64_t>(residualStepUs);
            emaUs += static_cast<int64_t>(residualStepUs);
        }
    }
    m_Pending.phaseDebtUs = debtUs;
    m_Pending.phaseResidualEmaUs = emaUs;
    m_Pending.basisMappingUs = mappingUs;
    remainingDebtUs = debtUs;
    const uint64_t correctionMagnitudeQ16 =
        static_cast<uint64_t>(correctionUs < 0 ? -correctionUs : correctionUs) *
        kQ16One;
    tickQ16 = correctionUs < 0 ?
        (tickQ16 > correctionMagnitudeQ16 ? tickQ16 - correctionMagnitudeQ16 : 0) :
        saturatingAdd(tickQ16, correctionMagnitudeQ16);
    m_Pending.phaseDebtUs = debtUs;
    m_Pending.phaseResidualEmaUs = emaUs;

    // Never earlier than the mapped source clock itself.
    const uint64_t sourceBasisUs = rawBasisUs > playoutDelayUs ?
        rawBasisUs - playoutDelayUs : 0;
    const uint64_t sourceBasisQ16 = sourceBasisUs > maximum / kQ16One ?
        maximum : sourceBasisUs * kQ16One;
    tickQ16 = std::max(tickQ16, sourceBasisQ16);

    // A frame that arrives after its tick presents as soon as it is ready:
    // on a VRR panel one interval stretched by the shortfall is less visible
    // than a full repeated frame, and the schedule then continues from the
    // slot actually used and walks the lag back at the bounded step. The
    // caller applies that slip; the lag returned here is against the tick
    // the frame should have made, so the calibrator still sees the full
    // lateness and can grow the cushion to cover it. Whole periods of
    // shortfall are reported so the worker can shed a backlog by dropping a
    // frame that a fresher successor has already overtaken.
    const uint64_t earliestQ16 = earliestBasisUs > maximum / kQ16One ?
        maximum : earliestBasisUs * kQ16One;
    if (tickQ16 < earliestQ16) {
        missedTicks = (earliestQ16 - tickQ16) / periodQ16;
    }

    m_Pending.hasSmoothedBasis = true;
    m_Pending.smoothedBasisUsQ16 = tickQ16;
    m_Pending.smoothedBasisOrdinal = m_SourceFrameOrdinal;
    return signedDifference(roundedQ16(tickQ16), rawBasisUs);
}

int64_t VrrTimingController::cadenceSmoothingAdjustUs(
    const CadenceObservation& cadence, bool rebased, uint64_t rawBasisUs,
    uint64_t playoutDelayUs, uint64_t reserveUs)
{
    const bool localCadencePolicy = m_Parameters.playoutSmoothingWindowedCadence == 6 ||
        m_Parameters.playoutSmoothingWindowedCadence == 7;
    uint64_t localPeriodUs = 0;
    if (m_Parameters.playoutSmoothingWindowedCadence >= 3 && !localCadencePolicy) {
        // A cadence reset drops only the retained phase. The standing reserve
        // stays, so a reset never steps the whole schedule by the reserve.
        const int64_t standingUs = static_cast<int64_t>(absorbReserveUs());
        if (m_Parameters.playoutSmoothingGainPerMille == 0 || rebased ||
            cadence.sourceRateChanged || cadence.phaseDiscontinuity ||
            !cadence.eligible || !cadence.usedRtpTimestamp || cadence.frameDelta != 1 ||
            cadence.intervalUs == 0) {
            resetCadenceSmoothing();
            return standingUs;
        }
        // This trigger uses consecutive host RTP gaps and the existing source
        // cadence reference. Local mapping, arrival, decode and presentation
        // times never enter it. Keep source phase separate from readiness
        // clipping so a late decoder cannot become new smoothing demand.
        // Round this gap independently, like the fitted reference. The
        // timeline's carried conversion remainder can alternate by 1 us;
        // it must not make an exactly-at-T gap intermittently exceed T.
        const uint64_t sourceGapUs =
            (uint64_t(cadence.rtpDelta) * kMicrosecondsPerSecond + kRtpClockRate / 2) / kRtpClockRate;
        m_SourceGapSmoothingUs = m_Parameters.playoutSmoothingWindowedCadence >= 5 ?
            Vrr13::sourceGapPhaseUs(sourceGapUs, m_SourcePeriodUs,
                intervalQualityToleranceUs(m_Parameters), smoothingPhaseLimitUs(),
                m_Parameters.playoutSmoothingRecenterPerMille, m_SourceGapSmoothingUs,
                m_Parameters.playoutSmoothingPresentableFloor != 0 ?
                    saturatingAdd(m_DisplayPeriodUs, m_Parameters.playoutSmoothingFloorMarginUs) : 0) :
            Vrr13::sourceGapSmoothingUs(sourceGapUs, m_SourcePeriodUs,
                intervalQualityToleranceUs(m_Parameters), m_SourceGapSmoothingUs);
        m_SmoothingEngaged = true;
        return std::max(m_SourceGapSmoothingUs + standingUs, -static_cast<int64_t>(playoutDelayUs));
    }
    // A half-period interval is 375 RTP ticks at 120 FPS: converting it to
    // whole microseconds alternates 4166/4167. Do not turn that rounding into
    // a burst. Also retain bounded short/long compensation while the fitted
    // period follows a small rate drift. Truly compressed bursts stay resets.
    const bool compensatedBurstPolicy = m_Parameters.playoutSmoothingWindowedCadence >= 2;
    constexpr uint64_t rtpQuantumUs = (kMicrosecondsPerSecond + kRtpClockRate - 1) / kRtpClockRate;
    const bool compensatedShortInterval = compensatedBurstPolicy &&
        cadence.intervalUs * 4 >= m_SourcePeriodUs &&
        m_PreviousSmoothingIntervalUs > m_SourcePeriodUs &&
        cadence.intervalUs < m_SourcePeriodUs &&
        withinPercent((cadence.intervalUs + m_PreviousSmoothingIntervalUs) / 2,
                      m_SourcePeriodUs, 25);
    const bool boundedInterval = cadence.intervalUs <= m_SourcePeriodUs * 5 / 2 &&
        (cadence.intervalUs * 2 + (compensatedBurstPolicy ? rtpQuantumUs : 0) >= m_SourcePeriodUs ||
         compensatedShortInterval);
    if (m_Parameters.playoutResponsiveBuffer) {
        bool stable;
        if (m_Parameters.playoutSmoothingWindowedCadence) {
            // Qualify cadence over several intervals, not the side of a
            // rounding boundary an individual RTP stamp lands on. A normal
            // interval between compensating short/long pairs is still steady
            // cadence. Keep true gaps, stalls and rate transitions as resets.
            const bool continuous = !rebased && !cadence.sourceRateChanged &&
                !cadence.phaseDiscontinuity && cadence.eligible && cadence.frameDelta == 1 &&
                cadence.intervalUs != 0 && (boundedInterval || localCadencePolicy);
            if (!continuous) {
                m_SmoothingCadenceCount = 0;
                m_SmoothingCadenceIndex = 0;
                stable = false;
            }
            else {
                m_SmoothingCadenceIntervals[m_SmoothingCadenceIndex] = cadence.intervalUs;
                m_SmoothingCadenceIndex = (m_SmoothingCadenceIndex + 1) %
                    m_SmoothingCadenceIntervals.size();
                m_SmoothingCadenceCount = std::min(m_SmoothingCadenceCount + 1,
                                                   m_SmoothingCadenceIntervals.size());
                uint64_t totalUs = 0;
                for (size_t i = 0; i < m_SmoothingCadenceCount; ++i)
                    totalUs += m_SmoothingCadenceIntervals[i];
                if (localCadencePolicy && m_SmoothingCadenceCount == m_SmoothingCadenceIntervals.size()) {
                    // Four uniform source gaps establish a new local cadence
                    // before the longer source-period fit has discarded its
                    // previous-rate history. Do not call that run unstable
                    // merely because it is outside the old fit's 25% band.
                    const uint64_t meanUs = totalUs / m_SmoothingCadenceCount;
                    const uint64_t toleranceUs = intervalQualityToleranceUs(m_Parameters);
                    bool uniform = true;
                    for (const uint64_t gapUs : m_SmoothingCadenceIntervals) {
                        uniform &= gapUs > meanUs ? gapUs - meanUs <= toleranceUs :
                            meanUs - gapUs <= toleranceUs;
                    }
                    if (uniform) localPeriodUs = meanUs;
                }
                stable = m_SmoothingCadenceCount == m_SmoothingCadenceIntervals.size() &&
                    (localPeriodUs != 0 ||
                     withinPercent(totalUs, m_SourcePeriodUs * m_SmoothingCadenceCount, 25));
            }
            m_PreviousSmoothingIntervalUs = cadence.eligible && cadence.frameDelta == 1 &&
                !rebased && !cadence.phaseDiscontinuity ? cadence.intervalUs : 0;
        }
        else {
            // Judge adjacent intervals together: alternating short/long frames are
            // exactly what the smoother is meant to handle, not a sustained change
            // of source rate. Keep the 200 ms recovery gate for actual transitions.
            const bool compensatingPair = m_Parameters.playoutResponsiveBuffer >= 2 &&
                m_PreviousSmoothingIntervalUs &&
                !withinPercent(m_PreviousSmoothingIntervalUs, m_SourcePeriodUs, 25) &&
                ((cadence.intervalUs < m_SourcePeriodUs && m_PreviousSmoothingIntervalUs > m_SourcePeriodUs) ||
                 (cadence.intervalUs > m_SourcePeriodUs && m_PreviousSmoothingIntervalUs < m_SourcePeriodUs));
            const auto confidenceIntervalUs = compensatingPair ?
                (cadence.intervalUs + m_PreviousSmoothingIntervalUs) / 2 : cadence.intervalUs;
            stable = !rebased && !cadence.sourceRateChanged &&
                !cadence.phaseDiscontinuity && cadence.eligible && cadence.frameDelta == 1 &&
                withinPercent(confidenceIntervalUs, m_SourcePeriodUs, 25);
            m_PreviousSmoothingIntervalUs = cadence.eligible && cadence.frameDelta == 1 &&
                !rebased && !cadence.phaseDiscontinuity ? cadence.intervalUs : 0;
        }
        if (!stable) m_CadenceStableSinceUs = 0;
        else if (!m_CadenceStableSinceUs) m_CadenceStableSinceUs = m_SourceTimeUs;
        if (!m_CadenceStableSinceUs || m_SourceTimeUs < m_CadenceStableSinceUs ||
            m_SourceTimeUs - m_CadenceStableSinceUs < m_Parameters.playoutSmoothingRecoveryUs) {
            resetCadenceSmoothing();
            return 0;
        }
    }
    const uint64_t gainPerMille = m_Parameters.playoutSmoothingGainPerMille;
    if (gainPerMille == 0 || gainPerMille >= 1000) {
        resetCadenceSmoothing();
        return 0;
    }
    if (rebased || cadence.sourceRateChanged || cadence.phaseDiscontinuity ||
            !cadence.eligible || cadence.intervalUs == 0) {
        // A new epoch, a material rate change, or too little cadence history
        // to trust: present on the raw slot and rebuild from here.
        resetCadenceSmoothing();
        return 0;
    }
    const uint64_t intervalUs = cadence.intervalUs;
    // The cumulative rate fit is the authority on the source period. The
    // tracked period only follows short-term drift around it; if the two
    // disagree by more than a quarter (a rate change the fit has absorbed,
    // or a bad seed from a startup burst) re-seed from the fit rather than
    // rejecting every interval as a stall from then on. Revision 6 instead
    // trusts its qualified uniform local source run for this short-term
    // predictor. Repeatedly reseeding from the lagging long fit manufactures
    // positive retiming and cadence resets after the source already sped up.
    const uint64_t fittedPeriodUs = std::max<uint64_t>(1,
        localPeriodUs != 0 ? localPeriodUs : m_SourcePeriodUs);
    const bool localPeriodChanged = localPeriodUs != 0 &&
        (m_SmoothedPeriodUs > localPeriodUs ? m_SmoothedPeriodUs - localPeriodUs :
            localPeriodUs - m_SmoothedPeriodUs) > intervalQualityToleranceUs(m_Parameters);
    if (m_SmoothedPeriodUs == 0 ||
            localPeriodChanged || (!localCadencePolicy &&
            (m_SmoothedPeriodUs > fittedPeriodUs + fittedPeriodUs / 4 ||
             m_SmoothedPeriodUs + fittedPeriodUs / 4 < fittedPeriodUs))) {
        m_SmoothedPeriodUs = fittedPeriodUs;
        m_SmoothedPeriodRemainder = 0;
        m_SmoothedPeriodFeedbackRemainder = 0;
    }
    if (!boundedInterval && localPeriodUs == 0) {
        // A host stall or burst is not cadence. Keep the period estimate,
        // restart the schedule on this frame's raw slot.
        resetCadenceSmoothing();
        return 0;
    }
    else {
        const uint64_t alpha = std::min<uint64_t>(
            1000, m_Parameters.playoutSmoothingPeriodAlphaPerMille);
        const int64_t deltaUs = static_cast<int64_t>(intervalUs) -
            static_cast<int64_t>(m_SmoothedPeriodUs);
        // Retain sub-microsecond updates. At a small alpha, truncating every
        // frame leaves a permanent period error and therefore phase debt.
        // Older captured revisions retain their original integer behavior.
        const int64_t update = deltaUs * static_cast<int64_t>(alpha) +
            (compensatedBurstPolicy ? m_SmoothedPeriodRemainder : 0);
        m_SmoothedPeriodUs = static_cast<uint64_t>(
            static_cast<int64_t>(m_SmoothedPeriodUs) + update / 1000);
        m_SmoothedPeriodRemainder = compensatedBurstPolicy ? update % 1000 : 0;
        if (m_SmoothedPeriodUs == 0) {
            m_SmoothedPeriodUs = 1;
        }
    }
    if (!m_HaveSmoothedBasis) {
        return 0;
    }
    const int64_t predictedUs = static_cast<int64_t>(
        saturatingAdd(m_LastSmoothedBasisUs, m_SmoothedPeriodUs));
    const int64_t errorUs = predictedUs - static_cast<int64_t>(rawBasisUs);
    const int64_t snapUs = static_cast<int64_t>(
        m_SmoothedPeriodUs * m_Parameters.playoutSmoothingSnapPerMille / 1000);
    if (errorUs > snapUs || errorUs < -snapUs) {
        resetCadenceSmoothing();
        return 0;
    }
    if (m_Parameters.playoutSmoothingPeriodFeedbackPerMillion != 0) {
        // A drifting game rate leaves the interval average behind, and the
        // phase blend turns that period error into a standing offset from
        // the raw slots. Integrate the phase error into the period as well,
        // so the smoothed slot follows a rate ramp instead of lagging it.
        const int64_t feedback = -errorUs * static_cast<int64_t>(std::min<uint64_t>(
                m_Parameters.playoutSmoothingPeriodFeedbackPerMillion, 1000000)) +
            m_SmoothedPeriodFeedbackRemainder;
        const int64_t periodUs = static_cast<int64_t>(m_SmoothedPeriodUs) +
            feedback / 1000000;
        m_SmoothedPeriodFeedbackRemainder = feedback % 1000000;
        m_SmoothedPeriodUs = static_cast<uint64_t>(std::max<int64_t>(1, periodUs));
    }
    int64_t adjustUs = errorUs * static_cast<int64_t>(1000 - gainPerMille) / 1000;
    // The reserve already occupies part of the positive retiming budget. Never
    // present before the mapped source slot, reserve or not.
    adjustUs = std::min(adjustUs, static_cast<int64_t>(
        m_Parameters.playoutSmoothingMaxLagUs -
        std::min(m_Parameters.playoutSmoothingMaxLagUs, reserveUs)));
    adjustUs = std::max(adjustUs, -static_cast<int64_t>(
        saturatingAdd(playoutDelayUs, reserveUs)));
    m_SmoothingEngaged = true;
    return adjustUs;
}

uint64_t VrrTimingController::absorbReserveUs() const
{
    if ((m_Parameters.playoutSmoothingWindowedCadence != 5 && m_Parameters.playoutSmoothingWindowedCadence != 7) ||
            m_Parameters.playoutSmoothingGainPerMille == 0) {
        return 0;
    }
    const uint64_t maximumUs = m_Parameters.playoutSmoothingAbsorbReserveUs;
    const uint64_t perMille = m_Parameters.playoutSmoothingAbsorbReservePeriodPerMille;
    return perMille != 0 ? std::min(maximumUs, scaledPerMille(m_SourcePeriodUs, perMille)) : maximumUs;
}

uint64_t VrrTimingController::smoothingPhaseLimitUs() const
{
    const uint64_t perMille = m_Parameters.playoutSmoothingMaxLagPeriodPerMille;
    return m_Parameters.playoutSmoothingWindowedCadence >= 5 && perMille != 0 ?
        std::max(m_Parameters.playoutSmoothingMaxLagUs, scaledPerMille(m_SourcePeriodUs, perMille)) :
        m_Parameters.playoutSmoothingMaxLagUs;
}

int64_t VrrTimingController::bufferedSmoothingUs(int64_t smoothingUs) const
{
    return m_Parameters.playoutSmoothingWindowedCadence >= 5 &&
            m_Parameters.playoutSmoothingWindowedCadence != 6 &&
            m_Parameters.playoutSmoothingWindowedCadence != 7 && !metronomeEnabled() ?
        std::min<int64_t>(smoothingUs, 0) : smoothingUs;
}

bool VrrTimingController::smoothingReserveEnabled() const
{
    return m_Parameters.playoutSmoothingReserveMaxUs != 0 &&
        m_Parameters.playoutSmoothingGainPerMille != 0 &&
        m_Parameters.playoutSmoothingReservePercentilePerMille != 0;
}

void VrrTimingController::observeSmoothingReserve(bool engaged,
                                                  int64_t shortfallUs,
                                                  uint64_t nowUs)
{
    // Attack gradually so one step cannot jump an unsmoothed schedule, and
    // require enough placed frames that a single late arrival is not a
    // percentile. Release is time-based so it is independent of FPS.
    constexpr uint64_t kAttackPerFrameUs = 250;
    constexpr size_t kMinimumSamples = 32;
    const uint64_t elapsedUs = m_LastSmoothingReserveUpdateUs != 0 &&
            nowUs > m_LastSmoothingReserveUpdateUs ?
        std::min<uint64_t>(nowUs - m_LastSmoothingReserveUpdateUs, 100000) : 0;
    m_LastSmoothingReserveUpdateUs = nowUs;
    const uint64_t maximumUs = std::min(m_Parameters.playoutSmoothingReserveMaxUs,
                                        m_Parameters.playoutSmoothingMaxLagUs);
    uint64_t desiredUs = 0;
    if (engaged) {
        m_SmoothingShortfalls[m_SmoothingShortfallIndex] = shortfallUs;
        m_SmoothingShortfallIndex = (m_SmoothingShortfallIndex + 1) %
            m_SmoothingShortfalls.size();
        m_SmoothingShortfallCount = std::min(m_SmoothingShortfallCount + 1,
                                             m_SmoothingShortfalls.size());
        if (m_SmoothingShortfallCount >= kMinimumSamples) {
            std::array<int64_t, 128> ordered;
            std::copy_n(m_SmoothingShortfalls.begin(), m_SmoothingShortfallCount,
                        ordered.begin());
            const uint64_t perMille = std::min<uint64_t>(
                1000, m_Parameters.playoutSmoothingReservePercentilePerMille);
            const size_t rank = std::max<size_t>(
                1, (m_SmoothingShortfallCount * perMille + 999) / 1000);
            std::nth_element(ordered.begin(), ordered.begin() + (rank - 1),
                             ordered.begin() + m_SmoothingShortfallCount);
            const int64_t quantileUs = ordered[rank - 1];
            const int64_t toleranceUs = static_cast<int64_t>(std::min<uint64_t>(
                m_Parameters.playoutSmoothingReserveToleranceUs,
                std::numeric_limits<int64_t>::max()));
            desiredUs = quantileUs > toleranceUs ?
                std::min(static_cast<uint64_t>(quantileUs - toleranceUs), maximumUs) : 0;
        }
        else {
            desiredUs = m_SmoothingReserveUs;
        }
    }
    if (desiredUs > m_SmoothingReserveUs) {
        m_SmoothingReserveUs = std::min(desiredUs,
            saturatingAdd(m_SmoothingReserveUs, kAttackPerFrameUs));
        m_SmoothingReserveReleaseRemainder = 0;
    }
    else if (desiredUs < m_SmoothingReserveUs) {
        const uint64_t numerator = saturatingAdd(
            m_Parameters.playoutSmoothingReserveReleaseUsPerSecond * elapsedUs,
            m_SmoothingReserveReleaseRemainder);
        const uint64_t releaseUs = numerator / kMicrosecondsPerSecond;
        m_SmoothingReserveReleaseRemainder = numerator % kMicrosecondsPerSecond;
        m_SmoothingReserveUs -= std::min(m_SmoothingReserveUs - desiredUs, releaseUs);
    }
    m_SmoothingReserveUs = std::min(m_SmoothingReserveUs, maximumUs);
}

VrrTimingController::CadenceObservation
VrrTimingController::observeCadence(const PacedFrame& frame)
{
    CadenceObservation observation;

    uint64_t frameDelta = 1;
    if (m_HaveLastFrameNumber && frame.frameNumber() >= 0) {
        if (frame.frameNumber() <= m_LastFrameNumber) {
            observation.needsRebase = true;
            return observation;
        }
        frameDelta = static_cast<uint64_t>(frame.frameNumber() -
                                           m_LastFrameNumber);
    }
    observation.frameDelta = frameDelta;

    if (frame.timestampValid() && m_LastTimestampValid) {
        const uint32_t wrappedDelta =
            frame.rtpTimestamp() - m_LastRtpTimestamp;
        if (wrappedDelta == 0 || wrappedDelta > 0x7fffffffU) {
            observation.needsRebase = true;
            return observation;
        }

        const uint64_t carriedRemainder = m_LastCadenceUsedRtp ?
            m_RtpConversionRemainder : 0;
        const uint64_t intervalNumerator =
            static_cast<uint64_t>(wrappedDelta) * kMicrosecondsPerSecond +
            carriedRemainder;
        const uint64_t intervalUs = intervalNumerator / kRtpClockRate;
        if (intervalUs > m_Parameters.maximumForwardMovementUs) {
            observation.needsRebase = true;
            return observation;
        }

        m_RtpConversionRemainder = intervalNumerator % kRtpClockRate;
        m_FrameConversionRemainder = 0;
        m_LastCadenceUsedRtp = true;

        observation.intervalUs = intervalUs;
        observation.rtpDelta = wrappedDelta;
        observation.usedRtpTimestamp = true;
        observeRtpCadence(wrappedDelta, observation);
        return observation;
    }

    if (m_Config.streamRateHz <= 0 ||
        frameDelta > static_cast<uint64_t>(m_Config.streamRateHz)) {
        observation.needsRebase = true;
        return observation;
    }

    const uint64_t carriedRemainder = !m_LastCadenceUsedRtp ?
        m_FrameConversionRemainder : 0;
    const uint64_t intervalNumerator =
        frameDelta * kMicrosecondsPerSecond + carriedRemainder;
    observation.intervalUs = intervalNumerator /
        static_cast<uint64_t>(m_Config.streamRateHz);
    m_FrameConversionRemainder = intervalNumerator %
        static_cast<uint64_t>(m_Config.streamRateHz);
    m_RtpConversionRemainder = 0;
    m_LastCadenceUsedRtp = false;
    m_CadenceSamples.clear();
    m_RateCandidateSamples.clear();
    m_InputLayerCadence.reset();
    observation.eligible = frameDelta == 1;
    return observation;
}

void VrrTimingController::observeRtpCadence(
    uint32_t rtpDelta, CadenceObservation& observation)
{
    const CadenceSample previous {
        m_SourceFrameOrdinal,
        m_UnwrappedRtpTicks,
    };
    m_SourceFrameOrdinal = saturatingAdd(m_SourceFrameOrdinal,
                                          observation.frameDelta);
    m_UnwrappedRtpTicks = saturatingAdd(m_UnwrappedRtpTicks,
                                         static_cast<uint64_t>(rtpDelta));
    const CadenceSample current {
        m_SourceFrameOrdinal,
        m_UnwrappedRtpTicks,
    };

    if (m_Parameters.playoutInputHitchMissesOnly >= 7) {
        const uint64_t layerPeriod = m_InputLayerPeriodUs ? m_InputLayerPeriodUs : m_SourcePeriodUs;
        const uint64_t period = m_Parameters.playoutInputHitchMissesOnly >= 8 ?
            m_InputLayerCadence.observeConfirmed(
                std::max(observation.intervalUs, roundedQ16(m_ConfiguredStreamPeriodQ16)),
                layerPeriod, m_SourcePeriodUs, observation.frameDelta == 1,
                m_Parameters.playoutInputHitchMissesOnly >= 9 ? 30 : 0) :
            m_InputLayerCadence.observe(observation.intervalUs, layerPeriod, observation.frameDelta == 1);
        if (period) {
            observation.sourceRateChanged = acceptSourcePeriodQ16(period * kQ16One);
            observation.confirmedInputLayerPeriodUs = m_SourcePeriodUs;
            m_CadenceSamples.clear();
            m_CadenceSamples.push_back(current);
            m_RateCandidateSamples.clear();
            observation.phaseDiscontinuity = true;
            observation.eligible = true;
            return;
        }
    }

    const bool majorDeparture = isMajorCadenceDeparture(
        observation.intervalUs, observation.frameDelta);

    if (!m_RateCandidateSamples.empty()) {
        // A normal interval immediately after a major departure identifies an
        // isolated source/capture gap. Preserve the stable rate, discard the
        // provisional segment, and begin a fresh cumulative phase at the
        // current sample.
        const uint64_t observedPeriodUs = std::max<uint64_t>(
            1, observation.intervalUs / observation.frameDelta);
        const bool returnedToStableCadence =
            observedPeriodUs *
                    m_Parameters.candidateCadenceRatioDenominator <=
                m_SourcePeriodUs *
                    m_Parameters.candidateCadenceRatioNumerator &&
            m_SourcePeriodUs *
                    m_Parameters.candidateCadenceRatioDenominator <=
                observedPeriodUs *
                    m_Parameters.candidateCadenceRatioNumerator;
        if (returnedToStableCadence) {
            m_RateCandidateSamples.clear();
            m_CadenceSamples.clear();
            m_CadenceSamples.push_back(current);
            observation.phaseDiscontinuity = true;
            return;
        }

        appendCadenceSample(m_RateCandidateSamples, current);
        observation.phaseDiscontinuity = true;
        const uint64_t candidateSpanUs =
            (m_RateCandidateSamples.back().rtpTicks -
             m_RateCandidateSamples.front().rtpTicks) *
            kMicrosecondsPerSecond / kRtpClockRate;
        if (m_RateCandidateSamples.size() >=
                m_Parameters.rateCandidateSamples &&
            candidateSpanUs >= m_Parameters.rateCandidateMinimumUs) {
            const uint64_t candidatePeriodQ16 = fittedSourcePeriodQ16(
                m_RateCandidateSamples);
            if (candidatePeriodQ16 != 0) {
                observation.sourceRateChanged = acceptSourcePeriodQ16(
                    candidatePeriodQ16);
                m_CadenceSamples = m_RateCandidateSamples;
                m_RateCandidateSamples.clear();
                observation.eligible = true;
            }
        }
        return;
    }

    if (majorDeparture) {
        m_RateCandidateSamples.push_back(previous);
        m_RateCandidateSamples.push_back(current);
        observation.phaseDiscontinuity = true;
        return;
    }

    appendCadenceSample(m_CadenceSamples, current);
    observation.eligible = true;
    if (m_CadenceSamples.size() >= m_Parameters.minimumCadenceSamples) {
        const uint64_t fittedPeriodQ16 = fittedSourcePeriodQ16(
            m_CadenceSamples);
        if (fittedPeriodQ16 != 0 &&
            acceptSourcePeriodQ16(fittedPeriodQ16)) {
            observation.sourceRateChanged = true;
            observation.phaseDiscontinuity = true;
        }
    }
}

void VrrTimingController::appendCadenceSample(
    std::deque<CadenceSample>& samples, const CadenceSample& sample)
{
    samples.push_back(sample);
    while (samples.size() > m_Parameters.maximumCadenceSamples) {
        samples.pop_front();
    }

    while (samples.size() > m_Parameters.minimumCadenceSamples) {
        const uint64_t spanTicks = samples.back().rtpTicks -
            samples.front().rtpTicks;
        const uint64_t spanUs = spanTicks * kMicrosecondsPerSecond /
            kRtpClockRate;
        if (spanUs <= cadenceWindowUs()) {
            break;
        }
        samples.pop_front();
    }
}

uint64_t VrrTimingController::fittedSourcePeriodQ16(
    const std::deque<CadenceSample>& samples) const
{
    if (samples.size() < 2) {
        return 0;
    }

    const CadenceSample& first = samples.front();
    const CadenceSample& last = samples.back();
    if (last.frameOrdinal <= first.frameOrdinal ||
        last.rtpTicks <= first.rtpTicks) {
        return 0;
    }

    // RTP timestamps from a host-refresh-quantized source form a staircase.
    // OLS weighs the placement of each staircase step, so its slope breathes
    // as a long atom crosses the window.  The endpoint span measures only
    // the net source movement and still accounts for skipped local frames.
    const uint64_t spanFrames = last.frameOrdinal - first.frameOrdinal;
    const uint64_t spanTicks = last.rtpTicks - first.rtpTicks;
    constexpr uint64_t kPeriodQ16Scale =
        kMicrosecondsPerSecond * kQ16One;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();

    // The cadence window is at most one previous window plus one valid
    // one-second RTP interval, so these products are comfortably 64-bit in
    // normal operation. Keep explicit guards for malformed frame numbers.
    if (spanTicks > maximum / kPeriodQ16Scale ||
        spanFrames > maximum / kRtpClockRate) {
        return 0;
    }

    const uint64_t numerator = spanTicks * kPeriodQ16Scale;
    const uint64_t denominator = spanFrames * kRtpClockRate;
    const uint64_t quotient = numerator / denominator;
    const uint64_t remainder = numerator % denominator;
    const uint64_t halfway = denominator / 2 + denominator % 2;
    if (remainder >= halfway && quotient < maximum) {
        return quotient + 1;
    }
    return quotient;
}

uint64_t VrrTimingController::cadenceWindowUs() const
{
    const uint64_t displayFloorUs = saturatingAdd(m_DisplayPeriodUs,
                                                   m_GuardUs);
    const uint64_t headroomUs = m_SourcePeriodUs > displayFloorUs ?
        m_SourcePeriodUs - displayFloorUs : 0;
    const uint64_t looseHeadroomUs =
        m_Parameters.looseHeadroomDisplayPeriods >
                std::numeric_limits<uint64_t>::max() / m_DisplayPeriodUs ?
            std::numeric_limits<uint64_t>::max() :
            m_DisplayPeriodUs *
                m_Parameters.looseHeadroomDisplayPeriods;
    if (headroomUs >= looseHeadroomUs) {
        return m_Parameters.looseCadenceWindowUs;
    }
    if (headroomUs <= m_DisplayPeriodUs) {
        return m_Parameters.tightCadenceWindowUs;
    }

    const uint64_t tightnessNumerator = looseHeadroomUs - headroomUs;
    const uint64_t windowRangeUs = m_Parameters.tightCadenceWindowUs -
        m_Parameters.looseCadenceWindowUs;
    return m_Parameters.looseCadenceWindowUs +
        windowRangeUs * tightnessNumerator /
            std::max<uint64_t>(1, looseHeadroomUs - m_DisplayPeriodUs);
}

bool VrrTimingController::isMajorCadenceDeparture(
    uint64_t intervalUs, uint64_t frameDelta) const
{
    if (intervalUs == 0 || frameDelta == 0 || m_SourcePeriodUs == 0) {
        return false;
    }
    const uint64_t observedPeriodUs = std::max<uint64_t>(
        1, intervalUs / frameDelta);
    return observedPeriodUs * m_Parameters.majorCadenceRatioDenominator >
               m_SourcePeriodUs *
                   m_Parameters.majorCadenceRatioNumerator ||
        m_SourcePeriodUs * m_Parameters.majorCadenceRatioDenominator >
               observedPeriodUs *
                   m_Parameters.majorCadenceRatioNumerator;
}

bool VrrTimingController::acceptSourcePeriodQ16(uint64_t periodUsQ16)
{
    if (periodUsQ16 == 0) {
        return false;
    }

    // Filter the raw fit for the metronome before the floor below is
    // applied; a material change re-seeds it so the tick follows a real
    // rate change within a frame rather than a filter window.
    const uint64_t windowFrames = std::max<uint64_t>(
        1, m_Parameters.playoutMetronomePeriodWindowFrames);
    if (m_MetronomePeriodUsQ16 == 0) {
        m_MetronomePeriodUsQ16 = periodUsQ16;
    }
    else if (periodUsQ16 >= m_MetronomePeriodUsQ16) {
        m_MetronomePeriodUsQ16 +=
            (periodUsQ16 - m_MetronomePeriodUsQ16) / windowFrames;
    }
    else {
        m_MetronomePeriodUsQ16 -=
            (m_MetronomePeriodUsQ16 - periodUsQ16) / windowFrames;
    }

    // The negotiated stream rate is an upper bound on source FPS. Preserve
    // it at Q16 precision so fractional rates do not acquire an artificial
    // drift from the rounded microsecond period.
    periodUsQ16 = std::max(periodUsQ16, m_ConfiguredStreamPeriodQ16);
    const uint64_t previousPeriodUs = m_SourcePeriodUs;
    m_SourcePeriodUsQ16 = periodUsQ16;
    m_SourcePeriodUs = std::max<uint64_t>(1, roundedQ16(periodUsQ16));
    m_RenderBaselineUs = std::min(m_RenderBaselineUs,
                                  m_SourcePeriodUs);
    m_RenderLeadUs = clampUnsigned(m_RenderLeadUs,
                                   renderLeadFloorUs(),
                                   renderLeadCeilingUs());
    m_GpuReadinessLeadUs = std::min(m_GpuReadinessLeadUs,
                                    gpuReadinessCeilingUs());
    m_GuardUs = clampUnsigned(m_GuardUs,
                              m_BaseGuardUs,
                              guardCeilingUs());
    clampReadinessReserveToPolicy();
    const bool materialChange = !withinPercent(
        m_SourcePeriodUs, previousPeriodUs,
        m_Parameters.materialRateChangePercent);
    if (materialChange) {
        m_MetronomePeriodUsQ16 = m_SourcePeriodUsQ16;
    }
    return materialChange;
}

void VrrTimingController::anchorSourceTime(uint64_t sourceTimeUs)
{
    m_SourceTimeUs = sourceTimeUs;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    m_SourceTimeUsQ16 = sourceTimeUs > maximum / kQ16One ?
        maximum : sourceTimeUs * kQ16One;
}

void VrrTimingController::notePreparationDuration(
    uint64_t preparationDurationUs, uint64_t acquisitionWaitUs,
    uint64_t preparationCompleteUs, uint64_t gpuReadyWaitUs)
{
    const uint64_t rawPreparationDurationUs = preparationDurationUs;
    // Swapchain availability is not work that a larger jitter buffer fixes.
    // The worker already excludes its intentional waits from this duration.
    if (m_Parameters.playoutHistoryEnabled != 0) {
        preparationDurationUs -= std::min(preparationDurationUs, acquisitionWaitUs);
    }
    // A presenter may report a prepare-time completion wait separately.
    // Once that wait has its own bounded head start, do not charge it a second
    // time as generic render work. Legacy/replay policies leave this disabled,
    // preserving their exact learned render lead.
    if (m_Parameters.playoutGpuReadinessAdaptation != 0) {
        preparationDurationUs -= std::min(preparationDurationUs,
                                           gpuReadyWaitUs);
    }
    if (!m_Pending.valid) {
        return;
    }
    m_Pending.hasPreparationDuration = true;
    m_Pending.preparationDurationUs = preparationDurationUs;
    m_Pending.rawPreparationDurationUs = rawPreparationDurationUs;
    m_Pending.acquisitionWaitUs = std::min(rawPreparationDurationUs,
                                           acquisitionWaitUs);
    m_Pending.preparationCompleteUs = preparationCompleteUs;
}

void VrrTimingController::noteGpuReadyWait(uint64_t waitUs, bool completed,
                                           uint64_t completionUs)
{
    if (m_Parameters.playoutGpuReadinessAdaptation == 0 || !completed) {
        return;
    }

    const uint64_t at = completionUs != 0 ? completionUs :
        m_LastDecodeCompleteUs;
    if (at == 0) {
        return;
    }

    constexpr size_t kMaximumSamples = 512;
    const uint64_t windowUs = std::max<uint64_t>(
        1, m_Parameters.playoutGpuReadinessWindowUs);
    const auto removeOldest = [&]() {
        const uint64_t expiredWait = m_GpuReadinessSamples.front().waitUs;
        const auto sorted = std::lower_bound(
            m_GpuReadinessSortedWaits.begin(),
            m_GpuReadinessSortedWaits.end(), expiredWait);
        m_GpuReadinessSamples.pop_front();
        if (sorted != m_GpuReadinessSortedWaits.end() &&
                *sorted == expiredWait) {
            m_GpuReadinessSortedWaits.erase(sorted);
        }
        else {
            // Keep the percentile valid if a future state-restoration path
            // ever changes the FIFO without updating its sorted companion.
            m_GpuReadinessSortedWaits.clear();
            for (const GpuReadinessSample& sample : m_GpuReadinessSamples) {
                m_GpuReadinessSortedWaits.insert(
                    std::upper_bound(m_GpuReadinessSortedWaits.begin(),
                                     m_GpuReadinessSortedWaits.end(), sample.waitUs),
                    sample.waitUs);
            }
        }
    };
    while (m_GpuReadinessSamples.size() > 1 &&
           (m_GpuReadinessSamples.size() > kMaximumSamples ||
            (at > windowUs &&
             m_GpuReadinessSamples.front().completionUs < at - windowUs))) {
        removeOldest();
    }
    m_GpuReadinessSamples.push_back({at, waitUs});
    m_GpuReadinessSortedWaits.insert(
        std::upper_bound(m_GpuReadinessSortedWaits.begin(),
                         m_GpuReadinessSortedWaits.end(), waitUs),
        waitUs);
    while (m_GpuReadinessSamples.size() > kMaximumSamples) {
        removeOldest();
    }

    const unsigned int percentileValue = std::min(
        100U, m_Parameters.playoutGpuReadinessPercentile);
    const size_t rank = std::max<size_t>(
        1, (m_GpuReadinessSortedWaits.size() * percentileValue + 99) / 100);
    const uint64_t desired = clampUnsigned(
        saturatingAdd(m_GpuReadinessSortedWaits[rank - 1],
                      m_Parameters.playoutGpuReadinessMarginUs),
        0, gpuReadinessCeilingUs());
    if (desired > m_GpuReadinessLeadUs) {
        m_GpuReadinessLeadUs = std::min(
            desired,
            saturatingAdd(m_GpuReadinessLeadUs,
                          std::max<uint64_t>(1,
                              m_Parameters.playoutGpuReadinessAttackUs)));
    }
    else if (desired < m_GpuReadinessLeadUs) {
        const uint64_t elapsed = m_LastGpuReadinessUpdateUs != 0 &&
                at >= m_LastGpuReadinessUpdateUs ?
            std::min<uint64_t>(at - m_LastGpuReadinessUpdateUs, 33333) :
            std::max<uint64_t>(1, m_SourcePeriodUs);
        const uint64_t rate = m_Parameters.playoutGpuReadinessReleaseUsPerSecond;
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        const uint64_t releaseNumerator = rate != 0 &&
                elapsed > maximum / rate ? maximum : rate * elapsed;
        const uint64_t releaseUs = rate == 0 ? 0 :
            std::max<uint64_t>(1, releaseNumerator / kMicrosecondsPerSecond);
        m_GpuReadinessLeadUs -= std::min(
            m_GpuReadinessLeadUs - desired, releaseUs);
    }
    m_GpuReadinessLeadUs = std::min(m_GpuReadinessLeadUs,
                                    gpuReadinessCeilingUs());
    m_LastGpuReadinessUpdateUs = at;
}

void VrrTimingController::noteDeferredGpuReady(uint64_t waitUs,
                                               bool completed,
                                               uint64_t completionUs,
                                               uint64_t serviceUpperBoundUs,
                                               bool readinessWasPending)
{
    noteGpuReadyWait(waitUs, completed, completionUs);
    if (m_Parameters.playoutSerialServiceGate == 0 ||
            !m_Pending.valid || !completed || completionUs == 0) {
        return;
    }

    // A fence first observed after the cadence hold may have completed at any
    // point since preparation's initial poll. Its observation time must not
    // become current-frame readiness lateness: a late target wake with an
    // already-complete fence would otherwise manufacture buffer pressure.
    // Keep the upper bound only as conservative serial-service evidence and
    // use the measured residual wait for following-frame lead learning above.
    m_Pending.deferredGpuServiceUs = std::max(
        m_Pending.deferredGpuServiceUs, serviceUpperBoundUs);
    if (m_Parameters.playoutSerialServiceGate >= 2) {
        // Only the residual CPU wait occupies this serial worker. The entire
        // preparation-to-fence interval includes intentional cadence holding
        // and concurrent GPU execution, neither of which is serial CPU service.
        m_Pending.deferredGpuWaitUs = waitUs;
        if (readinessWasPending && waitUs != 0) {
            m_Pending.deferredGpuReadyUs = completionUs;
        }
    }
}

void VrrTimingController::noteSchedulerDelays(uint64_t renderDelayUs,
                                              uint64_t targetDelayUs,
                                              bool targetDelayValid)
{
    m_Pending.renderSchedulerUs = renderDelayUs;
    appendBounded(m_RenderSchedulerDelays, renderDelayUs,
                  m_Parameters.schedulerLearningSamples);
    if (targetDelayValid) {
        appendBounded(m_TargetSchedulerDelays, targetDelayUs,
                      m_Parameters.schedulerLearningSamples);
    }
    updateLearnedBudgets();
}

void VrrTimingController::noteSpacingDeficit(uint64_t deficitUs)
{
    if (deficitUs != 0) {
        m_CleanSpacingFrames = 0;
        const uint64_t increaseUs = std::max(m_Parameters.guardStepUs,
                                             deficitUs);
        m_GuardUs = std::min(guardCeilingUs(),
                             saturatingAdd(m_GuardUs, increaseUs));
        return;
    }

    if (m_GuardUs > m_BaseGuardUs &&
        ++m_CleanSpacingFrames >= m_Parameters.guardDecayFrames) {
        m_GuardUs -= std::min(m_Parameters.guardStepUs,
                              m_GuardUs - m_BaseGuardUs);
        m_CleanSpacingFrames = 0;
    }
}

void VrrTimingController::noteSubmission(bool submitted, bool cancelled,
                                         uint64_t submissionUs, uint64_t actualSubmissionUs)
{
    m_InputAuditFeedbackFrame = m_Pending.valid ? m_Pending.smoothness.frame : 0;
    m_InputAuditEligibilityReasons = (submitted?0ULL:1ULL) | (cancelled?2ULL:0ULL);
    if (!m_Pending.valid) {
        return;
    }

    const auto& cadenceSample = m_Pending.smoothness;
    const uint64_t enqueueUs = actualSubmissionUs ? actualSubmissionUs : submissionUs;
    if (m_Parameters.boundedCatchupEnabled) {
        m_PhaseTrajectory.commit(m_Pending.smoothness.frame, m_Pending.intervalIntendedUs,
            submissionUs, enqueueUs, submitted, cancelled, m_Pending.trajectoryValid);
    }

    m_SubmissionCadence.observe({cadenceSample.frame, cadenceSample.sourceTimestampUs,
        enqueueUs, enqueueUs, 0,
        submitted && !cancelled && cadenceSample.sourceTimestampValid},
        intervalQualityToleranceUs(m_Parameters));

    if (m_Parameters.playoutResponsiveBuffer >= 5) {
        const auto& p = m_Pending.prediction;
        const auto work = saturatingAdd(m_Pending.preparationDurationUs, m_Pending.renderSchedulerUs);
        const uint64_t preparationServiceUs =
            m_Pending.rawPreparationDurationUs -
            std::min(m_Pending.rawPreparationDurationUs,
                     m_Pending.acquisitionWaitUs);
        // A completion checked only after the cadence hold is an upper bound:
        // the GPU may have finished earlier. Treat that uncertainty
        // conservatively for growth so an unmeasured renderer tail cannot
        // authorize more standing latency.
        const uint64_t serialServiceUs = saturatingAdd(
            m_Pending.decodeSyncWaitUs,
            saturatingAdd(m_Pending.renderSchedulerUs,
                m_Parameters.playoutSerialServiceGate >= 2 ?
                    saturatingAdd(preparationServiceUs, m_Pending.deferredGpuWaitUs) :
                    std::max(preparationServiceUs, m_Pending.deferredGpuServiceUs)));
        const bool legacyServiceAbsorbable =
            m_Parameters.playoutSerialServiceGate != 0 ||
            (work <= p.period && p.decoderQueue <= p.period);
        const auto ready = std::max(m_Pending.deferredGpuReadyUs,
            m_Pending.preparationCompleteUs ? m_Pending.preparationCompleteUs : saturatingAdd(p.decoded, work));
        // Early retiming has its own bounded reserve. Only readiness beyond
        // the raw deadline may grow or renew the interval buffer's hold. Keep
        // a later smoothed deadline when it already covers genuine raw lateness.
        const uint64_t smoothingAdvanceUs =
            m_Parameters.playoutSmoothingReadinessBound != 0 &&
            m_Parameters.playoutResponsiveBuffer >= 6 ? p.smoothingAdvance : 0;
        const auto deadline = saturatingAdd(m_Pending.smoothness.intended,
                                             smoothingAdvanceUs);
        if (m_Parameters.playoutResponsiveBuffer >= 6) {
            m_IntervalBuffer.observe({m_Pending.smoothness.frame, m_Pending.intervalIntendedUs,
                submissionUs, deadline, ready, p.applied,
                submitted && !cancelled && m_Pending.intervalValid && m_Pending.hasPreparationDuration,
                p.eligible && legacyServiceAbsorbable,
                serialServiceUs, p.decoderQueue, m_Pending.inputSlotUs,
                m_Pending.inputRequiredUs, m_Pending.inputValid},
                playoutDelayMinimumUs(), playoutDelayMaximumUs(),
                m_Parameters.playoutMeanMissHoldUs, m_Parameters.playoutMeanMissReleaseUsPerSecond,
                m_Parameters.playoutResponsiveBuffer >= 7, m_Parameters.playoutOnTimeTargetPerMillion,
                intervalQualityToleranceUs(m_Parameters),
                intervalQualityWindowUs(m_Parameters),
                m_Parameters.playoutIntervalInitialWarmupUs,
                m_Parameters.playoutIntervalInitialMinimumSamples,
                m_Parameters.playoutRecentPressureRelease,
                m_Parameters.playoutSerialServiceGate,
                m_Parameters.playoutHoldRenewBelowTarget,
                m_Parameters.playoutResponsiveBuffer >= 9,
                m_Parameters.playoutResponsiveBuffer == 10,
                m_Parameters.playoutResponsiveBuffer >= 12,
                m_Parameters.playoutResponsiveBuffer >= 12 &&
                m_Parameters.playoutAbsorbedJitterRelease != 0);
            if (m_Parameters.playoutInputPercentileEnabled) {
                const auto capacity = m_IntervalBuffer.stats();
                const bool validInput = submitted && !cancelled && m_Pending.intervalValid &&
                    m_Pending.hasPreparationDuration && m_Pending.inputValid && p.eligible &&
                    capacity.averageValid && !capacity.serviceOverloaded;
                // A late input measurement alone is not a playback miss. The
                // observed interval must stretch beyond tolerance, attributed
                // to this frame's readiness missing its intended deadline.
                // A compressed successor is not a second miss for that hitch.
                const auto& outcome = capacity.update;
                const uint64_t toleranceUs = intervalQualityToleranceUs(m_Parameters);
                const bool inputHitch = outcome.frame == m_Pending.smoothness.frame &&
                    outcome.attributedFrame == m_Pending.smoothness.frame &&
                    outcome.intervalErrorUs > toleranceUs && outcome.latenessUs > toleranceUs;
                m_InputAuditEligibilityReasons |=
                    (m_Pending.intervalValid?0ULL:4ULL) |
                    (m_Pending.hasPreparationDuration?0ULL:8ULL) |
                    (m_Pending.inputValid?0ULL:16ULL) | (p.eligible?0ULL:32ULL) |
                    (capacity.averageValid?0ULL:64ULL) | (capacity.serviceOverloaded?128ULL:0ULL) |
                    (inputHitch?0ULL:256ULL) |
                    (m_Pending.intervalIntendedUs?0ULL:1024ULL);
                if (m_Parameters.playoutInputHitchMissesOnly && m_Pending.inputRequiredUs > 0 &&
                    uint64_t(m_Pending.inputRequiredUs) - std::min(uint64_t(m_Pending.inputRequiredUs),
                        m_Parameters.playoutInputPercentileToleranceUs) > playoutDelayMaximumUs())
                    m_InputAuditEligibilityReasons |= 512ULL;
                m_InputPercentileBeforeUs = p.applied;
                m_InputPercentileBuffer.setLearningTrim(
                    static_cast<size_t>(m_Parameters.playoutInputLearningTrim));
                const auto inputBefore = m_InputPercentileBuffer.stats();
                m_InputPercentileBuffer.setStableAdaptation(m_Parameters.playoutInputStableAdaptation);
                m_InputPercentileBuffer.observe(m_Pending.smoothness.frame,
                    enqueueUs ? enqueueUs : m_Pending.inputObservationUs,
                    m_Pending.intervalIntendedUs, m_Pending.inputRequiredUs, validInput,
                    playoutDelayMinimumUs(), playoutDelayMaximumUs(),
                    m_Parameters.playoutInputPercentileTargetPerMillion,
                    m_Parameters.playoutInputPercentileWindowUs,
                    m_Parameters.playoutInputPercentileRiseUsPerSecond,
                    m_Parameters.playoutInputPercentileFallUsPerSecond,
                    m_Pending.inputReadinessBudgetUs, m_Parameters.playoutInputPercentileToleranceUs,
                    inputRecoveryCreditUs(), m_Pending.inputClockOffsetUs, inputHitch,
                    inputHitchPolicy(m_Pending.inputObservationUs));
                if (m_Parameters.playoutInputHitchMissesOnly >= 8) {
                    const auto inputAfter = m_InputPercentileBuffer.stats();
                    m_InputCoverageHistory.observe(enqueueUs ? enqueueUs : m_Pending.inputObservationUs,
                        m_Parameters.playoutInputPercentileWindowUs,
                        inputAfter.eligible == inputBefore.eligible + 1,
                        inputAfter.absorbed > inputBefore.absorbed, inputAfter.missed > inputBefore.missed);
                }
            }
        }
        else m_MeanMissBuffer.observe(submissionUs, ready > deadline ? ready - deadline : 0,
            p.applied, submitted && !cancelled && m_Pending.hasPreparationDuration &&
                m_Pending.smoothness.eligible && p.eligible && work <= p.period &&
                    p.decoderQueue <= p.period &&
                    (m_Parameters.playoutSerialServiceGate == 0 ||
                     serialServiceUs <= m_SourcePeriodUs),
            playoutDelayMinimumUs(), playoutDelayMaximumUs(),
            m_Parameters.playoutMeanMissHoldUs, m_Parameters.playoutMeanMissReleaseUsPerSecond);
    }

    if (m_Parameters.playoutReadinessHitchThresholdUs) {
        const auto& p = m_Pending.prediction;
        const auto work = saturatingAdd(m_Pending.preparationDurationUs,
                                       m_Pending.renderSchedulerUs);
        // Reconstruct readiness without our intentional render/target waits,
        // swapchain acquisition or post-submit blocking. Overload/backpressure
        // and discontinuities cannot be cured by storing more frames.
        const auto result = m_ReadinessFeedback.observe({
            m_Pending.smoothness.frame, m_Pending.smoothness.intended,
            submissionUs, saturatingAdd(p.decoded, work), p.applied,
            submitted && !cancelled && m_Pending.hasPreparationDuration &&
                m_Pending.smoothness.eligible && p.eligible &&
                work <= p.period && p.decoderQueue <= p.period},
            m_Parameters.playoutReadinessHitchThresholdUs);
        if (result.interval) {
            const auto ns = [](uint64_t us) {
                return int64_t(std::min<uint64_t>(us, INT64_MAX / 1000)) * 1000;
            };
            m_PlayoutHistory.observe(ns(result.demand), ns(p.applied), ns(submissionUs));
            if (result.demand) {
                m_UnclampedRequestedPlayoutDelayUs = std::max(
                    m_UnclampedRequestedPlayoutDelayUs, result.demand);
                m_RequestedPlayoutDelayUs = std::max(m_RequestedPlayoutDelayUs,
                    std::min(result.demand, playoutDelayMaximumUs()));
            }
        }
    }

    if (m_Parameters.playoutSmoothnessFeedbackEnabled) {
        if (submitted && !cancelled) {
            auto sample = m_Pending.smoothness;
            sample.at = submissionUs;
            const auto before = m_SubmissionSmoothness.observedIntervals();
            const uint64_t demand = m_SubmissionSmoothness.observe(sample, submissionUs,
                m_Parameters.playoutSubmissionEstimateFallback != 0);
            if (m_Parameters.playoutSubmissionEstimateFallback &&
                    m_SubmissionSmoothness.observedIntervals() > before) {
                ++m_EstimatedCadenceIntervals;
                m_EstimatedCadenceHitches += demand != 0;
                // A submission timestamp is a lower-confidence presentation
                // proxy. Never mix it into the verified display counters or
                // teach native service latency from it. It may guide padding
                // only while verified native feedback is unavailable.
                if (demand && !m_Parameters.playoutPredictionOnly &&
                        !hasRecentNativeFeedback(submissionUs)) {
                    m_UnclampedRequestedPlayoutDelayUs = std::max(
                        m_UnclampedRequestedPlayoutDelayUs, demand);
                    m_RequestedPlayoutDelayUs = std::max(m_RequestedPlayoutDelayUs, demand);
                }
            }
        }
        else m_SubmissionSmoothness.breakSequence();
    }
    const bool nativeLatch = m_NativeLatchPending;
    m_NativeLatchPending = false;
    if (m_Parameters.playoutLateRecovery != 0 && !m_Parameters.boundedCatchupEnabled) {
        // Preparation/submission may miss a target after schedule() returned,
        // with no old queued frames at all. Arm recovery from that actual
        // result as well as from queue age. Small timer noise does not arm it;
        // failed/cancelled work cannot establish a recovery anchor.
        m_CatchupActive = submitted && !cancelled && m_Pending.intervalValid &&
            (m_CatchupActive || submissionUs >
                saturatingAdd(m_Pending.smoothness.intended, 500));
    }
    if (submitted) {
        // Cancellation is a reason, not proof that nothing reached the native
        // presentation queue (Vulkan must submit some abandoned images).
        // A latched present waits in the flip queue for the panel's minimum
        // period after the previous flip; it cannot reach scanout sooner.
        // A native latch queues behind the observed refresh when available.
        if (nativeLatch) {
            m_SpacingAnchorUs = std::max(m_SpacingAnchorUs, m_NativeLatchFlipUs);
        }
        m_SpacingAnchorUs = m_HaveLastSubmission && (m_LatchedPresentation || nativeLatch) ?
            std::max(submissionUs, saturatingAdd(spacingAnchorUs(), m_DisplayPeriodUs)) :
            submissionUs;
        m_HaveLastSubmission = true;
        m_LastSubmissionUs = submissionUs;

        if (!cancelled) {
            if (m_Pending.hasSmoothedBasis) {
                m_LastSmoothedBasisUsQ16 = m_Pending.smoothedBasisUsQ16;
                m_LastSmoothedBasisUs = roundedQ16(m_Pending.smoothedBasisUsQ16);
                m_LastSmoothedBasisOrdinal = m_Pending.smoothedBasisOrdinal;
                m_PhaseDebtUs = m_Pending.phaseDebtUs;
                m_PhaseResidualEmaUs = m_Pending.phaseResidualEmaUs;
                m_LastBasisMappingUs = m_Pending.basisMappingUs;
                m_HaveSmoothedBasis = true;
            }
            if (m_Pending.cadenceEligible) {
                appendBounded(m_ReadyOffsets, m_Pending.readyOffsetUs,
                              readinessLearningSampleLimit());
            }
            if (m_Parameters.playoutReadinessFloorPerMille != 0 &&
                    m_Pending.intervalValid) {
                noteReadinessFloorSample(submissionUs, m_Pending.readyOffsetUs);
            }
            // The first preparation after a presenter or source epoch starts
            // includes one-time renderer setup. Learning it as typical render
            // cost pushes the following targets out by that setup time.
            const bool initialPreparation =
                m_Parameters.preparationInitialSampleExcluded != 0 &&
                !m_PreparationInitialSampleSeen;
            if (m_Pending.hasPreparationDuration) {
                m_PreparationInitialSampleSeen = true;
            }
            if (m_Pending.hasPreparationDuration && !initialPreparation) {
                if (m_Parameters.playoutPredictionEnabled &&
                    !m_Parameters.playoutReadinessHitchThresholdUs) {
                    m_ReadinessPrediction.observe(m_PlayoutHistory, m_Pending.prediction,
                        m_Pending.preparationDurationUs, m_Pending.renderSchedulerUs,
                        m_Parameters.playoutResponsiveBuffer && m_Parameters.playoutResponsiveBuffer < 5 ? &m_RecentReadiness : nullptr);
                }
                appendBounded(m_PreparationDurations,
                              m_Pending.preparationDurationUs,
                              m_Parameters.preparationLearningSamples);
            }
            updateReadinessModel();
            updateLearnedBudgets();
        }
    }

    m_Pending = PendingFrame {};
}

Vrr13::SmoothnessFeedback::Sample VrrTimingController::smoothnessSample(const VrrTimingDecision& d) const
{
    Vrr13::SmoothnessFeedback::Sample sample;
    sample.frame = d.frameNumber;
    sample.sourceTimestampUs = d.diagnosticSourceTimestampUs;
    sample.sourceTimestampValid = d.diagnosticSourceTimestampValid;
    // Judge cadence against the intended source schedule. A newly learned
    // compositor lead changes our prediction, not the spacing we want to
    // display. Including its frame-to-frame changes manufactures buffer
    // demand even when both frames were prepared and presented on time.
    // Keep the old reference available for exact replay of existing captures.
    sample.intended = m_Parameters.playoutStableSmoothnessReference ?
        d.originalTargetUs : d.originalScanoutUs;
    if (m_Parameters.playoutNativeHitchAdaptation || m_Parameters.playoutPredictionOnly) {
        // Score client-added spacing against the game's source cadence. Changes
        // in our padding or render estimate must not redefine a smooth result.
        sample.intended = d.sourceTimeUs;
    }
    sample.buffer = d.playoutDelayUs;
    sample.headroom = m_Parameters.playoutNativeHitchAdaptation ? 0 : d.recoveryHeadroomUs;
    sample.eligible = d.cadenceEligible && !d.rebased && !d.phaseDiscontinuity &&
        !d.sourceRateChanged && d.sourceIntervalUs <= std::max<uint64_t>(25000, d.sourcePeriodUs * 3 / 2);
    return sample;
}

bool VrrTimingController::hasRecentNativeFeedback(uint64_t now) const
{
    return m_NativeSmoothness.samples() &&
        std::abs(signedDifference(now, m_NativeSmoothness.lastObservedUs())) <= 100000;
}

const Vrr13::SmoothnessFeedback& VrrTimingController::activeSmoothnessFeedback(uint64_t now) const
{
    return m_Parameters.playoutSubmissionEstimateFallback && !hasRecentNativeFeedback(now) ?
        m_SubmissionSmoothness : m_NativeSmoothness;
}

void VrrTimingController::notePresentation(const Vrr13::PresentationObservation& observation)
{
    // Production retains native cadence as diagnostic evidence. schedule()
    // ignores this model's lead/floor; buffer growth uses client submission
    // intervals with readiness attribution, independently of display feedback.
    if (!m_Parameters.playoutPredictionEnabled) return;
    if (!m_Parameters.playoutSmoothnessFeedbackEnabled) {
        m_PresentationPrediction.observe(observation,
            m_Parameters.playoutRequireDisplayEvents != 0);
        return;
    }
    if (observation.submitted) {
        if (m_FeedbackModeValid && m_FeedbackLatched != observation.latched) {
            m_NativeSmoothness.breakSequence();
            m_PresentTiming.breakSequence();
            m_DisplayCadence.breakSequence();
        }
        m_FeedbackModeValid = true;
        m_FeedbackLatched = observation.latched;
    }
    m_PresentationPrediction.observe(observation, [this, &observation](const Vrr13::SmoothnessFeedback::Sample& sample,
                                                                        uint64_t observed, uint64_t submittedUs,
                                                                        uint64_t plannedUs) {
        if (observation.timeKind == Vrr13::PresentationTimeKind::DisplayEvent) {
            m_DisplayCadence.observe({sample.frame, sample.sourceTimestampUs, sample.at,
                observed, sample.uncertainty, sample.sourceTimestampValid},
                intervalQualityToleranceUs(m_Parameters));
            m_PresentTiming.observe({sample.frame, submittedUs, sample.at, plannedUs, sample.uncertainty, observed},
                                    {intervalQualityToleranceUs(m_Parameters), m_DisplayPeriodUs,
                                     m_SourcePeriodUs, m_Parameters.vrrFloorLatchGapUs});
        }
        const auto intervalsBefore = m_NativeSmoothness.observedIntervals();
        const uint64_t demand = m_NativeSmoothness.observe(sample, observed,
            observation.timeKind == Vrr13::PresentationTimeKind::DisplayEvent ||
            m_Parameters.playoutNativeHitchAdaptation != 0);
        if (observation.timeKind == Vrr13::PresentationTimeKind::DisplayEvent &&
                m_NativeSmoothness.observedIntervals() > intervalsBefore) {
            ++m_NativeCadenceIntervals;
            m_NativeCadenceHitches += demand != 0;
        }
        if (m_Parameters.playoutNativeHitchAdaptation && demand != 0) {
            // Only a new, matched native interval miss authorizes growth.
            // Demand belongs to the delayed frame's original padding, so a
            // catch-up sample cannot repeatedly charge today's larger buffer.
            m_UnclampedRequestedPlayoutDelayUs = std::max(
                m_UnclampedRequestedPlayoutDelayUs, demand);
            m_RequestedPlayoutDelayUs = std::max(m_RequestedPlayoutDelayUs, demand);
        }
    }, m_Parameters.playoutRequireDisplayEvents != 0);
}

void VrrTimingController::updateLearnedBudgets()
{
    if (!m_PreparationDurations.empty()) {
        m_RenderBaselineUs = std::min(
            percentile(m_PreparationDurations,
                       m_Parameters.renderBaselinePercentile),
            m_SourcePeriodUs);
        m_RenderLeadUs = clampUnsigned(
            saturatingAdd(percentile(
                              m_PreparationDurations,
                              m_Parameters.preparationPercentile),
                          m_Parameters.renderLeadSlackUs),
            renderLeadFloorUs(), renderLeadCeilingUs());
        // The p99 tail and readiness reserve share one hard policy budget.
        // Enforce a newly larger render tail immediately without advancing
        // the gradual readiness acquisition a second time.
        clampReadinessReserveToPolicy();
    }

    if (!m_RenderSchedulerDelays.empty()) {
        m_RenderWakeLeadUs = std::min(
            m_Parameters.maximumRenderWakeLeadUs,
            percentile(m_RenderSchedulerDelays,
                       m_Parameters.schedulerPercentile));
    }

    if (!m_TargetSchedulerDelays.empty()) {
        m_TargetWakeLeadUs = std::min(
            m_Parameters.maximumTargetWakeLeadUs,
            percentile(m_TargetSchedulerDelays,
                       m_Parameters.schedulerPercentile));
    }
}

void VrrTimingController::updateReadinessModel()
{
    if (m_ReadyOffsets.size() < m_Parameters.minimumReadinessSamples) {
        return;
    }

    // Learn exogenous decode-arrival variation, not absolute source phase or
    // queue age created by this controller. The selected low percentile is
    // the local phase baseline. Smoothness mode uses a VRR8-style p10/p95
    // spread so the reserve remains visible to the latency policy instead of
    // hiding a high-percentile phase shift outside the budget.
    // Near the display ceiling, preserve the p90 arrival tail because there is
    // too little cadence slack to absorb a late frame. Slower sources can use
    // p80 and avoid making the slowest fifth standing latency for every frame.
    const int64_t lowUs = percentile(
        m_ReadyOffsets, m_Parameters.readinessLowPercentile);
    const unsigned int highPercentile = headroomUs() > m_DisplayPeriodUs ?
        m_Parameters.readinessLoosePercentile :
        m_Parameters.readinessTightPercentile;
    const int64_t highUs = percentile(m_ReadyOffsets, highPercentile);
    const uint64_t spreadUs = highUs > lowUs ?
        static_cast<uint64_t>(highUs - lowUs) : 0;
    const uint64_t candidateDemandUs = clampUnsigned(
        saturatingAdd(spreadUs, m_Parameters.arrivalSpreadGuardUs),
        m_Parameters.minimumReadinessReserveUs, readinessCeilingUs());

    if (!m_ReadinessModelValid) {
        m_ReadinessDemandUs = candidateDemandUs;
        m_ReadinessModelValid = true;
    }
    else if (candidateDemandUs > m_ReadinessDemandUs) {
        // Attack faster than release, but never let one observation window
        // impose its entire tail on subsequent frames.
        const uint64_t difference = candidateDemandUs - m_ReadinessDemandUs;
        m_ReadinessDemandUs += std::max<uint64_t>(
            1, difference * m_Parameters.readinessAttackNumerator /
                m_Parameters.readinessAttackDenominator);
    }
    else if (candidateDemandUs < m_ReadinessDemandUs) {
        const uint64_t difference = m_ReadinessDemandUs - candidateDemandUs;
        m_ReadinessDemandUs -= std::max<uint64_t>(
            1, difference * m_Parameters.readinessReleaseNumerator /
                m_Parameters.readinessReleaseDenominator);
    }

    m_ReadinessPhaseUs = lowUs;
    applyReadinessBudget(true);
}

void VrrTimingController::applyReadinessBudget(bool acquireReserve,
                                               bool immediateAcquisition)
{
    // Cadence slack can absorb most arrival variation without committing a
    // decoded frame early. Preserve one quarter as service margin, matching
    // the field-tested VRR8 reserve rule, and retain a small floor near the
    // panel ceiling where Mailbox still needs a standing cadence cushion.
    // In this projected-source-clock design, small cadence slack cannot
    // substitute for readiness reserve: doing so lets quantized late arrivals
    // clamp the target to "now" and turns their 8/16 ms atoms into visible
    // presentation bursts. Credit slack only when at least a full additional
    // display period is available; tight and near-ceiling cadences retain the
    // complete learned cushion.
    const uint64_t cadenceHeadroomUs = headroomUs();
    const uint64_t usableHeadroomUs =
        m_SourcePeriodUs >= m_DisplayPeriodUs *
                m_Parameters.looseHeadroomDisplayPeriods ?
            cadenceHeadroomUs * m_Parameters.usableHeadroomNumerator /
                m_Parameters.usableHeadroomDenominator : 0;
    const uint64_t learnedDemandUs = m_ReadinessModelValid ?
        m_ReadinessDemandUs : m_Parameters.coldStartReadinessDemandUs;
    const uint64_t effectiveDemandUs = std::max(
        learnedDemandUs, readinessPeriodFloorUs());
    m_AppliedReadinessReserveUs = std::min(
        readinessReserveCeilingUs(),
        std::max(minimumReadinessReserveUs(),
                 effectiveDemandUs > usableHeadroomUs ?
                     effectiveDemandUs - usableHeadroomUs : 0));

    const int64_t ceilingUs = static_cast<int64_t>(readinessCeilingUs());
    const int64_t reserveUs = static_cast<int64_t>(
        std::min<uint64_t>(m_AppliedReadinessReserveUs,
                           static_cast<uint64_t>(ceilingUs)));
    const int64_t desiredUs = m_ReadinessPhaseUs >
            std::numeric_limits<int64_t>::max() - reserveUs ?
        std::numeric_limits<int64_t>::max() :
        m_ReadinessPhaseUs + reserveUs;
    const int64_t clampedDesiredUs = std::max(
        -ceilingUs, std::min(desiredUs, ceilingUs));
    if (!acquireReserve) {
        m_ReadinessBudgetUs = std::max(
            -ceilingUs, std::min(m_ReadinessPhaseUs, ceilingUs));
    }
    else if (immediateAcquisition) {
        // A phase reset changes the source-clock origin, not the amount of
        // arrival variation already learned for this device. Reapply the
        // bounded reserve in the new epoch so cadence transitions cannot
        // collapse smoothness back to zero for another learning window.
        m_ReadinessBudgetUs = clampedDesiredUs;
    }
    else if (clampedDesiredUs > m_ReadinessBudgetUs) {
        m_ReadinessBudgetUs += std::min<int64_t>(
            clampedDesiredUs - m_ReadinessBudgetUs,
            static_cast<int64_t>(m_Parameters.readinessAcquireStepUs));
    }
    else if (clampedDesiredUs < m_ReadinessBudgetUs) {
        m_ReadinessBudgetUs -= std::min<int64_t>(
            m_ReadinessBudgetUs - clampedDesiredUs,
            static_cast<int64_t>(m_Parameters.readinessAcquireStepUs));
    }

    clampReadinessReserveToPolicy();
}

void VrrTimingController::clampReadinessReserveToPolicy()
{
    // Demand release remains gradual, but a larger render tail must not leave
    // the combined pacing reserve above the half-scanout policy ceiling.
    const int64_t ceilingUs = static_cast<int64_t>(readinessCeilingUs());
    m_AppliedReadinessReserveUs = std::min(
        m_AppliedReadinessReserveUs,
        std::min(readinessReserveCeilingUs(),
                 static_cast<uint64_t>(ceilingUs)));
    const int64_t reserveUs = static_cast<int64_t>(
        m_AppliedReadinessReserveUs);
    const int64_t policyCeilingUs = m_ReadinessPhaseUs >
            std::numeric_limits<int64_t>::max() - reserveUs ?
        std::numeric_limits<int64_t>::max() :
        m_ReadinessPhaseUs + reserveUs;
    m_ReadinessBudgetUs = std::min(
        m_ReadinessBudgetUs,
        std::max(-ceilingUs, std::min(policyCeilingUs, ceilingUs)));
}

uint64_t VrrTimingController::timingBudgetUs() const
{
    return saturatingAdd(
        m_TimestampPlayoutActive ? effectivePlayoutDelayUs() :
                                   m_Parameters.sourcePlayoutDelayUs,
        saturatingAdd(
            m_AppliedReadinessReserveUs,
            saturatingAdd(pacingLatencyPolicyEnabled() ?
                              renderInsuranceUs() : m_RenderLeadUs,
                          m_Parameters.presentationSafetyUs)));
}

int64_t VrrTimingController::readinessBudgetUs() const
{
    return m_ReadinessBudgetUs;
}

uint64_t VrrTimingController::headroomUs() const
{
    const uint64_t floorUs = saturatingAdd(m_DisplayPeriodUs, m_GuardUs);
    return m_SourcePeriodUs > floorUs ? m_SourcePeriodUs - floorUs : 0;
}

uint64_t VrrTimingController::typicalRenderUs() const
{
    return m_PreparationDurations.empty() ? m_RenderLeadUs :
        std::clamp<uint64_t>(percentile(m_PreparationDurations, 50), 100, 100000);
}

uint64_t VrrTimingController::recoveryHeadroomUs() const
{
    const uint64_t occupied = std::max(m_DisplayPeriodUs, typicalRenderUs());
    return m_SourcePeriodUs > occupied ? m_SourcePeriodUs - occupied : 0;
}

uint64_t VrrTimingController::catchupCarryLimitUs() const
{
    const uint64_t fixedUs = m_Parameters.playoutCatchupCarryLimitUs;
    const uint64_t shareUs = m_Parameters.playoutCatchupCarryLimitPeriodPerMille != 0 ?
        scaledPerMille(m_SourcePeriodUs, m_Parameters.playoutCatchupCarryLimitPeriodPerMille) : 0;
    if (fixedUs == 0) return shareUs;
    return shareUs == 0 ? fixedUs : std::min(fixedUs, shareUs);
}

uint64_t VrrTimingController::inputRecoveryCreditUs() const
{
    const uint64_t physicalRoomUs = recoveryHeadroomUs();
    if (!m_Parameters.playoutInputBoundedRecoveryCredit) {
        return m_Parameters.playoutInputRecoveryCreditRoomPerMille != 0 ?
            scaledPerMille(physicalRoomUs, std::min<uint64_t>(1000, m_Parameters.playoutInputRecoveryCreditRoomPerMille)) :
            physicalRoomUs;
    }
    // A physical refresh gap is not permission to spend the entire gap in
    // one interval. Preserve free recovery credit only within the configured
    // cadence allowance; the trajectory still owns and gradually spends it.
    return std::min(physicalRoomUs, intervalQualityToleranceUs(m_Parameters));
}

uint64_t VrrTimingController::scaledDisplayPeriodUs(
    uint64_t numerator, uint64_t denominator) const
{
    if (numerator == 0 || denominator == 0) {
        return 0;
    }
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    if (m_DisplayPeriodUs > maximum / numerator) {
        return maximum;
    }
    return m_DisplayPeriodUs * numerator / denominator;
}

bool VrrTimingController::rateProtectedPresentation() const
{
    const int cutoffHz = VrrRatePolicy::protectedRateForRefresh(m_Config.displayRefreshHz);
    // Compare at the controller's microsecond resolution so RTP quantization
    // around an exact integer cutoff does not toggle presentation modes.
    // The configured rate seeds this period until the source fit is available.
    return m_CanLatchPresentation && cutoffHz > 0 &&
        m_SourcePeriodUs <= periodForRate(cutoffHz, 0);
}

uint64_t VrrTimingController::latchedPresentationHeadroomUs() const
{
    return std::max(
        m_Parameters.latchedPresentationHeadroomUs,
        scaledDisplayPeriodUs(
            m_Parameters.latchedPresentationHeadroomPeriodNumerator,
            m_Parameters.latchedPresentationHeadroomPeriodDenominator));
}

uint64_t VrrTimingController::latchedPresentationExitHeadroomUs() const
{
    return std::max(
        m_Parameters.latchedPresentationExitHeadroomUs,
        scaledDisplayPeriodUs(
            m_Parameters.latchedPresentationExitHeadroomPeriodNumerator,
            m_Parameters.latchedPresentationExitHeadroomPeriodDenominator));
}

uint64_t VrrTimingController::sourcePeriodUs() const
{
    return m_SourcePeriodUs;
}

uint64_t VrrTimingController::displayPeriodUs() const
{
    return m_DisplayPeriodUs;
}

uint64_t VrrTimingController::guardUs() const
{
    return m_GuardUs;
}

uint64_t VrrTimingController::renderLeadUs() const
{
    return m_RenderLeadUs;
}

uint64_t VrrTimingController::gpuReadinessLeadUs() const
{
    return std::min(m_GpuReadinessLeadUs, gpuReadinessCeilingUs());
}

uint64_t VrrTimingController::targetWakeLeadUs() const
{
    return m_TargetWakeLeadUs;
}

uint64_t VrrTimingController::earliestSubmissionUs() const
{
    if (!m_HaveLastSubmission) {
        return 0;
    }
    if (m_Parameters.nativeScheduledPresentation != 0) {
        // A synchronized presentation manager may coalesce several overdue
        // images into one refresh. Native acceptance does not promise each
        // image a visible interval. Reserve an admissible display deadline;
        // CPU enqueue may precede it on the native scheduled path.
        return saturatingAdd(m_LastSubmissionUs, m_DisplayPeriodUs);
    }
    if (m_Parameters.latchedFloorDisabled != 0 && m_LatchedPresentation) {
        // Latched presents omit the tearing flag, so the flip queue already
        // orders and spaces them. A software floor of one display period
        // plus a guard cannot sustain a source at the refresh rate and only
        // manufactured a backlog there.
        return 0;
    }
    // Persistent Vulkan Immediate cannot switch to a protected native present.
    // Enforce the same entry margin by waiting instead. Capable presenters
    // already selected protection for slots inside this margin; their adaptive
    // slots naturally satisfy it. Historical captures retain the old floor.
    const uint64_t safetyHeadroomUs = m_Parameters.playoutPerFrameLatch >= 2 ?
        latchedPresentationHeadroomUs() : 0;
    return saturatingAdd(
        spacingAnchorUs(),
        saturatingAdd(saturatingAdd(m_DisplayPeriodUs, m_GuardUs),
                      safetyHeadroomUs));
}

uint64_t VrrTimingController::lastSubmissionUs() const
{
    return m_LastSubmissionUs;
}

uint64_t VrrTimingController::spacingAnchorUs() const
{
    return m_Parameters.latchedFlipAnchor != 0 ?
        std::max(m_SpacingAnchorUs, m_LastSubmissionUs) : m_LastSubmissionUs;
}

size_t VrrTimingController::queuedFrameCapacity() const
{
    return m_Parameters.playoutQueueFrames != 0 ?
        static_cast<size_t>(std::min<uint64_t>(m_Parameters.playoutQueueFrames, VrrLargestQueuedFrames)) :
        VrrMaximumQueuedFrames;
}

uint64_t VrrTimingController::untornReferenceUs() const
{
    return m_LatchedPresentation ? m_LastSubmissionUs : spacingAnchorUs();
}

void VrrTimingController::noteNativeFlipProtection(uint64_t observedFlipUs)
{
    if (m_Parameters.nativeFlipProtection == 0) {
        return;
    }
    // Deliberately leaves m_LatchedPresentation (the planned decision and its
    // entry/exit hysteresis) alone; only the flip anchor learns the latch.
    m_NativeLatchPending = true;
    m_NativeLatchFlipUs = observedFlipUs;
}

bool VrrTimingController::hasLastSubmission() const
{
    return m_HaveLastSubmission;
}

int64_t VrrTimingController::playoutOffsetUs() const
{
    return m_AppliedPlayoutOffsetUs;
}

bool VrrTimingController::timestampPlayoutActive() const
{
    return m_TimestampPlayoutActive;
}

bool VrrTimingController::timestampPlayoutEnabled() const
{
    return m_Parameters.timestampPlayoutEnabled != 0;
}

void VrrTimingController::resetPlayoutOffsets()
{
    m_PlayoutOffsets.clear();
    m_PlayoutOffsetValid = false;
    m_AppliedPlayoutOffsetUs = 0;
    m_PlayoutSamplesSeen = 0;
    m_PlayoutOffsetClockValid = false;
    m_LastPlayoutOffsetObservationUs = 0;
    m_PlayoutOffsetSlewRemainder = 0;
    m_TimestampPlayoutActive = false;
    m_PlayoutOffsetConverging = false;
    ++m_PlayoutOffsetEpoch;
}

int64_t VrrTimingController::inputClockOffsetUs(uint64_t nowUs)
{
    if (!m_Parameters.playoutInputClockRelativeHistory ||
        !m_Parameters.normalizedSourceMapping) return 0;
    if (m_Parameters.playoutInputClockRelativeHistory == 1)
        return m_PlayoutOffsetValid ? m_AppliedPlayoutOffsetUs : 0;
    if (!m_PlayoutOffsetValid) return m_InputClockCoordinateUs;
    if (!m_InputClockValid) {
        m_InputClockValid = true;
        m_InputClockEpoch = m_PlayoutOffsetEpoch;
        m_InputClockLastOffsetUs = m_AppliedPlayoutOffsetUs;
        m_InputClockLastUs = m_InputDriftAnchorUs = nowUs;
        m_InputDriftAnchorRawUs = m_InputClockRawUs;
        if (m_Parameters.playoutInputClockRelativeHistory == 4)
            m_ReceiveDriftClock.advance(nowUs, m_ReceiveDriftPacketValid,
                m_ReceiveDriftPacketOffsetUs, m_SourcePeriodUs, m_PlayoutOffsetEpoch,
                m_Parameters.materialRateChangePercent);
        return m_InputClockCoordinateUs;
    }
    // A reseed is a sender clock jump, not movement of retained readiness:
    // the coordinate stays continuous and the slope bucket restarts.
    int64_t deltaUs = 0;
    if (m_InputClockEpoch != m_PlayoutOffsetEpoch) {
        m_InputClockEpoch = m_PlayoutOffsetEpoch;
        m_InputDriftAnchorUs = nowUs;
        m_InputDriftAnchorRawUs = m_InputClockRawUs;
    }
    else deltaUs = m_AppliedPlayoutOffsetUs - m_InputClockLastOffsetUs;
    m_InputClockLastOffsetUs = m_AppliedPlayoutOffsetUs;
    m_InputClockRawUs += deltaUs;
    m_InputClockCoordinateUs += deltaUs;
    if (nowUs <= m_InputClockLastUs) return m_InputClockCoordinateUs;

    if (m_Parameters.playoutInputClockRelativeHistory == 4) {
        m_InputClockLastUs = nowUs;
        m_InputClockCoordinateUs -= m_ReceiveDriftClock.advance(nowUs,
            m_ReceiveDriftPacketValid, m_ReceiveDriftPacketOffsetUs,
            m_SourcePeriodUs, m_PlayoutOffsetEpoch,
            m_Parameters.materialRateChangePercent);
        return m_InputClockCoordinateUs;
    }

    // Offset tracking follows host/client clock drift as well as mapping
    // movement. Drift moves future readiness by the same amount, so only the
    // movement it does not explain shifts retained requirements.
    if (m_Parameters.playoutInputClockRelativeHistory >= 3) {
        const uint64_t elapsedUs = std::min<uint64_t>(nowUs - m_InputClockLastUs, 60000000);
        m_InputClockLastUs = nowUs;
        m_InputClockDriftRemainder += m_InputDriftFitPpb * static_cast<int64_t>(elapsedUs);
        const int64_t driftUs = m_InputClockDriftRemainder / 1000000000;
        m_InputClockDriftRemainder -= driftUs * 1000000000;
        m_InputClockCoordinateUs -= driftUs;
        updateInputDriftFit(nowUs);
        return m_InputClockCoordinateUs;
    }
    // Mode 2 (2026-10-12 captures): median of per-second slopes. The applied
    // offset slews in bursts and is still on most seconds, so this median
    // reads zero drift on live sessions.
    constexpr int64_t kMaximumDriftPpb = 500000;
    constexpr int64_t kDriftBandPpb = 50000;
    constexpr size_t kMinimumDriftBuckets = 5;
    int64_t driftPpb = 0;
    if (m_InputDriftSlopeCount >= kMinimumDriftBuckets) {
        std::array<int64_t, 61> slopes{};
        std::copy_n(m_InputDriftSlopesPpb.begin(), m_InputDriftSlopeCount, slopes.begin());
        const auto end = slopes.begin() + m_InputDriftSlopeCount;
        const auto middle = slopes.begin() + m_InputDriftSlopeCount / 2;
        std::nth_element(slopes.begin(), middle, end);
        const int64_t median = *middle;
        int64_t sum = 0, count = 0;
        for (auto it = slopes.begin(); it != end; ++it) {
            if (*it >= median - kDriftBandPpb && *it <= median + kDriftBandPpb) {
                sum += *it;
                ++count;
            }
        }
        driftPpb = std::clamp(sum / count, -kMaximumDriftPpb, kMaximumDriftPpb);
    }
    const uint64_t elapsedUs = std::min<uint64_t>(nowUs - m_InputClockLastUs, 60000000);
    m_InputClockLastUs = nowUs;
    m_InputClockDriftRemainder += driftPpb * static_cast<int64_t>(elapsedUs);
    const int64_t driftUs = m_InputClockDriftRemainder / 1000000000;
    m_InputClockDriftRemainder -= driftUs * 1000000000;
    m_InputClockCoordinateUs -= driftUs;

    const uint64_t bucketUs = nowUs - m_InputDriftAnchorUs;
    if (bucketUs >= 1000000) {
        // A bucket spanning a stall measures the catch-up, not the rate.
        if (bucketUs <= 5000000) {
            m_InputDriftSlopesPpb[m_InputDriftSlopeIndex] =
                std::clamp<int64_t>(m_InputClockRawUs - m_InputDriftAnchorRawUs,
                    -5000000, 5000000) * 1000000000 / static_cast<int64_t>(bucketUs);
            m_InputDriftSlopeIndex = (m_InputDriftSlopeIndex + 1) % m_InputDriftSlopesPpb.size();
            m_InputDriftSlopeCount = std::min(m_InputDriftSlopeCount + 1, m_InputDriftSlopesPpb.size());
        }
        m_InputDriftAnchorUs = nowUs;
        m_InputDriftAnchorRawUs = m_InputClockRawUs;
    }
    return m_InputClockCoordinateUs;
}

void VrrTimingController::updateInputDriftFit(uint64_t nowUs)
{
    if (m_Parameters.playoutInputHitchMissesOnly >= 6) {
        const auto settling = [nowUs](uint64_t atUs) {
            return atUs && nowUs >= atUs && nowUs - atUs < kInputRateSettlingUs;
        };
        if (settling(m_InputRateRiseAtUs) || settling(m_InputRateDropAtUs)) {
            // A rate-dependent producer offset is a phase/baseline change,
            // not host/client clock drift. Never fit a line across that step.
            m_InputDriftPoints.clear();
            m_InputDriftLastFitUs = nowUs;
            m_InputDriftFitPpb = 0;
            return;
        }
    }
    // The applied offset is a windowed minimum: it holds, then slews in
    // bursts either way as early arrivals enter and leave the window. Only
    // its trend over a long baseline is clock drift. A least-squares line
    // over 120 s of 100 ms samples is refit once per second; the raw offset
    // stays continuous across reseeds, so a sender clock jump is no slope.
    constexpr uint64_t kPointIntervalUs = 100000;
    constexpr uint64_t kFitWindowUs = 120000000;
    constexpr uint64_t kMinimumFitSpanUs = 15000000;
    constexpr size_t kMinimumFitPoints = 50;
    constexpr double kMaximumDriftPpb = 500000.0;
    if (m_InputDriftPoints.empty() || nowUs - m_InputDriftPoints.back().atUs >= kPointIntervalUs)
        m_InputDriftPoints.push_back({nowUs, m_InputClockRawUs});
    while (nowUs - m_InputDriftPoints.front().atUs > kFitWindowUs)
        m_InputDriftPoints.pop_front();
    if (m_InputDriftLastFitUs && nowUs - m_InputDriftLastFitUs < 1000000) return;
    m_InputDriftLastFitUs = nowUs;
    const auto& first = m_InputDriftPoints.front();
    if (m_InputDriftPoints.size() < kMinimumFitPoints ||
        m_InputDriftPoints.back().atUs - first.atUs < kMinimumFitSpanUs) return;
    double meanX = 0, meanY = 0;
    for (const auto& point : m_InputDriftPoints) {
        meanX += double(point.atUs - first.atUs) / 1e6;
        meanY += double(point.rawUs - first.rawUs);
    }
    meanX /= double(m_InputDriftPoints.size());
    meanY /= double(m_InputDriftPoints.size());
    double sxx = 0, sxy = 0;
    for (const auto& point : m_InputDriftPoints) {
        const double x = double(point.atUs - first.atUs) / 1e6 - meanX;
        sxx += x * x;
        sxy += x * (double(point.rawUs - first.rawUs) - meanY);
    }
    // Microseconds per second is ppm; store ppb for sub-ppm integration.
    if (sxx > 0)
        m_InputDriftFitPpb = static_cast<int64_t>(std::llround(
            std::clamp(sxy / sxx * 1000.0, -kMaximumDriftPpb, kMaximumDriftPpb)));
}

int64_t VrrTimingController::observePlayoutOffset(uint64_t observationUs,
                                                int64_t offsetUs,
                                                bool cadenceEligible,
                                                bool phaseDiscontinuity)
{
    const bool timeBased = m_Parameters.playoutOffsetSlewUsPerSecond != 0;
    const bool clockReversed = timeBased && m_PlayoutOffsetClockValid &&
        observationUs < m_LastPlayoutOffsetObservationUs;
    const bool breakWindow = m_Parameters.playoutOffsetCadenceGate != 0 &&
        phaseDiscontinuity;
    if (m_PlayoutOffsetValid && (breakWindow || clockReversed)) {
        // A minimum from the previous source phase is not evidence that the
        // new phase can be presented earlier. Discard observations, not the
        // applied mapping or the interval buffer. End any remaining warmup:
        // adopting a new minimum immediately would introduce a phase jump.
        m_PlayoutOffsets.clear();
        m_PlayoutSamplesSeen = std::max<uint64_t>(
            m_PlayoutSamplesSeen, m_Parameters.playoutOffsetWarmupSamples);
        m_PlayoutOffsetSlewRemainder = 0;
    }

    uint64_t allowedSlewUs = m_Parameters.playoutOffsetSlewUs;
    if (timeBased) {
        allowedSlewUs = 0;
        if (m_PlayoutOffsetClockValid &&
            observationUs > m_LastPlayoutOffsetObservationUs) {
            // Clamp both factors before multiplying, even for direct callers
            // that bypass replay parameter validation. Keep fractional credit
            // so high FPS cannot round a small correction rate down to zero.
            const uint64_t elapsedUs = std::min<uint64_t>(
                observationUs - m_LastPlayoutOffsetObservationUs, 1000000);
            const uint64_t rate = std::min<uint64_t>(
                m_Parameters.playoutOffsetSlewUsPerSecond, 1000000);
            const uint64_t credit = elapsedUs * rate +
                m_PlayoutOffsetSlewRemainder;
            const uint64_t maximumStepUs = std::min<uint64_t>(
                m_Parameters.playoutOffsetMaximumStepUs, 1000000);
            allowedSlewUs = std::min(credit / 1000000, maximumStepUs);
            // A stall buys at most one bounded step, never future catch-up
            // debt. Unused whole microseconds are deliberately not banked.
            m_PlayoutOffsetSlewRemainder = allowedSlewUs < maximumStepUs ?
                credit % 1000000 : 0;
        }
        else {
            m_PlayoutOffsetSlewRemainder = 0;
        }
        m_LastPlayoutOffsetObservationUs = observationUs;
        m_PlayoutOffsetClockValid = true;
    }

    if (m_Parameters.playoutOffsetCadenceGate != 0 &&
        m_PlayoutOffsetValid && (!cadenceEligible || phaseDiscontinuity)) {
        // Seed a genuine epoch once, but never learn its floor from an
        // ineligible transition frame. Rejected samples cannot earn slew
        // credit while the source is stalled or the cadence is provisional.
        m_PlayoutOffsetSlewRemainder = 0;
        return m_AppliedPlayoutOffsetUs;
    }
    // Bound the window by both age and count so a pathological configuration
    // cannot grow the deque without limit.
    constexpr size_t kMaximumPlayoutOffsetSamples = 4096;
    m_PlayoutOffsets.push_back(PlayoutOffsetSample { observationUs, offsetUs });
    const uint64_t windowUs = m_Parameters.playoutOffsetWindowUs;
    while (m_PlayoutOffsets.size() > 1 &&
           (m_PlayoutOffsets.size() > kMaximumPlayoutOffsetSamples ||
            (observationUs > windowUs &&
             m_PlayoutOffsets.front().observationUs <
                 observationUs - windowUs))) {
        m_PlayoutOffsets.pop_front();
    }

    int64_t windowMinimumUs = std::numeric_limits<int64_t>::max();
    for (const PlayoutOffsetSample& sample : m_PlayoutOffsets) {
        windowMinimumUs = std::min(windowMinimumUs, sample.offsetUs);
    }

    ++m_PlayoutSamplesSeen;
    if (!m_PlayoutOffsetValid) {
        m_AppliedPlayoutOffsetUs = offsetUs;
        m_PlayoutOffsetValid = true;
    }
    else if (m_PlayoutSamplesSeen <= m_Parameters.playoutOffsetWarmupSamples) {
        // The first frame after an epoch is an arbitrary arrival. While the
        // window is still filling, adopt an earlier arrival immediately so
        // startup latency converges within a few frames instead of paying
        // the slew rate for the first several seconds.
        if (windowMinimumUs < m_AppliedPlayoutOffsetUs) {
            m_AppliedPlayoutOffsetUs = windowMinimumUs;
        }
    }
    else {
        // Steady state: track the earliest arrival in the window at a rate
        // far above clock drift but far below anything visible per frame.
        const int64_t slewUs = static_cast<int64_t>(std::min<uint64_t>(
            allowedSlewUs,
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
        if (windowMinimumUs > m_AppliedPlayoutOffsetUs) {
            m_AppliedPlayoutOffsetUs += std::min(
                slewUs, windowMinimumUs - m_AppliedPlayoutOffsetUs);
        }
        else if (windowMinimumUs < m_AppliedPlayoutOffsetUs) {
            m_AppliedPlayoutOffsetUs -= std::min(
                slewUs, m_AppliedPlayoutOffsetUs - windowMinimumUs);
        }
    }
    if (timeBased && (m_PlayoutSamplesSeen <= m_Parameters.playoutOffsetWarmupSamples ||
                      m_AppliedPlayoutOffsetUs == windowMinimumUs)) {
        m_PlayoutOffsetSlewRemainder = 0;
    }
    m_PlayoutOffsetConverging = m_AppliedPlayoutOffsetUs != windowMinimumUs;
    return m_AppliedPlayoutOffsetUs;
}

uint64_t VrrTimingController::playoutDelayUs() const
{
    const uint64_t delayUs = m_TimestampPlayoutActive ? effectivePlayoutDelayUs() :
                                                      m_Parameters.sourcePlayoutDelayUs;
    return m_Parameters.playoutInputHitchMissesOnly >= 9 ? delayUs :
        std::min(delayUs, playoutDelayCapUs());
}

namespace {
// A rate within 15% of a layer's anchor shares its evidence: 100 and 116 FPS
// share a layer, 60 and 120 FPS do not.
constexpr uint64_t kInputLayerSpreadPerMille = 150;
// A layer that saw fewer eligible frames while active was a rate the detector
// passed through during a change; it is discarded rather than stored.
constexpr uint64_t kInputLayerMinimumFrames = 30;
constexpr size_t kInputLayersStored = 8;
}

uint64_t VrrTimingController::inputHitchPolicy(uint64_t nowUs) const
{
    if (m_Parameters.playoutInputHitchMissesOnly != 4)
        return m_Parameters.playoutInputHitchMissesOnly;
    // Recorded policy 4: a faster source can leave the mapping several ms behind while its
    // minimum window and slew settle. Compensate that transition, not ordinary
    // steady-state mapping fluctuations: those keep the proven miss policy's
    // release behavior. A slow layer must first have its own qualified history.
    return m_InputRateRiseAtUs && nowUs >= m_InputRateRiseAtUs &&
        nowUs - m_InputRateRiseAtUs < kInputRateSettlingUs ? 4 : 2;
}

VrrTimingController::InputLayerChange VrrTimingController::updateInputLayer(
    uint64_t nowUs, const CadenceObservation& cadence)
{
    const bool confirmedLayers = m_Parameters.playoutInputHitchMissesOnly >= 8;
    const uint64_t periodUs = confirmedLayers ? cadence.confirmedInputLayerPeriodUs : m_SourcePeriodUs;
    const auto distance = [periodUs](uint64_t anchorUs) {
        return periodUs > anchorUs ? periodUs - anchorUs : anchorUs - periodUs;
    };
    const bool fastLayers = m_Parameters.playoutInputHitchMissesOnly >= 7;
    const auto within = [this, &distance, periodUs, fastLayers, confirmedLayers](uint64_t anchorUs) {
        if (confirmedLayers) return Vrr13::InputLayerCadence::sameOperatingRate(periodUs, anchorUs,
            m_Parameters.playoutInputHitchMissesOnly >= 9 ? 30 : 0);
        if (fastLayers) return Vrr13::InputLayerCadence::sameLayer(periodUs, anchorUs);
        return distance(anchorUs) * 1000 <= anchorUs * kInputLayerSpreadPerMille;
    };
    if (periodUs == 0) return InputLayerChange::None;
    if (m_InputLayerPeriodUs == 0) {
        m_InputLayerPeriodUs = periodUs;
        m_InputLayerIdentity = ++m_NextInputLayerIdentity;
        ++m_InputLayerCreates;
        return InputLayerChange::None;
    }
    if (within(m_InputLayerPeriodUs)) return InputLayerChange::None;

    // Layers whose evidence has aged out of their history are forgotten.
    for (auto it = m_InputLayers.begin(); it != m_InputLayers.end();) {
        const auto stats = it->buffer.stats();
        const uint64_t lastUs = m_Parameters.playoutInputLayerLifecycle ?
            it->buffer.lastEligibleObservationUs() : stats.lastInputObservationUs;
        if (!lastUs || (nowUs > lastUs && nowUs - lastUs >= stats.windowUs)) {
            ++m_InputLayerExpirations;
            it = m_InputLayers.erase(it);
        }
        else ++it;
    }
    auto stored = m_InputLayers.end();
    for (auto it = m_InputLayers.begin(); it != m_InputLayers.end(); ++it) {
        if (within(it->periodUs) && (stored == m_InputLayers.end() ||
                distance(it->periodUs) < distance(stored->periodUs))) stored = it;
    }
    const bool restored = stored != m_InputLayers.end();
    Vrr13::InputPercentileBuffer next;
    uint64_t nextPeriodUs = periodUs;
    uint64_t nextIdentity = 0;
    if (restored) {
        next = std::move(stored->buffer);
        nextPeriodUs = stored->periodUs;
        nextIdentity = stored->identity;
        m_InputLayers.erase(stored);
        next.continueTotalsFrom(m_InputPercentileBuffer);
        if (confirmedLayers) next.breakAdjacency();
    }
    else if (m_Parameters.playoutInputLayerSeedRatioPerMille == 0 ||
             m_InputLayerPeriodUs * 1000 <= periodUs * m_Parameters.playoutInputLayerSeedRatioPerMille) {
        next = m_InputPercentileBuffer;
        next.markEvidenceAsSeed();
    }
    else {
        // Lateness learned at a much slower rate (a loading screen, a menu)
        // says nothing about the faster one; start from fresh evidence.
        next.continueTotalsFrom(m_InputPercentileBuffer);
        if (m_Parameters.playoutInputLayerLifecycle >= 2)
            next.beginFreshLayerReacquisition(nowUs,
                m_Parameters.playoutInputPercentileTargetPerMillion,
                m_Parameters.playoutInputPercentileWindowUs);
    }
    const bool settled = m_InputPercentileBuffer.stats().eligible >=
        m_InputLayerEligibleAtStart + (fastLayers ? 8 : kInputLayerMinimumFrames);
    if (settled) {
        m_InputSettledLayerPeriodUs = m_InputLayerPeriodUs;
        if (m_InputLayers.size() >= kInputLayersStored) {
            m_InputLayers.erase(std::min_element(m_InputLayers.begin(), m_InputLayers.end(),
                [](const InputLayer& a, const InputLayer& b) {
                    return a.buffer.stats().lastInputObservationUs < b.buffer.stats().lastInputObservationUs;
                }));
        }
        m_InputPercentileBuffer.prepareForLayerStorage();
        m_InputLayers.push_back({m_InputLayerPeriodUs, std::move(m_InputPercentileBuffer), m_InputLayerIdentity});
    }
    m_InputPercentileBuffer = std::move(next);
    m_InputLayerPeriodUs = nextPeriodUs;
    ++m_InputLayerSwitches;
    if (restored) ++m_InputLayerRestores;
    else { nextIdentity = ++m_NextInputLayerIdentity; ++m_InputLayerCreates; }
    m_InputLayerIdentity = nextIdentity;
    // A restored layer already earned retention. A short visit must not throw
    // away its trusted queue just because it has not collected eight new frames.
    m_InputLayerEligibleAtStart = fastLayers && restored ? 0 :
        m_InputPercentileBuffer.stats().eligible;
    return restored ? InputLayerChange::Restored : InputLayerChange::Created;
}

VrrBufferAuditSnapshot VrrTimingController::bufferAudit(uint64_t nowUs, uint64_t appliedUs) const
{
    VrrBufferAuditSnapshot a;
    a.atUs = nowUs;
    const auto s = m_InputPercentileBuffer.stats();
    a.appliedUs=appliedUs; a.targetUs=s.targetUs; a.currentUs=s.currentUs;
    a.layerId=m_InputLayerIdentity; a.layerPeriodUs=m_InputLayerPeriodUs;
    a.layerSwitches=m_InputLayerSwitches; a.layerCreates=m_InputLayerCreates;
    a.layerRestores=m_InputLayerRestores; a.layerExpirations=m_InputLayerExpirations;
    a.cachedLayers=m_InputLayers.size(); a.lastEligibleUs=m_InputPercentileBuffer.lastEligibleObservationUs();
    a.lastObservationUs=s.lastInputObservationUs;
    a.eligibleAgeUs=a.lastEligibleUs && nowUs>=a.lastEligibleUs ? nowUs-a.lastEligibleUs : 0;
    a.samples=s.samples; a.eligible=s.eligible; a.missed=s.missed;
    a.windowUs=s.windowUs; a.probabilityPpm=s.probabilityPpm; a.toleranceUs=s.toleranceUs;
    a.minimumUs=s.minimumUs; a.maximumUs=s.maximumUs; a.vrrHeadroomUs=s.vrrHeadroomUs;
    a.rawQuantileUs=s.rawRequiredQuantileUs; a.unclippedTargetUs=s.headroomTargetUnclippedUs;
    a.readinessBudgetUs=s.readinessBudgetUs; a.clockOffsetUs=s.clockOffsetUs;
    a.reacquisitionRemaining=m_InputPercentileBuffer.reacquisitionRemaining();
    a.reacquisitionUntilUs=m_InputPercentileBuffer.reacquisitionUntilUs();
    a.feedbackFrame=m_InputAuditFeedbackFrame; a.eligibilityReasons=m_InputAuditEligibilityReasons;
    const auto slew=m_InputPercentileBuffer.lastSlewAudit();
    a.slewAtUs=slew.atUs; a.slewAuthorizedTargetUs=slew.authorizedTargetUs;
    a.slewBeforeUs=slew.beforeUs; a.slewDeltaUs=slew.deltaUs;
    a.flags=(m_InputPercentileBuffer.learning()?VrrBufferAuditSnapshot::Learning:0) |
        (s.tailUncertain?VrrBufferAuditSnapshot::TailUncertain:0) |
        (s.evidenceValid?VrrBufferAuditSnapshot::EvidenceValid:0) |
        (s.clipped?VrrBufferAuditSnapshot::Clipped:0) |
        (s.expiredUnknown?VrrBufferAuditSnapshot::ExpiredUnknown:0) |
        (s.growthBlocked?VrrBufferAuditSnapshot::GrowthBlocked:0) |
        (m_InputPercentileBuffer.layerTransitionActive()?VrrBufferAuditSnapshot::LayerTransition:0) |
        (m_InputPercentileBuffer.fastLayerDrain()?VrrBufferAuditSnapshot::FastDrain:0) |
        (a.reacquisitionRemaining && nowUs<a.reacquisitionUntilUs?VrrBufferAuditSnapshot::Reacquiring:0) |
        (s.lastInputEligible?VrrBufferAuditSnapshot::LastInputEligible:0);
    return a;
}

unsigned int VrrTimingController::playoutBandIndex() const
{
    return m_PlayoutBandValid ? m_PlayoutBandIndex : 0;
}

uint64_t VrrTimingController::playoutBandSamples() const
{
    if (m_Parameters.playoutHistoryEnabled != 0) return m_PlayoutHistory.evidence();
    if (!m_PlayoutBandValid) {
        return 0;
    }
    const auto band = m_PlayoutBands.find(m_PlayoutBandIndex);
    return band != m_PlayoutBands.end() ? band->second.samplesSeen : 0;
}

uint64_t VrrTimingController::effectivePlayoutDelayUs() const
{
    if (m_Parameters.playoutDelayAdaptive != 0) {
        // Before the first band opens, the delay the band will open with.
        const uint64_t delayUs = m_PlayoutBandValid ? m_AppliedPlayoutDelayUs :
                                                    playoutDelayStartUs();
        return m_Parameters.playoutInputHitchMissesOnly >= 9 ? delayUs :
            std::min(delayUs, playoutDelayCapUs());
    }
    return m_Parameters.sourcePlayoutDelayUs;
}

void VrrTimingController::noteReadinessFloorSample(uint64_t submissionUs,
                                                  int64_t readyOffsetUs)
{
    m_ReadinessFloorSamples.emplace_back(submissionUs, readyOffsetUs);
    const uint64_t windowUs = m_Parameters.playoutReadinessFloorWindowUs;
    while (!m_ReadinessFloorSamples.empty() &&
           (submissionUs < m_ReadinessFloorSamples.front().first ||
            submissionUs - m_ReadinessFloorSamples.front().first > windowUs)) {
        m_ReadinessFloorSamples.pop_front();
    }
    // Ten updates per second bound the selection cost at high frame rates.
    if (m_ReadinessFloorUpdatedUs != 0 &&
            submissionUs >= m_ReadinessFloorUpdatedUs &&
            submissionUs - m_ReadinessFloorUpdatedUs < 100000) {
        return;
    }
    m_ReadinessFloorUpdatedUs = submissionUs;
    if (m_ReadinessFloorSamples.size() < 32) {
        m_ReadinessFloorUs = 0;
        return;
    }
    std::vector<int64_t> offsets;
    offsets.reserve(m_ReadinessFloorSamples.size());
    for (const auto& sample : m_ReadinessFloorSamples) {
        offsets.push_back(sample.second);
    }
    const size_t index = std::min(offsets.size() - 1,
        static_cast<size_t>(offsets.size() *
            std::min<uint64_t>(m_Parameters.playoutReadinessFloorPerMille, 1000) / 1000));
    std::nth_element(offsets.begin(), offsets.begin() + index, offsets.end());
    m_ReadinessFloorUs = offsets[index] > 0 ? static_cast<uint64_t>(offsets[index]) : 0;
}

uint64_t VrrTimingController::playoutDelayMinimumUs() const
{
    uint64_t minimumUs = std::min(std::max(m_Parameters.playoutDelayMinimumUs,
                                           m_ReadinessFloorUs),
                                  playoutDelayCapUs());
    return m_Parameters.playoutHistoryEnabled != 0 ?
        std::min(minimumUs, playoutQueueLimitUs()) : minimumUs;
}

uint64_t VrrTimingController::scaledPerMille(uint64_t value,
                                             uint64_t perMille)
{
    const uint64_t whole = value / 1000;
    const uint64_t remainder = value % 1000;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    if (perMille != 0 && whole > maximum / perMille) {
        return maximum;
    }
    return whole * perMille + remainder * perMille / 1000;
}

void VrrTimingController::noteIntervalBufferEpoch(uint64_t atUs)
{
    if (m_SourcePeriodUs == 0) {
        return;
    }
    const uint64_t rateMilliHz = kMicrosecondsPerSecond * 1000 / m_SourcePeriodUs;
    if (m_EpochRateMilliHz == 0) {
        m_EpochRateMilliHz = rateMilliHz;
        m_EpochSinceUs = atUs;
        return;
    }
    const uint64_t ratio = m_Parameters.playoutEpochRateRatioPerMille;
    const auto departs = [ratio](uint64_t a, uint64_t b) {
        return a * 1000 >= b * ratio || b * 1000 >= a * ratio;
    };
    if (!departs(rateMilliHz, m_EpochRateMilliHz)) {
        m_EpochCandidateMilliHz = 0;
        return;
    }
    // A hitch can briefly re-fit the source period. Require the new rate to
    // persist before treating it as a different source.
    if (m_EpochCandidateMilliHz == 0 || departs(rateMilliHz, m_EpochCandidateMilliHz)) {
        m_EpochCandidateMilliHz = rateMilliHz;
        m_EpochCandidateSinceUs = atUs;
    }
    if (atUs < m_EpochCandidateSinceUs ||
            atUs - m_EpochCandidateSinceUs < m_Parameters.playoutEpochConfirmUs) {
        return;
    }
    m_EpochCandidateMilliHz = 0;
    // Quarter-octave buckets: nearby rates of one source share a demand.
    const auto bucket = [](uint64_t milliHz) {
        return static_cast<int>(std::lround(std::log2(double(milliHz) / 1000.0) * 4.0));
    };
    const uint64_t current = m_IntervalBuffer.demand(m_AppliedPlayoutDelayUs);
    // Only a demand learned over a sustained epoch describes that source.
    if (atUs >= m_EpochSinceUs && atUs - m_EpochSinceUs >= m_Parameters.playoutEpochSustainUs) {
        const int outgoing = bucket(m_EpochRateMilliHz);
        auto it = std::find_if(m_EpochDemands.begin(), m_EpochDemands.end(),
                               [outgoing](const auto& entry) { return entry.first == outgoing; });
        if (it != m_EpochDemands.end()) it->second = current;
        else m_EpochDemands.emplace_back(outgoing, current);
    }
    const int incoming = bucket(rateMilliHz);
    const std::pair<int, uint64_t>* nearest = nullptr;
    for (const auto& entry : m_EpochDemands) {
        if (std::abs(entry.first - incoming) <= 1 &&
                (!nearest || std::abs(entry.first - incoming) < std::abs(nearest->first - incoming))) {
            nearest = &entry;
        }
    }
    // Delay bought for another source rate is not evidence for this one.
    // Restore only downward; an unfamiliar rate keeps the current protection.
    if (nearest && nearest->second < current) {
        m_IntervalBuffer.restoreTarget(nearest->second, atUs);
    }
    m_EpochRateMilliHz = rateMilliHz;
    m_EpochSinceUs = atUs;
}

uint64_t VrrTimingController::playoutDelayMaximumUs() const
{
    uint64_t maximumUs = std::max(m_Parameters.playoutDelayMaximumUs,
                                  m_Parameters.playoutDelayMinimumUs);
    if (m_Parameters.playoutDelayMaximumPeriodPerMille != 0) {
        maximumUs = std::max(
            maximumUs,
            scaledPerMille(m_SourcePeriodUs,
                           m_Parameters.playoutDelayMaximumPeriodPerMille));
    }
    maximumUs = std::min(maximumUs, playoutDelayCapUs());
    return m_Parameters.playoutHistoryEnabled != 0 ?
        std::min(maximumUs, playoutQueueLimitUs()) : maximumUs;
}

uint64_t VrrTimingController::latencyFixDelayLimitUs() const
{
    return scaledPerMille(m_DisplayPeriodUs, m_Parameters.latencyFixDelayPeriodPerMille);
}


uint64_t VrrTimingController::playoutDelayCapUs() const
{
    if (m_Parameters.playoutDelayCapSourcePeriodPerMille != 0) {
        const uint64_t capPeriodUs =
            m_Parameters.playoutDelayCapUsesObservedPeriod != 0 ?
                m_SourcePeriodUs :
                (m_Parameters.playoutResponsiveBuffer ?
                     m_ConfiguredStreamPeriodUs : m_SourcePeriodUs);
        return scaledPerMille(
            capPeriodUs,
            m_Parameters.playoutDelayCapSourcePeriodPerMille);
    }
    // Parameterized captures predating the source-frame preset cap retain the
    // old display-relative limiter for exact replay.
    return m_LatencyFixActive ? latencyFixDelayLimitUs() :
        std::numeric_limits<uint64_t>::max();
}

uint64_t VrrTimingController::gpuReadinessCeilingUs() const
{
    if (m_Parameters.playoutGpuReadinessAdaptation == 0) {
        return 0;
    }
    return std::min(
        std::max<uint64_t>(1, m_Parameters.playoutGpuReadinessMaximumUs),
        std::max<uint64_t>(1, m_SourcePeriodUs));
}

void VrrTimingController::updateLatencyFixState()
{
    const int enterHz = VrrRatePolicy::protectedRateForRefresh(m_Config.displayRefreshHz);
    // Reuse the near-ceiling boundary (116 at 120 Hz). Leave only once half
    // that headroom again is available (114 Hz), so small fitted-rate changes
    // cannot repeatedly compress and refill the buffer.
    const int exitHz = std::max(1, enterHz -
        std::max(1, (m_Config.displayRefreshHz - enterHz) / 2));
    const bool active = m_Parameters.latencyFixEnabled != 0 &&
        m_Config.displayRefreshHz > 0 && m_SourcePeriodUs > 0 &&
        (m_Parameters.latencyFixAllRates != 0 ||
         (enterHz > 0 && m_SourcePeriodUs <= periodForRate(m_LatencyFixActive ? exitHz : enterHz, 0)));
    if (m_LatencyFixActive && !active) {
        // Do not resurrect a near-ceiling hitch's rejected padding demand
        // when the source leaves this mode. Fresh lower-rate misses can still
        // acquire protection through the ordinary feedback path.
        m_RequestedPlayoutDelayUs = std::min(m_RequestedPlayoutDelayUs, latencyFixDelayLimitUs());
        m_UnclampedRequestedPlayoutDelayUs = m_RequestedPlayoutDelayUs;
    }
    m_LatencyFixActive = active;
}

uint64_t VrrTimingController::playoutQueueLimitUs() const
{
    // One frame is active, four (captures that recorded no size: three) can
    // wait, and the next arrival needs a slot. Use the faster of fitted and
    // negotiated cadence during rate transitions.
    const uint64_t period = std::min(m_SourcePeriodUs, m_ConfiguredStreamPeriodUs);
    const uint64_t capacity = period * queuedFrameCapacity();
    const uint64_t work = saturatingAdd(m_RenderLeadUs, m_Parameters.presentationSafetyUs);
    const uint64_t smoothing = m_Parameters.playoutSmoothingGainPerMille != 0 ?
        (m_Parameters.playoutSmoothingWindowedCadence == 7 ? absorbReserveUs() :
         saturatingAdd(smoothingPhaseLimitUs(), absorbReserveUs())) : 0;
    const uint64_t occupied = saturatingAdd(work, smoothing);
    return capacity > occupied ? capacity - occupied : 0;
}

uint64_t VrrTimingController::playoutDelayStartUs() const
{
    if (m_Parameters.playoutDelayStartSeedUs != 0) {
        return clampUnsigned(m_Parameters.playoutDelayStartSeedUs,
                             playoutDelayMinimumUs(), playoutDelayMaximumUs());
    }
    uint64_t startUs = m_Parameters.playoutDelayStartUs != 0 ?
        m_Parameters.playoutDelayStartUs : m_Parameters.sourcePlayoutDelayUs;
    if (m_Parameters.playoutDelayStartPeriodPerMille != 0) {
        startUs = std::max(
            startUs,
            scaledPerMille(m_SourcePeriodUs,
                           m_Parameters.playoutDelayStartPeriodPerMille));
    }
    if (m_Parameters.playoutHistoryEnabled != 0) {
        // Only cold-start padding scales with display/work cost. Do not subtract
        // inter-frame headroom here: the learned target credits it exactly once.
        startUs = std::min(startUs, std::max(m_DisplayPeriodUs, m_RenderLeadUs));
    }
    return clampUnsigned(startUs, playoutDelayMinimumUs(),
                         playoutDelayMaximumUs());
}

void VrrTimingController::updatePlayoutDelay(
    const PacedFrame& frame, const CadenceObservation& cadence,
    bool rebased, int64_t readyOffsetUs, uint64_t nowUs)
{
    if (m_Parameters.playoutDelayAdaptive == 0) {
        m_PlayoutBandValid = false;
        return;
    }

    if (m_Parameters.playoutHistoryEnabled != 0) {
        updatePlayoutHistory(frame, cadence, rebased, readyOffsetUs, nowUs);
        return;
    }

    // Band selection from the fitted source rate, with hysteresis so a rate
    // hovering at a band edge does not bounce between two reservoirs.
    const uint64_t rateHz = m_SourcePeriodUs != 0 ?
        (kMicrosecondsPerSecond + m_SourcePeriodUs / 2) / m_SourcePeriodUs : 0;
    const uint64_t widthHz = std::max<uint64_t>(
        1, m_Parameters.playoutBandWidthHz);
    const unsigned int candidate = static_cast<unsigned int>(
        std::min<uint64_t>(rateHz / widthHz,
                           std::numeric_limits<unsigned int>::max()));
    bool switched = false;
    if (!m_PlayoutBandValid) {
        m_PlayoutBandIndex = candidate;
        m_PlayoutBandValid = true;
        switched = true;
    }
    else if (candidate != m_PlayoutBandIndex) {
        const uint64_t lowEdgeHz = static_cast<uint64_t>(candidate) * widthHz;
        const uint64_t highEdgeHz = lowEdgeHz + widthHz - 1;
        const uint64_t hysteresisHz = std::max<uint64_t>(1, widthHz / 6);
        const bool confirmed = candidate > m_PlayoutBandIndex ?
            rateHz >= lowEdgeHz + hysteresisHz :
            rateHz + hysteresisHz <= highEdgeHz;
        if (confirmed) {
            m_PlayoutBandIndex = candidate;
            switched = true;
        }
    }

    PlayoutBand& band = m_PlayoutBands[m_PlayoutBandIndex];
    if (switched) {
        // A band not visited for a long time re-converges from the start
        // value instead of trusting a stale distribution.
        if (band.applied && m_Parameters.playoutBandStaleUs != 0 &&
                nowUs > band.lastUsedUs &&
                nowUs - band.lastUsedUs > m_Parameters.playoutBandStaleUs) {
            band = PlayoutBand {};
        }
        if (!band.applied) {
            band.appliedDelayUs = playoutDelayStartUs();
            band.applied = true;
        }
    }
    band.lastUsedUs = nowUs;

    // Admit this frame's lateness against the mapped sender clock. The
    // statistic is exogenous: the delay we choose never changes it, so there
    // is no feedback loop. Pairs spanning a host stall are excluded.
    const uint64_t mappingUs = sourceMappingUs(frame);
    const bool steadyArrival = m_HaveLastDecodeComplete &&
        mappingUs >= m_LastDecodeCompleteUs &&
        mappingUs - m_LastDecodeCompleteUs <=
            m_Parameters.playoutStallExclusionUs;
    // A sender interval well below the fitted period is a host burst: frames
    // captured back-to-back after a capture stall. They arrive spaced by
    // encode time, so their lateness against the mapping is an artifact of
    // the burst, not of the network or decoder, and it would only inflate the
    // delay for every normal frame.
    const uint64_t burstFloorUs = m_SourcePeriodUs / 1000 *
        std::min<uint64_t>(1000, m_Parameters.playoutBurstExclusionPerMille) +
        (m_SourcePeriodUs % 1000) *
        std::min<uint64_t>(1000, m_Parameters.playoutBurstExclusionPerMille) /
        1000;
    const bool steadySender = cadence.intervalUs != 0 &&
        cadence.intervalUs <= m_Parameters.playoutStallExclusionUs &&
        cadence.intervalUs >= burstFloorUs;
    // The frames that arrive bunched behind an arrival stall carry the
    // stall's backlog as lateness, not the link's jitter. Excluding only the
    // first of them let one hiccup pin the delay at its cap for the life of
    // the reservoir.
    bool burstExcluded = false;
    if (!steadyArrival) {
        if (m_Parameters.playoutStallBurstExclusion != 0 &&
                m_HaveLastDecodeComplete &&
                mappingUs >= m_LastDecodeCompleteUs) {
            const uint64_t gapUs = mappingUs -
                m_LastDecodeCompleteUs;
            m_BurstExclusionFrames = gapUs /
                std::max<uint64_t>(1, m_SourcePeriodUs);
        }
    }
    else if (m_BurstExclusionFrames != 0) {
        --m_BurstExclusionFrames;
        burstExcluded = true;
    }
    if (!rebased && !switched && cadence.eligible &&
            !cadence.phaseDiscontinuity && steadyArrival && steadySender &&
            !burstExcluded) {
        const uint64_t latenessUs = readyOffsetUs > 0 ?
            static_cast<uint64_t>(readyOffsetUs) : 0;
        const size_t capacity = std::max<size_t>(
            1, m_Parameters.playoutDelayReservoirSamples);
        if (band.latenessUs.size() < capacity) {
            band.latenessUs.push_back(latenessUs);
        }
        else {
            band.latenessUs[band.nextIndex % capacity] = latenessUs;
        }
        band.nextIndex = (band.nextIndex + 1) % capacity;
        ++band.samplesSeen;
    }

    // Desired delay: the configured lateness percentile plus a margin, once
    // the band has enough samples to estimate it.
    uint64_t desiredUs = band.appliedDelayUs;
    const size_t minimumSamples = std::max<size_t>(
        1, m_Parameters.playoutDelayMinimumSamples);
    if (band.latenessUs.size() >= minimumSamples) {
        std::vector<uint64_t> ordered(band.latenessUs.begin(),
                                      band.latenessUs.end());
        const uint64_t perMille = std::min<uint64_t>(
            1000, m_Parameters.playoutDelayPercentilePerMille);
        const size_t rank = std::max<size_t>(
            1, static_cast<size_t>(
                   (static_cast<uint64_t>(ordered.size()) * perMille + 999) /
                   1000));
        std::nth_element(ordered.begin(), ordered.begin() + (rank - 1),
                         ordered.end());
        // A frame later than the delay by less than the tolerance is a
        // sub-threshold stretch, not a hitch, so the delay only needs to
        // cover lateness beyond the tolerance.
        const uint64_t coveredUs = saturatingAdd(
            ordered[rank - 1], m_Parameters.playoutDelayMarginUs);
        desiredUs = clampUnsigned(
            coveredUs > m_Parameters.playoutDelayToleranceUs ?
                coveredUs - m_Parameters.playoutDelayToleranceUs : 0,
            playoutDelayMinimumUs(), playoutDelayMaximumUs());
    }

    // Attack quickly, release slowly, and never release below the start
    // value until the band has seen enough samples to trust its tail.
    if (desiredUs > band.appliedDelayUs) {
        band.appliedDelayUs += std::min(
            desiredUs - band.appliedDelayUs,
            m_Parameters.playoutDelayAttackUs);
    }
    else if (desiredUs < band.appliedDelayUs &&
             band.samplesSeen >= m_Parameters.playoutDelayReleaseSamples) {
        band.appliedDelayUs -= std::min(
            band.appliedDelayUs - desiredUs,
            m_Parameters.playoutDelayReleaseUs);
    }
    // A cap that shrank with the fitted period is approached at the release
    // rate rather than in one step, so the presented slot never jumps.
    const uint64_t maximumUs = playoutDelayMaximumUs();
    if (band.appliedDelayUs > maximumUs) {
        band.appliedDelayUs -= std::min(
            band.appliedDelayUs - maximumUs,
            std::max<uint64_t>(1, m_Parameters.playoutDelayReleaseUs));
    }
    band.appliedDelayUs = std::max(band.appliedDelayUs,
                                   playoutDelayMinimumUs());
    if (m_Parameters.playoutDelaySlewAcrossBands == 0 ||
            !m_AppliedPlayoutDelayValid) {
        m_AppliedPlayoutDelayUs = band.appliedDelayUs;
    }
    else if (band.appliedDelayUs > m_AppliedPlayoutDelayUs) {
        // The band's level is the goal; the delay in force walks to it at
        // the same slew it moves within a band, so a band change never
        // steps a frame off its slot.
        m_AppliedPlayoutDelayUs += std::min(
            band.appliedDelayUs - m_AppliedPlayoutDelayUs,
            std::max<uint64_t>(1, m_Parameters.playoutDelayAttackUs));
    }
    else if (band.appliedDelayUs < m_AppliedPlayoutDelayUs) {
        m_AppliedPlayoutDelayUs -= std::min(
            m_AppliedPlayoutDelayUs - band.appliedDelayUs,
            std::max<uint64_t>(1, m_Parameters.playoutDelayReleaseUs));
    }
    m_AppliedPlayoutDelayValid = true;
}

void VrrTimingController::updatePlayoutHistory(
    const PacedFrame& frame, const CadenceObservation& cadence,
    bool rebased, int64_t requiredUs, uint64_t nowUs)
{
    const uint64_t at = sourceMappingUs(frame);
    const uint64_t elapsed = m_LastHistoryArrivalUs && at >= m_LastHistoryArrivalUs ?
        std::min<uint64_t>(at - m_LastHistoryArrivalUs, 33333) : 0;
    m_LastHistoryArrivalUs = at;
    if (!m_AppliedPlayoutDelayValid) {
        m_AppliedPlayoutDelayUs = playoutDelayStartUs();
        m_AppliedPlayoutDelayValid = true;
    }
    m_PlayoutBandValid = true;
    m_PlayoutBandIndex = 0; // One distribution, shared across source rates.

    if (m_Parameters.playoutPredictionOnly) {
        if (m_Parameters.playoutResponsiveBuffer >= 5) {
            if (m_Parameters.playoutInputPercentileEnabled) {
                if (m_Parameters.playoutInputHitchMissesOnly >= 8)
                    m_InputCoverageHistory.advance(nowUs, m_Parameters.playoutInputPercentileWindowUs);
                const uint64_t vrrHeadroomUs = inputRecoveryCreditUs();
                if (!m_InputPercentileBuffer.stats().initialized) {
                    m_InputPercentileBuffer.reset(m_AppliedPlayoutDelayUs);
                    m_InputLayerPeriodUs = m_SourcePeriodUs;
                    m_InputLayerEligibleAtStart = 0;
                }
                else if (rebased) {
                    m_InputPercentileBuffer.breakSequence();
                    // Requalify capacity without erasing the measured long score.
                    m_IntervalBuffer.breakSequence();
                }
                else if (cadence.sourceRateChanged || cadence.phaseDiscontinuity) {
                    // Clearing the evidence here made every return to a rate
                    // relearn from nothing at 1 ms/s: 198 of the first 200
                    // frames back at 110 FPS missed (22:55 capture).
                    m_InputPercentileBuffer.breakAdjacency();
                    m_IntervalBuffer.breakSequence();
                }
                // A rate seen before resumes its own layer's evidence,
                // protection and coverage at once. A new rate starts from the
                // previous layer's evidence rescaled by the VRR headroom floor,
                // in both directions: gained headroom is credited, lost
                // headroom is restored up to the evidence-backed target.
                uint64_t lostHeadroomUs = 0;
                const uint64_t previousLayerBufferUs = m_InputPercentileBuffer.stats().currentUs;
                const uint64_t previousLayerPeriodUs = m_InputLayerPeriodUs;
                const auto layerChange = updateInputLayer(nowUs, cadence);
                if (m_Parameters.playoutInputHitchMissesOnly >= 9 && layerChange != InputLayerChange::None) {
                    m_BufferLayerChanged = true;
                    m_BufferLayerDrain = m_InputLayerPeriodUs > previousLayerPeriodUs;
                    m_InputRateDropAtUs = m_BufferLayerDrain ? nowUs : 0;
                }
                if (m_Parameters.playoutInputHitchMissesOnly >= 4 &&
                    layerChange != InputLayerChange::None && m_InputSettledLayerPeriodUs &&
                    m_InputSettledLayerPeriodUs * 4 >= m_SourcePeriodUs * 5) {
                    m_InputRateRiseAtUs = nowUs;
                }
                if (m_Parameters.playoutInputRateDropSnap && m_Parameters.playoutInputHitchMissesOnly < 9 &&
                        layerChange != InputLayerChange::None) {
                    // A period at least a quarter longer than the last settled
                    // layer: 116 to 60 or 30 FPS, or 60 to 30 FPS. The fitted
                    // period ramps through short-lived layers on the way, so
                    // each of them re-arms; any other change ends the window.
                    const bool drop = m_InputSettledLayerPeriodUs &&
                        m_SourcePeriodUs * 4 >= m_InputSettledLayerPeriodUs * 5;
                    m_InputRateDropAtUs = drop ? nowUs : 0;
                }
                if (layerChange == InputLayerChange::Created ||
                        (m_Parameters.playoutInputHitchMissesOnly < 7 &&
                         layerChange == InputLayerChange::None && cadence.sourceRateChanged)) {
                    if (vrrHeadroomUs > m_InputHeadroomUs)
                        m_InputPercentileBuffer.creditHeadroom(vrrHeadroomUs - m_InputHeadroomUs);
                    else lostHeadroomUs = m_InputHeadroomUs - vrrHeadroomUs;
                }
                m_InputHeadroomUs = vrrHeadroomUs;
                // Before inputClockOffsetUs() updates its last raw offset,
                // transfer only actual mapping movement. Its drift-adjusted
                // history coordinate is a different quantity. A switched
                // layer first keeps its own protection, not another basis.
                if (m_Parameters.playoutInputHitchMissesOnly >= 5 && m_Parameters.playoutInputHitchMissesOnly < 9 &&
                    layerChange == InputLayerChange::None && m_InputClockValid &&
                    m_InputClockEpoch == m_PlayoutOffsetEpoch && m_InputRateRiseAtUs &&
                    nowUs >= m_InputRateRiseAtUs && nowUs - m_InputRateRiseAtUs < kInputRateSettlingUs) {
                    m_InputPercentileBuffer.remapProtection(
                        m_InputClockLastOffsetUs, m_AppliedPlayoutOffsetUs);
                }
                // Retire expired evidence at the decision, including the first
                // frame after quiet time; this adds no new jitter observation.
                if (m_Parameters.playoutInputHitchMissesOnly == 9) {
                    const uint64_t sinceDrop = m_InputRateDropAtUs && nowUs >= m_InputRateDropAtUs ?
                        nowUs - m_InputRateDropAtUs : UINT64_MAX;
                    const bool settling = sinceDrop < 10000000 &&
                        (sinceDrop < 1000000 || m_PlayoutOffsetConverging);
                    m_InputPercentileBuffer.setRateDropDrain(settling);
                    if (settling) m_BufferLayerDrain = true;
                    else m_InputRateDropAtUs = 0;
                }
                m_InputPercentileBuffer.setStableAdaptation(m_Parameters.playoutInputStableAdaptation);
                m_InputPercentileBuffer.advance(nowUs, playoutDelayMinimumUs(),
                    playoutDelayMaximumUs(), m_Parameters.playoutInputPercentileTargetPerMillion,
                    m_Parameters.playoutInputPercentileWindowUs,
                    m_Parameters.playoutInputPercentileRiseUsPerSecond,
                    m_Parameters.playoutInputPercentileFallUsPerSecond,
                    m_ReadinessBudgetUs, m_Parameters.playoutInputPercentileToleranceUs,
                    vrrHeadroomUs, inputClockOffsetUs(nowUs),
                    inputHitchPolicy(nowUs));
                m_InputPercentileBuffer.raiseForHeadroomLoss(lostHeadroomUs);
                if (layerChange != InputLayerChange::None) {
                    m_InputPercentileBuffer.transitionFrom(previousLayerBufferUs);
                    if (m_InputPercentileBuffer.fastLayerDrain()) m_BufferLayerDrain = true;
                }
                if (m_InputRateDropAtUs && m_Parameters.playoutInputHitchMissesOnly < 8) {
                    // After a drop the mapping often moves several ms over
                    // seconds at the offset slew rate, and the source-relative
                    // target moves the opposite way. Follow the target exactly
                    // while that settles (at least 1 s, at most 10 s), so the
                    // mapping plus buffer stays where the evidence needs it
                    // instead of carrying the excess for tens of seconds.
                    constexpr uint64_t kRateDropSnapMinimumUs = 1000000;
                    constexpr uint64_t kRateDropSnapMaximumUs = 10000000;
                    const uint64_t sinceUs = nowUs > m_InputRateDropAtUs ? nowUs - m_InputRateDropAtUs : 0;
                    if (sinceUs < kRateDropSnapMaximumUs &&
                            (sinceUs < kRateDropSnapMinimumUs || m_PlayoutOffsetConverging))
                        m_InputPercentileBuffer.followTarget();
                    else m_InputRateDropAtUs = 0;
                }
                const uint64_t demand = m_InputPercentileBuffer.demand(m_AppliedPlayoutDelayUs);
                m_UnclampedRequestedPlayoutDelayUs = m_InputPercentileBuffer.stats().clipped ?
                    m_InputPercentileBuffer.stats().headroomTargetUnclippedUs : demand;
                m_RequestedPlayoutDelayUs = m_Parameters.playoutInputHitchMissesOnly >= 9 ? demand :
                    clampUnsigned(demand, playoutDelayMinimumUs(), playoutDelayMaximumUs());
                // The direct observer already slews on the observation clock.
                // Do not also apply legacy per-frame attack/hold/release rules.
                m_AppliedPlayoutDelayUs = m_RequestedPlayoutDelayUs;
                return;
            }
            if (rebased || cadence.sourceRateChanged || cadence.phaseDiscontinuity) {
                m_MeanMissBuffer.breakSequence();
                if (rebased || cadence.phaseDiscontinuity) m_IntervalBuffer.breakSequence();
            }
            if (m_Parameters.playoutResponsiveBuffer >= 6 &&
                    m_Parameters.playoutEpochRateRatioPerMille != 0) {
                noteIntervalBufferEpoch(at);
            }
            const uint64_t requestedDemandUs =
                m_Parameters.playoutResponsiveBuffer >= 6 ?
                    m_IntervalBuffer.demand(m_AppliedPlayoutDelayUs) :
                    m_MeanMissBuffer.demand(m_AppliedPlayoutDelayUs);
            m_UnclampedRequestedPlayoutDelayUs = requestedDemandUs;
            m_RequestedPlayoutDelayUs = clampUnsigned(
                requestedDemandUs, playoutDelayMinimumUs(),
                playoutDelayMaximumUs());
            // Increases spread over several frames; release is time-based in
            // the observer. No prediction or cached percentile can authorize growth.
            if (m_RequestedPlayoutDelayUs > m_AppliedPlayoutDelayUs)
                m_AppliedPlayoutDelayUs += std::min<uint64_t>(125,
                    m_RequestedPlayoutDelayUs - m_AppliedPlayoutDelayUs);
            else if (m_Parameters.playoutDelayDecreaseSlewUs != 0)
                m_AppliedPlayoutDelayUs -= std::min(m_Parameters.playoutDelayDecreaseSlewUs,
                    m_AppliedPlayoutDelayUs - m_RequestedPlayoutDelayUs);
            else
                m_AppliedPlayoutDelayUs = m_RequestedPlayoutDelayUs;
            m_AppliedPlayoutDelayUs = std::min(m_AppliedPlayoutDelayUs, playoutDelayMaximumUs());
            return;
        }
        if (m_Parameters.playoutResponsiveBuffer) {
            if (m_Parameters.playoutResponsiveBuffer >= 3 && cadence.sourceRateChanged) {
                // A confirmed material rate change retires incompatible history.
                // Preserve the applied delay and slew it toward fresh evidence.
                m_RecentReadiness = Vrr13::RecentReadiness(
                    m_Parameters.playoutReadinessWindowUs,
                    m_Parameters.playoutOnTimeTargetPerMillion,
                    m_Parameters.playoutResponsiveBuffer >= 4);
                m_ReadinessPrediction.reset();
            }
            // Calibration is diagnostic only. The selected live percentile
            // and expiring miss boost own demand, subject to the latency cap.
            // Revision 4 admits growth only for a hard miss or prevalent
            // 1-2 ms pressure; the raw target remains available for release.
            uint64_t requestedPlayoutDelayUs = saturatingAdd(
                m_RecentReadiness.demand(at), m_Parameters.playoutDelayMarginUs);
            if (m_Parameters.playoutResponsiveBuffer >= 4 &&
                    requestedPlayoutDelayUs > m_AppliedPlayoutDelayUs &&
                    !m_RecentReadiness.allowsGrowth(at, m_AppliedPlayoutDelayUs)) {
                requestedPlayoutDelayUs = m_AppliedPlayoutDelayUs;
            }
            m_UnclampedRequestedPlayoutDelayUs = requestedPlayoutDelayUs;
            m_RequestedPlayoutDelayUs = requestedPlayoutDelayUs;
            const auto desired = clampUnsigned(m_RequestedPlayoutDelayUs,
                playoutDelayMinimumUs(), playoutDelayMaximumUs());
            if (desired > m_AppliedPlayoutDelayUs) {
                m_AppliedPlayoutDelayUs += std::min(
                    desired - m_AppliedPlayoutDelayUs,
                    m_Parameters.playoutDelayAttackUs);
            }
            else if (m_RecentReadiness.canRelease(at)) {
                // Drain faster when both display and processing have spare
                // capacity; never spend post-deadline headroom as readiness.
                const auto headroom = std::min(recoveryHeadroomUs(),
                    m_SourcePeriodUs > m_RenderLeadUs ? m_SourcePeriodUs - m_RenderLeadUs : 0);
                const auto speed = 1000 + headroom * 1000 /
                    std::max<uint64_t>(1, m_SourcePeriodUs);
                const auto release = std::min<uint64_t>(250, scaledPerMille(
                    scaledPerMille(m_Parameters.playoutDelayReleaseUs, elapsed * 120 / 1000), speed));
                m_AppliedPlayoutDelayUs -= std::min(
                    m_AppliedPlayoutDelayUs - desired, release);
            }
            m_AppliedPlayoutDelayUs = std::min(m_AppliedPlayoutDelayUs, playoutDelayMaximumUs());
            return;
        }
        // Predict the required protection from delivery, FIFO work and
        // scheduler observations. Count existing padding once: neither our
        // intentional waits nor post-submission display delay is more work.
        const uint64_t protection = uint64_t(
            (m_PlayoutHistory.common() + m_PlayoutHistory.boost()) / 1000);
        if (m_Parameters.playoutReadinessHitchThresholdUs) {
            // History may retain/release a level, but only a fresh attributable
            // output hitch can raise it. In particular, cache loading cannot
            // authorize growth. Tolerance was already applied at the event;
            // do not add another fixed readiness margin.
            m_RequestedPlayoutDelayUs = std::min(m_RequestedPlayoutDelayUs, protection);
            m_UnclampedRequestedPlayoutDelayUs = m_RequestedPlayoutDelayUs;
        }
        else {
            m_RequestedPlayoutDelayUs = saturatingAdd(protection,
                                                    m_Parameters.playoutDelayMarginUs);
            m_UnclampedRequestedPlayoutDelayUs = m_RequestedPlayoutDelayUs;
        }
        const uint64_t desired = clampUnsigned(m_RequestedPlayoutDelayUs,
            playoutDelayMinimumUs(), playoutDelayMaximumUs());
        if (desired > m_AppliedPlayoutDelayUs) {
            m_AppliedPlayoutDelayUs += std::min(desired - m_AppliedPlayoutDelayUs,
                                              m_Parameters.playoutDelayAttackUs);
        }
        else if (m_PlayoutHistory.canRelease()) {
            const uint64_t release = scaledPerMille(m_Parameters.playoutDelayReleaseUs,
                                                    elapsed * 120 / 1000);
            m_AppliedPlayoutDelayUs -= std::min(m_AppliedPlayoutDelayUs - desired, release);
        }
        // The cold-start seed is already applied above, not extra protection
        // to add to the margin. Capacity remains a hard bound through rebases.
        m_AppliedPlayoutDelayUs = std::min(m_AppliedPlayoutDelayUs, playoutDelayMaximumUs());
        return;
    }

    if (m_Parameters.playoutNativeHitchAdaptation) {
        // Readiness may veto release. Growth requires a new native hitch or
        // an enabled submission estimate while native timing is unavailable.
        // Leave 3 ms above measured readiness when releasing padding.
        const uint64_t releaseFloor = saturatingAdd(
            uint64_t(m_PlayoutHistory.common() / 1000), 3000);
        // A demand beyond storage capacity must not pin release forever or
        // resurrect growth later when capacity becomes available again.
        m_UnclampedRequestedPlayoutDelayUs = m_RequestedPlayoutDelayUs;
        m_RequestedPlayoutDelayUs = std::min(m_RequestedPlayoutDelayUs,
                                            playoutDelayMaximumUs());
        if (m_RequestedPlayoutDelayUs <= m_AppliedPlayoutDelayUs) {
            m_RequestedPlayoutDelayUs = std::min(m_AppliedPlayoutDelayUs,
                                               releaseFloor);
        }
        const uint64_t desired = clampUnsigned(m_RequestedPlayoutDelayUs,
            playoutDelayMinimumUs(), playoutDelayMaximumUs());
        if (desired > m_AppliedPlayoutDelayUs) {
            m_AppliedPlayoutDelayUs += std::min(desired - m_AppliedPlayoutDelayUs,
                                              m_Parameters.playoutDelayAttackUs);
        }
        else if (activeSmoothnessFeedback(at).samples() &&
                 std::abs(signedDifference(at, activeSmoothnessFeedback(at).lastObservedUs())) <= 100000 &&
                 activeSmoothnessFeedback(at).canRelease() &&
                 m_PlayoutHistory.canRelease()) {
            const uint64_t release = scaledPerMille(m_Parameters.playoutDelayReleaseUs,
                                                    elapsed * 120 / 1000);
            m_AppliedPlayoutDelayUs -= std::min(m_AppliedPlayoutDelayUs - desired, release);
        }
        // Storage safety still applies when the source rate or work changes.
        m_AppliedPlayoutDelayUs = std::min(m_AppliedPlayoutDelayUs, playoutDelayMaximumUs());
        return;
    }

    // Classify stalls on the sender's clock. A long receive/decode gap with
    // steady RTP is receiver jitter and must not be discarded as a host stall.
    // The period term also admits ordinary 30 FPS content (33.3 ms).
    const uint64_t stall = std::max(m_Parameters.playoutStallExclusionUs,
                                    scaledPerMille(m_SourcePeriodUs, 1500));
    const uint64_t burst = scaledPerMille(m_SourcePeriodUs,
                                         m_Parameters.playoutBurstExclusionPerMille);
    if (!m_Parameters.playoutPredictionEnabled && !rebased && cadence.eligible && !cadence.phaseDiscontinuity &&
        cadence.frameDelta == 1 && cadence.intervalUs >= burst &&
        cadence.intervalUs <= stall) {
        const uint64_t raw = saturatingAdd(uint64_t(std::max<int64_t>(0, requiredUs)),
                                           m_Parameters.playoutDelayMarginUs);
        const uint64_t covered = raw > m_Parameters.playoutDelayToleranceUs ?
            raw - m_Parameters.playoutDelayToleranceUs : 0;
        // Reserve uses nanoseconds; clamp malformed replay clocks before conversion.
        const auto ns = [](uint64_t us) {
            return int64_t(std::min<uint64_t>(us, INT64_MAX / 1000)) * 1000;
        };
        const uint64_t decoderQueueUs = frame.decoderQueueUs();
        m_WorkloadEpisode.observe(m_PlayoutHistory, ns(covered),
            ns(m_AppliedPlayoutDelayUs), ns(at), decoderQueueUs, m_SourcePeriodUs);
    }
    const bool smoothnessControlsDelay = m_Parameters.playoutSmoothnessFeedbackEnabled &&
        !m_Parameters.playoutReadinessDrivenAdaptation;
    uint64_t protection = uint64_t(m_PlayoutHistory.target(
        100000000, int64_t(playoutDelayStartUs()) * 1000,
        m_Parameters.playoutPredictionEnabled && !smoothnessControlsDelay &&
            !m_Parameters.playoutReadinessDrivenAdaptation ? int64_t(recoveryHeadroomUs()) * 1000 : 0) / 1000);
    if (smoothnessControlsDelay) {
        protection = std::max(protection, std::max(m_SubmissionSmoothness.protectionUs(), m_NativeSmoothness.protectionUs()));
        protection -= std::min(protection, recoveryHeadroomUs());
    }
    // Native/submission interval errors remain measured outcomes. A delay
    // after readiness can move with the playout target, so charging the
    // existing buffer plus that error produces a ratchet with no benefit.
    // Inter-frame recovery headroom cannot pay this frame's readiness
    // shortfall: time available after its deadline is not padding before it.
    // FIFO service already accounts for backlog recovery when it estimates
    // the readiness demand, so do not subtract that headroom again here.
    m_RequestedPlayoutDelayUs = saturatingAdd(protection,
        m_Parameters.playoutPredictionEnabled ? m_Parameters.playoutDelayMarginUs : 0);
    m_UnclampedRequestedPlayoutDelayUs = m_RequestedPlayoutDelayUs;
    const uint64_t desired = clampUnsigned(m_RequestedPlayoutDelayUs,
        playoutDelayMinimumUs(), playoutDelayMaximumUs());
    // Bound attack below the smoothness threshold; production responds faster
    // than legacy captures while avoiding a single large timeline step.
    // Release uses elapsed wall time at the former 120 FPS rate, with a cap
    // on recovery gaps. Five-minute memory does not imply five-minute startup.
    if (desired > m_AppliedPlayoutDelayUs) {
        m_AppliedPlayoutDelayUs += std::min(desired - m_AppliedPlayoutDelayUs,
                                          m_Parameters.playoutDelayAttackUs);
    }
    else if (m_PlayoutHistory.canRelease() && (!smoothnessControlsDelay ||
             (m_SubmissionSmoothness.canRelease() && m_NativeSmoothness.canRelease()))) {
        const uint64_t release = scaledPerMille(m_Parameters.playoutDelayReleaseUs,
                                                elapsed * 120 / 1000);
        m_AppliedPlayoutDelayUs -= std::min(m_AppliedPlayoutDelayUs - desired, release);
    }
    // Capacity is a hard safety bound, including after a source-rate increase.
    m_AppliedPlayoutDelayUs = std::min(m_AppliedPlayoutDelayUs, playoutQueueLimitUs());
}

uint64_t VrrTimingController::rtpTicksToUs(uint64_t ticks)
{
    constexpr uint64_t kTicksPerMillisecond = kRtpClockRate / 1000;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    if (ticks > maximum / 1000) {
        return ticks / kTicksPerMillisecond * 1000;
    }
    return ticks * 1000 / kTicksPerMillisecond;
}

const VrrTimingParameters& VrrTimingController::parameters() const
{
    return m_Parameters;
}

VrrTimingDiagnostics VrrTimingController::diagnostics() const
{
    VrrTimingDiagnostics value;
    value.readinessPhaseUs = m_ReadinessPhaseUs;
    value.readinessDemandUs = m_ReadinessDemandUs;
    value.appliedReadinessReserveUs = m_AppliedReadinessReserveUs;
    value.renderBaselineUs = m_RenderBaselineUs;
    value.renderInsuranceUs = renderInsuranceUs();
    value.gpuReadinessLeadUs = gpuReadinessLeadUs();
    value.pacingLatencyBudgetUs = pacingLatencyBudgetUs();
    value.cadenceSamples = m_CadenceSamples.size();
    value.rateCandidateSamples = m_RateCandidateSamples.size();
    value.readinessSamples = m_ReadyOffsets.size();
    value.preparationSamples = m_PreparationDurations.size();
    value.renderSchedulerSamples = m_RenderSchedulerDelays.size();
    value.targetSchedulerSamples = m_TargetSchedulerDelays.size();
    value.cleanSpacingFrames = m_CleanSpacingFrames;
    value.phaseErrorFrames = m_PhaseErrorFrames;
    value.readinessModelValid = m_ReadinessModelValid;
    return value;
}

uint64_t VrrTimingController::renderLeadFloorUs() const
{
    // Once measurements show a lower baseline, the historical 1 ms floor is
    // discretionary too and cannot violate the pacing policy at very high Hz.
    return std::min(
        std::min(m_Parameters.renderLeadFloorUs, m_SourcePeriodUs),
        saturatingAdd(m_RenderBaselineUs, renderInsuranceCeilingUs()));
}

uint64_t VrrTimingController::renderLeadCeilingUs() const
{
    uint64_t ceilingUs = std::min(
        saturatingAdd(m_RenderBaselineUs, renderInsuranceCeilingUs()),
        m_SourcePeriodUs);
    // Zero disables the legacy absolute render ceiling. Nonzero values remain
    // available to replay old captures and run explicit policy experiments.
    if (m_Parameters.renderLeadCeilingUs != 0) {
        ceilingUs = std::min(ceilingUs,
                             m_Parameters.renderLeadCeilingUs);
    }
    return std::max(renderLeadFloorUs(), ceilingUs);
}

uint64_t VrrTimingController::pacingLatencyBudgetUs() const
{
    if (!pacingLatencyPolicyEnabled()) {
        return 0;
    }
    // Immediate VRR presentation saves roughly half a scanout on average.
    // Spend no more than that on readiness plus render-tail insurance. The
    // render baseline is unavoidable work in fixed and adaptive modes alike.
    // Smoothness mode explicitly permits a cadence-scaled additional source
    // interval, matching the worker's extra stale-frame tolerance. A zero
    // explicit ratio retains the pre-schema behavior for old captures.
    const uint64_t lowLatencyBudgetUs = m_DisplayPeriodUs /
        std::max<uint64_t>(1, m_Parameters.pacingLatencyBudgetDivisor);
    uint64_t extraBudgetUs = 0;
    if (m_Parameters.pacingLatencyExtraPeriodNumerator != 0) {
        const uint64_t numerator =
            m_Parameters.pacingLatencyExtraPeriodNumerator;
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        const uint64_t scaledUs = m_SourcePeriodUs > maximum / numerator ?
            maximum : m_SourcePeriodUs * numerator;
        extraBudgetUs = scaledUs /
            m_Parameters.pacingLatencyExtraPeriodDenominator;
    }
    else if (m_Config.allowAdditionalQueuedFrame &&
             m_Parameters.pacingLatencyQueueModeExtra != 0) {
        // Pre-schema captures made under the retired smoothness option.
        extraBudgetUs = m_SourcePeriodUs;
    }
    return saturatingAdd(lowLatencyBudgetUs, extraBudgetUs);
}

bool VrrTimingController::pacingLatencyPolicyEnabled() const
{
    // Zero is reserved for replaying schema-5 captures made before the
    // baseline/tail policy existed. New production configurations use 2.
    return m_Parameters.pacingLatencyBudgetDivisor != 0;
}

uint64_t VrrTimingController::minimumReadinessReserveUs() const
{
    if (!pacingLatencyPolicyEnabled()) {
        return m_Parameters.minimumReadinessReserveUs;
    }
    return std::min(m_Parameters.minimumReadinessReserveUs,
                    pacingLatencyBudgetUs());
}

uint64_t VrrTimingController::renderInsuranceCeilingUs() const
{
    if (!pacingLatencyPolicyEnabled()) {
        return std::numeric_limits<uint64_t>::max();
    }
    const uint64_t budgetUs = pacingLatencyBudgetUs();
    const uint64_t minimumReserveUs = minimumReadinessReserveUs();
    return budgetUs > minimumReserveUs ?
        budgetUs - minimumReserveUs : 0;
}

uint64_t VrrTimingController::renderInsuranceUs() const
{
    return m_RenderLeadUs > m_RenderBaselineUs ?
        m_RenderLeadUs - m_RenderBaselineUs : 0;
}

uint64_t VrrTimingController::readinessReserveCeilingUs() const
{
    if (!pacingLatencyPolicyEnabled()) {
        return readinessCeilingUs();
    }
    const uint64_t budgetUs = pacingLatencyBudgetUs();
    const uint64_t insuranceUs = renderInsuranceUs();
    return budgetUs > insuranceUs ? budgetUs - insuranceUs : 0;
}

uint64_t VrrTimingController::readinessPeriodFloorUs() const
{
    if (m_Parameters.readinessPeriodFloorNumerator == 0 ||
            headroomUs() > m_DisplayPeriodUs) {
        return 0;
    }

    const uint64_t numerator =
        m_Parameters.readinessPeriodFloorNumerator;
    const uint64_t denominator =
        m_Parameters.readinessPeriodFloorDenominator;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    const uint64_t scaledUs = m_SourcePeriodUs > maximum / numerator ?
        maximum : m_SourcePeriodUs * numerator;
    return scaledUs / denominator;
}

size_t VrrTimingController::readinessLearningSampleLimit() const
{
    if (m_Parameters.readinessLearningWindowUs == 0) {
        return m_Parameters.readinessLearningSamples;
    }

    const uint64_t roundedSamples = std::max<uint64_t>(
        1, (m_Parameters.readinessLearningWindowUs +
            m_SourcePeriodUs / 2) / m_SourcePeriodUs);
    return static_cast<size_t>(std::max<uint64_t>(
        m_Parameters.minimumReadinessSamples,
        std::min<uint64_t>(m_Parameters.readinessLearningSamples,
                           roundedSamples)));
}

uint64_t VrrTimingController::readinessCeilingUs() const
{
    return std::min(m_Parameters.readinessCeilingUs, m_SourcePeriodUs);
}

uint64_t VrrTimingController::guardCeilingUs() const
{
    const uint64_t sourceSlackUs = m_SourcePeriodUs > m_DisplayPeriodUs ?
        m_SourcePeriodUs - m_DisplayPeriodUs : 0;
    return std::max(
        m_BaseGuardUs,
        std::min(m_Parameters.maximumAdaptiveGuardUs, sourceSlackUs));
}

uint64_t VrrTimingController::periodForRate(int rateHz, uint64_t fallbackUs)
{
    if (rateHz <= 0) {
        return fallbackUs;
    }
    const uint64_t rate = static_cast<uint64_t>(rateHz);
    return std::max<uint64_t>(1,
        (kMicrosecondsPerSecond + rate / 2) / rate);
}

uint64_t VrrTimingController::periodForRateQ16(int rateHz,
                                               uint64_t fallbackQ16)
{
    if (rateHz <= 0) {
        return fallbackQ16;
    }
    const uint64_t rate = static_cast<uint64_t>(rateHz);
    constexpr uint64_t kPeriodQ16Scale =
        kMicrosecondsPerSecond * kQ16One;
    return std::max<uint64_t>(1,
        (kPeriodQ16Scale + rate / 2) / rate);
}

uint64_t VrrTimingController::saturatingAdd(uint64_t left, uint64_t right)
{
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return left > maximum - right ? maximum : left + right;
}

uint64_t VrrTimingController::addSigned(uint64_t value, int64_t adjustment)
{
    if (adjustment >= 0) {
        return saturatingAdd(value, static_cast<uint64_t>(adjustment));
    }
    const uint64_t magnitude =
        static_cast<uint64_t>(-(adjustment + 1)) + 1;
    return value > magnitude ? value - magnitude : 0;
}

int64_t VrrTimingController::signedDifference(uint64_t left, uint64_t right)
{
    if (left >= right) {
        const uint64_t difference = left - right;
        return difference > static_cast<uint64_t>(
                   std::numeric_limits<int64_t>::max()) ?
            std::numeric_limits<int64_t>::max() :
            static_cast<int64_t>(difference);
    }

    const uint64_t difference = right - left;
    if (difference > static_cast<uint64_t>(
                         std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::min();
    }
    return -static_cast<int64_t>(difference);
}

uint64_t VrrTimingController::roundedQ16(uint64_t valueQ16)
{
    return saturatingAdd(valueQ16, kQ16Half) / kQ16One;
}

bool VrrTimingController::withinPercent(uint64_t value, uint64_t reference,
                                        unsigned int percent)
{
    if (reference == 0) {
        return value == 0;
    }
    const uint64_t difference = value > reference ? value - reference :
                                                     reference - value;
    return static_cast<long double>(difference) * 100.0L <=
        static_cast<long double>(reference) * percent;
}
