#include "vrrtimingcontroller.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace {

constexpr uint64_t kMicrosecondsPerSecond = 1000000ULL;
constexpr uint64_t kRtpClockRate = 90000ULL;
constexpr uint64_t kQ16One = 1ULL << 16;
constexpr uint64_t kQ16Half = kQ16One >> 1;
// Both queue policies are a fixed jitter buffer hung off the sender
// timestamps: every frame presents at its RTP time plus one constant delay.
// Arrival variation up to that delay is invisible; anything larger is one
// late frame and nothing else moves. On the reference 116 FPS / 120 Hz rig
// decode-versus-RTP jitter is about 1 ms at p50 and 4 ms at p95, so 2 ms
// absorbs the common case at low latency and 5 ms covers roughly p97.
// One VRR queue policy. The adaptive calibrator learns the playout delay per
// source-rate band from the exogenous lateness of arrivals against the mapped
// sender clock, targeting the p99.9 of lateness plus a margin within 1 to
// 8 ms. On the 2026-09-01 20:04 capture p99 let one frame in a hundred
// through as a hitch; p99.9 halved those hitches (13 to 6 in 106 s) for
// 1.2 ms more median latency, a trade the user judged well worth it. A new
// band starts high and releases slowly so startup and regime changes cost
// latency rather than hitches. The fixed delay below is only used when the
// calibrator is disabled by replay parameters.
constexpr uint64_t kFixedPlayoutDelayUs = 3000;
constexpr uint64_t kPlayoutStartUs = 6000;
constexpr uint64_t kPlayoutMinimumUs = 1000;
constexpr uint64_t kPlayoutMaximumUs = 8000;
constexpr uint64_t kPlayoutPercentilePerMille = 999;
constexpr uint64_t kPlayoutBurstExclusionPerMille = 750;

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
    std::sort(ordered.begin(), ordered.end());
    const unsigned int percentileValue = std::min(100U, requestedPercentile);
    const size_t rank = std::max<size_t>(
        1, (ordered.size() * percentileValue + 99) / 100);
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

} // namespace

VrrTimingParameters vrrTimingParametersForSession(
    const VrrSessionConfig& config)
{
    // Present at sender time plus a learned delay. The readiness reserve, its
    // per-frame slewing, and every phase re-anchor are off on this path: they
    // each moved the target between frames the source had spaced evenly,
    // which was the visible stutter. The projected-clock policy and the
    // retired smoothness budget remain reachable through explicit parameters
    // so older captures replay under the policy they ran.
    (void) config;
    VrrTimingParameters parameters;
    parameters.timestampPlayoutEnabled = 1;
    parameters.playoutDelayAdaptive = 1;
    parameters.sourcePlayoutDelayUs = kFixedPlayoutDelayUs;
    parameters.playoutDelayStartUs = kPlayoutStartUs;
    parameters.playoutDelayMinimumUs = kPlayoutMinimumUs;
    parameters.playoutDelayMaximumUs = kPlayoutMaximumUs;
    parameters.playoutDelayPercentilePerMille = kPlayoutPercentilePerMille;
    parameters.playoutBurstExclusionPerMille = kPlayoutBurstExclusionPerMille;
    parameters.pacingLatencyQueueModeExtra = 0;
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
    m_LastSubmissionUs = 0;
    m_CleanSpacingFrames = 0;
    m_PhaseErrorFrames = 0;
    clearTimeline(false);
}

void VrrTimingController::rebase()
{
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
    const uint64_t previousGuardUs = m_GuardUs;

    m_SourcePeriodUsQ16 = m_ConfiguredStreamPeriodQ16;
    m_SourcePeriodUs = std::max<uint64_t>(
        1, roundedQ16(m_SourcePeriodUsQ16));
    m_LatchedPresentation = m_CanLatchPresentation && m_SourcePeriodUs <
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
    // Learned per-band delays survive a source-phase rebase like the other
    // learned budgets; only a full reset discards them.
    if (!retainLearnedBudgets) {
        m_PlayoutBands.clear();
        m_PlayoutBandValid = false;
        m_AppliedPlayoutDelayUs = 0;
    }
    m_HaveLastDecodeComplete = false;
    m_LastDecodeCompleteUs = 0;
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
    m_RenderSchedulerDelays.clear();
    m_TargetSchedulerDelays.clear();
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
        m_GuardUs = m_BaseGuardUs;
    }
    clampReadinessReserveToPolicy();
}

