#pragma once

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdint>
#include <climits>

// Learns the amount of decode-arrival variation that must be hidden before a
// frame is committed to rendering. The saved model is deliberately raw: it
// contains arrival-jitter demand, confidence, and volatility for normalized
// cadence-headroom buckets. Policy and recovery state are applied live and
// are never persisted, so changing the user's latency preference does not
// poison the learned hardware/link model.
class VrrQueueAgeController
{
public:
    struct Target {
        uint64_t queueAgeUs;
        uint64_t preparationFloorUs;
        uint64_t protectionReserveUs;
        uint64_t modelDemandUs;
        uint64_t effectiveDemandUs;
        uint64_t headroomCreditUs;
        uint64_t pressureReserveUs;
        uint64_t backlogReliefUs;
        uint16_t confidence;
        uint16_t headroomPermille;
        bool learned;
        bool restored;
    };

    struct CacheEntry {
        uint16_t headroomPermille;
        uint16_t confidence;
        uint32_t demandUs;
        uint32_t volatilityUs;
    };

    struct WindowStatistics {
        uint64_t p10Us;
        uint64_t p20Us;
        uint64_t medianUs;
        uint64_t p90Us;
    };

    struct PhaseDecisionInput {
        WindowStatistics stats;
        uint64_t targetAgeUs;
        uint64_t previousTargetAgeUs;
        int64_t medianSetpointErrorUs;
        uint64_t sourceIntervalUs;
        uint64_t maxAdvanceStepUs;
        uint64_t maxDelayStepUs;
        int sampleCount;
        bool hasSetpointError;
        bool nearCeiling;
        bool windowTainted;
        bool phaseAdvanceActive;
        bool phaseDelayActive;
        bool fastRecoveryActive;
        bool staleSchedule;
        bool overfillEligible;
        bool targetStable;
    };

    struct PhaseDecision {
        uint64_t advanceUs;
        uint64_t delayUs;
        bool requestOverfillDrop;
    };

    enum class Policy {
        LowestLatency,
        Balanced,
        Smoothest,
    };

    VrrQueueAgeController(uint64_t policyValueUs, bool forceStatic) :
        m_Policy(policyFromLegacyValue(policyValueUs)),
        m_ForceStatic(forceStatic),
        m_StaticTargetUs(qBound(kMinimumStaticReserveUs, policyValueUs,
                                kMaximumStaticReserveUs)),
        m_AppliedReserveUs(kColdStartDemandUs),
        m_PressureReserveUs(0),
        m_BacklogReliefUs(0),
        m_LastBucket(-1),
        m_LastHeadroomPermille(UINT16_MAX),
        m_AttackEvidenceUs(0),
        m_ReleaseEvidenceUs(0)
    {
    }

    static Policy policyFromLegacyValue(uint64_t valueUs)
    {
        if (valueUs <= 3000) {
            return Policy::LowestLatency;
        }
        if (valueUs >= 5500) {
            return Policy::Smoothest;
        }
        return Policy::Balanced;
    }

    static const char* policyName(Policy policy)
    {
        switch (policy) {
        case Policy::LowestLatency:
            return "lowest-latency";
        case Policy::Smoothest:
            return "smoothest";
        case Policy::Balanced:
        default:
            return "balanced";
        }
    }

    Policy policy() const
    {
        return m_Policy;
    }

    static int windowSampleCount(uint64_t sourceIntervalUs, int fallbackFps)
    {
        if (sourceIntervalUs != 0) {
            return qBound(16, (int)(500000ULL / sourceIntervalUs), 120);
        }
        return fallbackFps > 0 ? qBound(16, fallbackFps / 2, 120) : 30;
    }

    static WindowStatistics summarizeWindow(uint32_t* samples, int count)
    {
        WindowStatistics result = {};
        if (samples == nullptr || count <= 0) {
            return result;
        }

        std::sort(samples, samples + count);
        auto percentile = [samples, count](int value) -> uint64_t {
            return samples[(count - 1) * value / 100];
        };
        result.p10Us = percentile(10);
        result.p20Us = percentile(20);
        result.medianUs = percentile(50);
        result.p90Us = percentile(90);
        return result;
    }

    static int64_t summarizeSignedMedian(int64_t* samples, int count)
    {
        if (samples == nullptr || count <= 0) {
            return 0;
        }

        std::sort(samples, samples + count);
        return samples[(count - 1) / 2];
    }

    static int64_t signedAgeErrorUs(uint64_t actualUs, uint64_t targetUs)
    {
        uint64_t magnitudeUs = actualUs >= targetUs ?
            actualUs - targetUs : targetUs - actualUs;
        int64_t boundedMagnitudeUs = (int64_t)qMin(
            magnitudeUs, (uint64_t)INT64_MAX);
        return actualUs >= targetUs ?
            boundedMagnitudeUs : -boundedMagnitudeUs;
    }

    // Early renderer preparation is real decode-to-render-start occupancy,
    // but it is not a dejitter reserve: the cadence thread cannot trim it
    // away and it must never enter the persistent arrival model. Track a
    // conservative live envelope with a quick bounded attack and slow
    // release. Samples longer than a source interval (or 12 ms) are stalls,
    // not a useful standing floor.
    static uint64_t updatePreparationFloorUs(uint64_t currentFloorUs,
                                             uint64_t sampleUs,
                                             uint64_t sourceIntervalUs,
                                             bool clean)
    {
        if (sourceIntervalUs == 0) {
            return currentFloorUs;
        }

        uint64_t maximumFloorUs = qMin(sourceIntervalUs,
                                        kMaximumPreparationFloorUs);
        currentFloorUs = qMin(currentFloorUs, maximumFloorUs);
        if (!clean || sampleUs > maximumFloorUs) {
            return currentFloorUs;
        }
        if (currentFloorUs == 0) {
            return sampleUs;
        }

        if (sampleUs > currentFloorUs) {
            uint64_t differenceUs = sampleUs - currentFloorUs;
            return currentFloorUs +
                qMax((uint64_t)1, (differenceUs + 3) / 4);
        }
        if (sampleUs < currentFloorUs) {
            uint64_t differenceUs = currentFloorUs - sampleUs;
            return currentFloorUs -
                qMax((uint64_t)1, differenceUs / 64);
        }
        return currentFloorUs;
    }

