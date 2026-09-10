#pragma once

#include <libplacebo/vulkan.h>

// Keep the native replacement boundary explicit and testable. A libplacebo
// start_frame holds a swapchain lock until submit_frame; destroying that chain
// with an acquired image would deadlock or lose the image's ownership.
inline bool recreatePlVkSwapchain(pl_vulkan vulkan, pl_swapchain* swapchain,
                                  const pl_vulkan_swapchain_params& parameters,
                                  const pl_color_space& colorspace,
                                  bool hasPendingFrame)
{
    if (hasPendingFrame) {
        return false;
    }
    pl_swapchain_destroy(swapchain);
    *swapchain = pl_vulkan_create_swapchain(vulkan, &parameters);
    if (*swapchain == nullptr) {
        return false;
    }
    // Restore before resize/start_frame chooses the new surface format. The
    // next video frame may have unchanged HDR metadata and skip re-hinting.
    pl_swapchain_colorspace_hint(*swapchain, &colorspace);
    return true;
}
