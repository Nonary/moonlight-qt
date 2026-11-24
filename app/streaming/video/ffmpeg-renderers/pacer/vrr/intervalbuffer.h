#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "presenttiming.h"
#include "inputbuffer.h"
#include "cadencedetection.h"

namespace Vrr13 {
// Score each adjacent interval against its intended spacing. Historical VRR18
// feedback retained a separate score based on the one-second mean error.
class IntervalBuffer {
public:
    static constexpr uint64_t ToleranceUs = 500;
    static constexpr uint64_t ScoreBucketUs = 100000;
    static constexpr size_t MaximumScoreBuckets = 3000; // five minutes
    struct Sample {
        uint64_t frame = 0, intended = 0, submitted = 0, deadline = 0, ready = 0, buffer = 0;
        bool valid = false, absorbable = false;
        uint64_t serialService = 0, decoderQueue = 0;
        uint64_t spacingFloor = 0;
        // Sender clock after Reduce judder. A zero period selects historical replay.
        uint64_t smoothedRtpSlotUs = 0, sourcePeriodUs = 0;
        bool inputOnTime = false;
        // Producer decode readiness plus normal render service, excluding
        // intentional queue residence. Zero means no independent measurement.
        uint64_t independentReadyUs = 0;
        bool sourceInTolerance = true; // Set by the observer for attribution on the following frame.
        bool floorFollowsPrevious = false;
        uint64_t scanoutFloor = 0;
        uint64_t predecessorBufferUs = 0, predecessorAdvanceUs = 0;

