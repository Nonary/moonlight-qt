#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "presenttiming.h"
#include "inputbuffer.h"
#include "cadencedetection.h"
#include "intervalscore.h"

// Recorded values of playout_vrr18_interval_feedback.
constexpr uint64_t Vrr18AveragedIntervalFeedback = 1;
constexpr uint64_t Vrr18SharedIntervalScore = 2;
constexpr uint64_t Vrr18SourceCadenceFeedback = 3;
constexpr uint64_t Vrr18RecoverableIntervalFeedback = 4;
constexpr uint64_t Vrr18GoalIntervalFeedback = 5;
constexpr uint64_t Vrr18PredictiveIntervalFeedback = 6;
constexpr uint64_t Vrr18CostAwareIntervalFeedback = 7;
constexpr uint64_t Vrr18AnticipatoryIntervalFeedback = 8;
constexpr uint64_t Vrr18TargetAwareRelease = 9;
constexpr uint64_t Vrr18ScoreQualifiedRelease = 10;
constexpr uint64_t Vrr18ProfileReleaseBudget = 11;
constexpr uint64_t Vrr18SelectedGoalRelease = 12;
constexpr uint64_t Vrr18ZeroReserve = 13;
constexpr uint64_t Vrr18FrameBudget = 14;
constexpr uint64_t Vrr18CeilingBuffer = 15;
constexpr uint64_t Vrr18CeilingReleaseHistory = 16;
constexpr uint64_t Vrr18ProgressiveCeilingRelease = 17;
constexpr uint64_t Vrr18CeilingForecastBudget = 18;
constexpr uint64_t Vrr18FrameHitchForecast = 19;

namespace Vrr13 {
// Score each adjacent interval against its intended spacing. Historical VRR18
// feedback retained a separate score based on the one-second mean error.
class IntervalBuffer {
public:
    static constexpr uint64_t ToleranceUs = 500;
    static constexpr uint64_t ShortWindowUs = 1000000;
    static constexpr uint64_t ShortBucketUs = 10000;
    static constexpr uint64_t ScoreBucketUs = IntervalScoreBucketUs;
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
        bool recoverableHitch = false;
        bool incomingStretch = false;
        uint64_t sourceIntervalUs = 0;

        uint64_t independentLatenessUs() const {
            const auto unbufferedUs = deadline - std::min(deadline, buffer);
            return independentReadyUs - std::min(independentReadyUs, unbufferedUs);
        }

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
        uint64_t intendedIntervalUs = 0, intervalErrorUs = 0, latenessUs = 0;
        uint64_t attemptedIncreaseUs = 0, clippedIncreaseUs = 0;
        uint64_t holdRemainingUs = 0, cooldownRemainingUs = 0;
        Action action = Action::Learning;
        bool stretchedBeyondTolerance = false;
        bool clientInterval = false;
        bool clientTimingHitch = false;
        bool qualifiedStartDelay = false;
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
                 bool goalFeedback = false, bool predictiveFeedback = false,
                 bool costAwareFeedback = false, bool anticipatoryFeedback = false,
                 bool targetAwareRelease = false, uint64_t releaseRevision = 0, bool canQualifyRelease = true,
                 bool latchedCeiling = false, bool priceVariableCadence = false) {
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
            advanceScore(s.submitted, scoreWindowUs);
            publishScore(m_Score.totals());
            if (beforeUs == boundedUs) update.action = Action::SequenceBreak;
            return;
        }

        const auto actual = s.submitted - previous.submitted;
        const auto intended = s.intended - previous.intended;
        const auto error = std::max(actual, intended) - std::min(actual, intended);
        // One late input can stretch its incoming interval and compress its
        // outgoing interval. Require delivery evidence for both endpoints.
        const bool clientInterval = s.inputOnTime && previous.inputOnTime;
        update.intendedIntervalUs = intended;
        update.clientInterval = clientInterval;
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
        const bool requireHitch = releaseRevision >= Vrr18FrameHitchForecast;
        if (requireHitch && s.smoothedRtpSlotUs > previous.smoothedRtpSlotUs)
            m_Previous.sourceIntervalUs = s.smoothedRtpSlotUs - previous.smoothedRtpSlotUs;
        if (requireHitch && update.stretchedBeyondTolerance) {
            m_Previous.incomingStretch = true;
            // A long sender gap followed by a compensating short one can look
            // like late delivery on the short slot. If that source movement
            // explains the entire excess and the handoff was already as fast
            // as the fitted cadence, chasing the short slot recreates the gap.
            const auto excess = error - toleranceUs;
            const auto period = s.sourcePeriodUs;
            const bool sourceCatchup = m_Previous.sourceIntervalUs && period > m_Previous.sourceIntervalUs &&
                period - m_Previous.sourceIntervalUs >= excess &&
                previous.sourceIntervalUs > period && previous.sourceIntervalUs - period >= excess &&
                actual - std::min(actual, period) <= toleranceUs;
            m_Previous.recoverableHitch = !sourceCatchup && canCoverInterval(previous, s, excess, maximum);
        }
        // A spacing floor can conceal the incoming stretch. In that case the
        // compressed successor can still measure the cost of the earlier late
        // frame, but cannot corroborate another incoming hitch.
        const bool recoverableHitch = !requireHitch || delayed.recoverableHitch ||
            (actual < intended && error > toleranceUs && !previous.incomingStretch &&
             canCoverInterval(s, previous, error - toleranceUs, maximum));
        const auto benefit = requireRecoverableInterval && !goalFeedback ?
            intervalBenefit(delayed, other, error, toleranceUs, maximum) : Benefit{};
        const auto tick = s.submitted / ShortBucketUs;
        m_Window.advance(tick, ShortWindowBuckets);
        auto& bucket = m_Window.current();
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
        const auto window = m_Window.totals();
        m_Stats.averageErrorUs = window.samples ? double(window.total) / window.samples : 0;
        m_Stats.clientAverageValid = window.clientSamples != 0;
        m_Stats.clientAverageErrorUs = window.clientSamples ? double(window.clientErrorUs) / window.clientSamples : 0;
        m_Stats.calibrationSamples = m_SequenceSamples;
        m_Stats.calibrationCoverageUs = s.submitted - m_First;
        m_Stats.averageValid = window.samples >= 2 && m_SequenceSamples >= m_SequenceMinimumSamples &&
            m_Stats.calibrationCoverageUs >= m_SequenceWarmupUs;
        if (m_Stats.averageValid) m_Stats.initialCalibrationComplete = true;
        m_Stats.serviceOverloaded = m_Stats.averageValid &&
            (window.service > window.intended || window.decoderQueue > window.intended);
        const bool pressure = window.total > window.samples * toleranceUs;
        advanceScore(s.submitted, scoreWindowUs);
        auto& score = m_Score.current();
        score.evaluated += actual;
        if (error > toleranceUs) score.failed += actual;
        const double excessUs = error > toleranceUs ? double(error - toleranceUs) : 0.0;
        const double loss = intervalLossFraction(error, intended, toleranceUs);
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
        publishScore(m_Score.totals());

