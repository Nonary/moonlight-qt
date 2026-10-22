#pragma once
#include "inputpercentilebuffer.h"
#include <deque>
#include <vector>

namespace Vrr13 {
// Session coverage. Each actual eligible outcome enters once and ages on the
// session clock, independent of which rate layer currently owns adaptation.
class InputCoverageHistory {
public:
    // Lost-cause spans: a second in which misses exceed several times the
    // profile allowance is the client failing wholesale (startup warm-up,
    // overload), not a tail a buffer trims. Its outcomes still count, so the
    // buffer can respond, but expire after LostCauseRetainUs instead of the
    // selected history; otherwise a bad startup holds the goal failing for
    // minutes after playback is clean.
    static constexpr uint64_t LostCauseWindowUs = 1000000;
    static constexpr uint64_t LostCauseRetainUs = 10000000;
    static constexpr size_t LostCauseMinimumFrames = 20;
    static constexpr uint64_t LostCauseMinimumMissPpm = 100000;
    static constexpr uint64_t LostCauseAllowanceMultiple = 4;
    void setLostCause(bool enabled, uint64_t probabilityPpm) {
        m_LostCauseEnabled = enabled;
        const uint64_t tail = 1000000 - std::clamp<uint64_t>(probabilityPpm, 1, 999999);
        m_LostCauseMissPpm = std::clamp<uint64_t>(tail * LostCauseAllowanceMultiple,
                                                  LostCauseMinimumMissPpm, 1000000);
    }
    void advance(uint64_t nowUs, uint64_t windowUs) {
        // A stale/cancelled callback cannot rewind the reporting clock or
        // erase history. The controller explicitly resets a new session.
        if (nowUs < m_LastUs) return;
        m_LastUs = nowUs;
        windowUs = std::clamp(windowUs, InputPercentileBuffer::MinimumWindowUs,
                             InputPercentileBuffer::MaximumWindowUs);
        while (!m_Outcomes.empty() && nowUs - m_Outcomes.front().atUs >= windowUs)
            pop(m_Outcomes);
        const uint64_t lostRetainUs = std::min(windowUs, LostCauseRetainUs);
        while (!m_LostCause.empty() && nowUs - m_LostCause.front().atUs >= lostRetainUs)
            pop(m_LostCause);
        while (!m_Recent.empty() && nowUs - m_Recent.front().atUs >= LostCauseWindowUs) {
            m_RecentMissed -= m_Recent.front().missed;
            m_Recent.pop_front();
        }
        m_ReportingWindowUs = std::min<uint64_t>(windowUs, 10000000);
        while (!m_ReportingOutcomes.empty() && nowUs - m_ReportingOutcomes.front().atUs >= m_ReportingWindowUs)
            popReporting();
        while (!m_ReportingIneligible.empty() && nowUs - m_ReportingIneligible.front() >= m_ReportingWindowUs)
            m_ReportingIneligible.pop_front();
    }
    void observe(uint64_t nowUs, uint64_t windowUs, bool eligible, bool absorbed, bool missed, uint64_t requiredProtectionUs = 0) {
        if (!nowUs || nowUs < m_LastUs) return;
        advance(nowUs, windowUs);
        if (!eligible) {
            // Reporting only: excluded inputs never enter coverage, so a
            // window of them would otherwise read as frozen or 100%.
            m_ReportingIneligible.push_back(nowUs);
            if (m_ReportingIneligible.size() > InputPercentileBuffer::MaximumSamples)
                m_ReportingIneligible.pop_front();
            return;
        }
        const uint64_t required = absorbed || missed ? requiredProtectionUs : 0;
        m_Outcomes.push_back({nowUs, absorbed, missed, required});
        if (required) {
            m_DisturbanceRequiredSumUs += double(required); ++m_DisturbanceRequiredSamples;
            if (missed) { m_HitchRequiredSumUs += double(required); ++m_HitchRequiredSamples; }
        }
        m_Absorbed += absorbed; m_Missed += missed;
        if (m_LostCauseEnabled) classifyLostCause(nowUs, missed);
        while (m_Outcomes.size() + m_LostCause.size() > InputPercentileBuffer::MaximumSamples)
            pop(m_LostCause.empty() || (!m_Outcomes.empty() &&
                m_Outcomes.front().atUs < m_LostCause.front().atUs) ? m_Outcomes : m_LostCause);
        m_ReportingOutcomes.push_back({nowUs, absorbed, missed});
        m_ReportingAbsorbed += absorbed; m_ReportingMissed += missed;
        while (m_ReportingOutcomes.size() > InputPercentileBuffer::MaximumSamples) popReporting();
    }
    // The full selected window authorizes revision-4 release. The separate
    // short reporting window never controls this gate. An empty window has
    // no failing score; existing unknown-history expiry still applies.
    bool belowGoal(uint64_t probabilityPpm) const {
        const uint64_t tail = 1000000 - std::clamp<uint64_t>(probabilityPpm, 1, 999999);
        return eligible() && m_Missed * 1000000 > eligible() * tail;
    }
    // Minimum protection actually needed by qualifying hitches, excluding
    // their already tolerated excess. The sizing policy adds its reserve once.
    double hitchRequiredSumUs() const { return m_HitchRequiredSumUs; }
    uint64_t hitchRequiredSamples() const { return m_HitchRequiredSamples; }
    // Absorbed and missed disturbances together. Selection does not depend
    // on the applied buffer, so this mean can fall below it.
    double disturbanceRequiredSumUs() const { return m_DisturbanceRequiredSumUs; }
    uint64_t disturbanceRequiredSamples() const { return m_DisturbanceRequiredSamples; }
    uint64_t eligible() const { return m_Outcomes.size() + m_LostCause.size(); }
    uint64_t missed() const { return m_Missed; }
    bool lostCauseActive() const { return m_LostCauseActive; }
    // Earliest outcome of the current lost-cause span; 0 when inactive.
    uint64_t lostCauseSinceUs() const { return m_LostCauseActive ? m_LostCauseSinceUs : 0; }
    uint64_t estimateProbability(uint64_t probabilityPpm) const {
        return InputBufferEstimate::calibratedProbability(probabilityPpm, eligible(), m_Missed);
    }
    void report(InputPercentileBuffer::Stats& stats, bool responsive = false) const {
        stats.recentEligible = eligible();
        stats.recentAbsorbed = m_Absorbed;
        stats.recentMissed = m_Missed;
        stats.lostCauseRetained = m_LostCause.size();
        stats.lostCauseActive = m_LostCauseActive;
        if (responsive) {
            stats.reportingWindowUs = m_ReportingWindowUs;
            stats.reportingEligible = m_ReportingOutcomes.size();
            stats.reportingAbsorbed = m_ReportingAbsorbed;
            stats.reportingMissed = m_ReportingMissed;
            stats.reportingIneligible = m_ReportingIneligible.size();
        }
    }
private:
    struct Outcome { uint64_t atUs; bool absorbed, missed; uint64_t requiredUs = 0; };
    struct Recent { uint64_t atUs; bool missed; };
    void classifyLostCause(uint64_t nowUs, bool missed) {
        m_Recent.push_back({nowUs, missed});
        m_RecentMissed += missed;
        const bool active = m_Recent.size() >= LostCauseMinimumFrames &&
            m_RecentMissed * 1000000 >= m_Recent.size() * m_LostCauseMissPpm;
        if (active && !m_LostCauseActive) m_LostCauseSinceUs = m_Recent.front().atUs;
        m_LostCauseActive = active;
        if (!active) return;
        // Move this span's outcomes, including the second that revealed it,
        // to short retention. Totals are unchanged; only expiry differs.
        const uint64_t cutoff = std::max(m_LostCauseSinceUs, m_LastLostCauseUs + 1);
        std::vector<Outcome> moved;
        while (!m_Outcomes.empty() && m_Outcomes.back().atUs >= cutoff) {
            moved.push_back(m_Outcomes.back());
            m_Outcomes.pop_back();
        }
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) m_LostCause.push_back(*it);
        m_LastLostCauseUs = nowUs;
    }
    void pop(std::deque<Outcome>& outcomes) {
        const Outcome& front = outcomes.front();
        m_Absorbed -= front.absorbed;
        m_Missed -= front.missed;
        if (const uint64_t required = front.requiredUs) {
            m_DisturbanceRequiredSumUs -= double(required);
            if (!--m_DisturbanceRequiredSamples) m_DisturbanceRequiredSumUs = 0;
            if (front.missed) {
                m_HitchRequiredSumUs -= double(required);
                if (!--m_HitchRequiredSamples) m_HitchRequiredSumUs = 0;
            }
        }
        outcomes.pop_front();
    }
    void popReporting() {
        m_ReportingAbsorbed -= m_ReportingOutcomes.front().absorbed;
        m_ReportingMissed -= m_ReportingOutcomes.front().missed;
        m_ReportingOutcomes.pop_front();
    }
    std::deque<Outcome> m_Outcomes, m_LostCause, m_ReportingOutcomes;
    std::deque<Recent> m_Recent;
    std::deque<uint64_t> m_ReportingIneligible;
    uint64_t m_ReportingWindowUs = 0, m_ReportingAbsorbed = 0, m_ReportingMissed = 0;
    uint64_t m_LastUs = 0, m_Absorbed = 0, m_Missed = 0;
    uint64_t m_RecentMissed = 0, m_LostCauseMissPpm = LostCauseMinimumMissPpm;
    uint64_t m_LostCauseSinceUs = 0, m_LastLostCauseUs = 0;
    bool m_LostCauseEnabled = false, m_LostCauseActive = false;
    double m_HitchRequiredSumUs = 0, m_DisturbanceRequiredSumUs = 0;
    uint64_t m_HitchRequiredSamples = 0, m_DisturbanceRequiredSamples = 0;
};
}