        uint64_t maximumSubmissionAdvanceUs(uint64_t minimumBufferUs) const {
            if (!ready || !independentReadyUs) return 0;
            // A previous handoff can move earlier only as far as its own
            // readiness and spacing allow. Carry the fixed part of its floor
            // through successive handoffs, even when each frame was ready early.
            const uint64_t predecessorRelease = predecessorBufferUs -
                std::min(predecessorBufferUs, minimumBufferUs);
            const uint64_t floorAdvance = floorFollowsPrevious ?
                std::min({spacingFloor, predecessorAdvanceUs, predecessorRelease}) : 0;
            const uint64_t earliest = std::max({std::min(ready, independentReadyUs),
                                                scanoutFloor, spacingFloor - floorAdvance});
            return submitted - std::min(submitted, earliest);
        }
    };
    // Observation only. This explains the request made after an outcome;
    // the controller applies that request to subsequent frames. In particular,
    // rejected growth is not retained as debt or fed back into demand().
    enum class Action : uint8_t {
        Learning, SequenceBreak, Grow, Capped, CurrentPressure, HistoryHold,
        RecoveryHold, Release, Minimum, NotAbsorbable, Cooldown, NoFreshMiss, LimitChange, InputPercentileGrow, InputPercentileRelease, InputPercentileHold, InputPercentileUnknown,
        ForecastGrow
    };
    struct Update {
        uint64_t atUs = 0, frame = 0, attributedFrame = 0;
        uint64_t beforeUs = 0, requestedUs = 0, minimumUs = 0, maximumUs = 0;
        uint64_t intervalErrorUs = 0, latenessUs = 0;
        uint64_t attemptedIncreaseUs = 0, clippedIncreaseUs = 0;
        uint64_t holdRemainingUs = 0, cooldownRemainingUs = 0;
        Action action = Action::Learning;
        bool stretchedBeyondTolerance = false;
        bool clientTimingHitch = false;
    };
    struct Stats {
        double averageErrorUs = 0;
        uint64_t evaluatedUs = 0, failedUs = 0;
        double weightedLossUs = 0;
        // Reporting uses only intervals with timely input at both ends.
        uint64_t clientEvaluatedUs = 0;
        double clientWeightedLossUs = 0;
        double clientAverageErrorUs = 0;
        bool clientAverageValid = false;
        // Historical control history, retained for exact replay.
        uint64_t bufferEvaluatedUs = 0, bufferFailedUs = 0;
        double bufferWeightedLossUs = 0;
        uint64_t toleranceUs = ToleranceUs;
        uint64_t scoreWindowUs = 30000000;
        bool severityWeighted = false;
        bool averageValid = false;
        bool serviceOverloaded = false; // Qualified one-second workload, diagnostic only.
        bool initialCalibrationComplete = false;
        uint64_t calibrationCoverageUs = 0, calibrationSamples = 0;
        bool inputPercentileEnabled = false;
        InputGoalBuffer::Stats inputPercentile;
        Update update;
        uint64_t lastGrowthAtUs = 0, lastGrowthUs = 0;
        uint64_t lastClippedAtUs = 0, lastClippedUs = 0;
        // Filled by the controller; post-submission timing never changes
        // this buffer's score or requests.
        PresentTiming::Stats present;
        CadenceDetection::Stats submissionCadence, displayCadence;
        double lossFraction() const {
            return evaluatedUs ? std::clamp(
                (severityWeighted ? weightedLossUs : double(failedUs)) / evaluatedUs,
                0.0, 1.0) : 0.0;
        }
        double qualityPercent() const { return 100.0 * (1.0 - lossFraction()); }
        double clientQualityPercent() const {
            return clientEvaluatedUs ?
                100.0 * (1.0 - std::clamp(clientWeightedLossUs / clientEvaluatedUs, 0.0, 1.0)) : 0.0;
        }
        double bufferLossFraction() const {
            return bufferEvaluatedUs ? std::clamp(
                (severityWeighted ? bufferWeightedLossUs : double(bufferFailedUs)) / bufferEvaluatedUs,
                0.0, 1.0) : 0.0;
        }
        double bufferQualityPercent() const { return 100.0 * (1.0 - bufferLossFraction()); }
    };
    void observe(const Sample& s, uint64_t minimum, uint64_t maximum,
                 uint64_t hold, uint64_t releaseRate,
                 bool severityWeighted = false, uint64_t targetPerMillion = 990000,
                 uint64_t toleranceUs = 500,
                 uint64_t scoreWindowUs = 30000000,
                 uint64_t initialWarmupUs = 1000000,
                 size_t initialMinimumSamples = 2,
                 bool vrr18Feedback = false,
                 uint64_t releaseTauUs = 0,
                 bool sharedScore = false, bool requireRecoverableInterval = false,
                 bool goalFeedback = false, bool predictiveFeedback = false) {
        m_Stats.toleranceUs = toleranceUs;
        m_Stats.scoreWindowUs = scoreWindowUs;
        m_Stats.severityWeighted = severityWeighted;
        minimum = std::min(minimum, maximum);
        if (!m_Initialized) { m_Target = s.buffer; m_Initialized = true; }
        const uint64_t beforeUs = m_Target;
        m_Target = std::clamp(m_Target, minimum, maximum);
        const uint64_t boundedUs = m_Target;
        const auto previous = m_Previous;
        const bool adjacent = m_HavePrevious && s.valid && s.frame == previous.frame + 1 &&
            s.submitted > previous.submitted && s.intended > previous.intended &&
            s.submitted - previous.submitted < 1000000 && s.intended - previous.intended < 1000000;
        if (s.submitted) {
            // Keep this clock independent of m_Previous: an invalid sample may
            // replace m_Previous with a zero timestamp before the next frame.
            const bool unobservedGap = m_LastObservedSubmittedUs &&
                (s.submitted <= m_LastObservedSubmittedUs ||
                 s.submitted - m_LastObservedSubmittedUs >= 1000000);
            const bool prolongedIneligible = m_LastCleanEvidenceUs &&
                s.submitted > m_LastCleanEvidenceUs &&
                s.submitted - m_LastCleanEvidenceUs >= 2000000;
            if (unobservedGap || prolongedIneligible) {
                m_CleanEvidenceUs = 0;
                m_LastCleanEvidenceUs = 0;
            }
            m_LastObservedSubmittedUs = s.submitted;
        }
        if (!adjacent) {
            // A short dropped-frame or phase break does not erase already
            // observed clean recovery; requalification still starts afresh.
            breakSequence();
        }
        auto& update = m_Stats.update;
        update = {};
        update.atUs = s.submitted;
        update.frame = s.frame;
        update.beforeUs = beforeUs;
        update.requestedUs = m_Target;
        if (beforeUs != boundedUs) update.action = Action::LimitChange;
        update.minimumUs = minimum;
        update.maximumUs = maximum;
        m_Previous = s;
        m_HavePrevious = s.valid && s.submitted && s.intended;
        if (!adjacent) {
            // Without an incoming source interval, this frame cannot explain
            // a compressed successor as a recoverable late frame.
            // Without an adjacent predecessor there is no evidence that
            // this handoff's floor could move. Retain it for the next frame.
            if (goalFeedback) m_Previous.predecessorAdvanceUs = 0;
            else m_Previous.spacingFloor = 0;
            m_Previous.sourceInTolerance = s.sourcePeriodUs == 0;
            updateScore(s.submitted, scoreWindowUs);
            if (beforeUs == boundedUs) update.action = Action::SequenceBreak;
            return;
        }

        const auto actual = s.submitted - previous.submitted;
        const auto intended = s.intended - previous.intended;
        const auto error = std::max(actual, intended) - std::min(actual, intended);
        // One late input can stretch its incoming interval and compress its
        // outgoing interval. Require delivery evidence for both endpoints.
        const bool clientInterval = s.inputOnTime && previous.inputOnTime;
        update.intervalErrorUs = error;
        update.stretchedBeyondTolerance = actual > intended && error > toleranceUs;
        update.clientTimingHitch = clientInterval && update.stretchedBeyondTolerance;
        // Offset learning and presentation waits move the client clock. Neither
        // changes when the host produced the two frames.
        uint64_t sourceErrorUs = 0;
        if (s.sourcePeriodUs) {
            const uint64_t expected = previous.smoothedRtpSlotUs + s.sourcePeriodUs;
            sourceErrorUs = std::max(expected, s.smoothedRtpSlotUs) - std::min(expected, s.smoothedRtpSlotUs);
        }
        const bool sourceInTolerance = sourceErrorUs <= toleranceUs;
        m_Previous.sourceInTolerance = sourceInTolerance;
        if (goalFeedback && s.floorFollowsPrevious && previous.independentReadyUs) {
            m_Previous.predecessorBufferUs = previous.buffer;
            m_Previous.predecessorAdvanceUs = previous.maximumSubmissionAdvanceUs(minimum);
        }
        const auto& delayed = actual >= intended ? m_Previous : previous;
        const auto& other = actual >= intended ? previous : m_Previous;
        const auto benefit = requireRecoverableInterval && !goalFeedback ?
            intervalBenefit(delayed, other, error, toleranceUs, maximum) : Benefit{};
        const auto tick = s.submitted / 10000;
        auto& bucket = m_Window[tick % m_Window.size()];
        if (bucket.tick != tick) bucket = Bucket{tick};
        ++bucket.samples;
        bucket.total += error;
        bucket.marginalUs += benefit.nearbyUs;
        bucket.recoverableLossUs += actual * std::min(1.0, double(benefit.excessUs) / intended);
        if (clientInterval) {
            ++bucket.clientSamples;
            bucket.clientErrorUs += error;
        }
        bucket.sourceErrorUs += sourceErrorUs;
        bucket.evaluatedUs += actual;
        bucket.service += s.serialService;
        bucket.decoderQueue += s.decoderQueue;
        bucket.intended += intended;
        if (!m_First) {
            m_First = s.submitted;
            // Only a fresh session may use the shorter calibration window.
            // Keep this sequence's threshold stable after it qualifies, and
            // never rearm it on a source-rate/phase break or dropped frame.
            m_SequenceWarmupUs = m_Stats.initialCalibrationComplete ? 1000000 : initialWarmupUs;
            m_SequenceMinimumSamples = m_Stats.initialCalibrationComplete ? 2 : initialMinimumSamples;
        }
        ++m_SequenceSamples;
        uint64_t samples = 0, total = 0, service = 0, decoderQueue = 0, intendedTime = 0;
        uint64_t windowSourceErrorUs = 0;
        double recoverableLossUs = 0;
        uint64_t evaluatedUs = 0, marginalTotalUs = 0;
        uint64_t clientSamples = 0, clientErrorUs = 0;
        for (const auto& b : m_Window) {
            if (tick >= b.tick && tick - b.tick < m_Window.size()) {
                samples += b.samples; total += b.total;
                windowSourceErrorUs += b.sourceErrorUs;
                recoverableLossUs += b.recoverableLossUs;
                evaluatedUs += b.evaluatedUs;
                marginalTotalUs += b.marginalUs;
                clientSamples += b.clientSamples;
                clientErrorUs += b.clientErrorUs;
                service += b.service; decoderQueue += b.decoderQueue; intendedTime += b.intended;
            }
        }
        m_Stats.averageErrorUs = samples ? double(total) / samples : 0;
        m_Stats.clientAverageValid = clientSamples != 0;
        m_Stats.clientAverageErrorUs = clientSamples ? double(clientErrorUs) / clientSamples : 0;
        m_Stats.calibrationSamples = m_SequenceSamples;
        m_Stats.calibrationCoverageUs = s.submitted - m_First;
        m_Stats.averageValid = samples >= 2 && m_SequenceSamples >= m_SequenceMinimumSamples &&
            m_Stats.calibrationCoverageUs >= m_SequenceWarmupUs;
        if (m_Stats.averageValid) m_Stats.initialCalibrationComplete = true;
        m_Stats.serviceOverloaded = m_Stats.averageValid &&
            (service > intendedTime || decoderQueue > intendedTime);
        const bool pressure = total > samples * toleranceUs;
        auto& score = m_Score[(s.submitted / 100000) % m_Score.size()];
        if (score.tick != s.submitted / 100000) score = ScoreBucket{s.submitted / 100000};
        score.evaluated += actual;
        if (error > toleranceUs) score.failed += actual;
        const double excessUs = error > toleranceUs ? double(error - toleranceUs) : 0.0;
        const double loss = std::min(1.0, excessUs / intended);
        if (severityWeighted) score.weightedLoss += actual * loss;
        if (clientInterval) {
            score.clientEvaluatedUs += actual;
            score.clientWeightedLossUs += actual * loss;
        }
        const double bufferExcessUs = vrr18Feedback ?
            std::max(0.0, m_Stats.averageErrorUs - toleranceUs) : excessUs;
        const double bufferLoss = std::min(1.0, bufferExcessUs / intended);
        if (!sharedScore && (!vrr18Feedback || m_Stats.averageValid)) {
            score.bufferEvaluated += actual;
            if (bufferExcessUs > 0) score.bufferFailed += actual;
            if (severityWeighted) score.bufferWeightedLoss += actual * bufferLoss;
        }
        updateScore(s.submitted, scoreWindowUs);

        const bool steadySource = sourceInTolerance && previous.sourceInTolerance &&
            windowSourceErrorUs <= samples * toleranceUs;
        if (goalFeedback) {
            m_Reserve.expire(s.submitted, scoreWindowUs);
            if (s.absorbable && previous.absorbable && service <= intendedTime &&
                    decoderQueue <= intendedTime && s.independentReadyUs && previous.independentReadyUs) {
                m_Reserve.observe(previous, m_Previous, intended, minimum, maximum, toleranceUs,
                                  steadySource, targetPerMillion, hold);
            }
        }
        if (!m_Stats.averageValid) return;

        const double allowedLoss = (1000000 - std::min<uint64_t>(targetPerMillion, 1000000)) / 1000000.0;
        const double scoreLoss = sharedScore ? m_Stats.lossFraction() : m_Stats.bufferLossFraction();
        const bool belowTarget = scoreLoss > allowedLoss;
        const bool freshError = severityWeighted ? error > toleranceUs : error != 0;
        const bool sourceQualified = sourceInTolerance && delayed.sourceInTolerance &&
            windowSourceErrorUs <= samples * toleranceUs;
        const bool useDeliveryEvidence = requireRecoverableInterval && !sourceQualified;
        const bool deliveryPressure = marginalTotalUs > 0 && recoverableLossUs > allowedLoss * evaluatedUs;
        const bool sustainedPressure = goalFeedback ? m_Reserve.targetUs > m_Target : useDeliveryEvidence ? deliveryPressure :
            (severityWeighted ? bufferLoss > allowedLoss : pressure);
        // Old score debt holds protection, but cannot authorize another attack
        // without current, attributable error outside the preset's allowance.
        const bool historicalPressure = severityWeighted && belowTarget;
        const bool historyHolds = !vrr18Feedback && historicalPressure;
        const auto deadline = std::max(delayed.deadline, delayed.spacingFloor);
        const auto lateness = delayed.ready - std::min(delayed.ready, deadline);
        const bool windowAbsorbable = service <= intendedTime && decoderQueue <= intendedTime;
        uint64_t forecastUs = 0;
        if (predictiveFeedback) {
            if (steadySource && windowAbsorbable && s.absorbable && previous.absorbable &&
                    s.independentReadyUs && previous.independentReadyUs) {
                // A late input may stretch one pair and compress the following pair.
                // Count its recoverable stretch once. Every observed pair, including
                // a covered hitch, still contributes time to the loss allowance.
                const Benefit stretch = actual > intended ?
                    intervalBenefit(m_Previous, previous, error, toleranceUs, maximum) : Benefit{};
                forecastUs = m_Forecast.observe(actual, intended, stretch, allowedLoss,
                    std::min(previous.buffer, s.buffer), toleranceUs);
            }
            else m_Forecast = {};
        }
        const auto headroom = maximum - std::min(maximum, delayed.buffer);
        const bool withinBudget = !requireRecoverableInterval ||
            lateness - std::min(lateness, toleranceUs) <= headroom;
        const auto freshBenefitUs = goalFeedback ?
            freshReadinessBenefit(delayed, other, error, toleranceUs, maximum, intended) : 0;
        const bool corroborated = freshBenefitUs && delayed.absorbable &&
            m_Reserve.corroborates(s.submitted, delayed.frame);
        const bool delayedAbsorbable = delayed.absorbable && windowAbsorbable && withinBudget &&
            (goalFeedback ? corroborated :
             useDeliveryEvidence ? benefit.excessUs != 0 : sourceQualified);
        const bool targetAllowsHold = !severityWeighted || belowTarget;
        const bool pressureHolds = (vrr18Feedback || targetAllowsHold) && sustainedPressure &&
            (freshError && delayedAbsorbable && lateness);
        const bool uncertainRelease = goalFeedback && belowTarget && windowAbsorbable &&
            freshError && delayed.absorbable && delayed.independentReadyUs > deadline &&
            lateness > toleranceUs && bufferLoss > allowedLoss;
        const uint64_t releaseTarget = m_Reserve.targetUs > minimum ? m_Reserve.targetUs +
            std::min(toleranceUs, maximum - std::min(maximum, m_Reserve.targetUs)) : minimum;
        const bool goalHold = goalFeedback && m_Target <= releaseTarget;
        const bool holdProtection = goalHold || uncertainRelease || pressureHolds ||
            (targetAllowsHold && historyHolds);
        const bool renewsHold = (goalFeedback ? pressureHolds || uncertainRelease : holdProtection) &&
            (!severityWeighted || belowTarget);
        if (renewsHold) {
            m_LastPressure = s.submitted;
            if (severityWeighted) m_ReleaseFraction = 0;
        }
        {
            if (renewsHold || ((vrr18Feedback || targetAllowsHold) &&
                               (!s.absorbable || !windowAbsorbable))) {
                m_CleanEvidenceUs = 0;
                m_LastCleanEvidenceUs = 0;
            }
            else if (!holdProtection && s.absorbable && windowAbsorbable) {
                m_CleanEvidenceUs = std::min<uint64_t>(
                    hold, m_CleanEvidenceUs + std::min<uint64_t>(actual, 100000));
                m_LastCleanEvidenceUs = s.submitted;
            }
        }
        update.attributedFrame = delayed.frame;
        update.latenessUs = lateness;
        const auto remaining = [](uint64_t elapsed, uint64_t duration) {
            return elapsed < duration ? duration - elapsed : 0;
        };
        update.holdRemainingUs = std::max(
            remaining(m_CleanEvidenceUs, hold),
            m_LastPressure ? remaining(s.submitted - m_LastPressure, hold) : 0);
        update.cooldownRemainingUs = !goalFeedback && m_LastAttack ?
            remaining(s.submitted - m_LastAttack, 250000) : 0;
        update.action = pressureHolds || goalHold ? Action::CurrentPressure :
            (targetAllowsHold && historyHolds) ? Action::HistoryHold : Action::RecoveryHold;
        const bool grow = sustainedPressure && (goalFeedback || !severityWeighted || belowTarget);
        if (grow && freshError && delayedAbsorbable && lateness &&
                (goalFeedback || !m_LastAttack || s.submitted - m_LastAttack >= 250000)) {
            const auto excess = useDeliveryEvidence ?
                (marginalTotalUs + samples - 1) / samples :
                severityWeighted ?
                uint64_t(std::ceil(std::min(250.0, std::max(0.0, bufferExcessUs - allowedLoss * intended)))) :
                (total - samples * toleranceUs + samples - 1) / samples;
            const auto freshExcess = useDeliveryEvidence ? benefit.excessUs :
                severityWeighted ? error - toleranceUs : error;
            const auto base = std::min(delayed.buffer, maximum);
            const auto increase = goalFeedback ?
                std::min(m_Reserve.targetUs - std::min(m_Reserve.targetUs, base), lateness) :
                std::min({uint64_t(250), excess, lateness, severityWeighted ? freshExcess : uint64_t(250)});
            update.attemptedIncreaseUs = increase;
            update.clippedIncreaseUs = increase - std::min(increase, maximum - base);
            if (update.clippedIncreaseUs) {
                m_Stats.lastClippedAtUs = s.submitted;
                m_Stats.lastClippedUs = update.clippedIncreaseUs;
            }
            m_Target = std::max(m_Target, base + std::min(increase, maximum - base));
            update.action = update.clippedIncreaseUs ? Action::Capped :
                m_Target > boundedUs ? Action::Grow : Action::CurrentPressure;
            if (m_Target > boundedUs) {
                if (goalFeedback) {
                    m_LastPressure = s.submitted;
                    m_CleanEvidenceUs = 0;
                }
                m_Stats.lastGrowthAtUs = s.submitted;
                m_Stats.lastGrowthUs = m_Target - boundedUs;
            }
            m_LastAttack = s.submitted;
            m_ReleaseFraction = 0;
        }
        else if (!holdProtection && s.absorbable &&
                (windowAbsorbable && m_CleanEvidenceUs >= hold) &&
                (!m_LastPressure || s.submitted - m_LastPressure >= hold)) {
            uint64_t rate = releaseRate;
            if (releaseTauUs) {
                const uint64_t excessUs = m_Target - minimum;
                // Leave enough time to observe pressure near the floor
                // before shedding the remaining protection in one pass.
                if (excessUs <= rate * releaseTauUs / 1000000)
                    rate = (excessUs * 1000000 + releaseTauUs - 1) / releaseTauUs;
            }
            m_ReleaseFraction += std::min<uint64_t>(actual, 100000) * rate;
            const auto release = m_ReleaseFraction / 1000000;
            m_ReleaseFraction %= 1000000;
            const auto candidate = m_Target - std::min(m_Target - minimum, release);
            m_Target = goalFeedback ? std::max(candidate, std::min(m_Target, releaseTarget)) : candidate;
            update.action = m_Target == minimum ? Action::Minimum : Action::Release;
        }
        else if (grow) {
            update.action = !delayedAbsorbable ? Action::NotAbsorbable :
                !freshError || !lateness ? Action::NoFreshMiss : Action::Cooldown;
        }
        else if (!holdProtection &&
                 !(s.absorbable &&
                   (windowAbsorbable))) {
            update.action = Action::NotAbsorbable;
        }
        forecastUs = std::min(forecastUs, maximum);
        if (belowTarget && forecastUs > m_Target && forecastUs > boundedUs) {
            m_Target = forecastUs;
            m_LastPressure = s.submitted;
            m_CleanEvidenceUs = m_ReleaseFraction = 0;
            m_Stats.lastGrowthAtUs = s.submitted;
            m_Stats.lastGrowthUs = m_Target - boundedUs;
            update.action = Action::ForecastGrow;
            update.attributedFrame = 0;
            update.latenessUs = 0;
            update.holdRemainingUs = hold;
            update.attemptedIncreaseUs = m_Target - boundedUs;
            update.clippedIncreaseUs = 0;
        }
        if (beforeUs != boundedUs) update.action = Action::LimitChange;
        update.requestedUs = m_Target;
    }
    uint64_t demand(uint64_t applied) const { return m_Initialized ? m_Target : applied; }
    // Replace the standing target at a source epoch and protect it for one
    // hold from the given time, as if pressure had just been observed.
    void restoreTarget(uint64_t target, uint64_t atUs) {
        if (!m_Initialized) return;
        m_Forecast = {};
        m_Target = target;
        m_LastPressure = atUs;
        m_ReleaseFraction = 0;
    }
    Stats stats() const { return m_Stats; }
    void breakSequence() {
        m_Forecast = {};
        m_Window = {}; m_First = m_ReleaseFraction = m_SequenceSamples = 0; m_HavePrevious = false;
        m_Stats.averageErrorUs = 0; m_Stats.averageValid = false;
        m_Stats.clientAverageErrorUs = 0; m_Stats.clientAverageValid = false;
        m_Stats.calibrationCoverageUs = m_Stats.calibrationSamples = 0;
        m_Stats.update = {};
        m_Stats.update.action = Action::SequenceBreak;
    }
    void reset() { *this = IntervalBuffer{}; }
private:
    struct Benefit {
        uint64_t excessUs = 0;  // Error outside tolerance that the remaining buffer can cover.
        uint64_t nearbyUs = 0;  // Part of that error covered by one tolerance of extra buffer.
    };

