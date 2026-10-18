#pragma once
#include "inputpercentilebuffer.h"
#include <deque>

namespace Vrr13 {
// Session coverage. Each actual eligible outcome enters once and ages on the
// session clock, independent of which rate layer currently owns adaptation.
class InputCoverageHistory {
public:
    void advance(uint64_t nowUs, uint64_t windowUs) {
        // A stale/cancelled callback cannot rewind the reporting clock or
        // erase history. The controller explicitly resets a new session.
        if (nowUs < m_LastUs) return;
        m_LastUs = nowUs;
        windowUs = std::clamp(windowUs, InputPercentileBuffer::MinimumWindowUs,
                             InputPercentileBuffer::MaximumWindowUs);
        while (!m_Outcomes.empty() && nowUs - m_Outcomes.front().atUs >= windowUs)
            pop();
        m_ReportingWindowUs = std::min<uint64_t>(windowUs, 10000000);
        while (!m_ReportingOutcomes.empty() && nowUs - m_ReportingOutcomes.front().atUs >= m_ReportingWindowUs)
            popReporting();
    }
    void observe(uint64_t nowUs, uint64_t windowUs, bool eligible, bool absorbed, bool missed) {
        if (!nowUs || nowUs < m_LastUs) return;
        advance(nowUs, windowUs);
        if (!eligible) return;
        m_Outcomes.push_back({nowUs, absorbed, missed});
        m_Absorbed += absorbed; m_Missed += missed;
        while (m_Outcomes.size() > InputPercentileBuffer::MaximumSamples) pop();
        m_ReportingOutcomes.push_back({nowUs, absorbed, missed});
        m_ReportingAbsorbed += absorbed; m_ReportingMissed += missed;
        while (m_ReportingOutcomes.size() > InputPercentileBuffer::MaximumSamples) popReporting();
    }
    // The full selected window authorizes revision-4 release. The separate
    // short reporting window never controls this gate. An empty window has
    // no failing score; existing unknown-history expiry still applies.
    bool belowGoal(uint64_t probabilityPpm) const {
        const uint64_t tail = 1000000 - std::clamp<uint64_t>(probabilityPpm, 1, 999999);
        return !m_Outcomes.empty() && m_Missed * 1000000 > m_Outcomes.size() * tail;
    }
    uint64_t estimateProbability(uint64_t probabilityPpm) const {
        return InputBufferEstimate::calibratedProbability(probabilityPpm, m_Outcomes.size(), m_Missed);
    }
    void report(InputPercentileBuffer::Stats& stats, bool responsive = false) const {
        stats.recentEligible = m_Outcomes.size();
        stats.recentAbsorbed = m_Absorbed;
        stats.recentMissed = m_Missed;
        if (responsive) {
            stats.reportingWindowUs = m_ReportingWindowUs;
            stats.reportingEligible = m_ReportingOutcomes.size();
            stats.reportingAbsorbed = m_ReportingAbsorbed;
            stats.reportingMissed = m_ReportingMissed;
        }
    }
private:
    struct Outcome { uint64_t atUs; bool absorbed, missed; };
    void pop() {
        m_Absorbed -= m_Outcomes.front().absorbed;
        m_Missed -= m_Outcomes.front().missed;
        m_Outcomes.pop_front();
    }
    void popReporting() {
        m_ReportingAbsorbed -= m_ReportingOutcomes.front().absorbed;
        m_ReportingMissed -= m_ReportingOutcomes.front().missed;
        m_ReportingOutcomes.pop_front();
    }
    std::deque<Outcome> m_Outcomes, m_ReportingOutcomes;
    uint64_t m_ReportingWindowUs = 0, m_ReportingAbsorbed = 0, m_ReportingMissed = 0;
    uint64_t m_LastUs = 0, m_Absorbed = 0, m_Missed = 0;
};
}
