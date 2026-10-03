#pragma once

#include <libplacebo/vulkan.h>

// Mailbox already waits for a display opportunity and replaces stale queued
// images instead of tearing. It can therefore satisfy a protected latch
// request without adding the controller's software spacing floor. Immediate
// and FIFO cannot provide this fallback: Immediate may tear, while FIFO can
// accumulate queued frames.
inline bool plVkPersistentPresentModeProvidesLatchProtection(
    VkPresentModeKHR mode)
{
    return mode == VK_PRESENT_MODE_MAILBOX_KHR;
}