    // The absolute decode/source clock offset is irrelevant. The robust
    // clean-window spread is the exogenous arrival-phase variation that a
    // dejitter reserve may usefully hide; unlike queue age, it is not created
    // by this controller's own target. Repeated readiness pressure handles a
    // real tail that the p5-p95 envelope misses, while isolated extrema no
    // longer teach a full frame of standing latency.
    static uint64_t summarizeArrivalSpread(int64_t* samples, int count)
    {
        if (samples == nullptr || count <= 0) {
            return 0;
        }
        std::sort(samples, samples + count);
        int lowIndex = (count - 1) * 5 / 100;
        int highIndex = (count - 1) * 95 / 100;
        int64_t low = samples[lowIndex];
        int64_t high = samples[highIndex];
        return high > low ? (uint64_t)(high - low) : 0;
    }

    // Calculate an arrival phase relative to a source timestamp in the same
    // short observation window. RTP timestamps are 32-bit and wrap every
    // ~13 hours, so subtract them in signed modular space rather than mixing
    // an absolute RTP-derived clock with the local monotonic clock.
    static bool arrivalPhaseFromRtpTimestamps(uint32_t sourceTimestamp90k,
                                              uint32_t referenceTimestamp90k,
                                              uint64_t decodeTimeUs,
                                              uint64_t referenceDecodeTimeUs,
                                              int64_t* phaseUs)
    {
        if (phaseUs == nullptr || decodeTimeUs < referenceDecodeTimeUs) {
            return false;
        }

        int32_t sourceDeltaTicks = static_cast<int32_t>(
            sourceTimestamp90k - referenceTimestamp90k);
        if (sourceDeltaTicks < 0) {
            // Reordered or discontinuous source timestamps are not usable
            // arrival evidence for this window.
            return false;
        }

        uint64_t sourceDeltaUs = (uint64_t)sourceDeltaTicks * 1000000ULL /
            90000ULL;
        uint64_t decodeDeltaUs = decodeTimeUs - referenceDecodeTimeUs;
        if (sourceDeltaUs > (uint64_t)INT64_MAX ||
                decodeDeltaUs > (uint64_t)INT64_MAX ||
                sourceDeltaUs > kMaximumArrivalObservationSpanUs ||
                decodeDeltaUs > kMaximumArrivalObservationSpanUs) {
            return false;
        }

        int64_t candidatePhaseUs =
            (int64_t)decodeDeltaUs - (int64_t)sourceDeltaUs;
        if (qAbs(candidatePhaseUs) >
                (qint64)kMaximumArrivalPhaseExcursionUs) {
            // A large source-clock jump or multi-frame stall is a timestamp
            // discontinuity, not steady-state arrival jitter. Let the next
            // observation window establish a fresh reference instead of
            // poisoning the model at its 12 ms hard ceiling.
            return false;
        }

        *phaseUs = candidatePhaseUs;
        return true;
    }

    static PhaseDecision decidePhase(const PhaseDecisionInput& input)
    {
        PhaseDecision result = {};
        if (input.sampleCount <= 0) {
            return result;
        }

        // The total queue target can move within a feedback window as the
        // preparation floor, cadence headroom, and backlog relief change.
        // Prefer the median of each frame's contemporaneous setpoint error;
        // comparing a raw half-second median only with the newest target can
        // make a settled queue alternate between build and trim.
        int64_t setpointErrorUs = input.hasSetpointError ?
            input.medianSetpointErrorUs :
            signedAgeErrorUs(input.stats.medianUs, input.targetAgeUs);

        if (!input.phaseDelayActive && !input.fastRecoveryActive &&
                setpointErrorUs > 750) {
            uint64_t maxAdvanceStepUs = input.maxAdvanceStepUs != 0 ?
                input.maxAdvanceStepUs : 300;
            result.advanceUs = qBound(
                (uint64_t)20,
                (uint64_t)setpointErrorUs /
                    (uint64_t)input.sampleCount,
                maxAdvanceStepUs);
        }
        else if (!input.windowTainted &&
                 !input.phaseAdvanceActive && !input.phaseDelayActive &&
                 !input.fastRecoveryActive &&
                 setpointErrorUs < -750) {
            uint64_t maxDelayStepUs = input.maxDelayStepUs;
            if (maxDelayStepUs == 0) {
                maxDelayStepUs = 100;
                if (input.targetAgeUs > input.previousTargetAgeUs) {
                    maxDelayStepUs = qBound((uint64_t)100,
                        input.sourceIntervalUs / 24, (uint64_t)400);
                }
            }
            result.delayUs = qBound(
                (uint64_t)20,
                (uint64_t)(-setpointErrorUs) /
                    (uint64_t)input.sampleCount,
                maxDelayStepUs);
        }

        result.requestOverfillDrop = input.nearCeiling &&
            !input.windowTainted && !input.staleSchedule &&
            input.overfillEligible && input.targetStable &&
            input.sourceIntervalUs != 0 &&
            input.stats.p20Us > input.sourceIntervalUs;
        return result;
    }

    uint64_t phaseAdvanceStepLimitUs(uint64_t sourceIntervalUs,
                                     uint64_t serviceFloorUs) const
    {
        uint64_t headroomUs = sourceIntervalUs > serviceFloorUs ?
            sourceIntervalUs - serviceFloorUs : 0;
        uint16_t headroom = headroomPermille(sourceIntervalUs,
                                             serviceFloorUs);

        // At lower content rates, a bounded phase trim can use a much larger
        // fraction of the source interval without producing a panel-rate
        // burst. Keeping the high-refresh 150-250 us limits at 30 FPS made a
        // confirmed 6 ms reserve release take about a second after the model
        // had already decided that latency was excess. This cap drains that
        // finite phase error in roughly half a second while leaving the
        // high-refresh path unchanged.
        if (headroom >= kFastLowRateRecoveryHeadroomPermille) {
            return qBound((uint64_t)200, sourceIntervalUs / 40,
                          (uint64_t)1000);
        }
        switch (m_Policy) {
        case Policy::LowestLatency:
            return qBound((uint64_t)20, headroomUs / 3, (uint64_t)250);
        case Policy::Smoothest:
            return qBound((uint64_t)20, headroomUs / 6, (uint64_t)150);
        case Policy::Balanced:
        default:
            return qBound((uint64_t)20, headroomUs / 4, (uint64_t)200);
        }
    }

