#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// The pacing clock has a local epoch. Windows presentation uses QPC-scaled
// 100 ns units. Correlate them for each present, just as for display feedback;
// an epoch measured before sleep must never schedule a later present.
struct CompositionPresentTarget {
    uint64_t time100ns = 0;
    uint64_t uncertaintyUs = 0;

    static CompositionPresentTarget translate(uint64_t targetUs,
                                               uint64_t reference100ns,
                                               uint64_t beforeUs,
                                               uint64_t afterUs)
    {
        if (!reference100ns || !beforeUs || afterUs < beforeUs ||
                afterUs - beforeUs > 500) {
            return {};
        }
        const uint64_t midpointUs = beforeUs + (afterUs - beforeUs) / 2;
        const uint64_t uncertaintyUs = (afterUs - beforeUs + 1) / 2 + 1;
        // An elapsed logical deadline requests presentation as soon as possible.
        // Never push it a source/display period into the future to hide lateness.
        if (targetUs <= midpointUs) {
            return {reference100ns, uncertaintyUs};
        }
        constexpr uint64_t max = (std::numeric_limits<uint64_t>::max)();
        const uint64_t deltaUs = targetUs - midpointUs;
        if (deltaUs > (max - reference100ns) / 10) {
            return {};
        }
        return {reference100ns + deltaUs * 10, uncertaintyUs};
    }
};

// Each accepted present owns one of the existing displayable buffers. The
// native availability/fence/status evidence releases this accounting; a CPU
// deadline or a Queued notification alone does not prove display or retirement.
template <size_t Capacity>
class CompositionPresentOwnership {
public:
    void reset() { m_Ids = {}; }

    bool accept(size_t slot, uint64_t id)
    {
        if (slot >= Capacity || !id || m_Ids[slot]) return false;
        for (const auto acceptedId : m_Ids) {
            if (acceptedId == id) return false;
        }
        m_Ids[slot] = id;
        return true;
    }

    void available(size_t slot)
    {
        if (slot < Capacity) m_Ids[slot] = 0;
    }

    void terminal(uint64_t id)
    {
        if (!id) return;
        for (auto& acceptedId : m_Ids) {
            if (acceptedId == id) acceptedId = 0;
        }
    }

    void retireThrough(uint64_t id)
    {
        for (auto& acceptedId : m_Ids) {
            if (acceptedId && acceptedId <= id) acceptedId = 0;
        }
    }

    unsigned outstanding() const
    {
        unsigned count = 0;
        for (const auto id : m_Ids) count += id != 0;
        return count;
    }

private:
    std::array<uint64_t, Capacity> m_Ids = {};
};
