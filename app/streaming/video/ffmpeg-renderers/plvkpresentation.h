#pragma once

#include <vulkan/vulkan_core.h>

#include <optional>

enum class PlVkVrrSurface
{
    Unsupported,
    Wayland,
    Immediate,
    Gamescope,
};

// Called only after the session and renderer have qualified for VRR. The
// predicate must query the actual Vulkan surface, including any WSI layer.
template<typename SupportsMode>
std::optional<VkPresentModeKHR> selectPlVkVrrPresentMode(
    PlVkVrrSurface surface, bool gamescopeWsi, bool gamescopeMailbox,
    SupportsMode supportsMode)
{
    if (surface == PlVkVrrSurface::Wayland) {
        if (supportsMode(VK_PRESENT_MODE_MAILBOX_KHR)) {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    else if (surface == PlVkVrrSurface::Immediate || surface == PlVkVrrSurface::Gamescope) {
        if (supportsMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        if (surface == PlVkVrrSurface::Gamescope) {
            // Gamescope commonly exposes Mailbox without Immediate. Its WSI
            // layer sends Mailbox to the driver even for a FIFO request, but
            // forwards the application's original mode to the compositor.
            // Opt in to Mailbox to avoid its FIFO commit scheduling
            // after Moonlight has already paced the frame.
            if (gamescopeMailbox && supportsMode(VK_PRESENT_MODE_MAILBOX_KHR)) {
                return VK_PRESENT_MODE_MAILBOX_KHR;
            }
            // Retain the existing WSI compatibility path when the experiment
            // is disabled or Mailbox is unavailable. Ordinary desktop FIFO
            // must not qualify as adaptive.
            if (gamescopeWsi) {
                return VK_PRESENT_MODE_FIFO_KHR;
            }
        }
    }
    return std::nullopt;
}
