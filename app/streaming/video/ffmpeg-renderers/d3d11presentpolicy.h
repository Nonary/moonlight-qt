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

struct PendingObservation
{
    bool valid;
    bool pending;
};

// Preserve the controller's planned mode unless DXGI proves a predecessor is
// still pending. The current raster phase cannot predict the phase of a later
// asynchronous flip; making every active-raster observation latch would push
// otherwise safe near-refresh streams onto the driver's slower sync path.
template<class PendingQuery>
bool latch(bool plannedLatched, bool synchronizeAll, PendingQuery&& pendingQuery)
{
    if (plannedLatched || synchronizeAll) return true;
    const PendingObservation observation = pendingQuery();
    return observation.valid && observation.pending;
}

}