    uint64_t phaseDelayStepLimitUs(uint64_t sourceIntervalUs,
                                   uint64_t serviceFloorUs) const
    {
        uint16_t headroom = headroomPermille(sourceIntervalUs,
                                             serviceFloorUs);
        uint64_t urgency = 400 - qMin((uint16_t)400, headroom);
        switch (m_Policy) {
        case Policy::LowestLatency:
            return 100 + urgency * 100 / 400;
        case Policy::Smoothest:
            return 250 + urgency * 350 / 400;
        case Policy::Balanced:
        default:
            return 150 + urgency * 250 / 400;
        }
    }

    static bool shouldDiscardPhaseAdvance(bool staleSchedule,
                                          bool scheduleRecoveryRebased)
    {
        return staleSchedule || scheduleRecoveryRebased;
    }

    static bool isReadinessNearMiss(uint64_t protectionAgeUs,
                                    uint64_t targetProtectionUs,
                                    bool recoverySuppressed)
    {
        // Near-empty controllable protection is only starvation evidence
        // when it also missed the active protection setpoint materially.
        // Renderer preparation has already been removed by the caller; if it
        // remained in this comparison, a 4-5 ms preparation floor would hide
        // every genuine near-miss.
        return !recoverySuppressed && protectionAgeUs < 750 &&
            protectionAgeUs + kReadinessEvidenceSlackUs <
                targetProtectionUs;
    }

    static uint16_t headroomPermille(uint64_t sourceIntervalUs,
                                    uint64_t serviceFloorUs)
    {
        if (sourceIntervalUs == 0 || sourceIntervalUs <= serviceFloorUs) {
            return 0;
        }
        uint64_t value = (sourceIntervalUs - serviceFloorUs) * 1000 /
            sourceIntervalUs;
        return (uint16_t)qMin(value, (uint64_t)kMaximumHeadroomPermille);
    }

    // Persistence is supplied by the caller. Restored confidence is limited
    // for the *current* headroom regime and must earn clean live windows
    // before it may refresh its cache entry. The high-refresh cap is stronger
    // than the low-rate cap because that is where a warm reserve prevents
    // visible cadence loss and where recovery has the least physical slack.
    bool restoreModel(const CacheEntry& cached)
    {
        if (cached.headroomPermille > kMaximumHeadroomPermille ||
                cached.headroomPermille % kHeadroomBucketPermille != 0 ||
                cached.volatilityUs > kMaximumRawDemandUs ||
                cached.confidence > 1000) {
            return false;
        }
        int bucket = bucketForPermille(cached.headroomPermille);
        if (cached.demandUs < kMinimumRawDemandUs ||
                cached.demandUs > kMaximumRawDemandUs ||
                cached.confidence < kMinimumPersistedConfidence) {
            return false;
        }

        Model& model = m_Models[bucket];
        model.valid = true;
        model.restored = true;
        model.demandUs = cached.demandUs;
        model.volatilityUs = qMin(cached.volatilityUs,
                                   (uint32_t)kMaximumRawDemandUs);
        model.confidence = cached.confidence;
        model.liveCleanWindows = 0;
        model.driftDirection = 0;
        model.driftWindows = 0;
        model.driftBaselineUs = 0;
        model.driftThresholdUs = 0;
        return true;
    }

    int cacheEntryCount() const
    {
        int count = 0;
        for (const Model& model : m_Models) {
            if (model.valid &&
                    model.confidence >= kMinimumPersistedConfidence &&
                    model.liveCleanWindows >= kMinimumLiveWindowsForPersistence) {
                count++;
            }
        }
        return count;
    }

    CacheEntry cacheEntryAt(int requestedIndex) const
    {
        CacheEntry empty = {};
        int current = 0;
        for (int bucket = 0; bucket < kBucketCount; bucket++) {
            const Model& model = m_Models[bucket];
            if (!model.valid ||
                    model.confidence < kMinimumPersistedConfidence ||
                    model.liveCleanWindows < kMinimumLiveWindowsForPersistence) {
                continue;
            }
            if (current++ == requestedIndex) {
                CacheEntry result = {};
                result.headroomPermille = bucket * kHeadroomBucketPermille;
                result.confidence = model.confidence;
                result.demandUs = model.demandUs;
                result.volatilityUs = model.volatilityUs;
                return result;
            }
        }
        return empty;
    }

    // Hard invalidation is for an actual pipeline identity change. Ordinary
    // content-rate movement selects/interpolates another headroom bucket and
    // must not discard learned state.
    void resetLearning()
    {
        for (Model& model : m_Models) {
            model = {};
        }
        m_AppliedReserveUs = kColdStartDemandUs;
        m_PressureReserveUs = 0;
        m_BacklogReliefUs = 0;
        m_LastBucket = -1;
        m_LastHeadroomPermille = UINT16_MAX;
        m_AttackEvidenceUs = 0;
        m_ReleaseEvidenceUs = 0;
    }

    // Near-ceiling membership is presentation state, not cache identity. It
    // deliberately does not add or clear reserve.
    void enterNearCeiling(uint64_t)
    {
    }

    void leaveNearCeiling()
    {
    }

    // A bounded series of genuine readiness misses raises only transient
    // pressure. Recovery/stall samples never enter the persistent raw model.
    uint64_t notePressure(uint64_t deficitUs, uint64_t sourceIntervalUs,
                          uint64_t serviceFloorUs)
    {
        if (m_ForceStatic || deficitUs == 0) {
            return 0;
        }

        // Backlog relief is the inverse of readiness pressure. Consume it
        // first so fresh starvation evidence can restore protection without
        // stacking pressure on a stale negative correction.
        uint64_t resolvedReliefUs = qMin(m_BacklogReliefUs, deficitUs);
        m_BacklogReliefUs -= resolvedReliefUs;
        deficitUs -= resolvedReliefUs;

        uint64_t scaledDeficitUs = deficitUs;
        switch (m_Policy) {
        case Policy::LowestLatency:
            scaledDeficitUs /= 2;
            break;
        case Policy::Balanced:
            scaledDeficitUs = scaledDeficitUs * 3 / 4;
            break;
        case Policy::Smoothest:
            break;
        }
        if (scaledDeficitUs != 0) {
            scaledDeficitUs = qBound((uint64_t)200, scaledDeficitUs,
                                      (uint64_t)2500);
            m_PressureReserveUs = qMin(
                m_PressureReserveUs + scaledDeficitUs,
                (uint64_t)kMaximumPressureReserveUs);
        }

        uint16_t headroom = headroomPermille(sourceIntervalUs,
                                             serviceFloorUs);
        int bucket = bucketForPermille(headroom);
        if (m_Models[bucket].valid) {
            m_Models[bucket].confidence =
                m_Models[bucket].confidence > 100 ?
                    m_Models[bucket].confidence - 100 : 0;
        }

        uint64_t beforeUs = m_AppliedReserveUs;
        uint64_t desiredUs = desiredReserveUs(headroom, sourceIntervalUs,
                                               serviceFloorUs);
        slewAppliedReserve(desiredUs, true);
        return m_AppliedReserveUs > beforeUs ?
            m_AppliedReserveUs - beforeUs : 0;
    }