    // Forecast the reserve for the next second from the measured cost of
    // recurring stretches. Each completed window makes one absolute proposal.
    // Taking the smaller of two consecutive proposals keeps one burst from
    // buying a standing reserve and prevents spending the same evidence twice.
    struct HitchForecast {
        uint64_t observe(uint64_t actualUs, uint64_t intendedUs, Benefit stretch,
                         double allowedLoss, uint64_t appliedUs, uint64_t stepUs) {
            minimumAppliedUs = elapsedUs ? std::min(minimumAppliedUs, appliedUs) : appliedUs;
            elapsedUs += actualUs;
            // Use the same duration weighting and one-interval loss ceiling as
            // the observed pacing score, including when a hitch saturates it.
            const double weight = double(actualUs) / intendedUs;
            const uint64_t before = std::min(intendedUs, stretch.excessUs);
            const uint64_t after = std::min(intendedUs, stretch.excessUs - stretch.nearbyUs);
            lossUs += weight * before;
            improvementUs += weight * (before - after);
            if (elapsedUs < 1000000) return 0;

            const double shortfallUs = lossUs - allowedLoss * elapsedUs;
            uint64_t proposalUs = 0;
            if (shortfallUs > 0 && improvementUs > 0) {
                const double fraction = std::min(1.0, shortfallUs / improvementUs);
                proposalUs = minimumAppliedUs + uint64_t(std::ceil(stepUs * fraction));
            }
            const uint64_t forecastUs = std::min(previousProposalUs, proposalUs);
            previousProposalUs = proposalUs;
            elapsedUs = 0;
            lossUs = improvementUs = 0;
            return forecastUs;
        }

