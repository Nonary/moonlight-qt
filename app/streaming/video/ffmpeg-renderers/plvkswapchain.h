#pragma once

#include <libplacebo/vulkan.h>

inline pl_tex_params plVkOffscreenRenderTargetParams(pl_tex target)
{
    pl_tex_params params = {};
    params.w = target->params.w;
    params.h = target->params.h;
    params.format = target->params.format;
    params.renderable = true;
    params.blit_src = true;
    // libplacebo clears letterbox borders with a blit-destination operation,
    // even though the finished image is only copied out of this texture.
    params.blit_dst = true;
    return params;
}

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