    // A cadence-queue drop means the controller retained more standing age
    // than the current service path could carry. Apply a small persistent
    // negative correction and clear contradictory readiness pressure. A
    // later genuine near-miss consumes this relief before attacking upward.
    uint64_t noteBacklog()
    {
        if (m_ForceStatic) {
            return 0;
        }

        uint64_t stepUs;
        switch (m_Policy) {
        case Policy::LowestLatency:
            stepUs = 750;
            break;
        case Policy::Smoothest:
            stepUs = 250;
            break;
        case Policy::Balanced:
        default:
            stepUs = 500;
            break;
        }

        m_PressureReserveUs = 0;
        uint64_t addedUs = qMin(
            stepUs, kMaximumBacklogReliefUs - m_BacklogReliefUs);
        m_BacklogReliefUs += addedUs;
        uint16_t headroom = m_LastHeadroomPermille != UINT16_MAX ?
            m_LastHeadroomPermille : 0;
        uint64_t minimumUs = policyMinimumReserveUs(headroom);
        if (m_AppliedReserveUs > minimumUs) {
            m_AppliedReserveUs -= qMin(
                addedUs, m_AppliedReserveUs - minimumUs);
        }
        m_AttackEvidenceUs = 0;
        m_ReleaseEvidenceUs = 0;
        return addedUs;
    }

    // arrivalSpreadUs is a robust spread of decode-completion phase relative
    // to source timestamps. Queue-age percentiles remain servo feedback only.
    // A zero spread is valid evidence; hasArrivalEvidence distinguishes it
    // from a window that did not receive enough source timestamps to measure.
    void observeWindow(uint64_t lowAgeUs, uint64_t highAgeUs,
                       uint64_t arrivalSpreadUs, bool hasArrivalEvidence,
                       uint64_t windowDurationUs, bool clean,
                       uint64_t sourceIntervalUs, uint64_t serviceFloorUs,
                       bool nearCeiling)
    {
        Q_UNUSED(lowAgeUs);
        Q_UNUSED(highAgeUs);
        Q_UNUSED(nearCeiling);

        if (m_ForceStatic) {
            return;
        }
        if (!clean || sourceIntervalUs == 0) {
            m_AttackEvidenceUs = 0;
            m_ReleaseEvidenceUs = 0;
            return;
        }

        uint16_t headroom = headroomPermille(sourceIntervalUs,
                                             serviceFloorUs);
        int bucket = bucketForPermille(headroom);
        if (hasArrivalEvidence) {
            uint64_t candidateUs = qBound(
                (uint64_t)kMinimumRawDemandUs,
                arrivalSpreadUs + kArrivalGuardUs,
                (uint64_t)kMaximumRawDemandUs);
            updateModel(bucket, candidateUs);
        }

        uint64_t normalizedWindowUs = qMax(windowDurationUs,
                                            kControlWindowUs);
        if (m_BacklogReliefUs != 0) {
            uint64_t releaseUs = scaleStepForWindow(
                dynamics().backlogReliefReleaseStepUs,
                normalizedWindowUs);
            m_BacklogReliefUs -= qMin(m_BacklogReliefUs, releaseUs);
        }
        uint64_t desiredUs = desiredReserveUs(headroom, sourceIntervalUs,
                                               serviceFloorUs);
        if (desiredUs > m_AppliedReserveUs + kSlewDeadbandUs) {
            m_AttackEvidenceUs += normalizedWindowUs;
            m_ReleaseEvidenceUs = 0;
            if (m_AttackEvidenceUs >=
                    (uint64_t)dynamics().attackConfirmWindows *
                        kControlWindowUs) {
                slewAppliedReserve(desiredUs, true, false,
                                   normalizedWindowUs);
            }
        }
        else if (m_AppliedReserveUs > desiredUs + kSlewDeadbandUs) {
            m_ReleaseEvidenceUs += normalizedWindowUs;
            m_AttackEvidenceUs = 0;
            bool fastLowRateRecovery =
                headroom >= kFastLowRateRecoveryHeadroomPermille;
            uint64_t releaseConfirmUs = fastLowRateRecovery ?
                kControlWindowUs :
                (uint64_t)dynamics().releaseConfirmWindows *
                    kControlWindowUs;
            if (m_ReleaseEvidenceUs >= releaseConfirmUs) {
                if (fastLowRateRecovery) {
                    // There is ample display service headroom at this rate.
                    // Do not retain high-refresh protection for tens of
                    // seconds; the cadence-scaled phase trim will consume the
                    // finite excess smoothly below.
                    m_AppliedReserveUs = desiredUs;
                }
                else {
                    slewAppliedReserve(desiredUs, false, false,
                                       normalizedWindowUs);
                }
            }
        }
        else {
            m_AttackEvidenceUs = 0;
            m_ReleaseEvidenceUs = 0;
            m_AppliedReserveUs = desiredUs;
        }

        if (m_PressureReserveUs != 0) {
            uint64_t pressureReleaseStepUs =
                dynamics().pressureReleaseStepUs;
            if (headroom >
                    kHighRefreshCacheFullAuthorityHeadroomPermille) {
                // A clean lower-rate window has real service slack with
                // which to recover from arrival variation. Do not retain a
                // readiness attack there for as long as at 100-116 FPS,
                // where the display ceiling leaves almost no recovery room.
                // This changes release only after clean evidence; an active
                // near-miss taints the window and retains full pressure.
                uint64_t releaseProgress = qMin(
                    headroom,
                    kFastLowRateRecoveryHeadroomPermille) -
                    kHighRefreshCacheFullAuthorityHeadroomPermille;
                uint64_t releaseSpan =
                    kFastLowRateRecoveryHeadroomPermille -
                    kHighRefreshCacheFullAuthorityHeadroomPermille;
                pressureReleaseStepUs +=
                    pressureReleaseStepUs * 2 * releaseProgress /
                        releaseSpan;
            }
            uint64_t releaseUs = scaleStepForWindow(
                pressureReleaseStepUs, normalizedWindowUs);
            m_PressureReserveUs -= qMin(m_PressureReserveUs, releaseUs);
        }
    }

