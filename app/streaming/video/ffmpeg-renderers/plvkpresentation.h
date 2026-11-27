#pragma once

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cstdint>
#include <optional>

// A Gamescope FIFO completion usually arrives one refresh after the previous
// one. Its observation can be late by a coarse poll. Sleep in shorter steps
// across that uncertainty window and revert to coarse polling if a repaint
// is delayed. A prediction only controls when we ask for the terminal ID.
constexpr uint64_t gamescopeRetirementPollUs(uint64_t nowUs, uint64_t lastRetiredUs,
                                            uint64_t refreshPeriodUs)
{
    constexpr uint64_t coarseUs = 1000;
    constexpr uint64_t fineUs = 125;
    if (!lastRetiredUs || !refreshPeriodUs || nowUs < lastRetiredUs) return coarseUs;

    const uint64_t elapsedUs = nowUs - lastRetiredUs;
    if (elapsedUs >= refreshPeriodUs && elapsedUs - refreshPeriodUs >= coarseUs) return coarseUs;
    if (elapsedUs < refreshPeriodUs && refreshPeriodUs - elapsedUs > coarseUs + fineUs) {
        return std::min(coarseUs, refreshPeriodUs - elapsedUs - coarseUs);
    }
    return fineUs;
}

enum class PlVkVrrSurface
{
    Unsupported,
    Wayland,
    Immediate,
    Gamescope,
};

// The capability probe must try the same timed swapchain as playback.
constexpr bool plVkRequestsScheduledPresentation(bool enableVrr, bool ultraLowLatency,
                                                 bool testOnly, bool timingExperiment)
{
    return enableVrr && (ultraLowLatency || (!testOnly && timingExperiment));
}

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
