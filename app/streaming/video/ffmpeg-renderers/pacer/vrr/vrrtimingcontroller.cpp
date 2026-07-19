#include "vrrtimingcontroller.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr uint64_t kMicrosecondsPerSecond = 1000000ULL;
constexpr uint64_t kRtpClockRate = 90000ULL;
constexpr uint64_t kQ16One = 1ULL << 16;
constexpr uint64_t kQ16Half = kQ16One >> 1;
constexpr uint64_t kMaximumForwardMovementUs = kMicrosecondsPerSecond;
constexpr uint64_t kRenderLeadFloorUs = 1000ULL;
constexpr uint64_t kRenderLeadCeilingUs = 6500ULL;
constexpr uint64_t kRenderLeadSlackUs = 500ULL;
constexpr uint64_t kPresentationSafetyUs = 250ULL;
constexpr uint64_t kReadinessCeilingUs = 10000ULL;
constexpr uint64_t kMaximumWakeLeadUs = 2000ULL;
constexpr uint64_t kMaximumTargetWakeLeadUs = 500ULL;
constexpr uint64_t kMinimumGuardUs = 100ULL;
// Below this learned headroom, immediate flips cannot absorb ordinary
// scheduler and flip-queue jitter, so the controller requests latched
// presentation instead. At such near-refresh cadences latching costs only a
// few repeated frames per second, while an early flip is a visible tear.
constexpr uint64_t kLatchedPresentationHeadroomUs = 1500ULL;
constexpr uint64_t kLatchedPresentationExitHeadroomUs = 2000ULL;
constexpr uint64_t kMaximumBaseGuardUs = 250ULL;
constexpr uint64_t kMaximumAdaptiveGuardUs = 1000ULL;
constexpr uint64_t kGuardStepUs = 50ULL;
constexpr unsigned int kGuardDecayFrames = 120;
constexpr size_t kLearningSampleCount = 32;
constexpr size_t kMinimumCadenceSamples = 6;
constexpr size_t kMaximumCadenceSamples = 128;
constexpr size_t kRateCandidateSampleCount = 3;
constexpr uint64_t kLooseCadenceWindowUs = 500000ULL;
constexpr uint64_t kTightCadenceWindowUs = 1000000ULL;
// Raw timestamp atoms may legitimately dither by nearly 2:1. Only a much
// larger one-frame departure starts the provisional fast-recovery path.
constexpr uint64_t kMajorCadenceRatioNumerator = 7ULL;
constexpr uint64_t kMajorCadenceRatioDenominator = 2ULL;
constexpr uint64_t kCandidateCadenceRatio = 2ULL;
constexpr unsigned int kMaterialRateChangePercent = 12;

uint64_t clampUnsigned(uint64_t value, uint64_t low, uint64_t high)
{
    if (high < low) {
        high = low;
    }
    return std::max(low, std::min(value, high));
}

} // namespace

VrrTimingController::VrrTimingController(const VrrSessionConfig& config) :
    m_Config(config)
{
    reset();
}