    Target target(bool nearCeiling, bool fixedNearTarget,
                  uint64_t sourceIntervalUs, uint64_t minFrameIntervalUs,
                  uint64_t serviceFloorUs, uint64_t preparationFloorUs,
                  uint64_t scheduleGuardUs, uint64_t clampZoneUs)
    {
        Q_UNUSED(scheduleGuardUs);
        Target result = {};
        uint16_t headroom = headroomPermille(sourceIntervalUs,
                                             serviceFloorUs);
        int bucket = bucketForPermille(headroom);
        uint64_t totalCapUs = sourceIntervalUs != 0 ?
            sourceIntervalUs : kMaximumRawDemandUs;
        uint64_t boundedPreparationFloorUs =
            qMin(preparationFloorUs, totalCapUs);

        if (m_ForceStatic) {
            result.queueAgeUs = m_StaticTargetUs;
            result.preparationFloorUs =
                qMin(boundedPreparationFloorUs, result.queueAgeUs);
            result.protectionReserveUs =
                result.queueAgeUs - result.preparationFloorUs;
            result.modelDemandUs = m_StaticTargetUs;
            result.effectiveDemandUs = m_StaticTargetUs;
        }
        else {
            ModelEstimate estimate = estimateForHeadroom(headroom);
            uint64_t effectiveDemandUs =
                effectiveDemandUsForPolicy(estimate, headroom);
            uint64_t desiredUs = desiredReserveUs(headroom, sourceIntervalUs,
                                                   serviceFloorUs);
            if (m_LastBucket < 0) {
                // Start directly from the confidence-weighted cache estimate;
                // a cold model uses a cadence-aware high-refresh prior.
                // Neither path adds a policy-specific pad.
                m_AppliedReserveUs = desiredUs;
            }
            else if (bucket != m_LastBucket &&
                     desiredUs > m_AppliedReserveUs) {
                // A validated high-refresh cache is intentionally restored
                // immediately after returning from a low-FPS scene. The
                // low-rate path is allowed to shed reserve quickly because it
                // can recover quickly; bringing that scene's tiny target back
                // to 100-116 FPS one 100 us transition step at a time is what
                // used to leave the exact cache-protected regime underfilled.
                // Other bucket transitions retain the bounded handoff, which
                // prevents ordinary cadence dither from creating a target
                // sawtooth.
                bool highRefreshCacheReentry = estimate.restored &&
                    headroom <= kHighRefreshCacheFullAuthorityHeadroomPermille &&
                    m_LastHeadroomPermille >=
                        kCacheAuthorityTaperEndPermille;
                if (highRefreshCacheReentry) {
                    m_AppliedReserveUs = desiredUs;
                }
                else {
                    slewAppliedReserve(desiredUs, true, true, 0, headroom);
                }
                m_ReleaseEvidenceUs = 0;
            }
            m_LastBucket = bucket;
            m_LastHeadroomPermille = headroom;

            // This is a hard bound on controller-induced standing latency.
            // An unavoidable source stall may still produce an older frame,
            // but the learned/cache/pressure state cannot request more than
            // one source interval.
            m_AppliedReserveUs = qMin(
                m_AppliedReserveUs, reserveCapUs(sourceIntervalUs));

            result.preparationFloorUs = boundedPreparationFloorUs;
            result.protectionReserveUs = qMin(
                m_AppliedReserveUs,
                totalCapUs - result.preparationFloorUs);
            result.queueAgeUs = result.preparationFloorUs +
                result.protectionReserveUs;
            result.modelDemandUs = estimate.demandUs;
            result.effectiveDemandUs = effectiveDemandUs;
            result.confidence = estimate.confidence;
            result.learned = estimate.confidence >= kLearnedConfidence;
            result.restored = estimate.restored;
            result.headroomPermille = headroom;
            uint64_t baseAfterHeadroomUs = qMax(
                policyMinimumReserveUs(headroom),
                effectiveDemandUs > usableHeadroomCreditUs(
                    sourceIntervalUs, serviceFloorUs) ?
                    effectiveDemandUs - usableHeadroomCreditUs(
                        sourceIntervalUs, serviceFloorUs) : 0);
            result.headroomCreditUs =
                effectiveDemandUs > baseAfterHeadroomUs ?
                    effectiveDemandUs - baseAfterHeadroomUs : 0;
            result.pressureReserveUs = m_PressureReserveUs;
            result.backlogReliefUs = m_BacklogReliefUs;
        }

        if (nearCeiling && sourceIntervalUs != 0) {
            uint64_t nearCeilingCapUs =
                qMin(sourceIntervalUs,
                     minFrameIntervalUs + clampZoneUs);
            // The physical near-ceiling cap must constrain the internal
            // applied state too. Otherwise a pressure attack can hide reserve
            // above the reported target and expose it after the cadence leaves
            // this band.
            m_AppliedReserveUs = qMin(m_AppliedReserveUs, nearCeilingCapUs);
            if (fixedNearTarget) {
                result.queueAgeUs = nearCeilingCapUs;
                result.preparationFloorUs =
                    qMin(boundedPreparationFloorUs, result.queueAgeUs);
                result.protectionReserveUs =
                    result.queueAgeUs - result.preparationFloorUs;
                result.modelDemandUs = nearCeilingCapUs;
                result.effectiveDemandUs = nearCeilingCapUs;
                result.headroomCreditUs = 0;
                result.pressureReserveUs = 0;
                result.backlogReliefUs = 0;
                result.learned = false;
                result.restored = false;
            }
            else {
                result.queueAgeUs = qMin(result.queueAgeUs,
                                          nearCeilingCapUs);
                result.preparationFloorUs = qMin(
                    result.preparationFloorUs, result.queueAgeUs);
                result.protectionReserveUs =
                    result.queueAgeUs - result.preparationFloorUs;
            }
        }
        return result;
    }