        const bool steadySource = sourceInTolerance && previous.sourceInTolerance &&
            window.sourceErrorUs <= window.samples * toleranceUs;
        if (goalFeedback) {
            m_Reserve.expire(s.submitted, scoreWindowUs);
            if (s.absorbable && previous.absorbable && window.service <= window.intended &&
                    window.decoderQueue <= window.intended && s.independentReadyUs && previous.independentReadyUs) {
                m_Reserve.observe(previous, m_Previous, intended, minimum, maximum, toleranceUs,
                                  steadySource, targetPerMillion, hold, releaseRevision >= Vrr18ZeroReserve);
            }
        }
        if (!m_Stats.averageValid) return;

        const double allowedLoss = (1000000 - std::min<uint64_t>(targetPerMillion, 1000000)) / 1000000.0;
        double scoreLoss = sharedScore ? m_Stats.lossFraction() : m_Stats.bufferLossFraction();
        if (severityWeighted && nearThreshold(scoreLoss, allowedLoss, MaximumScoreBuckets)) {
            publishScore(m_Score.orderedTotals());
            scoreLoss = sharedScore ? m_Stats.lossFraction() : m_Stats.bufferLossFraction();
        }
        const bool belowTarget = scoreLoss > allowedLoss;
        // Keep one percentage point of margin (the Low Latency preset's
        // allowance). A relaxed goal can reclaim delay faster only with
        // room in both its scored history and the most recent second.
        // Charging the full recent error is conservative during a transition.
        if (targetAwareRelease && allowedLoss > 0.01 && window.intended) {
            const double recentError = double(window.total) / double(window.intended);
            const double surplus = allowedLoss - std::max(scoreLoss, recentError);
            if (surplus > 0.01) {
                const double speed = std::min(10.0, surplus / 0.01);
                hold = std::min(hold, std::max<uint64_t>(1000000, uint64_t(hold / speed)));
                releaseRate = std::min<uint64_t>(1000, uint64_t(releaseRate * speed));
                releaseTauUs = std::min(releaseTauUs, std::max<uint64_t>(1000000, uint64_t(releaseTauUs / speed)));
            }
        }
        const bool freshError = severityWeighted ? error > toleranceUs : error != 0;
        const bool sourceQualified = sourceInTolerance && delayed.sourceInTolerance &&
            window.sourceErrorUs <= window.samples * toleranceUs;
        const bool useDeliveryEvidence = requireRecoverableInterval && !sourceQualified;
        const double allowedRecoveryLossUs = allowedLoss * window.evaluatedUs;
        const bool deliveryPressure = window.marginalUs > 0 &&
            (nearThreshold(window.recoverableLossUs, allowedRecoveryLossUs, ShortWindowBuckets) ?
             m_Window.orderedTotals().recoverableLossUs : window.recoverableLossUs) > allowedRecoveryLossUs;
        // Old score debt holds protection, but cannot authorize another attack
        // without current, attributable error outside the preset's allowance.
        const bool historicalPressure = severityWeighted && belowTarget;
        const bool historyHolds = !vrr18Feedback && historicalPressure;
        const auto deadline = std::max(delayed.deadline, delayed.spacingFloor);
        const auto lateness = delayed.ready - std::min(delayed.ready, deadline);
        const bool windowAbsorbable = window.service <= window.intended && window.decoderQueue <= window.intended;
        uint64_t forecastUs = 0;
        if (costAwareFeedback) {
            if (windowAbsorbable && s.absorbable && previous.absorbable &&
                    s.independentReadyUs && previous.independentReadyUs) {
                const bool cadenceBounded = window.sourceErrorUs <= 2 * window.samples * toleranceUs;
                forecastUs = m_ReserveCosts.observe(delayed, other, actual, intended,
                    error, toleranceUs, maximum, allowedLoss, steadySource, cadenceBounded,
                    anticipatoryFeedback && !belowTarget, releaseRevision >= Vrr18FrameBudget,
                    std::max(0.0, allowedLoss * m_Stats.evaluatedUs - m_Stats.weightedLossUs),
                    latchedCeiling && releaseRevision >= Vrr18CeilingForecastBudget,
                    recoverableHitch);
            }
            else m_ReserveCosts = {};
        }
        else if (predictiveFeedback) {
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
        // A direct readiness response already prices both endpoints within the
        // cap. An interval that exceeds even the full remaining movement cannot
        // corroborate that estimate or renew its pressure hold.
        const bool fitsAllowance = !requireHitch ||
            (error - std::min(error, toleranceUs) <= headroom && (steadySource || recoverableHitch));
        const bool corroborated = fitsAllowance && freshBenefitUs && delayed.absorbable &&
            m_Reserve.corroborates(s.submitted, delayed.frame);
        const bool delayedAbsorbable = delayed.absorbable && windowAbsorbable && withinBudget &&
            (goalFeedback ? corroborated :
             useDeliveryEvidence ? benefit.excessUs != 0 : sourceQualified);
        uint64_t releaseTarget = m_Reserve.targetUs > minimum ? m_Reserve.targetUs +
            std::min(toleranceUs, maximum - std::min(maximum, m_Reserve.targetUs)) : minimum;
        uint64_t approvedReleaseUs = maximum;
        bool budgetCoversReserve = false;
        bool qualifiedAtCurrentReserve = false;
        auto readinessTarget = m_Reserve.targetUs;
        // Software spacing can conceal late readiness. Require independent
        // input to fit the unbuffered slot before qualifying a release there.
        const auto readyAtMinimum = [&](const Sample& frame) {
            const auto lateUs = frame.independentLatenessUs();
            return lateUs - std::min(lateUs, minimum) <= toleranceUs;
        };
        const bool cleanSoftwareInput = releaseRevision >= Vrr18FrameBudget && targetAwareRelease &&
            !canQualifyRelease && readyAtMinimum(previous) && readyAtMinimum(s);
        // A variable-rate pair can qualify a release against its own intended
        // interval. Price both constrained handoffs and independent readiness,
        // as at the ceiling, before spending the measured score allowance.
        const bool priceSourceInterval = steadySource || priceVariableCadence ||
            (latchedCeiling && releaseRevision >= Vrr18CeilingReleaseHistory);
        if ((canQualifyRelease || cleanSoftwareInput) && releaseRevision >= Vrr18ScoreQualifiedRelease && minimum < maximum && priceSourceInterval && windowAbsorbable &&
                s.absorbable && previous.absorbable && s.independentReadyUs && previous.independentReadyUs &&
                allowedLoss > 0 && (!belowTarget || releaseRevision >= Vrr18SelectedGoalRelease)) {
            // Historical revisions projected release against a fraction of a preset budget.
            // The live policy uses the same loss allowance as the measured pacing score.
            const bool selectedGoal = releaseRevision >= Vrr18SelectedGoalRelease;
            const double historicalCap = releaseRevision >= Vrr18ProfileReleaseBudget ? 0.01 : 0.005;
            // The live score includes a partial 100 ms bucket. A workload
            // exactly on goal over full windows can dip below it before that
            // bucket fills. Budget for the shortest covered scoring window.
            const double releaseAllowance = selectedGoal ?
                allowedLoss * (1.0 - double(ScoreBucketUs) / double(std::max(scoreWindowUs, ScoreBucketUs))) :
                0.9 * std::min(allowedLoss, historicalCap);
            const auto qualifiedReserve = m_ReleaseCosts.observe(previous, m_Previous, actual, intended,
                error, toleranceUs, minimum, maximum, releaseAllowance, selectedGoal, releaseRevision,
                latchedCeiling, priceVariableCadence);
            // A startup seed must have actually run at the approved level.
            // Early partial-window release can move the actuator, but only
            // agreement across completed cost windows can seed a reconnect.
            update.qualifiedStartDelay = selectedGoal && !belowTarget && m_ReleaseCosts.hasHistory() &&
                (releaseRevision < Vrr18ProgressiveCeilingRelease || qualifiedReserve >= m_ReleaseCosts.confirmedReserve()) &&
                qualifiedReserve < maximum && qualifiedReserve == s.buffer && previous.buffer == s.buffer;
            const bool appliedQualifies = qualifiedReserve <= std::min(s.buffer, previous.buffer);
            const bool honorApproval = selectedGoal || (releaseRevision >= Vrr18ProfileReleaseBudget && allowedLoss > 0.005);
            qualifiedAtCurrentReserve = canQualifyRelease && honorApproval && appliedQualifies &&
                qualifiedReserve < maximum && qualifiedReserve <= boundedUs;
            if (canQualifyRelease && selectedGoal && !belowTarget && m_ReleaseCosts.hasHistory()) {
                // A failed lower trial does not establish that the entire
                // readiness estimate is needed. While at goal, use a qualified
                // level or retry one step above a recently approved level.
                // The short-term forecast can still respond to a new regime.
                readinessTarget = std::min(readinessTarget, qualifiedReserve < maximum ?
                    qualifiedReserve : m_ReleaseCosts.nextReserveAfterApproval());
            }
            budgetCoversReserve = canQualifyRelease && !belowTarget && qualifiedReserve < releaseTarget && appliedQualifies;
            if (!belowTarget) {
                if (canQualifyRelease) releaseTarget = std::min(releaseTarget, qualifiedReserve);
                if (releaseRevision >= Vrr18FrameBudget && appliedQualifies &&
                        m_Target <= std::max(s.buffer, previous.buffer))
                    approvedReleaseUs = qualifiedReserve;
            }
        }
        else m_ReleaseCosts = {};
        const bool sustainedPressure = goalFeedback ? readinessTarget > m_Target && !budgetCoversReserve :
            useDeliveryEvidence ? deliveryPressure : (severityWeighted ? bufferLoss > allowedLoss : pressure);
        const bool targetAllowsHold = !severityWeighted || belowTarget;
        const bool pressureHolds = (vrr18Feedback || targetAllowsHold) && sustainedPressure &&
            (freshError && delayedAbsorbable && lateness);
        const bool uncertainRelease = goalFeedback && belowTarget && windowAbsorbable &&
            fitsAllowance && freshError && delayed.absorbable && delayed.independentReadyUs > deadline &&
            lateness > toleranceUs && bufferLoss > allowedLoss;
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
                (window.marginalUs + window.samples - 1) / window.samples :
                severityWeighted ?
                uint64_t(std::ceil(std::min(250.0, std::max(0.0, bufferExcessUs - allowedLoss * intended)))) :
                (window.total - window.samples * toleranceUs + window.samples - 1) / window.samples;
            const auto freshExcess = useDeliveryEvidence ? benefit.excessUs :
                severityWeighted ? error - toleranceUs : error;
            const auto base = std::min(delayed.buffer, maximum);
            const auto increase = goalFeedback ?
                std::min(readinessTarget - std::min(readinessTarget, base), lateness) :
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
        else if (approvedReleaseUs < m_Target) {
            // Measured interval costs qualify this reserve at the selected goal.
            // The actuator spends only the unused spacing tolerance per frame.
            m_Target = approvedReleaseUs;
            m_ReleaseFraction = 0;
            update.action = m_Target == minimum ? Action::Minimum : Action::Release;
        }
        // A changing source cadence alone cannot establish that the reserve is safe to remove.
        else if (!holdProtection && s.absorbable &&
                (releaseRevision < Vrr18FrameBudget || steadySource) &&
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
        // Sustained, independently attributable loss can predict a new delivery
        // regime while clean older history still masks its effect on the score.
        // A short forecast can see clustered misses on a stream whose current
        // reserve already passed both release windows. Let that approval stand
        // until new loss exhausts it or the score falls below goal.
        if ((belowTarget || (anticipatoryFeedback && !qualifiedAtCurrentReserve)) &&
                forecastUs > m_Target && forecastUs > boundedUs) {
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
        update.qualifiedStartDelay = update.qualifiedStartDelay && m_Target == s.buffer;
    }
    struct FrameMovement {
        uint64_t riseUs = 0, fallUs = 0;
    };
    static FrameMovement movementBudget(uint64_t expectedUs, uint64_t nominalUs,
                                        uint64_t earliestUs, uint64_t toleranceUs) {
        const auto unchangedUs = std::max(nominalUs, earliestUs);
        const auto upperUs = std::max(unchangedUs, expectedUs +
            std::min(toleranceUs, std::numeric_limits<uint64_t>::max() - expectedUs));
        const auto lowerUs = std::min(unchangedUs, expectedUs - std::min(expectedUs, toleranceUs));
        // Readiness or display spacing can hide a reserve adjustment. Outside
        // the band, allow correction or a hidden step, never a larger error.
        return {std::min(toleranceUs, upperUs - nominalUs),
                earliestUs >= lowerUs ? toleranceUs : std::min(toleranceUs, nominalUs - lowerUs)};
    }
    uint64_t demand(uint64_t applied) const { return m_Initialized ? m_Target : applied; }
    // Replace the standing target at a source epoch and protect it for one
    // hold from the given time, as if pressure had just been observed.
    void restoreTarget(uint64_t target, uint64_t atUs) {
        if (!m_Initialized) return;
        m_Forecast = {};
        m_ReserveCosts = {};
        m_ReleaseCosts = {};
        m_Target = target;
        m_LastPressure = atUs;
        m_ReleaseFraction = 0;
    }
    // An independent readiness estimate belongs to the observed source rate.
    // Keep the applied reserve and pacing score while the new rate earns its
    // own protection and the release law drains excess from the old stream.
    void forgetReadinessReserve() { m_Reserve = {}; }
    Stats stats() const { return m_Stats; }
    bool startDelayQualified() const { return m_Stats.update.qualifiedStartDelay; }
    void breakSequence() {
        m_Forecast = {};
        m_ReserveCosts = {};
        m_ReleaseCosts = {};
        m_Window.clear(); m_First = m_ReleaseFraction = m_SequenceSamples = 0; m_HavePrevious = false;
        m_Stats.averageErrorUs = 0; m_Stats.averageValid = false;
        m_Stats.clientAverageErrorUs = 0; m_Stats.clientAverageValid = false;
        m_Stats.calibrationCoverageUs = m_Stats.calibrationSamples = 0;
        m_Stats.update = {};
        m_Stats.update.action = Action::SequenceBreak;
    }
    void reset() { *this = IntervalBuffer{}; }
private:
    // Adding the same standing reserve moves both deadlines later. Compare the
    // wait it would add at each endpoint, including any delay already supplied
    // by readiness, the previous flip or the scheduler. Only independently late
    // input can earn the improvement in the measured handoff interval.
    static bool canCoverInterval(const Sample& onTime, const Sample& late,
                                 uint64_t excessUs, uint64_t maximumUs) {
        if (!late.absorbable || !onTime.absorbable ||
            !late.independentReadyUs || !onTime.independentReadyUs) return false;
        const auto readiness = [](const Sample& s) { return std::min(s.ready, s.independentReadyUs); };
        const auto addedWait = [maximumUs](const Sample& s, uint64_t boundary) {
            const uint64_t increase = maximumUs - std::min(maximumUs, s.buffer);
            const auto remaining = boundary - std::min(boundary, s.deadline);
            return increase - std::min(increase, remaining);
        };
        const auto fits = [excessUs](uint64_t before, uint64_t after) {
            return before >= after && before - after >= excessUs;
        };
        return fits(addedWait(onTime, onTime.submitted), addedWait(late, late.submitted)) &&
               fits(addedWait(onTime, readiness(onTime)), addedWait(late, readiness(late)));
    }
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

    // Evaluate the same observed interval at several absolute reserve sizes.
    // Only its independently recoverable error can justify spending latency.
    struct ReserveCosts {
        static constexpr uint64_t WindowUs = 375000;
        static constexpr uint64_t StepUs = 250;
        static constexpr size_t Levels = 13; // Up to 3 ms beyond the applied reserve.

        uint64_t observe(const Sample& delayed, const Sample& other, uint64_t actualUs,
                         uint64_t intendedUs, uint64_t errorUs, uint64_t toleranceUs,
                         uint64_t maximumUs, double allowedLoss, bool steadySource, bool cadenceBounded,
                         bool beforeGoalCrossing, bool frameBudget, double remainingGoalBudgetUs,
                         bool spendScoreSurplus, bool recoverableHitch) {
            const uint64_t appliedUs = std::max(delayed.buffer, other.buffer);
            if (!elapsedUs) anchorUs = std::min(appliedUs, maximumUs);
            elapsedUs += actualUs;
            const double weight = double(actualUs) / intendedUs;
            const auto excess = [=](double error) {
                return std::clamp(error - toleranceUs, 0.0, double(intendedUs));
            };
            const double observedLoss = excess(double(errorUs));
            // An unavoidable miss remains in the reported score. Including its
            // deficit here would make unrelated, recoverable misses buy more delay.
            if (recoverableHitch) observedLossUs += weight * observedLoss;
            const uint64_t headroomUs = maximumUs - std::min(maximumUs, appliedUs);
            const uint64_t lateAtCapUs = coveredReadiness(delayed, headroomUs);
            const uint64_t earlyAtCapUs = coveredReadiness(other, headroomUs);
            uint64_t confirmedUs = 0;
            // A larger ceiling reserve can persist through two release windows.
            // Spread the unspent rolling loss budget over that same span when
            // pricing the purchase. As new losses use it up, the forecast can
            // buy more before the rolling score crosses its target.
            const double forecastAllowance = allowedLoss + (beforeGoalCrossing && spendScoreSurplus ?
                remainingGoalBudgetUs / (2 * ReleaseCosts::WindowUs) : 0.0);
            const auto pricedProposal = [&](uint64_t spanUs) {
                const auto full = proposal(maximumUs, allowedLoss * spanUs);
                // A corroborated correction that needs only the first trial
                // level can finish covering a recurring spike immediately.
                if (forecastAllowance == allowedLoss || full <= candidate(1, maximumUs)) return full;
                return proposal(maximumUs, forecastAllowance * spanUs);
            };
            if (lateAtCapUs || earlyAtCapUs) {
                // When the source rate varies, require independent input lateness
                // to explain the observed interval error to within tolerance.
                const double inputErrorUs = double(lateAtCapUs) - double(earlyAtCapUs);
                const bool explained = steadySource ||
                    (cadenceBounded && inputErrorUs > 0 && std::abs(errorUs - inputErrorUs) <= toleranceUs);
                const bool recoverable = recoverableHitch && explained &&
                    intervalBenefit(delayed, other, errorUs, toleranceUs, maximumUs).excessUs != 0;
                bool improvesStretch = false;
                for (size_t i = 0; i < Levels; ++i) {
                    const uint64_t candidateUs = candidate(i, maximumUs);
                    const uint64_t extraUs = candidateUs - std::min(candidateUs, appliedUs);
                    const double remaining = double(errorUs) - double(coveredReadiness(delayed, extraUs))
                        + double(coveredReadiness(other, extraUs));
                    const double gain = observedLoss - excess(std::abs(remaining));
                    // Charge a new compression even if this pair was clean.
                    // Only independently attributable delivery can earn a benefit.
                    savedLossUs[i] += weight * (recoverable ? gain : std::min(0.0, gain));
                    improvesStretch |= recoverable && actualUs > intendedUs && gain > 0;
                }
                if (improvesStretch) {
                    if (!firstStretchUs) firstStretchUs = delayed.submitted;
                    lastStretchUs = delayed.submitted;
                    // Acting before the rolling score crosses its target buys
                    // delay only if the next block still has a recoverable
                    // stretch. A burst ending in the previous block gets no
                    // reserve on a subsequent clean frame.
                    confirmedUs = pendingProposalUs;
                    if (frameBudget && recurringStretch(delayed, toleranceUs, beforeGoalCrossing,
                                                       remainingGoalBudgetUs) && steadySource && previousProposalUs) {
                        // Charge a full block's allowance before acting on a partial block.
                        const auto supportedUs = pricedProposal(WindowUs);
                        confirmedUs = std::max(confirmedUs, std::min(previousProposalUs, supportedUs));
                    }
                    pendingProposalUs = 0;
                }
            }
            if (elapsedUs < WindowUs) return confirmedUs;

            const uint64_t proposalUs = pricedProposal(elapsedUs);
            // A burst straddling a block boundary is still a single episode.
            const bool recurring = previousFirstStretchUs && lastStretchUs >= previousFirstStretchUs &&
                lastStretchUs - previousFirstStretchUs >= WindowUs;
            const uint64_t corroboratedUs = recurring ? std::min(previousProposalUs, proposalUs) : 0;
            pendingProposalUs = beforeGoalCrossing ? corroboratedUs : 0;
            previousProposalUs = proposalUs;
            previousFirstStretchUs = firstStretchUs;
            elapsedUs = firstStretchUs = lastStretchUs = 0;
            observedLossUs = 0;
            savedLossUs = {};
            return std::max(confirmedUs, beforeGoalCrossing ? uint64_t(0) : corroboratedUs);
        }

    private:
        bool recurringStretch(const Sample& delayed, uint64_t toleranceUs, bool beforeGoalCrossing,
                              double remainingGoalBudgetUs) {
            const uint64_t cycleFrames = delayed.frame - std::min(delayed.frame, lastStretchFrame);
            const auto slotUs = delayed.smoothedRtpSlotUs;
            const auto cycleUs = slotUs - std::min(slotUs, lastStretchSlotUs);
            const auto slotDelayUs = delayed.independentLatenessUs();
            const bool comparable = std::max(slotDelayUs, lastSlotDelayUs) -
                std::min(slotDelayUs, lastSlotDelayUs) <= toleranceUs;
            const bool recurring = cycleFrames > 1 && cycleFrames == previousCycleFrames &&
                cycleUs && previousCycleUs &&
                std::max(cycleUs, previousCycleUs) - std::min(cycleUs, previousCycleUs) <= toleranceUs;
            // With surplus in the rolling score, the spike sizes must also
            // repeat before a partial block may anticipate a deficit.
            const bool affordable = !beforeGoalCrossing || observedLossUs > remainingGoalBudgetUs ||
                (comparable && previousComparable);
            previousComparable = comparable && lastStretchFrame;
            lastSlotDelayUs = slotDelayUs;
            previousCycleFrames = lastStretchFrame ? cycleFrames : 0;
            previousCycleUs = lastStretchSlotUs ? cycleUs : 0;
            lastStretchFrame = delayed.frame;
            lastStretchSlotUs = slotUs;
            return recurring && affordable;
        }

        uint64_t lastStretchFrame = 0, previousCycleFrames = 0;
        uint64_t lastStretchSlotUs = 0, previousCycleUs = 0, lastSlotDelayUs = 0;
        bool previousComparable = false;
        uint64_t elapsedUs = 0, anchorUs = 0, previousProposalUs = 0;
        uint64_t firstStretchUs = 0, lastStretchUs = 0, previousFirstStretchUs = 0, pendingProposalUs = 0;
        double observedLossUs = 0;
        std::array<double, Levels> savedLossUs{};

        uint64_t proposal(uint64_t maximumUs, double lossBudgetUs) const {
            const double initialGainUs = savedLossUs.front();
            const double deficitUs = observedLossUs - initialGainUs - lossBudgetUs;
            const double availableGainUs = *std::max_element(savedLossUs.begin(), savedLossUs.end()) - initialGainUs;
            if (!firstStretchUs || deficitUs <= 0 || availableGainUs <= 0) return 0;

            // If the goal is out of reach, buy only material progress toward it.
            // Take the cheapest level recovering 90% of that available gain.
            double neededGainUs = deficitUs;
            if (availableGainUs < deficitUs) {
                if (availableGainUs < 0.1 * std::max(lossBudgetUs, deficitUs)) return 0;
                neededGainUs = 0.9 * availableGainUs;
            }
            for (size_t i = 1; i < Levels; ++i) {
                if (savedLossUs[i] >= initialGainUs + neededGainUs) return candidate(i, maximumUs);
            }
            return 0;
        }

        uint64_t candidate(size_t level, uint64_t maximumUs) const {
            const uint64_t anchor = std::min(anchorUs, maximumUs);
            return anchor + std::min<uint64_t>(level * StepUs, maximumUs - anchor);
        }
    };

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
                     bool steadySource, uint64_t targetPpm, uint64_t holdUs, bool zeroReserve) {
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

            const bool initialEstimate = zeroReserve ? lastObservedUs == 0 : smoothedUs == 0;
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

            if (clean || initialEstimate) smoothedUs = clean ? estimateUs : double(minimumUs);
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

    // A covered periodic miss can keep the readiness estimate above the
    // smallest reserve that meets the pacing goal. Check both edges of the
    // interval over time before releasing below that estimate.
    struct ReleaseCosts {
        static constexpr uint64_t WindowUs = 4000000;
        static constexpr uint64_t StepUs = 250;
        static constexpr size_t FineLevels = 13; // 250 us steps through 3 ms above the minimum.
        static constexpr size_t OuterSteps = 20;
        static constexpr size_t SubMillisecondSteps = 4;
        static constexpr size_t Levels = FineLevels + SubMillisecondSteps + OuterSteps;

        bool hasHistory() const { return confirmedLevel <= levelCount; }
        uint64_t confirmedReserve() const { return candidate(std::max(previousLevel, confirmedLevel)); }
        uint64_t nextReserveAfterApproval() const { return candidate(std::min(confirmedLevel, previousLevel) + 1); }

        uint64_t observe(const Sample& previous, const Sample& current, uint64_t actualUs,
                         uint64_t intendedUs, uint64_t errorUs, uint64_t toleranceUs,
                         uint64_t minimumUs, uint64_t maximumUs, double allowedLoss, bool selectedGoal,
                         uint64_t revision, bool latchedCeiling, bool priceVariableCadence) {
            if (minimumUs != minimum || maximumUs != maximum || latchedCeiling != measuredAtCeiling) {
                // Extend the fine grid downward while preserving its coverage of
                // reserves up to 4 ms and the recorded grids of earlier policies.
                const size_t fineCount = FineLevels + (revision >= Vrr18ZeroReserve ? SubMillisecondSteps : 0);
                const uint64_t range = maximumUs - minimumUs;
                const uint64_t outerRange = range - std::min(range, StepUs * (fineCount - 1));
                uint64_t stride = std::max<uint64_t>(StepUs, outerRange / OuterSteps + (outerRange % OuterSteps != 0));
                const bool stableGrid = priceVariableCadence || (latchedCeiling && revision >= Vrr18CeilingReleaseHistory);
                if (stableGrid) stride = ((stride + StepUs - 1) / StepUs) * StepUs;
                // A fitted period moves the frame allowance by microseconds.
                // Trials keep their accumulated costs while their reserve values
                // agree. Levels above the current capacity are priced at that
                // interval's cap, including when one endpoint still carries
                // the preceding frame's larger reserve.
                if (!stableGrid || minimum != minimumUs || measuredAtCeiling != latchedCeiling || outerStepUs != stride)
                    *this = {};
                minimum = minimumUs;
                maximum = maximumUs;
                measuredAtCeiling = latchedCeiling;
                fineLevels = fineCount;
                levelCount = fineLevels + (selectedGoal ? OuterSteps : 0);
                outerStepUs = stride;
            }
            elapsedUs += actualUs;
            const auto loss = [&](double duration, double error) {
                return duration * std::clamp((error - double(toleranceUs)) / double(intendedUs), 0.0, 1.0);
            };
            const double observedLoss = loss(double(actualUs), double(errorUs));
            observedLossUs += observedLoss;
            const auto projectedLoss = [&](double projectedInterval) {
                const double interval = std::max(1.0, projectedInterval);
                return loss(interval, std::abs(interval - double(intendedUs)));
            };
            if (selectedGoal) {
                // Each handoff can advance only up to its readiness and spacing
                // floors. If both endpoints advance equally, the interval stays intact.
                const auto advance = [](const Sample& s, uint64_t reserve) {
                    const auto release = s.buffer - std::min(reserve, s.buffer);
                    return std::min(release, s.maximumSubmissionAdvanceUs(reserve));
                };
                const auto inputAdvance = [](const Sample& s, uint64_t reserve) {
                    const auto slack = s.deadline - std::min(s.deadline, s.independentReadyUs);
                    return std::min(slack, s.buffer - std::min(s.buffer, reserve));
                };
                for (size_t i = 0; i < levelCount; ++i) {
                    const auto reserve = candidate(i);
                    if (reserve >= std::max(previous.buffer, current.buffer)) break;
                    double cost = projectedLoss(double(actualUs) + double(advance(previous, reserve)) -
                                                double(advance(current, reserve)));
                    if (latchedCeiling || priceVariableCadence) {
                        // A display floor or retimed slot can mask late input.
                        // Price both ends at their independent readiness as well.
                        cost = std::max(cost, projectedLoss(double(actualUs) + double(inputAdvance(previous, reserve)) -
                                                            double(inputAdvance(current, reserve))));
                    }
                    extraLossUs[i] += std::max(0.0, cost - observedLoss);
                }
            }
            else {
                const auto late = [](const Sample& s, uint64_t reserve) {
                    return ReadinessReserve::lateUs(s, double(std::min(reserve, s.buffer)));
                };
                const double previousLate = late(previous, previous.buffer);
                const double currentLate = late(current, current.buffer);
                if (late(previous, minimum) > previousLate || late(current, minimum) > currentLate) {
                    for (size_t i = 0; i < FineLevels; ++i) {
                        const auto reserve = candidate(i);
                        const auto cost = projectedLoss(double(actualUs) + late(current, reserve) -
                            currentLate - (late(previous, reserve) - previousLate));
                        extraLossUs[i] += std::max(0.0, cost - observedLoss);
                    }
                }
            }
            peakAppliedUs = std::max({peakAppliedUs, previous.buffer, current.buffer});
            const double budget = allowedLoss * double(std::max(WindowUs, elapsedUs));
            const auto affordable = [&](size_t i) {
                return observedLossUs + extraLossUs[i] <= budget;
            };
            if (elapsedUs >= WindowUs) {
                size_t nextLevel = levelCount;
                for (size_t i = levelCount; i > 0; --i) {
                    if (!affordable(i - 1)) break;
                    nextLevel = i - 1;
                }
                olderLevel = confirmedLevel;
                confirmedLevel = previousLevel;
                previousLevel = nextLevel;
                elapsedUs = 0;
                observedLossUs = 0;
                peakAppliedUs = 0;
                extraLossUs = {};
                return candidate(std::max(confirmedLevel, previousLevel));
            }
            if (revision >= Vrr18FrameBudget && !hasHistory() && observedLossUs == 0 && extraLossUs[0] == 0) {
                // Spend only the loss budget already observed. An unseen late
                // frame could expose the removed reserve on both adjacent
                // intervals. Initially charge both edges in full. This lets
                // clean evidence qualify a small step immediately and larger
                // steps as confidence grows, without a startup cooldown.
                size_t affordableLevel = levelCount;
                for (size_t i = levelCount; i > 0; --i) {
                    const auto reserve = candidate(i - 1);
                    const auto release = peakAppliedUs - std::min(peakAppliedUs, reserve);
                    if (observedLossUs + extraLossUs[i - 1] + 2.0 * release > allowedLoss * elapsedUs) break;
                    affordableLevel = i - 1;
                }
                if (previousLevel <= levelCount) affordableLevel = std::max(affordableLevel, previousLevel);
                return candidate(affordableLevel);
            }
            // Revoke levels that exhaust this window's budget. Revision 12
            // can still use a higher level approved in both completed windows.
            const size_t floor = std::max(previousLevel, confirmedLevel);
            if (latchedCeiling && revision >= Vrr18ProgressiveCeilingRelease &&
                    olderLevel == confirmedLevel && confirmedLevel < levelCount && previousLevel < confirmedLevel &&
                    candidate(confirmedLevel) - candidate(previousLevel) > toleranceUs) {
                // Two older windows agreed on the high reserve and the latest one
                // measured a decline larger than one frame's adjustment. Advance
                // only through trials with no added loss in the current window.
                // Pay for both edges of a returning late frame from the original
                // high reserve so consecutive reductions cannot reuse the budget.
                const auto confirmedUs = candidate(confirmedLevel);
                const double earnedBudgetUs = allowedLoss * elapsedUs;
                size_t trial = confirmedLevel;
                while (trial > previousLevel) {
                    const auto lower = trial - 1;
                    const auto exposedUs = confirmedUs - candidate(lower);
                    if (extraLossUs[lower] > 0 || observedLossUs + 2.0 * double(exposedUs) > earnedBudgetUs) break;
                    trial = lower;
                }
                if (trial < confirmedLevel) return candidate(trial);
            }
            for (size_t i = floor; i < levelCount; ++i) {
                if (affordable(i)) return candidate(i);
                if (!selectedGoal) break;
            }
            return maximum;
        }

    private:
        uint64_t candidate(size_t level) const {
            if (level >= levelCount || (levelCount > fineLevels && level == levelCount - 1)) return maximum;
            const uint64_t fine = std::min(level, fineLevels - 1);
            return minimum + std::min(maximum - minimum, fine * StepUs + (level - fine) * outerStepUs);
        }

        uint64_t minimum = 0, maximum = 0, elapsedUs = 0, peakAppliedUs = 0;
        uint64_t outerStepUs = StepUs;
        size_t fineLevels = FineLevels, levelCount = FineLevels, previousLevel = Levels + 1, confirmedLevel = Levels + 1;
        size_t olderLevel = Levels + 1;
        double observedLossUs = 0;
        std::array<double, Levels> extraLossUs{};
        bool measuredAtCeiling = false;
    };

    struct Bucket {
        uint64_t samples = 0, total = 0;
        uint64_t service = 0, decoderQueue = 0, intended = 0, sourceErrorUs = 0;
        uint64_t clientSamples = 0, clientErrorUs = 0, evaluatedUs = 0, marginalUs = 0;
        double recoverableLossUs = 0;
        void add(const Bucket& b) {
            samples += b.samples; total += b.total;
            service += b.service; decoderQueue += b.decoderQueue;
            intended += b.intended; sourceErrorUs += b.sourceErrorUs;
            clientSamples += b.clientSamples; clientErrorUs += b.clientErrorUs;
            evaluatedUs += b.evaluatedUs; marginalUs += b.marginalUs;
            recoverableLossUs += b.recoverableLossUs;
        }
    };
    struct ScoreBucket {
        uint64_t evaluated = 0, failed = 0;
        double weightedLoss = 0;
        uint64_t clientEvaluatedUs = 0;
        double clientWeightedLossUs = 0;
        uint64_t bufferEvaluated = 0, bufferFailed = 0;
        double bufferWeightedLoss = 0;
        void add(const ScoreBucket& b) {
            evaluated += b.evaluated; failed += b.failed; weightedLoss += b.weightedLoss;
            clientEvaluatedUs += b.clientEvaluatedUs; clientWeightedLossUs += b.clientWeightedLossUs;
            bufferEvaluated += b.bufferEvaluated; bufferFailed += b.bufferFailed;
            bufferWeightedLoss += b.bufferWeightedLoss;
        }
    };

    // A moving window spans the tail of the previous block and the prefix of
    // the current one. Precompute those tails once per block, then accumulate
    // each closed bucket once. Adding only nonnegative losses avoids residual
    // "misses" after old losses leave a 100% target window.
    template<class Totals, size_t Capacity>
    class BucketHistory {
    public:
        void advance(uint64_t tick, uint64_t width) {
            if (tick == m_Tick && width == m_Width) return;
            const uint64_t block = tick - tick % width;
            if (width != m_Width || block != m_Block || tick < m_Tick) {
                m_Block = block;
                m_Width = width;
                m_Prefix = {};
                m_Tick = block;
                if (block) {
                    m_Tails.resize(size_t(width - 1));
                    Totals tail;
                    for (size_t i = 0; i < m_Tails.size(); ++i) {
                        addBucket(tail, block - 1 - i);
                        m_Tails[i] = tail;
                    }
                }
            }
            while (m_Tick < tick) addBucket(m_Prefix, m_Tick++);
        }

        Totals& current() {
            auto& entry = m_Buckets[m_Tick % Capacity];
            if (entry.tick != m_Tick) entry = {m_Tick, {}};
            return entry.value;
        }

        Totals totals() const {
            auto sum = m_Prefix;
            addBucket(sum, m_Tick);
            const uint64_t tailCount = m_Width - 1 - (m_Tick - m_Block);
            if (m_Block && tailCount) sum.add(m_Tails[size_t(tailCount - 1)]);
            return sum;
        }

        // Floating-point grouping can differ by a few ulps. Reproduce the
        // original physical ring order for a comparison close enough to flip.
        Totals orderedTotals() const {
            Totals sum;
            for (const auto& entry : m_Buckets) {
                if (entry.tick <= m_Tick && m_Tick - entry.tick < m_Width) sum.add(entry.value);
            }
            return sum;
        }

        void clear() {
            std::fill(m_Buckets.begin(), m_Buckets.end(), Entry{});
            m_Width = 0;
        }

    private:
        struct Entry { uint64_t tick = 0; Totals value; };
        void addBucket(Totals& sum, uint64_t tick) const {
            const auto& entry = m_Buckets[tick % Capacity];
            if (entry.tick == tick) sum.add(entry.value);
        }
        std::vector<Entry> m_Buckets = std::vector<Entry>(Capacity);
        std::vector<Totals> m_Tails;
        Totals m_Prefix;
        uint64_t m_Block = 0, m_Width = 0, m_Tick = 0;
    };

    static bool nearThreshold(double value, double limit, size_t buckets) {
        // Both sums consist of at most 'buckets' nonnegative bucket values.
        // Each grouping can accumulate up to about buckets * eps relative error.
        return limit > 0 && std::abs(value - limit) <=
            4 * buckets * std::numeric_limits<double>::epsilon() * std::max(value, limit);
    }
    void advanceScore(uint64_t at, uint64_t windowUs) {
        const uint64_t windowBuckets = std::clamp<uint64_t>(
            (windowUs + ScoreBucketUs - 1) / ScoreBucketUs,
            1, MaximumScoreBuckets);
        m_Score.advance(at / ScoreBucketUs, windowBuckets);
    }
    void publishScore(const ScoreBucket& score) {
        m_Stats.evaluatedUs = score.evaluated; m_Stats.failedUs = score.failed;
        m_Stats.weightedLossUs = score.weightedLoss;
        m_Stats.clientEvaluatedUs = score.clientEvaluatedUs;
        m_Stats.clientWeightedLossUs = score.clientWeightedLossUs;
        m_Stats.bufferEvaluatedUs = score.bufferEvaluated; m_Stats.bufferFailedUs = score.bufferFailed;
        m_Stats.bufferWeightedLossUs = score.bufferWeightedLoss;
    }
    ReadinessReserve m_Reserve;
    ReleaseCosts m_ReleaseCosts;
    HitchForecast m_Forecast;
    ReserveCosts m_ReserveCosts;
    static constexpr size_t ShortWindowBuckets = ShortWindowUs / ShortBucketUs;
    BucketHistory<Bucket, ShortWindowBuckets> m_Window;
    BucketHistory<ScoreBucket, MaximumScoreBuckets> m_Score;
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