void VrrTimingController::initializeTimeline(const PacedFrame& frame)
{
    m_HaveTimeline = true;
    anchorSourceTime(frame.decodeCompleteUs());
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

VrrTimingDecision VrrTimingController::schedule(const PacedFrame& frame,
                                                 uint64_t nowUs)
{
    m_Pending = PendingFrame {};

    CadenceObservation cadence;
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
                    anchorSourceTime(frame.decodeCompleteUs());
                }

                m_LastFrameNumber = frame.frameNumber();
                m_HaveLastFrameNumber = frame.frameNumber() >= 0;
                m_LastRtpTimestamp = frame.rtpTimestamp();
                m_LastTimestampValid = frame.timestampValid();
            }
        }
    }

    // Timestamp playout: the target is the sender timestamp mapped into the
    // local clock plus one constant delay. The mapping offset is the windowed
    // minimum of decode-complete minus RTP time, slewed a few microseconds per
    // frame so host/client clock drift is tracked without ever moving one
    // frame's target relative to its neighbours. Nothing below re-anchors on
    // a late or early frame: a late frame simply clamps to "now" and the next
    // frame returns to its own slot.
    const bool timestampPlayout = timestampPlayoutEnabled() &&
        frame.timestampValid() && (rebased || cadence.usedRtpTimestamp);
    m_TimestampPlayoutActive = timestampPlayout;
    const uint64_t rtpUs = timestampPlayout ?
        rtpTicksToUs(m_UnwrappedRtpTicks) : 0;
    int64_t readyOffsetUs = 0;
    if (timestampPlayout) {
        const int64_t offsetUs = signedDifference(frame.decodeCompleteUs(),
                                                  rtpUs);
        const int64_t appliedOffsetUs = observePlayoutOffset(
            frame.decodeCompleteUs(), offsetUs);
        anchorSourceTime(addSigned(rtpUs, appliedOffsetUs));
        readyOffsetUs = signedDifference(frame.decodeCompleteUs(),
                                         m_SourceTimeUs);
        m_ReadinessBudgetUs = 0;
        m_ReadinessPhaseUs = 0;
        m_PhaseErrorFrames = 0;
        updatePlayoutDelay(frame, cadence, rebased, readyOffsetUs, nowUs);
    }
    else {
        readyOffsetUs = signedDifference(frame.decodeCompleteUs(),
                                         m_SourceTimeUs);
        if (!rebased && !cadence.phaseDiscontinuity && cadence.eligible) {
            const int64_t ceilingUs =
                static_cast<int64_t>(readinessCeilingUs());
            if (readyOffsetUs < -ceilingUs) {
                // A frame that is ready well before the old slower clock must
                // not wait behind an obsolete cutscene cadence. The display
                // floor and latched near-refresh mode still bound how quickly
                // it can submit.
                anchorSourceTime(frame.decodeCompleteUs());
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
                anchorSourceTime(frame.decodeCompleteUs());
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

    const uint64_t playoutDelayUs = timestampPlayout ?
        effectivePlayoutDelayUs() : m_Parameters.sourcePlayoutDelayUs;
    uint64_t targetUs = saturatingAdd(
        addSigned(m_SourceTimeUs, m_ReadinessBudgetUs),
        saturatingAdd(
            playoutDelayUs,
            saturatingAdd(m_RenderLeadUs,
                          m_Parameters.presentationSafetyUs)));
    targetUs = std::max(
        targetUs,
        saturatingAdd(nowUs,
                      saturatingAdd(m_RenderLeadUs,
                                    m_Parameters.presentationSafetyUs)));

    // This is a live, one-slot path. An unconfirmed RTP/frame jump may
    // describe already-skipped content, never hundreds of milliseconds that
    // the client should wait again. Reseed poisoned playout phase without
    // discarding the cumulative cadence model.
    const uint64_t maximumDirectTargetUs = saturatingAdd(
        nowUs,
        saturatingAdd(
            std::max(m_ConfiguredStreamPeriodUs, m_SourcePeriodUs),
            saturatingAdd(
                playoutDelayUs,
                saturatingAdd(m_RenderLeadUs,
                              m_Parameters.presentationSafetyUs))));
    if (targetUs > maximumDirectTargetUs) {
        // Do not clear cadence history when a faster source makes the old
        // playout phase point into the future. Reseed phase from this already
        // decoded frame and let the cumulative fit heal the rate.
        anchorSourceTime(frame.decodeCompleteUs());
        m_ReadyOffsets.clear();
        m_ReadinessBudgetUs = 0;
        m_ReadinessPhaseUs = 0;
        if (timestampPlayout) {
            // The frame is more than a source period earlier than the mapped
            // clock predicts: the sender clock jumped. Re-seed the offset on
            // this frame rather than making it wait out a stale mapping.
            resetPlayoutOffsets();
            observePlayoutOffset(
                frame.decodeCompleteUs(),
                signedDifference(frame.decodeCompleteUs(), rtpUs));
        }
        readyOffsetUs = 0;
        cadence.phaseDiscontinuity = true;
        cadence.eligible = false;
        m_PhaseErrorFrames = 0;
        targetUs = saturatingAdd(
            std::max(frame.decodeCompleteUs(), nowUs),
            saturatingAdd(
                playoutDelayUs,
                saturatingAdd(m_RenderLeadUs,
                              m_Parameters.presentationSafetyUs)));
    }

    targetUs = std::max(targetUs, earliestSubmissionUs());
    const uint64_t totalLeadUs = saturatingAdd(m_RenderLeadUs,
                                               m_RenderWakeLeadUs);
    const uint64_t renderStartUs = targetUs > totalLeadUs ?
        targetUs - totalLeadUs : 0;

    VrrTimingDecision decision;
    decision.sourceTimeUs = m_SourceTimeUs;
    decision.sourceIntervalUs = cadence.intervalUs;
    decision.sourcePeriodUs = m_SourcePeriodUs;
    decision.readyOffsetUs = readyOffsetUs;
    decision.readinessBudgetUs = m_ReadinessBudgetUs;
    decision.playoutDelayUs = playoutDelayUs;
    decision.renderStartUs = renderStartUs;
    decision.targetUs = targetUs;
    decision.guardUs = m_GuardUs;
    decision.headroomUs = headroomUs();
    decision.timingBudgetUs = timingBudgetUs();
    decision.renderLeadUs = m_RenderLeadUs;
    decision.renderWakeLeadUs = m_RenderWakeLeadUs;
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
    if (!m_CanLatchPresentation) {
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

    m_Pending.valid = true;
    // Timestamp playout never feeds the learned readiness reserve.
    m_Pending.cadenceEligible = decision.cadenceEligible && !timestampPlayout;
    m_Pending.readyOffsetUs = readyOffsetUs;
    m_LastDecodeCompleteUs = frame.decodeCompleteUs();
    m_HaveLastDecodeComplete = true;
    return decision;
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
        if (m_RateCandidateSamples.size() >=
                m_Parameters.rateCandidateSamples) {
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
    m_GuardUs = clampUnsigned(m_GuardUs,
                              m_BaseGuardUs,
                              guardCeilingUs());
    clampReadinessReserveToPolicy();
    return !withinPercent(m_SourcePeriodUs, previousPeriodUs,
                          m_Parameters.materialRateChangePercent);
}

void VrrTimingController::anchorSourceTime(uint64_t sourceTimeUs)
{
    m_SourceTimeUs = sourceTimeUs;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    m_SourceTimeUsQ16 = sourceTimeUs > maximum / kQ16One ?
        maximum : sourceTimeUs * kQ16One;
}

void VrrTimingController::notePreparationDuration(
    uint64_t preparationDurationUs)
{
    if (!m_Pending.valid) {
        return;
    }
    m_Pending.hasPreparationDuration = true;
    m_Pending.preparationDurationUs = preparationDurationUs;
}

void VrrTimingController::noteSchedulerDelays(uint64_t renderDelayUs,
                                              uint64_t targetDelayUs,
                                              bool targetDelayValid)
{
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
                                         uint64_t submissionUs)
{
    if (!m_Pending.valid) {
        return;
    }

    if (submitted) {
        // Cancellation is a reason, not proof that nothing reached the native
        // presentation queue (Vulkan must submit some abandoned images).
        m_HaveLastSubmission = true;
        m_LastSubmissionUs = submissionUs;

        if (!cancelled) {
            if (m_Pending.cadenceEligible) {
                appendBounded(m_ReadyOffsets, m_Pending.readyOffsetUs,
                              readinessLearningSampleLimit());
            }
            if (m_Pending.hasPreparationDuration) {
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

uint64_t VrrTimingController::targetWakeLeadUs() const
{
    return m_TargetWakeLeadUs;
}

uint64_t VrrTimingController::earliestSubmissionUs() const
{
    if (!m_HaveLastSubmission) {
        return 0;
    }
    return saturatingAdd(
        m_LastSubmissionUs,
        saturatingAdd(m_DisplayPeriodUs, m_GuardUs));
}

uint64_t VrrTimingController::lastSubmissionUs() const
{
    return m_LastSubmissionUs;
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
    m_TimestampPlayoutActive = false;
}

int64_t VrrTimingController::observePlayoutOffset(uint64_t decodeCompleteUs,
                                                  int64_t offsetUs)
{
    // Bound the window by both age and count so a pathological configuration
    // cannot grow the deque without limit.
    constexpr size_t kMaximumPlayoutOffsetSamples = 4096;
    m_PlayoutOffsets.push_back(PlayoutOffsetSample { decodeCompleteUs,
                                                     offsetUs });
    const uint64_t windowUs = m_Parameters.playoutOffsetWindowUs;
    while (m_PlayoutOffsets.size() > 1 &&
           (m_PlayoutOffsets.size() > kMaximumPlayoutOffsetSamples ||
            (decodeCompleteUs > windowUs &&
             m_PlayoutOffsets.front().decodeCompleteUs <
                 decodeCompleteUs - windowUs))) {
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
            m_Parameters.playoutOffsetSlewUs,
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
    return m_AppliedPlayoutOffsetUs;
}

uint64_t VrrTimingController::playoutDelayUs() const
{
    return m_TimestampPlayoutActive ? effectivePlayoutDelayUs() :
                                      m_Parameters.sourcePlayoutDelayUs;
}

unsigned int VrrTimingController::playoutBandIndex() const
{
    return m_PlayoutBandValid ? m_PlayoutBandIndex : 0;
}

uint64_t VrrTimingController::playoutBandSamples() const
{
    if (!m_PlayoutBandValid) {
        return 0;
    }
    const auto band = m_PlayoutBands.find(m_PlayoutBandIndex);
    return band != m_PlayoutBands.end() ? band->second.samplesSeen : 0;
}

uint64_t VrrTimingController::effectivePlayoutDelayUs() const
{
    if (m_Parameters.playoutDelayAdaptive != 0 && m_PlayoutBandValid) {
        return m_AppliedPlayoutDelayUs;
    }
    return m_Parameters.sourcePlayoutDelayUs;
}

uint64_t VrrTimingController::playoutDelayMinimumUs() const
{
    return m_Parameters.playoutDelayMinimumUs;
}

uint64_t VrrTimingController::playoutDelayMaximumUs() const
{
    return std::max(m_Parameters.playoutDelayMaximumUs,
                    m_Parameters.playoutDelayMinimumUs);
}

uint64_t VrrTimingController::playoutDelayStartUs() const
{
    const uint64_t startUs = m_Parameters.playoutDelayStartUs != 0 ?
        m_Parameters.playoutDelayStartUs : m_Parameters.sourcePlayoutDelayUs;
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
    const bool steadyArrival = m_HaveLastDecodeComplete &&
        frame.decodeCompleteUs() >= m_LastDecodeCompleteUs &&
        frame.decodeCompleteUs() - m_LastDecodeCompleteUs <=
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
    if (!rebased && !switched && cadence.eligible &&
            !cadence.phaseDiscontinuity && steadyArrival && steadySender) {
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
    band.appliedDelayUs = clampUnsigned(
        band.appliedDelayUs, playoutDelayMinimumUs(),
        playoutDelayMaximumUs());
    m_AppliedPlayoutDelayUs = band.appliedDelayUs;
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