    uint64_t measuredReserveUs() const
    {
        if (m_LastBucket < 0) {
            return UINT64_MAX;
        }
        return estimateForHeadroom(m_LastHeadroomPermille).demandUs;
    }

private:
    static constexpr int kHeadroomBucketPermille = 50;
    static constexpr int kMaximumHeadroomPermille = 950;
    static constexpr int kBucketCount =
        kMaximumHeadroomPermille / kHeadroomBucketPermille + 1;
    static constexpr uint64_t kMinimumAppliedReserveUs = 500;
    static constexpr uint64_t kColdStartDemandUs = 1500;
    static constexpr uint64_t kMinimumRawDemandUs = 500;
    static constexpr uint64_t kMaximumRawDemandUs = 12000;
    static constexpr uint64_t kMaximumPreparationFloorUs = 12000;
    static constexpr uint64_t kMaximumPressureReserveUs = 4000;
    static constexpr uint64_t kMaximumBacklogReliefUs = 3000;
    static constexpr uint64_t kArrivalGuardUs = 250;
    static constexpr uint64_t kMaximumArrivalObservationSpanUs = 2000000;
    static constexpr uint64_t kMaximumArrivalPhaseExcursionUs = 50000;
    static constexpr uint64_t kReadinessEvidenceSlackUs = 250;
    static constexpr uint64_t kSlewDeadbandUs = 150;
    static constexpr uint64_t kControlWindowUs = 500000;
    static constexpr uint64_t kMinimumStaticReserveUs = 1500;
    static constexpr uint64_t kMaximumStaticReserveUs = 6000;
    static constexpr uint16_t kMinimumPersistedConfidence = 200;
    // High-refresh content has very little physical service slack: cached
    // arrival variation is worth more there than at lower FPS, where a queue
    // can be recovered quickly. Restore 90% authority through the top end of
    // the range and taper to the old 60% cap before low-rate behavior takes
    // over. The independent live-validation rules still self-heal stale data.
    static constexpr uint16_t kHighRefreshRestoredConfidence = 900;
    static constexpr uint16_t kStandardRestoredConfidence = 600;
    static constexpr uint16_t kHighRefreshCacheFullAuthorityHeadroomPermille =
        150;
    static constexpr uint16_t kCacheAuthorityTaperEndPermille = 300;
    static constexpr uint16_t kFastLowRateRecoveryHeadroomPermille = 450;
    static constexpr uint64_t kHighRefreshColdStartDemandUs = 2500;
    static constexpr uint16_t kLearnedConfidence = 300;
    static constexpr uint16_t kMinimumLiveWindowsForPersistence = 4;

    struct Dynamics {
        int attackConfirmWindows;
        int releaseConfirmWindows;
        uint64_t attackStepUs;
        uint64_t releaseStepUs;
        uint64_t transitionStepUs;
        uint64_t pressureReleaseStepUs;
        uint64_t backlogReliefReleaseStepUs;
    };

    struct Model {
        bool valid;
        bool restored;
        uint32_t demandUs;
        uint32_t volatilityUs;
        uint16_t confidence;
        uint16_t liveCleanWindows;
        int8_t driftDirection;
        uint8_t driftWindows;
        uint32_t driftBaselineUs;
        uint32_t driftThresholdUs;
    };

    struct ModelEstimate {
        uint64_t demandUs;
        uint64_t volatilityUs;
        uint16_t confidence;
        bool restored;
    };

    const Dynamics& dynamics() const
    {
        static const Dynamics lowest = { 3, 1, 650, 800, 100, 800, 250 };
        static const Dynamics balanced = { 1, 4, 2000, 250, 250, 250, 125 };
        static const Dynamics smoothest = { 1, 6, 2400, 175, 350, 200, 75 };
        switch (m_Policy) {
        case Policy::LowestLatency:
            return lowest;
        case Policy::Smoothest:
            return smoothest;
        case Policy::Balanced:
        default:
            return balanced;
        }
    }

    static uint64_t scaleStepForWindow(uint64_t stepUs,
                                       uint64_t windowDurationUs)
    {
        uint64_t scaledUs = stepUs * windowDurationUs / kControlWindowUs;
        return qMax((uint64_t)1, scaledUs);
    }

    static int bucketForPermille(uint16_t permille)
    {
        int rounded = (permille + kHeadroomBucketPermille / 2) /
            kHeadroomBucketPermille;
        return qBound(0, rounded, kBucketCount - 1);
    }

    static int bucketFor(uint64_t sourceIntervalUs,
                         uint64_t serviceFloorUs)
    {
        return bucketForPermille(
            headroomPermille(sourceIntervalUs, serviceFloorUs));
    }

    static uint64_t coldStartDemandUsForHeadroom(uint16_t headroom)
    {
        if (headroom <= kHighRefreshCacheFullAuthorityHeadroomPermille) {
            return kHighRefreshColdStartDemandUs;
        }
        if (headroom >= kCacheAuthorityTaperEndPermille) {
            return kColdStartDemandUs;
        }

        uint64_t progress = headroom -
            kHighRefreshCacheFullAuthorityHeadroomPermille;
        uint64_t span = kCacheAuthorityTaperEndPermille -
            kHighRefreshCacheFullAuthorityHeadroomPermille;
        return kHighRefreshColdStartDemandUs -
            (kHighRefreshColdStartDemandUs - kColdStartDemandUs) *
                progress / span;
    }

    static uint16_t restoredConfidenceCapForHeadroom(uint16_t headroom)
    {
        if (headroom <= kHighRefreshCacheFullAuthorityHeadroomPermille) {
            return kHighRefreshRestoredConfidence;
        }
        if (headroom >= kCacheAuthorityTaperEndPermille) {
            return kStandardRestoredConfidence;
        }

        uint16_t progress = headroom -
            kHighRefreshCacheFullAuthorityHeadroomPermille;
        uint16_t span = kCacheAuthorityTaperEndPermille -
            kHighRefreshCacheFullAuthorityHeadroomPermille;
        return kHighRefreshRestoredConfidence -
            (uint16_t)((kHighRefreshRestoredConfidence -
                        kStandardRestoredConfidence) * progress / span);
    }

    static ModelEstimate adjustEstimateForHeadroom(ModelEstimate estimate,
                                                    uint16_t headroom)
    {
        if (estimate.confidence == 0) {
            estimate.demandUs = coldStartDemandUsForHeadroom(headroom);
        }
        else if (estimate.restored) {
            estimate.confidence = qMin(
                estimate.confidence,
                restoredConfidenceCapForHeadroom(headroom));
        }
        return estimate;
    }

    uint64_t usableHeadroomCreditUs(uint64_t sourceIntervalUs,
                                    uint64_t serviceFloorUs) const
    {
        if (sourceIntervalUs <= serviceFloorUs) {
            return 0;
        }
        uint64_t physicalHeadroomUs = sourceIntervalUs - serviceFloorUs;
        switch (m_Policy) {
        case Policy::LowestLatency:
            return physicalHeadroomUs * 3 / 4;
        case Policy::Smoothest:
            return physicalHeadroomUs / 4;
        case Policy::Balanced:
        default:
            return physicalHeadroomUs / 2;
        }
    }

