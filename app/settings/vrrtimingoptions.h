#pragma once

#include <algorithm>

// Source-frame allowance, percentage in hundredths, recent history, and
// interval tolerance in microseconds.
// Zero means use the selected preset (for callers without saved preferences).
struct VrrTimingOptions {
    int bufferPerMille = 0;
    int targetHundredths = 0;
    int historySeconds = 0;
    int toleranceUs = 0;

    static VrrTimingOptions preset(int mode)
    {
        return mode == 2 ? VrrTimingOptions{500, 9900, 60, 500} :
               mode == 0 ? VrrTimingOptions{4000, 9995, 300, 250} :
                           VrrTimingOptions{1000, 9950, 120, 500};
    }

    VrrTimingOptions resolved(int mode) const
    {
        const auto defaults = preset(mode);
        return {
            std::max(250, std::min(4000, bufferPerMille == 0 ? defaults.bufferPerMille : bufferPerMille)),
            std::max(9000, std::min(9999, targetHundredths == 0 ? defaults.targetHundredths : targetHundredths)),
            std::max(10, std::min(300, historySeconds == 0 ? defaults.historySeconds : historySeconds)),
            ((std::max(250, std::min(2000, toleranceUs == 0 ? defaults.toleranceUs : toleranceUs)) + 125) / 250) * 250
        };
    }
};
