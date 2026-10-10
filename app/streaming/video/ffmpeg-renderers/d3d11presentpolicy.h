#pragma once

#include <cstdint>

namespace D3D11PresentPolicy {

// A completion observed after releasing the renderer mutex belongs only to
// that exact prepared marker. Cancellation or replacement invalidates it.
class PreparedCompletion
{
public:
    void begin(uint64_t value) { m_Value = value; m_Ready = false; }
    void clear() { begin(0); }
    bool complete(uint64_t value, bool success)
    {
        if (success && value != 0 && value == m_Value) m_Ready = true;
        return ready(value);
    }
    bool ready(uint64_t value) const
    {
        return m_Ready && value != 0 && value == m_Value;
    }

private:
    uint64_t m_Value = 0;
    bool m_Ready = false;
};

struct FlipObservation
{
    bool predecessorKnown = false;
    bool statisticsValid = false;
    bool pending = false;
    bool rasterValid = false;
    bool inVerticalBlank = false;
    uint64_t rasterQueryStartUs = 0;
    uint64_t rasterQueryEndUs = 0;
};

// A retired predecessor can still be scanning out. Unknown or active scanout
// must not authorize a tearing flip. A fresh blank is a risk filter, not a
// reservation of the phase when a later asynchronous flip reaches the panel.
template<class ObservationQuery, class Clock>
bool latch(bool plannedLatched, bool synchronizeAll, uint64_t protectionWindowUs,
           ObservationQuery&& observationQuery, Clock&& clock)
{
    if (plannedLatched || synchronizeAll) return true;
    if (!protectionWindowUs) return false; // Explicitly disabled protection.
    const FlipObservation observation = observationQuery();
    if (!observation.predecessorKnown || !observation.statisticsValid ||
            observation.pending || !observation.rasterValid ||
            !observation.inVerticalBlank) return true;
    const uint64_t nowUs = clock();
    // Bound the entire observation age, including any preemption after the
    // query. This conservative budget does not claim a remaining blank time.
    const uint64_t precisionUs = protectionWindowUs / 16 < 250 ?
        protectionWindowUs / 16 : 250;
    return !precisionUs || !observation.rasterQueryStartUs ||
        observation.rasterQueryEndUs < observation.rasterQueryStartUs ||
        nowUs < observation.rasterQueryEndUs ||
        nowUs - observation.rasterQueryStartUs > precisionUs;
}

}
