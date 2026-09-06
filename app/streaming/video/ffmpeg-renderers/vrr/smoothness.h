#pragma once

#include "timing.h"
#include <cmath>

namespace Vrr {
struct SmoothnessReport {
    double percent = 0;
    double coverage = 0;
    Ns p95Error = 0, p99Error = 0, worstHold = 0;
    uint64_t intervals = 0, hitches = 0;
    bool available = false;
};

// A source-relative cadence diagnostic, not perceptual smoothness or buffer coverage. Only
// correlated presentation timestamps contribute. Submission cadence cannot
// award a smoothness score. Stable low FPS can be regular but is not high FPS.
class Smoothness {
public:
    explicit Smoothness(Ns minInterval) : m_MinInterval(minInterval) {}
    void submitted(uint64_t id, Ns source) {
        ++m_TotalSubmitted;
        if (m_LastSubmittedId && id == m_LastSubmittedId + 1 && source > m_LastSource && source - m_LastSource < Second)
            m_SourceIntervals.add(std::max(m_MinInterval, source - m_LastSource));
        m_LastSource = source; m_LastSubmittedId = id;
        m_Pending[m_Next] = {id, source, false, m_TotalSubmitted};
        m_Next = (m_Next + 1) % m_Pending.size();
        m_Count = std::min(m_Count + 1, m_Pending.size());
    }
    void feedback(const Feedback& f) {
        if (f.outcome == Outcome::Reset) {
            m_LastPresentation = 0; m_Intervals = {}; m_SourceIntervals = {};
            m_Pending = {}; m_Count = m_Next = m_IntervalCount = m_IntervalNext = 0; return;
        }
        Pending* pending = nullptr;
        for (auto& p : m_Pending) if (f.id && p.id == f.id && !p.known) { pending = &p; break; }
        if (!pending) return;
        if (f.outcome == Outcome::Discarded) { pending->known = pending->discarded = true; ++m_TotalKnown; return; }
        if (f.outcome != Outcome::Presented || f.quality == Quality::Unavailable ||
            f.presented <= m_LastPresentation || f.presented > f.observed ||
            f.observed - f.presented > 100 * Millisecond || f.uncertainty < 0 || f.uncertainty > 500000) return;
        pending->known = true;
        ++m_TotalKnown;
        if (m_Output && f.output && m_Output != f.output) {
            m_LastPresentation = 0; m_Intervals = {}; m_IntervalCount = m_IntervalNext = 0;
        }
        m_Output = f.output;
        const Ns sourceStep = pending->source - m_LastPresentedSource;
        // Missing/unusable feedback is missing evidence, not proof that the
        // screen held the previous frame throughout that gap. Re-anchor instead.
        uint64_t confirmedDiscards = 0;
        if (pending->ordinal > m_LastOrdinal + 1) {
            for (const auto& p : m_Pending)
                confirmedDiscards += p.discarded && p.ordinal > m_LastOrdinal && p.ordinal < pending->ordinal;
        }
        const bool completeEvidence = pending->ordinal == m_LastOrdinal + confirmedDiscards + 1;
        if (m_LastPresentation && completeEvidence && sourceStep > 0 && sourceStep < Second) {
            const Ns interval = f.presented - m_LastPresentation;
            const bool skippedFrames = pending->id > m_LastPresentedId + 1;
            // Follow the actual source cadence, including variable FPS. Only
            // known locally skipped frames use the normal single-frame period.
            const Ns reference = skippedFrames ? m_SourceIntervals.percentile(50, m_MinInterval) :
                std::max(m_MinInterval, sourceStep);
            const Ns skippedContent = skippedFrames ? std::max<Ns>(0, sourceStep - interval) : 0;
            const Ns error = std::max(std::abs(interval - reference), skippedContent);
            const Ns tolerance = std::max<Ns>(500000, reference / 10) + f.uncertainty;
            // Deadband removes invisible timer noise. Severity then grows over
            // one whole source interval, rather than saturating at a 1 us miss.
            const double severity = std::clamp(double(error - tolerance) / reference, 0.0, 1.0);
            m_TotalDuration += interval;
            m_TotalImpaired += interval * severity;
            ++m_TotalIntervals;
            const bool hitch = interval > reference + std::max<Ns>(2 * Millisecond, reference / 4);
            m_TotalHitches += hitch;
            m_TotalWorstHold = std::max(m_TotalWorstHold, interval);
            m_Intervals[m_IntervalNext] = {interval, error, severity,
                hitch};
            m_IntervalNext = (m_IntervalNext + 1) % m_Intervals.size();
            m_IntervalCount = std::min(m_IntervalCount + 1, m_Intervals.size());
        }
        m_LastPresentation = f.presented;
        m_LastPresentedSource = pending->source;
        m_LastOrdinal = pending->ordinal; m_LastPresentedId = pending->id;
    }
    SmoothnessReport report(Ns at = 0) const {
        SmoothnessReport r;
        size_t known = 0;
        for (const auto& p : m_Pending) known += p.id && p.known;
        r.coverage = m_Count ? 100.0 * known / m_Count : 0;
        double elapsed = 0, impaired = 0;
        Samples<256> errors;
        for (size_t i = 0; i < m_IntervalCount; ++i) {
            const auto& sample = m_Intervals[i];
            elapsed += sample.duration;
            impaired += sample.duration * sample.severity;
            errors.add(sample.error);
            r.worstHold = std::max(r.worstHold, sample.duration);
            r.hitches += sample.hitch;
        }
        r.intervals = m_IntervalCount;
        r.percent = elapsed ? std::clamp(100.0 * (1.0 - impaired / elapsed), 0.0, 100.0) : 0;
        r.p95Error = errors.percentile(95); r.p99Error = errors.percentile(99);
        r.available = r.intervals >= 30 && r.coverage >= 90 &&
            (!at || (at >= m_LastPresentation && at - m_LastPresentation <= 100 * Millisecond));
        return r;
    }
    SmoothnessReport sessionReport() const {
        auto r = report();
        r.percent = m_TotalDuration ? std::clamp(100.0 * (1.0 - double(m_TotalImpaired / m_TotalDuration)), 0.0, 100.0) : 0;
        r.coverage = m_TotalSubmitted ? 100.0 * m_TotalKnown / m_TotalSubmitted : 0;
        r.intervals = m_TotalIntervals;
        r.hitches = m_TotalHitches;
        r.worstHold = m_TotalWorstHold;
        r.available = r.intervals >= 30 && r.coverage >= 90;
        return r;
    }
private:
    struct Pending { uint64_t id = 0; Ns source = 0; bool known = false; uint64_t ordinal = 0; bool discarded = false; };
    struct Interval { Ns duration = 0, error = 0; double severity = 0; bool hitch = false; };
    Ns m_MinInterval, m_LastSource = 0, m_LastPresentation = 0, m_LastPresentedSource = 0;
    Samples<64> m_SourceIntervals;
    std::array<Pending, 256> m_Pending{};
    std::array<Interval, 256> m_Intervals{};
    size_t m_Next = 0, m_Count = 0, m_IntervalNext = 0, m_IntervalCount = 0;
    uint64_t m_TotalSubmitted = 0, m_TotalKnown = 0, m_TotalIntervals = 0;
    uint64_t m_TotalHitches = 0;
    uint64_t m_LastSubmittedId = 0, m_LastPresentedId = 0, m_LastOrdinal = 0, m_Output = 0;
    Ns m_TotalWorstHold = 0;
    long double m_TotalDuration = 0, m_TotalImpaired = 0;
};
}
