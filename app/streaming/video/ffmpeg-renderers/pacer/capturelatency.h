#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../timinggraph.h"

struct CaptureLatencyStats {
    bool valid = false;
    bool display = false;
    uint64_t medianUs = 0;
    uint64_t p95Us = 0;
    uint64_t uncertaintyUs = 0;
    size_t eligible = 0;
    size_t displayed = 0;
};

// PacerTelemetry serializes access. The present ID can arrive several frames
// later, so keep the submission and its display result in the same record.
class CaptureLatency {
public:
    void clear()
    {
        m_Next = m_Size = 0;
        m_LastId = 0;
        m_HaveId = false;
    }

    void record(const Overlay::TimingGraphInput& event, uint64_t hostStartUs,
                int64_t offsetUs, uint64_t uncertaintyUs, uint64_t displayUncertaintyUs)
    {
        if (event.backend && event.backend != m_Backend) {
            clear();
            m_Backend = event.backend;
        }

        if (event.submitted && event.idValid) {
            if (m_HaveId && event.submissionId < m_LastId) {
                clear();
            }
            m_LastId = event.submissionId;
            m_HaveId = true;
        }

        if (event.submitted && hostStartUs) {
            uint64_t captureUs;
            if (mapCaptureTime(hostStartUs, offsetUs, captureUs) &&
                    captureUs <= event.submissionUs) {
                m_Samples[m_Next] = {event.submissionUs, event.submissionId,
                                     captureUs, uncertaintyUs, 0, 0, event.idValid};
                m_Next = (m_Next + 1) % m_Samples.size();
                m_Size = (std::min)(m_Size + 1, m_Samples.size());
            }
        }

        if (!event.displayValid || !event.displayUs) {
            return;
        }

        for (size_t i = 0; i < m_Size; ++i) {
            auto& sample = m_Samples[(m_Next + m_Samples.size() - 1 - i) % m_Samples.size()];
            if (sample.idValid && sample.id < event.displayId) {
                break;
            }
            if (sample.idValid && sample.id == event.displayId &&
                    !sample.displayUs && event.displayUs >= sample.submissionUs &&
                    event.displayUs - sample.submissionUs < DisplayFreshnessUs) {
                sample.displayUs = event.displayUs;
                sample.displayUncertaintyUs = displayUncertaintyUs;
                break;
            }
        }
    }

    CaptureLatencyStats stats(uint64_t nowUs) const
    {
        CaptureLatencyStats stats;
        for (size_t i = 0; i < m_Size; ++i) {
            const auto& sample = m_Samples[i];
            if (inWindow(nowUs, sample.displayUs, DisplayFreshnessUs)) {
                stats.display = true;
                break;
            }
        }

        std::array<uint64_t, Capacity> sorted;
        size_t count = 0;
        for (size_t i = 0; i < m_Size; ++i) {
            const auto& sample = m_Samples[i];
            if (!inWindow(nowUs, sample.submissionUs, WindowUs)) {
                continue;
            }
            ++stats.eligible;
            const bool displayed = inWindow(nowUs, sample.displayUs, WindowUs);
            stats.displayed += displayed;
            if (stats.display && !displayed) {
                continue;
            }

            const uint64_t endUs = stats.display ? sample.displayUs : sample.submissionUs;
            sorted[count++] = endUs - sample.captureUs;
            stats.uncertaintyUs = (std::max)(stats.uncertaintyUs, sample.clockUncertaintyUs +
                (stats.display ? sample.displayUncertaintyUs : 0));
        }

        if (count) {
            std::sort(sorted.begin(), sorted.begin() + count);
            stats.valid = true;
            stats.medianUs = sorted[(count - 1) / 2];
            stats.p95Us = sorted[(count * 95 + 99) / 100 - 1];
        }
        return stats;
    }

private:
    static constexpr uint64_t WindowUs = 5000000;
    static constexpr uint64_t DisplayFreshnessUs = 2000000;
    static constexpr size_t Capacity = 2048;

    static bool inWindow(uint64_t nowUs, uint64_t sampleUs, uint64_t windowUs)
    {
        return sampleUs && nowUs >= sampleUs && nowUs - sampleUs < windowUs;
    }

    static bool mapCaptureTime(uint64_t hostUs, int64_t offsetUs, uint64_t& clientUs)
    {
        if (offsetUs < 0) {
            const auto magnitude = static_cast<uint64_t>(-(offsetUs + 1)) + 1;
            if (magnitude >= hostUs) {
                return false;
            }
            clientUs = hostUs - magnitude;
        }
        else {
            const auto offset = static_cast<uint64_t>(offsetUs);
            if (hostUs > (std::numeric_limits<uint64_t>::max)() - offset) {
                return false;
            }
            clientUs = hostUs + offset;
        }
        return true;
    }

    struct Sample {
        uint64_t submissionUs, id, captureUs, clockUncertaintyUs;
        uint64_t displayUs, displayUncertaintyUs;
        bool idValid;
    };

    std::array<Sample, Capacity> m_Samples;
    size_t m_Next = 0, m_Size = 0;
    uint64_t m_LastId = 0;
    uint32_t m_Backend = 0;
    bool m_HaveId = false;
};
