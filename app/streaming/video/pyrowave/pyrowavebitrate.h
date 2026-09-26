#pragma once

#include <algorithm>

// The PyroWave author's objective bitrate regression (see
// pyrowave/pyrowave/eval-results/objective-bitrate-evaluation.md).
#include "../../../../pyrowave/pyrowave/eval-results/pyrowave_regression_results.h"

// The author's good-quality bitrate in kbps: 35 dB PSNR-HVS-M-H at a viewing
// distance of twice the screen height, which tracks his subjective "good
// quality" curve, plus the 1.2x he allows for HDR10. His regression covers
// 720p to 4K pixel counts; resolutions outside use the nearest end.
inline int pyroWaveRecommendedKbps(int width, int height, int fps, bool chroma444, bool hdr)
{
    const double pixels = double(width) * height;
    int w = width, h = height;
    if (pixels < PYROWAVE_REGRESSION_MIN_PIXELS) {
        w = 1280;
        h = 720;
    }
    else if (pixels > PYROWAVE_REGRESSION_MAX_PIXELS) {
        w = 3840;
        h = 2160;
    }
    double mbits = pyrowave_psnr_hvs_m_h_estimate_mbits(35, w, h, PYROWAVE_HEIGHT_FACTOR_2_00,
                                                        chroma444 ? 1 : 0, std::max(fps, 1));
    if (hdr) {
        mbits *= 1.2;
    }
    return int(mbits * 1000.0);
}