    private:
        uint64_t elapsedUs = 0, minimumAppliedUs = 0, previousProposalUs = 0;
        double lossUs = 0, improvementUs = 0;
    };

    static uint64_t coveredReadiness(const Sample& sample, uint64_t extraUs) {
        const auto ready = std::min(sample.ready, sample.independentReadyUs);
        const auto deadline = std::max(sample.deadline, sample.spacingFloor);
        const auto floorLead = deadline - sample.deadline;
        const auto usable = extraUs - std::min(extraUs, floorLead);
        return std::min(usable, ready - std::min(ready, deadline));
    }

    // Both deadlines move together. Equal lateness at the two endpoints
    // leaves their spacing unchanged. Any residual from host cadence,
    // scheduler wakeups or present waits stays in the measured handoff error.
    static Benefit intervalBenefit(const Sample& delayed, const Sample& other,
                                    uint64_t error, uint64_t tolerance, uint64_t maximum) {
        if (error <= tolerance || !delayed.absorbable ||
                !delayed.independentReadyUs || !other.independentReadyUs) return {};
        const auto headroom = maximum - std::min(maximum, std::max(delayed.buffer, other.buffer));
        const auto deadline = std::max(delayed.deadline, delayed.spacingFloor);
        const auto lateness = delayed.ready - std::min(delayed.ready, deadline);
        if (lateness - std::min(lateness, tolerance) > headroom) return {};
        const auto improvement = [&](uint64_t extraUs) {
            const auto late = coveredReadiness(delayed, extraUs);
            return late - std::min(late, coveredReadiness(other, extraUs));
        };
        const auto excess = error - tolerance;
        if (improvement(headroom) < excess) return {};
        return {excess, std::min(excess, improvement(std::min(headroom, tolerance)))};
    }