    uint64_t policyMinimumReserveUs(uint16_t headroom) const
    {
        uint64_t minimumUs;
        switch (m_Policy) {
        case Policy::LowestLatency:
            minimumUs = kMinimumAppliedReserveUs;
            break;
        case Policy::Smoothest:
            minimumUs = 1250;
            break;
        case Policy::Balanced:
        default:
            minimumUs = 750;
            break;
        }

        // Below the service ceiling the policy floor prevents a cold start
        // from running with no timing elasticity. Once cadence headroom is
        // abundant, renderer preparation is the only unavoidable age and a
        // standing idle queue has no benefit. Taper continuously so rate
        // movement cannot toggle the target at a hard boundary.
        if (headroom <= kCacheAuthorityTaperEndPermille) {
            return minimumUs;
        }
        if (headroom >= kFastLowRateRecoveryHeadroomPermille) {
            return 0;
        }
        uint64_t remaining = kFastLowRateRecoveryHeadroomPermille -
            headroom;
        uint64_t span = kFastLowRateRecoveryHeadroomPermille -
            kCacheAuthorityTaperEndPermille;
        return minimumUs * remaining / span;
    }

    uint64_t reserveCapUs(uint64_t sourceIntervalUs) const
    {
        if (sourceIntervalUs == 0) {
            return kMaximumRawDemandUs;
        }
        uint64_t capUs = sourceIntervalUs;
        if (m_Policy == Policy::LowestLatency) {
            capUs = qMin(capUs, (uint64_t)2500);
        }
        return capUs;
    }

    ModelEstimate estimateForBucket(int bucket) const
    {
        const Model& exact = m_Models[bucket];
        if (exact.valid) {
            return { exact.demandUs, exact.volatilityUs,
                     exact.confidence, exact.restored };
        }

        int lower = bucket - 1;
        while (lower >= 0 && !m_Models[lower].valid) {
            lower--;
        }
        int upper = bucket + 1;
        while (upper < kBucketCount && !m_Models[upper].valid) {
            upper++;
        }

        // Distant rates are different timing regimes. Interpolate only when
        // both sides bracket this headroom, or borrow a single nearby bucket.
        if (lower >= 0 && upper < kBucketCount) {
            int width = upper - lower;
            int progress = bucket - lower;
            uint64_t demandUs =
                ((uint64_t)m_Models[lower].demandUs * (width - progress) +
                 (uint64_t)m_Models[upper].demandUs * progress) / width;
            uint64_t volatilityUs =
                ((uint64_t)m_Models[lower].volatilityUs * (width - progress) +
                 (uint64_t)m_Models[upper].volatilityUs * progress) / width;
            uint16_t confidence = qMin(m_Models[lower].confidence,
                                       m_Models[upper].confidence);
            return { demandUs, volatilityUs,
                     (uint16_t)(confidence * 3 / 4),
                     m_Models[lower].restored || m_Models[upper].restored };
        }

        int neighbor = lower >= 0 ? lower : upper;
        if (neighbor >= 0 && neighbor < kBucketCount &&
                qAbs(neighbor - bucket) <= 2) {
            const Model& model = m_Models[neighbor];
            uint16_t confidence = model.confidence /
                (uint16_t)(qAbs(neighbor - bucket) + 1);
            return { model.demandUs, model.volatilityUs,
                     confidence, model.restored };
        }
        return { kColdStartDemandUs, 0, 0, false };
    }

    ModelEstimate estimateForHeadroom(uint16_t headroom) const
    {
        int lowerBucket = headroom / kHeadroomBucketPermille;
        int upperBucket = qMin(lowerBucket + 1, kBucketCount - 1);
        int progress = headroom % kHeadroomBucketPermille;
        ModelEstimate lower = estimateForBucket(lowerBucket);
        if (upperBucket == lowerBucket || progress == 0) {
            return adjustEstimateForHeadroom(lower, headroom);
        }

        ModelEstimate upper = estimateForBucket(upperBucket);
        uint64_t remaining = kHeadroomBucketPermille - progress;
        ModelEstimate result = {};
        result.demandUs =
            (lower.demandUs * remaining + upper.demandUs * progress) /
                kHeadroomBucketPermille;
        result.volatilityUs =
            (lower.volatilityUs * remaining + upper.volatilityUs * progress) /
                kHeadroomBucketPermille;
        // Interpolating toward a missing adjacent knot may cause both sides
        // to borrow the same live model. Do not halve that model's authority
        // merely because the measured cadence sits between bucket centers.
        result.confidence = qMax(lower.confidence, upper.confidence);
        result.restored = lower.restored || upper.restored;
        return adjustEstimateForHeadroom(result, headroom);
    }

    uint64_t effectiveDemandUsForPolicy(const ModelEstimate& estimate,
                                        uint16_t headroom) const
    {
        // Volatility remains persisted for cache drift validation. Turning a
        // symmetric model-change metric directly into reserve makes a genuine
        // downward self-heal retain the very latency it is trying to remove;
        // rare positive tails are handled by the wider arrival statistic and
        // readiness-pressure path instead.
        return confidenceWeightedDemandUs(
            estimate, coldStartDemandUsForHeadroom(headroom));
    }

    uint64_t desiredReserveUs(uint16_t headroom,
                              uint64_t sourceIntervalUs,
                              uint64_t serviceFloorUs) const
    {
        ModelEstimate estimate = estimateForHeadroom(headroom);
        uint64_t effectiveDemandUs = effectiveDemandUsForPolicy(estimate,
                                                                 headroom);
        uint64_t headroomUs = usableHeadroomCreditUs(sourceIntervalUs,
                                                      serviceFloorUs);
        uint64_t minimumReserveUs = policyMinimumReserveUs(headroom);
        uint64_t baseUs = effectiveDemandUs > headroomUs ?
            effectiveDemandUs - headroomUs : minimumReserveUs;
        baseUs = qMax(baseUs, minimumReserveUs);
        uint64_t reserveCap = reserveCapUs(sourceIntervalUs);
        uint64_t minimumUs = qMin(minimumReserveUs, reserveCap);
        uint64_t protectedUs = qMin(baseUs + m_PressureReserveUs,
                                     reserveCap);
        uint64_t removableUs = protectedUs > minimumUs ?
            protectedUs - minimumUs : 0;
        return protectedUs - qMin(m_BacklogReliefUs, removableUs);
    }

