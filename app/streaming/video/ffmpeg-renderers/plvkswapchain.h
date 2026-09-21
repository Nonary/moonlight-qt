#pragma once

#include <libplacebo/vulkan.h>

// Mailbox already waits for a display opportunity and replaces stale queued
// images instead of tearing. It can therefore satisfy a protected latch
// request without adding the controller's software spacing floor. Immediate
// cannot provide this fallback. Ordinary FIFO may accumulate queued frames,
// but Gamescope WSI implements its FIFO contract with a Mailbox driver
// swapchain and compositor synchronization. Adding the software floor there
// can itself create a backlog below the physical refresh rate.
inline bool plVkPersistentPresentModeProvidesLatchProtection(
    VkPresentModeKHR mode, bool gamescopeWsi = false)
{
    return mode == VK_PRESENT_MODE_MAILBOX_KHR ||
        (gamescopeWsi && mode == VK_PRESENT_MODE_FIFO_KHR);
}