    static uint64_t freshReadinessBenefit(const Sample& delayed, const Sample& other,
                                          uint64_t error, uint64_t tolerance,
                                          uint64_t maximum, uint64_t intended) {
        if (error <= tolerance || !delayed.independentReadyUs || !other.independentReadyUs) return 0;
        const auto headroom = maximum - std::min(maximum, std::max(delayed.buffer, other.buffer));
        const auto late = coveredReadiness(delayed, headroom);
        const auto early = coveredReadiness(other, headroom);
        const auto excess = error - tolerance;
        const auto remaining = excess - std::min(excess, late - std::min(late, early));
        return std::min(excess, intended) - std::min(remaining, intended);
    }

    // Integrate only adjacent, independently timed input outcomes. The shadow
    // reserve asks how much lateness would survive at a given standing delay.
    // Nothing is purchased until actual handoff errors corroborate that need.
    struct ReadinessReserve {
        uint64_t targetUs = 0;

        void expire(uint64_t nowUs, uint64_t historyUs) {
            if (lastObservedUs && (nowUs < lastObservedUs || nowUs - lastObservedUs >= historyUs)) {
                *this = {};
            }
        }

        bool corroborates(uint64_t nowUs, uint64_t frame) {
            // A missed deadline can spoil both neighboring intervals. Require
            // two distinct late frames before increasing standing latency.
            if (frame != lastMissFrame) {
                previousMissUs = lastMissUs;
                lastMissUs = nowUs;
                lastMissFrame = frame;
            }
            return previousMissUs && nowUs >= previousMissUs && nowUs - previousMissUs <= 2000000;
        }

