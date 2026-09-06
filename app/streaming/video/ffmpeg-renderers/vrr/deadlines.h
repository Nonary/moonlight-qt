#pragma once
#include "timing.h"

namespace Vrr {
struct DeadlineReport {
    uint64_t submitted = 0, measured = 0, misses = 0;
    Ns duration = 0;
    double coverage = 0, feedbackCoverage = 0;
    bool available = false;
};

// Independent measurement of original intended scanout. No retiming, cached
// successes, inferred GPU readiness or missing-feedback successes enter this rate.
class Deadlines {
public:
    void submitted(uint64_t id, Ns at, Ns target) {
        expire(at);
        auto& b = m_Buckets[size_t(at / Second % 300)];
        if (b.second != at / Second) b = {at / Second, 0, 0, 0};
        ++b.submitted;
        m_Pending[m_Next] = {id, at, target};
        m_Next = (m_Next + 1) % m_Pending.size();
        if (!m_First || (m_Last && at - m_Last >= Reserve::Window)) m_First = at;
        m_Last = at;
    }
    void feedback(const Feedback& f) {
        if (f.outcome == Outcome::Reset) { m_Pending = {}; return; }
        auto it = std::find_if(m_Pending.begin(), m_Pending.end(), [&](const Pending& p) { return f.id && p.id == f.id; });
        if (it == m_Pending.end()) return;
        if (f.outcome != Outcome::Discarded &&
            (f.outcome != Outcome::Presented || f.quality == Quality::Unavailable ||
             f.uncertainty < 0 || f.uncertainty > 500000 || f.presented < it->at ||
             f.presented > f.observed || f.observed - f.presented > 100 * Millisecond)) return;
        expire(f.observed);
        auto& b = m_Buckets[size_t(it->at / Second % 300)];
        if (b.second == it->at / Second) {
            ++b.measured;
            b.misses += f.outcome == Outcome::Discarded || std::abs(f.presented - it->target) > Reserve::MissTolerance;
        }
        *it = {};
    }
    DeadlineReport report() const {
        DeadlineReport r;
        for (const auto& b : m_Buckets) {
            r.submitted += b.submitted; r.measured += b.measured; r.misses += b.misses;
        }
        r.duration = m_First ? std::min(Reserve::Window, m_Last - m_First) : 0;
        r.coverage = r.measured ? 100.0 * (1.0 - double(r.misses) / r.measured) : 0;
        r.feedbackCoverage = r.submitted ? 100.0 * r.measured / r.submitted : 0;
        r.available = r.measured >= 30 && r.feedbackCoverage >= 90;
        return r;
    }
private:
    void expire(Ns at) {
        const Ns second = at / Second;
        if (second <= m_ExpiredAt) return;
        for (auto& b : m_Buckets) if (b.second >= 0 && b.second <= second - 300) b = {};
        m_ExpiredAt = second;
    }
    struct Bucket { Ns second = -1; uint64_t submitted = 0, measured = 0, misses = 0; };
    struct Pending { uint64_t id = 0; Ns at = 0, target = 0; };
    std::array<Bucket, 300> m_Buckets{};
    std::array<Pending, 256> m_Pending{};
    size_t m_Next = 0;
    Ns m_First = 0, m_Last = 0, m_ExpiredAt = 0;
};
}
