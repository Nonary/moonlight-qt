#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cstdint>
#include <climits>

// Controls decode-completion-to-render-start queue age. The configured value
// is a policy cap for measured timing variation, not a fixed delay. Near the
// refresh ceiling, a time-based phase reserve decouples presentation from
// arrivals and a transient safety reserve protects acquisition and re-lock.
class VrrQueueAgeController
{
public:
    struct Target {
        uint64_t queueAgeUs;
        uint64_t policyReserveUs;
        uint64_t phaseReserveUs;
        uint64_t safetyReserveUs;
        bool learned;
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
        uint64_t sourceIntervalUs;
        int sampleCount;
        bool nearCeiling;
        bool windowTainted;
        bool phaseAdvanceActive;
        bool phaseDelayActive;
        bool fastRecoveryActive;
        bool staleSchedule;
        bool overfillEligible;
    };

    struct PhaseDecision {
        uint64_t advanceUs;
        uint64_t delayUs;
        bool requestOverfillDrop;
    };

    VrrQueueAgeController(uint64_t policyCapUs, bool forceStatic) :
        m_PolicyCapUs(policyCapUs),
        m_ForceStatic(forceStatic),
        m_SpreadHead(0),
        m_SpreadCount(0),
        m_RecoveryCleanWindows(0),
        m_CleanSafetyWindows(0),
        m_MeasuredReserveUs(UINT64_MAX),
        m_SafetyReserveUs(0)
    {
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

    static PhaseDecision decidePhase(const PhaseDecisionInput& input)
    {
        PhaseDecision result = {};
        if (input.sampleCount <= 0) {
            return result;
        }

        // Median age is the controlled value in both directions. Using a low
        // percentile only for build creates a limit cycle on wide windows:
        // trim moves the median to target, then build moves the low tail there.
        if (!input.phaseDelayActive && !input.fastRecoveryActive &&
                input.stats.medianUs > input.targetAgeUs + 1000) {
            result.advanceUs = qBound(
                (uint64_t)20,
                (input.stats.medianUs - input.targetAgeUs) /
                    (uint64_t)input.sampleCount,
                (uint64_t)250);
        }
        else if (!input.windowTainted &&
                  !input.phaseAdvanceActive && !input.phaseDelayActive &&
                  !input.fastRecoveryActive &&
                  input.stats.medianUs + 1000 < input.targetAgeUs) {
            // A small policy reserve is useful below the panel ceiling too:
            // it absorbs decode/network arrival jitter before it reaches the
            // cadence clock. Keeping the build bounded and gradual preserves
            // low latency while making the selected 2.5/4.5/6ms policy real
            // for the common 20-80 FPS range.
            uint64_t maxDelayStepUs = 100;
            if (input.targetAgeUs > input.previousTargetAgeUs) {
                maxDelayStepUs = qBound((uint64_t)100,
                    input.sourceIntervalUs / 32, (uint64_t)300);
            }
            result.delayUs = qBound(
                (uint64_t)20,
                (input.targetAgeUs - input.stats.medianUs) /
                    (uint64_t)input.sampleCount,
                maxDelayStepUs);
        }

        result.requestOverfillDrop = input.nearCeiling &&
            !input.windowTainted && !input.staleSchedule &&
            input.overfillEligible &&
            input.stats.p20Us > input.targetAgeUs +
                input.sourceIntervalUs / 2;
        return result;
    }

    static bool shouldDiscardPhaseAdvance(bool staleSchedule,
                                          bool scheduleRecoveryRebased)
    {
        return staleSchedule || scheduleRecoveryRebased;
    }

    void resetLearning()
    {
        m_SpreadHead = 0;
        m_SpreadCount = 0;
        m_RecoveryCleanWindows = 0;
        m_CleanSafetyWindows = 0;
        m_MeasuredReserveUs = UINT64_MAX;
    }

    void enterNearCeiling(uint64_t sourceIntervalUs)
    {
        m_RecoveryCleanWindows = kRecoveryHoldWindows;
        m_CleanSafetyWindows = 0;
        m_SafetyReserveUs = safetyReserveLimitUs(sourceIntervalUs);
    }

    void leaveNearCeiling()
    {
        m_RecoveryCleanWindows = 0;
        m_CleanSafetyWindows = 0;
        m_SafetyReserveUs = 0;
    }

    // lowAgeUs/highAgeUs are robust distribution bounds (currently p10/p90),
    // not raw extrema. Isolated stalls and near-empty samples are handled by
    // explicit recovery signals instead of being learned as normal variation.
    void observeWindow(uint64_t lowAgeUs, uint64_t highAgeUs, bool clean,
                       bool nearCeiling)
    {
        if (m_ForceStatic) {
            return;
        }

        if (!clean || lowAgeUs == UINT64_MAX || highAgeUs < lowAgeUs) {
            m_CleanSafetyWindows = 0;
            return;
        }

        uint64_t spreadUs = highAgeUs - lowAgeUs;
        m_SpreadUs[m_SpreadHead] =
            (uint32_t)qMin(spreadUs, (uint64_t)UINT32_MAX);
        m_SpreadHead = (m_SpreadHead + 1) % kSpreadWindowCount;
        if (m_SpreadCount < kSpreadWindowCount) {
            m_SpreadCount++;
        }

        // Recovery hold measures clean time, not learned-history age. Keeping
        // this inside the learning gate made a cold controller pay eight
        // windows to collect evidence and another seven to clear an
        // nominally eight-window hold.
        bool recoveryWasActive = nearCeiling &&
            m_RecoveryCleanWindows != 0;
        if (recoveryWasActive) {
            m_RecoveryCleanWindows--;
            m_CleanSafetyWindows = 0;
        }

        if (m_SpreadCount >= kLearningWindowCount) {
            uint64_t worstUs = 0;
            for (int i = 0; i < m_SpreadCount; i++) {
                worstUs = qMax(worstUs, (uint64_t)m_SpreadUs[i]);
            }
            uint64_t candidateReserveUs = worstUs + kSpreadGuardUs;
            if (m_MeasuredReserveUs == UINT64_MAX ||
                    candidateReserveUs > m_MeasuredReserveUs) {
                m_MeasuredReserveUs = candidateReserveUs;
            }
            else {
                m_MeasuredReserveUs -= qMin(
                    m_MeasuredReserveUs - candidateReserveUs,
                    kMeasuredReleaseStepUs);
            }

            // Attack comes from the new measured target immediately. Release
            // needs sustained usable windows, then both the learned target
            // and acquisition reserve move down at bounded rates.
            if (nearCeiling && !recoveryWasActive &&
                    m_SafetyReserveUs != 0) {
                if (m_CleanSafetyWindows < kSafetyReleaseDelayWindows) {
                    m_CleanSafetyWindows++;
                }
                else {
                    m_SafetyReserveUs -= qMin(m_SafetyReserveUs,
                                               kSafetyReleaseStepUs);
                }
            }
        }
    }

    Target target(bool nearCeiling, bool fixedNearTarget,
                  uint64_t sourceIntervalUs, uint64_t minFrameIntervalUs,
                  uint64_t scheduleGuardUs, uint64_t clampZoneUs) const
    {
        Target result = {};
        result.learned = !m_ForceStatic && m_MeasuredReserveUs != UINT64_MAX &&
            (!nearCeiling || m_RecoveryCleanWindows == 0);
        result.policyReserveUs = result.learned ?
            qBound(kMinimumReserveUs, m_MeasuredReserveUs, m_PolicyCapUs) :
            m_PolicyCapUs;
        result.queueAgeUs = result.policyReserveUs;

        if (!nearCeiling || sourceIntervalUs == 0) {
            return result;
        }

        uint64_t nearCeilingCapUs =
            qMin(sourceIntervalUs + scheduleGuardUs,
                 minFrameIntervalUs + clampZoneUs);
        if (fixedNearTarget) {
            result.learned = false;
            result.phaseReserveUs = nearCeilingCapUs;
            result.queueAgeUs = nearCeilingCapUs;
            return result;
        }

        // The interval-relative reserve is a steady-state lower bound inside
        // the selected policy, not another hidden buffer added on top.
        result.phaseReserveUs = qMin(m_PolicyCapUs,
                                     sourceIntervalUs * 5 / 8);
        result.safetyReserveUs = m_ForceStatic ?
            safetyReserveLimitUs(sourceIntervalUs) :
            qMin(m_SafetyReserveUs, safetyReserveLimitUs(sourceIntervalUs));
        result.queueAgeUs = qMin(qMax(result.policyReserveUs,
                                      result.phaseReserveUs) +
                                     result.safetyReserveUs,
                                 nearCeilingCapUs);
        return result;
    }

    uint64_t measuredReserveUs() const
    {
        return m_MeasuredReserveUs;
    }

private:
    static constexpr int kSpreadWindowCount = 24;
    static constexpr int kLearningWindowCount = 8;
    static constexpr int kRecoveryHoldWindows = 8;
    static constexpr int kSafetyReleaseDelayWindows = 4;
    static constexpr uint64_t kMinimumReserveUs = 1500;
    static constexpr uint64_t kSpreadGuardUs = 750;
    static constexpr uint64_t kMeasuredReleaseStepUs = 250;
    static constexpr uint64_t kSafetyReleaseStepUs = 250;
    static constexpr uint64_t kMaximumSafetyReserveUs = 2500;

    static uint64_t safetyReserveLimitUs(uint64_t sourceIntervalUs)
    {
        return qMin(sourceIntervalUs / 4, kMaximumSafetyReserveUs);
    }

    uint64_t m_PolicyCapUs;
    bool m_ForceStatic;
    uint32_t m_SpreadUs[kSpreadWindowCount];
    int m_SpreadHead;
    int m_SpreadCount;
    int m_RecoveryCleanWindows;
    int m_CleanSafetyWindows;
    uint64_t m_MeasuredReserveUs;
    uint64_t m_SafetyReserveUs;
};