        static double lateUs(const Sample& frame, double reserveUs) {
            const uint64_t readyUs = std::min(frame.ready, frame.independentReadyUs);
            const uint64_t unbufferedUs = frame.deadline - std::min(frame.buffer, frame.deadline);
            const double reductionUs = std::min(double(frame.predecessorAdvanceUs),
                std::max(0.0, double(frame.predecessorBufferUs) - reserveUs));
            const double floorUs = std::max(double(frame.scanoutFloor), double(frame.spacingFloor) - reductionUs);
            return std::max(0.0, double(readyUs) - std::max(double(unbufferedUs) + reserveUs, floorUs));
        }

        void observe(const Sample& previous, const Sample& current, uint64_t intendedUs,
                     uint64_t minimumUs, uint64_t maximumUs, uint64_t toleranceUs,
                     bool steadySource, uint64_t targetPpm, uint64_t holdUs) {
            estimateUs = std::clamp(estimateUs, double(minimumUs), double(maximumUs));
            const auto lossUs = [&](double reserveUs) {
                const double difference = std::abs(lateUs(current, reserveUs) - lateUs(previous, reserveUs));
                return std::clamp(difference - toleranceUs, 0.0, double(intendedUs));
            };
            const uint64_t actualUs = current.submitted - previous.submitted;
            const bool stretched = actualUs >= intendedUs;
            const auto measuredUs = freshReadinessBenefit(stretched ? current : previous,
                stretched ? previous : current, std::max(actualUs, intendedUs) - std::min(actualUs, intendedUs),
                toleranceUs, maximumUs, intendedUs);
            const double appliedUs = double(std::min(current.buffer, previous.buffer));
            const double remainingUs = std::clamp(measuredUs + appliedUs - estimateUs, 0.0, double(measuredUs));
            const double capLossUs = lossUs(maximumUs);
            const double predictedUs = std::max(0.0, lossUs(estimateUs) - capLossUs);
            const bool withinCap = std::max(lateUs(previous, maximumUs), lateUs(current, maximumUs)) <= toleranceUs;
            double needUs = 0;
            if (withinCap) {
                needUs = steadySource ? std::max(predictedUs, remainingUs) :
                    std::min(predictedUs, remainingUs + predictedUs / 2);
            }

            const double elapsedUs = lastObservedUs && current.submitted > lastObservedUs ?
                double(std::min<uint64_t>(200000, current.submitted - lastObservedUs)) : 0.0;
            lastObservedUs = current.submitted;
            const bool coveredAtMinimum = lossUs(minimumUs) <= capLossUs;
            if (!coveredAtMinimum && withinCap) lastNeedUs = current.submitted;
            const bool clean = coveredAtMinimum && !needUs && current.submitted - lastNeedUs >= holdUs;
            if (clean) estimateUs = std::max(double(minimumUs), estimateUs - 0.002 * intendedUs);

            // A tolerance of growth per second buys repeated costly losses
            // promptly without acquiring a whole frame for an isolated burst.
            // The observed pairs cover only independently demonstrable input
            // loss, so reserve part of the profile budget for the other stages.
            const double budgetUs = double(1000000 - std::min<uint64_t>(targetPpm, 1000000)) * intendedUs / 10000000;
            const double burstUs = double(toleranceUs) / 2;
            riseCreditUs = std::min(burstUs, riseCreditUs + double(toleranceUs) * elapsedUs / 1000000);
            const double changeUs = std::clamp((needUs - budgetUs) / 12, -burstUs, riseCreditUs);
            riseCreditUs -= std::max(0.0, changeUs);
            estimateUs = std::clamp(estimateUs + changeUs, double(minimumUs), double(maximumUs));

            if (clean || !smoothedUs) smoothedUs = clean ? estimateUs : double(minimumUs);
            else smoothedUs += (estimateUs - smoothedUs) * std::min(1.0, elapsedUs / std::max(1000000.0, double(holdUs)));
            const uint64_t stepUs = std::max<uint64_t>(1, toleranceUs / 4);
            const double excessUs = std::max(0.0, smoothedUs - minimumUs);
            targetUs = minimumUs + std::min(maximumUs - minimumUs, uint64_t(std::ceil(excessUs / stepUs)) * stepUs);
        }

