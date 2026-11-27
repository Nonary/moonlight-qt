#pragma once

#include <algorithm>
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

// A long interval within near-ceiling gameplay can cross the panel's repeat
// point. Refresh existing content only near the two-scan boundary between
// its observed flip and the extrapolated RTP slot.
struct CompositionGapRefresh {
    static uint64_t nextSlot(uint32_t priorFrame, uint32_t priorRtp, uint64_t priorTargetUs,
                             uint32_t frame, uint32_t rtp, uint64_t periodUs)
    {
        if (!priorTargetUs || !periodUs || frame - priorFrame != 1) return 0;
        const uint64_t gapUs = uint64_t(rtp - priorRtp) * 1000 / 90;
        if (gapUs < periodUs * 7 / 4 || gapUs >= periodUs * 2 ||
                gapUs > (std::numeric_limits<uint64_t>::max)() - priorTargetUs) return 0;
        return priorTargetUs + gapUs;
    }

    static bool hasSpareScan(uint64_t displayedUs, uint64_t nowUs,
                             uint64_t nextSlotUs, uint64_t periodUs)
    {
        // The predecessor of frame 2997 crossed the exact two-scan cutoff
        // by tens to hundreds of microseconds in repeated hardware trials.
        // Bound that phase allowance well below the refresh we can save.
        const uint64_t slotToleranceUs = (std::min)(periodUs / 16, uint64_t(500));
        return periodUs && displayedUs && displayedUs <= nowUs && nowUs < nextSlotUs &&
               nextSlotUs - displayedUs >= 2 * periodUs - slotToleranceUs &&
               nextSlotUs - nowUs >= periodUs - periodUs / 8;
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

    void repeat(size_t slot, uint64_t id)
    {
        // The same texture stays in use. Earlier retirement notifications
        // must not release its latest use.
        m_Ids.at(slot) = id;
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