void VrrTimingController::reset()
{
    m_DisplayPeriodUs = periodForRate(m_Config.displayRefreshHz, 16667);
    m_ConfiguredStreamPeriodUs = periodForRate(
        m_Config.streamRateHz, m_DisplayPeriodUs);
    m_BaseGuardUs = clampUnsigned(m_DisplayPeriodUs / 64,
                                  kMinimumGuardUs,
                                  kMaximumBaseGuardUs);

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
    const uint64_t previousRenderLeadUs = m_RenderLeadUs;
    const uint64_t previousRenderWakeLeadUs = m_RenderWakeLeadUs;
    const uint64_t previousTargetWakeLeadUs = m_TargetWakeLeadUs;
    const uint64_t previousGuardUs = m_GuardUs;

    m_SourcePeriodUs = m_ConfiguredStreamPeriodUs;
    m_SourcePeriodUsQ16 = m_SourcePeriodUs * kQ16One;
    m_LatchedPresentation = m_SourcePeriodUs <
        saturatingAdd(m_DisplayPeriodUs, kLatchedPresentationHeadroomUs);
    m_ReadinessBudgetUs = 0;
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

    m_CadenceSamples.clear();
    m_RateCandidateSamples.clear();
    m_ReadyOffsets.clear();
    m_PreparationDurations.clear();
    m_RenderSchedulerDelays.clear();
    m_TargetSchedulerDelays.clear();
    m_Pending = PendingFrame {};

    if (retainLearnedBudgets) {
        m_RenderLeadUs = clampUnsigned(previousRenderLeadUs,
                                       renderLeadFloorUs(),
                                       renderLeadCeilingUs());
        m_RenderWakeLeadUs = std::min(previousRenderWakeLeadUs,
                                      kMaximumWakeLeadUs);
        m_TargetWakeLeadUs = std::min(previousTargetWakeLeadUs,
                                      kMaximumTargetWakeLeadUs);
        m_GuardUs = clampUnsigned(previousGuardUs,
                                  m_BaseGuardUs,
                                  guardCeilingUs());
    }
    else {
        m_RenderLeadUs = clampUnsigned(kRenderLeadFloorUs,
                                       renderLeadFloorUs(),
                                       renderLeadCeilingUs());
        m_RenderWakeLeadUs = 0;
        m_TargetWakeLeadUs = 0;
        m_GuardUs = m_BaseGuardUs;
    }
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

    int64_t readyOffsetUs = signedDifference(frame.decodeCompleteUs(),
                                              m_SourceTimeUs);
    if (!rebased && !cadence.phaseDiscontinuity && cadence.eligible) {
        const int64_t ceilingUs = static_cast<int64_t>(readinessCeilingUs());
        if (readyOffsetUs < -ceilingUs) {
            // A frame that is ready well before the old slower clock must not
            // wait behind an obsolete cutscene cadence. The display floor and
            // latched near-refresh mode still bound how quickly it can submit.
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

        if (!cadence.phaseDiscontinuity && m_PhaseErrorFrames >= 3) {
            // A bounded readiness reserve cannot repay a sustained source
            // phase error. Re-anchor locally while retaining the cumulative
            // cadence fit, rather than repeatedly rebasing the whole model.
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
        // A new source epoch or local phase recovery is anchored by the first
        // directly observed ready offset. Cadence history is retained for the
        // phase-only cases above.
        m_ReadyOffsets.clear();
        const int64_t ceilingUs = static_cast<int64_t>(readinessCeilingUs());
        m_ReadinessBudgetUs = std::max(
            -ceilingUs, std::min(readyOffsetUs, ceilingUs));
    }

    uint64_t targetUs = saturatingAdd(
        addSigned(m_SourceTimeUs, m_ReadinessBudgetUs),
        saturatingAdd(m_RenderLeadUs, kPresentationSafetyUs));
    targetUs = std::max(
        targetUs,
        saturatingAdd(nowUs,
                      saturatingAdd(m_RenderLeadUs,
                                    kPresentationSafetyUs)));

    // This is a live, one-slot path. An unconfirmed RTP/frame jump may
    // describe already-skipped content, never hundreds of milliseconds that
    // the client should wait again. Reseed poisoned playout phase without
    // discarding the cumulative cadence model.
    const uint64_t maximumDirectTargetUs = saturatingAdd(
        nowUs,
        saturatingAdd(std::max(m_ConfiguredStreamPeriodUs,
                               m_SourcePeriodUs),
                      saturatingAdd(m_RenderLeadUs,
                                    kPresentationSafetyUs)));
    if (targetUs > maximumDirectTargetUs) {
        // Do not clear cadence history when a faster source makes the old
        // playout phase point into the future. Reseed phase from this already
        // decoded frame and let the cumulative fit heal the rate.
        anchorSourceTime(frame.decodeCompleteUs());
        m_ReadyOffsets.clear();
        m_ReadinessBudgetUs = 0;
        readyOffsetUs = 0;
        cadence.phaseDiscontinuity = true;
        cadence.eligible = false;
        m_PhaseErrorFrames = 0;
        targetUs = saturatingAdd(
            std::max(frame.decodeCompleteUs(), nowUs),
            saturatingAdd(m_RenderLeadUs, kPresentationSafetyUs));
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
    decision.renderStartUs = renderStartUs;
    decision.targetUs = targetUs;
    decision.guardUs = m_GuardUs;
    decision.headroomUs = headroomUs();
    decision.timingBudgetUs = timingBudgetUs();
    decision.renderLeadUs = m_RenderLeadUs;
    decision.renderWakeLeadUs = m_RenderWakeLeadUs;
    decision.targetWakeLeadUs = m_TargetWakeLeadUs;
    const uint64_t learnedHeadroomUs = headroomUs();
    if (m_LatchedPresentation) {
        if (learnedHeadroomUs >= kLatchedPresentationExitHeadroomUs) {
            m_LatchedPresentation = false;
        }
    }
    else if (learnedHeadroomUs < kLatchedPresentationHeadroomUs) {
        m_LatchedPresentation = true;
    }
    decision.latchedPresentation = m_LatchedPresentation;
    decision.usedRtpTimestamp = cadence.usedRtpTimestamp;
    decision.cadenceEligible = !rebased && cadence.eligible;
    decision.sourceRateChanged = !rebased && cadence.sourceRateChanged;
    decision.phaseDiscontinuity = !rebased && cadence.phaseDiscontinuity;
    decision.rebased = rebased;

    m_Pending.valid = true;
    m_Pending.cadenceEligible = decision.cadenceEligible;
    m_Pending.readyOffsetUs = readyOffsetUs;
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
        if (intervalUs > kMaximumForwardMovementUs) {
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
            observedPeriodUs <= m_SourcePeriodUs * kCandidateCadenceRatio &&
            m_SourcePeriodUs <= observedPeriodUs * kCandidateCadenceRatio;
        if (returnedToStableCadence) {
            m_RateCandidateSamples.clear();
            m_CadenceSamples.clear();
            m_CadenceSamples.push_back(current);
            observation.phaseDiscontinuity = true;
            return;
        }

        appendCadenceSample(m_RateCandidateSamples, current);
        observation.phaseDiscontinuity = true;
        if (m_RateCandidateSamples.size() >= kRateCandidateSampleCount) {
            const uint64_t candidatePeriodQ16 = fittedSourcePeriodQ16(
                m_RateCandidateSamples);
            if (candidatePeriodQ16 != 0) {
                acceptSourcePeriodQ16(candidatePeriodQ16);
                m_CadenceSamples = m_RateCandidateSamples;
                m_RateCandidateSamples.clear();
                observation.sourceRateChanged = true;
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
    if (m_CadenceSamples.size() >= kMinimumCadenceSamples) {
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
    while (samples.size() > kMaximumCadenceSamples) {
        samples.pop_front();
    }

    while (samples.size() > kMinimumCadenceSamples) {
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

    // Fit cumulative RTP movement against frame progression. Centering the
    // coordinates at the first point keeps the sums small and makes skipped
    // frame numbers naturally contribute their full source-frame distance.
    const long double firstFrame = static_cast<long double>(
        samples.front().frameOrdinal);
    const long double firstTicks = static_cast<long double>(
        samples.front().rtpTicks);
    long double sumX = 0.0L;
    long double sumY = 0.0L;
    for (const CadenceSample& sample : samples) {
        sumX += static_cast<long double>(sample.frameOrdinal) - firstFrame;
        sumY += static_cast<long double>(sample.rtpTicks) - firstTicks;
    }

    const long double count = static_cast<long double>(samples.size());
    const long double meanX = sumX / count;
    const long double meanY = sumY / count;
    long double covariance = 0.0L;
    long double variance = 0.0L;
    for (const CadenceSample& sample : samples) {
        const long double x =
            static_cast<long double>(sample.frameOrdinal) - firstFrame - meanX;
        const long double y =
            static_cast<long double>(sample.rtpTicks) - firstTicks - meanY;
        covariance += x * y;
        variance += x * x;
    }
    if (variance <= 0.0L || covariance <= 0.0L) {
        return 0;
    }

    const long double periodUs = covariance / variance *
        static_cast<long double>(kMicrosecondsPerSecond) /
        static_cast<long double>(kRtpClockRate);
    const long double periodQ16 = periodUs *
        static_cast<long double>(kQ16One);
    if (periodQ16 < 1.0L || periodQ16 >= static_cast<long double>(
            std::numeric_limits<uint64_t>::max())) {
        return 0;
    }
    return static_cast<uint64_t>(std::llround(periodQ16));
}

uint64_t VrrTimingController::cadenceWindowUs() const
{
    const uint64_t displayFloorUs = saturatingAdd(m_DisplayPeriodUs,
                                                   m_GuardUs);
    const uint64_t headroomUs = m_SourcePeriodUs > displayFloorUs ?
        m_SourcePeriodUs - displayFloorUs : 0;
    const uint64_t looseHeadroomUs = saturatingAdd(m_DisplayPeriodUs,
                                                    m_DisplayPeriodUs);
    if (headroomUs >= looseHeadroomUs) {
        return kLooseCadenceWindowUs;
    }
    if (headroomUs <= m_DisplayPeriodUs) {
        return kTightCadenceWindowUs;
    }

    const uint64_t tightnessNumerator = looseHeadroomUs - headroomUs;
    const uint64_t windowRangeUs = kTightCadenceWindowUs -
        kLooseCadenceWindowUs;
    return kLooseCadenceWindowUs +
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
    return observedPeriodUs * kMajorCadenceRatioDenominator >
               m_SourcePeriodUs * kMajorCadenceRatioNumerator ||
        m_SourcePeriodUs * kMajorCadenceRatioDenominator >
               observedPeriodUs * kMajorCadenceRatioNumerator;
}

bool VrrTimingController::acceptSourcePeriodQ16(uint64_t periodUsQ16)
{
    if (periodUsQ16 == 0) {
        return false;
    }

    const uint64_t previousPeriodUs = m_SourcePeriodUs;
    m_SourcePeriodUsQ16 = periodUsQ16;
    m_SourcePeriodUs = std::max<uint64_t>(1, roundedQ16(periodUsQ16));
    m_RenderLeadUs = clampUnsigned(m_RenderLeadUs,
                                   renderLeadFloorUs(),
                                   renderLeadCeilingUs());
    m_GuardUs = clampUnsigned(m_GuardUs,
                              m_BaseGuardUs,
                              guardCeilingUs());
    return !withinPercent(m_SourcePeriodUs, previousPeriodUs,
                          kMaterialRateChangePercent);
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
                  kLearningSampleCount);
    if (targetDelayValid) {
        appendBounded(m_TargetSchedulerDelays, targetDelayUs,
                      kLearningSampleCount);
    }
    updateLearnedBudgets();
}

void VrrTimingController::noteSpacingDeficit(uint64_t deficitUs)
{
    if (deficitUs != 0) {
        m_CleanSpacingFrames = 0;
        const uint64_t increaseUs = std::max(kGuardStepUs, deficitUs);
        m_GuardUs = std::min(guardCeilingUs(),
                             saturatingAdd(m_GuardUs, increaseUs));
        return;
    }

    if (m_GuardUs > m_BaseGuardUs &&
        ++m_CleanSpacingFrames >= kGuardDecayFrames) {
        m_GuardUs -= std::min(kGuardStepUs,
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
                              kLearningSampleCount);
            }
            if (m_Pending.hasPreparationDuration) {
                appendBounded(m_PreparationDurations,
                              m_Pending.preparationDurationUs,
                              kLearningSampleCount);
            }
            updateLearnedBudgets();
        }
    }

    m_Pending = PendingFrame {};
}

void VrrTimingController::updateLearnedBudgets()
{
    if (!m_ReadyOffsets.empty()) {
        const int64_t requestedUs = percentile(m_ReadyOffsets, 90);
        const int64_t ceilingUs = static_cast<int64_t>(readinessCeilingUs());
        m_ReadinessBudgetUs = std::max(
            -ceilingUs, std::min(requestedUs, ceilingUs));
    }

    if (!m_PreparationDurations.empty()) {
        m_RenderLeadUs = clampUnsigned(
            saturatingAdd(percentile(m_PreparationDurations, 90),
                          kRenderLeadSlackUs),
            renderLeadFloorUs(), renderLeadCeilingUs());
    }

    if (!m_RenderSchedulerDelays.empty()) {
        m_RenderWakeLeadUs = std::min(
            kMaximumWakeLeadUs,
            percentile(m_RenderSchedulerDelays, 95));
    }

    if (!m_TargetSchedulerDelays.empty()) {
        m_TargetWakeLeadUs = std::min(
            kMaximumTargetWakeLeadUs,
            percentile(m_TargetSchedulerDelays, 95));
    }
}

uint64_t VrrTimingController::timingBudgetUs() const
{
    const uint64_t positiveReadinessUs = m_ReadinessBudgetUs > 0 ?
        static_cast<uint64_t>(m_ReadinessBudgetUs) : 0;
    return saturatingAdd(
        positiveReadinessUs,
        saturatingAdd(m_RenderLeadUs, kPresentationSafetyUs));
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

uint64_t VrrTimingController::renderWakeLeadUs() const
{
    return m_RenderWakeLeadUs;
}

uint64_t VrrTimingController::targetWakeLeadUs() const
{
    return m_TargetWakeLeadUs;
}

uint64_t VrrTimingController::wakeLeadUs() const
{
    return std::max(m_RenderWakeLeadUs, m_TargetWakeLeadUs);
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

uint64_t VrrTimingController::renderLeadFloorUs() const
{
    return std::min(kRenderLeadFloorUs, m_SourcePeriodUs);
}

uint64_t VrrTimingController::renderLeadCeilingUs() const
{
    const uint64_t ceilingUs = std::min(kRenderLeadCeilingUs,
                                        m_SourcePeriodUs);
    return std::max(renderLeadFloorUs(), ceilingUs);
}

uint64_t VrrTimingController::readinessCeilingUs() const
{
    return std::min(kReadinessCeilingUs, m_SourcePeriodUs);
}

uint64_t VrrTimingController::guardCeilingUs() const
{
    const uint64_t sourceSlackUs = m_SourcePeriodUs > m_DisplayPeriodUs ?
        m_SourcePeriodUs - m_DisplayPeriodUs : 0;
    return std::max(
        m_BaseGuardUs,
        std::min(kMaximumAdaptiveGuardUs, sourceSlackUs));
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

uint64_t VrrTimingController::percentile(
    const std::deque<uint64_t>& values, unsigned int requestedPercentile)
{
    if (values.empty()) {
        return 0;
    }
    std::vector<uint64_t> ordered(values.begin(), values.end());
    std::sort(ordered.begin(), ordered.end());
    const unsigned int percentileValue = std::min(100U, requestedPercentile);
    const size_t rank = std::max<size_t>(
        1, (ordered.size() * percentileValue + 99) / 100);
    return ordered[rank - 1];
}

int64_t VrrTimingController::percentile(
    const std::deque<int64_t>& values, unsigned int requestedPercentile)
{
    if (values.empty()) {
        return 0;
    }
    std::vector<int64_t> ordered(values.begin(), values.end());
    std::sort(ordered.begin(), ordered.end());
    const unsigned int percentileValue = std::min(100U, requestedPercentile);
    const size_t rank = std::max<size_t>(
        1, (ordered.size() * percentileValue + 99) / 100);
    return ordered[rank - 1];
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

void VrrTimingController::appendBounded(std::deque<uint64_t>& values,
                                        uint64_t value, size_t limit)
{
    while (values.size() >= limit) {
        values.pop_front();
    }
    values.push_back(value);
}

void VrrTimingController::appendBounded(std::deque<int64_t>& values,
                                        int64_t value, size_t limit)
{
    while (values.size() >= limit) {
        values.pop_front();
    }
    values.push_back(value);
}