    private:
        double estimateUs = 0, smoothedUs = 0, riseCreditUs = 0;
        uint64_t lastObservedUs = 0, lastNeedUs = 0;
        uint64_t lastMissFrame = 0, lastMissUs = 0, previousMissUs = 0;
    };

    struct Bucket {
        uint64_t tick = 0, samples = 0, total = 0;
        uint64_t service = 0, decoderQueue = 0, intended = 0, sourceErrorUs = 0;
        uint64_t clientSamples = 0, clientErrorUs = 0, evaluatedUs = 0, marginalUs = 0;
        double recoverableLossUs = 0;
    };
    struct ScoreBucket {
        uint64_t tick = 0, evaluated = 0, failed = 0;
        double weightedLoss = 0;
        uint64_t clientEvaluatedUs = 0;
        double clientWeightedLossUs = 0;
        uint64_t bufferEvaluated = 0, bufferFailed = 0;
        double bufferWeightedLoss = 0;
    };
    void updateScore(uint64_t at, uint64_t windowUs) {
        m_Stats.evaluatedUs = m_Stats.failedUs = 0;
        m_Stats.weightedLossUs = 0;
        m_Stats.clientEvaluatedUs = 0;
        m_Stats.clientWeightedLossUs = 0;
        m_Stats.bufferEvaluatedUs = m_Stats.bufferFailedUs = 0;
        m_Stats.bufferWeightedLossUs = 0;
        const uint64_t windowBuckets = std::clamp<uint64_t>(
            (windowUs + ScoreBucketUs - 1) / ScoreBucketUs,
            1, MaximumScoreBuckets);
        const uint64_t tick = at / ScoreBucketUs;
        for (const auto& b : m_Score) {
            if (tick >= b.tick && tick - b.tick < windowBuckets) {
                m_Stats.evaluatedUs += b.evaluated; m_Stats.failedUs += b.failed;
                m_Stats.weightedLossUs += b.weightedLoss;
                m_Stats.clientEvaluatedUs += b.clientEvaluatedUs;
                m_Stats.clientWeightedLossUs += b.clientWeightedLossUs;
                m_Stats.bufferEvaluatedUs += b.bufferEvaluated;
                m_Stats.bufferFailedUs += b.bufferFailed;
                m_Stats.bufferWeightedLossUs += b.bufferWeightedLoss;
            }
        }
    }
    ReadinessReserve m_Reserve;
    HitchForecast m_Forecast;
    std::array<Bucket, 100> m_Window{};
    // Keep the long score history off the controller's stack. The controller
    // is instantiated in several independent workers and test fixtures.
    std::vector<ScoreBucket> m_Score = std::vector<ScoreBucket>(MaximumScoreBuckets);
    Sample m_Previous;
    Stats m_Stats;
    uint64_t m_Target = 0, m_First = 0, m_LastAttack = 0, m_LastPressure = 0, m_ReleaseFraction = 0;
    uint64_t m_CleanEvidenceUs = 0, m_LastObservedSubmittedUs = 0, m_LastCleanEvidenceUs = 0;
    uint64_t m_SequenceWarmupUs = 1000000;
    uint64_t m_SequenceSamples = 0;
    size_t m_SequenceMinimumSamples = 2;
    bool m_HavePrevious = false, m_Initialized = false;
};
}