    static uint64_t confidenceWeightedDemandUs(
        const ModelEstimate& estimate, uint64_t coldStartDemandUs)
    {
        if (!estimate.restored) {
            // Live observations are already rate-limited by the selected
            // policy's attack/release dynamics. Weighting their confidence
            // would make the 50-point-per-window confidence ramp slower than
            // every attack profile and erase the policies' useful behavior.
            return estimate.demandUs;
        }

        // Restored confidence is control authority, not just a diagnostic.
        // Blend a cached estimate toward the cadence-aware cold prior so a
        // stale extreme cannot impose its full latency (or risk) at stream
        // start. High-refresh cold starts are intentionally more protective;
        // lower-rate paths shed that reserve through usable service headroom.
        // The cache retains this reduced authority through soft invalidation
        // and earns full live authority after four clean validation windows.
        return ((uint64_t)estimate.demandUs * estimate.confidence +
                coldStartDemandUs * (1000 - estimate.confidence)) / 1000;
    }

    void updateModel(int bucket, uint64_t candidateUs)
    {
        Model& model = m_Models[bucket];
        if (!model.valid) {
            model.valid = true;
            model.restored = false;
            model.demandUs = (uint32_t)candidateUs;
            model.volatilityUs = 0;
            model.confidence = 100;
            model.liveCleanWindows = 1;
            model.driftDirection = 0;
            model.driftWindows = 0;
            model.driftBaselineUs = 0;
            model.driftThresholdUs = 0;
            return;
        }

        uint64_t oldDemandUs = model.demandUs;
        uint64_t differenceUs = candidateUs > oldDemandUs ?
            candidateUs - oldDemandUs : oldDemandUs - candidateUs;
        model.volatilityUs = (uint32_t)
            ((model.volatilityUs * 7ULL + differenceUs) / 8);

        uint64_t driftBaselineUs = model.driftWindows != 0 ?
            model.driftBaselineUs : oldDemandUs;
        uint64_t driftDifferenceUs = candidateUs > driftBaselineUs ?
            candidateUs - driftBaselineUs : driftBaselineUs - candidateUs;
        uint64_t driftThresholdUs = model.driftWindows != 0 ?
            model.driftThresholdUs : qMax(
                (uint64_t)500,
                qMax(oldDemandUs / 4, (uint64_t)model.volatilityUs * 2));
        int driftDirection = candidateUs > driftBaselineUs ? 1 :
            (candidateUs < driftBaselineUs ? -1 : 0);
        if (driftDirection != 0 && driftDifferenceUs > driftThresholdUs) {
            if (model.driftWindows != 0 &&
                    model.driftDirection == driftDirection) {
                model.driftWindows++;
            }
            else {
                // Keep the original prediction and threshold through the
                // confirmation period. Comparing against the moving EWMA
                // could make a sustained 25-35% shift look harmless before
                // it reached the self-healing threshold.
                model.driftBaselineUs = (uint32_t)oldDemandUs;
                model.driftThresholdUs = (uint32_t)driftThresholdUs;
                model.driftDirection = (int8_t)driftDirection;
                model.driftWindows = 1;
            }
        }
        else {
            model.driftDirection = 0;
            model.driftWindows = 0;
            model.driftBaselineUs = 0;
            model.driftThresholdUs = 0;
        }

        bool softInvalidated = model.driftWindows >= 3;
        if (softInvalidated) {
            // Sustained prediction error means the cache is stale. Lower its
            // authority and move halfway to the new regime instead of either
            // clinging to it or discarding all useful history.
            model.confidence /= 2;
            model.demandUs = (uint32_t)(
                ((uint64_t)model.driftBaselineUs + candidateUs) / 2);
            // The healed midpoint is a new prediction. Divergent evidence
            // that invalidated the old one cannot simultaneously validate
            // this replacement for persistence or full cache authority.
            model.liveCleanWindows = 0;
            model.driftDirection = 0;
            model.driftWindows = 0;
            model.driftBaselineUs = 0;
            model.driftThresholdUs = 0;
        }
        else if (candidateUs > oldDemandUs) {
            model.demandUs = (uint32_t)(oldDemandUs +
                qMax((uint64_t)1, (candidateUs - oldDemandUs) / 4));
        }
        else if (candidateUs < oldDemandUs) {
            model.demandUs = (uint32_t)(oldDemandUs -
                qMax((uint64_t)1, (oldDemandUs - candidateUs) / 8));
        }

        if (!softInvalidated &&
                model.liveCleanWindows < kMinimumLiveWindowsForPersistence) {
            model.liveCleanWindows++;
        }
        if (model.liveCleanWindows >= kMinimumLiveWindowsForPersistence) {
            model.restored = false;
        }
        model.confidence = qMin((uint16_t)1000,
                                (uint16_t)(model.confidence + 50));
    }

    void slewAppliedReserve(uint64_t desiredUs, bool attack,
                            bool transition = false,
                            uint64_t windowDurationUs = 0,
                            uint16_t headroom = UINT16_MAX)
    {
        const Dynamics& profile = dynamics();
        uint64_t stepUs = transition ? profile.transitionStepUs :
            (attack ? profile.attackStepUs : profile.releaseStepUs);
        if (transition && headroom != UINT16_MAX) {
            uint64_t minimumStepUs = qMax((uint64_t)50, stepUs / 3);
            uint64_t urgency = 400 - qMin((uint16_t)400, headroom);
            stepUs = minimumStepUs +
                (stepUs - minimumStepUs) * urgency / 400;
        }
        if (!transition && windowDurationUs != 0) {
            stepUs = scaleStepForWindow(stepUs, windowDurationUs);
        }
        if (attack && desiredUs > m_AppliedReserveUs) {
            m_AppliedReserveUs += qMin(desiredUs - m_AppliedReserveUs,
                                       stepUs);
        }
        else if (!attack && m_AppliedReserveUs > desiredUs) {
            m_AppliedReserveUs -= qMin(m_AppliedReserveUs - desiredUs,
                                       stepUs);
        }
    }

    Policy m_Policy;
    bool m_ForceStatic;
    uint64_t m_StaticTargetUs;
    std::array<Model, kBucketCount> m_Models {};
    uint64_t m_AppliedReserveUs;
    uint64_t m_PressureReserveUs;
    uint64_t m_BacklogReliefUs;
    int m_LastBucket;
    uint16_t m_LastHeadroomPermille;
    uint64_t m_AttackEvidenceUs;
    uint64_t m_ReleaseEvidenceUs;
};
