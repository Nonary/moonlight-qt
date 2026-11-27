#include "../../app/streaming/video/ffmpeg-renderers/plvkpresentation.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>

namespace {
int failures = 0;

void check(PlVkVrrSurface surface, bool wsi,
           std::initializer_list<VkPresentModeKHR> available,
           std::optional<VkPresentModeKHR> expected, const char* description,
           bool gamescopeMailbox = true)
{
    const auto selected = selectPlVkVrrPresentMode(surface, wsi, gamescopeMailbox,
        [&](VkPresentModeKHR mode) {
            return std::find(available.begin(), available.end(), mode) != available.end();
        });
    if (selected != expected || (selected &&
            std::find(available.begin(), available.end(), *selected) == available.end())) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}
}

int main()
{
    constexpr uint64_t last = 10000, refresh = 8333;
    static_assert(gamescopeRetirementPollUs(last, 0, refresh) == 1000);
    static_assert(gamescopeRetirementPollUs(last - 1, last, refresh) == 1000);
    static_assert(gamescopeRetirementPollUs(last + 6000, last, refresh) == 1000);
    static_assert(gamescopeRetirementPollUs(last + 7000, last, refresh) == 333);
    static_assert(gamescopeRetirementPollUs(last + refresh, last, refresh) == 125);
    static_assert(gamescopeRetirementPollUs(last + 9500, last, refresh) == 1000);
    static_assert(plVkRequestsScheduledPresentation(true, true, false, false));
    static_assert(plVkRequestsScheduledPresentation(true, true, true, false));
    static_assert(!plVkRequestsScheduledPresentation(true, false, false, false));
    static_assert(!plVkRequestsScheduledPresentation(false, true, false, true));
    static_assert(plVkRequestsScheduledPresentation(true, false, false, true));
    static_assert(!plVkRequestsScheduledPresentation(true, false, true, true));
    using Surface = PlVkVrrSurface;
    constexpr auto fifo = VK_PRESENT_MODE_FIFO_KHR;
    constexpr auto mailbox = VK_PRESENT_MODE_MAILBOX_KHR;
    constexpr auto immediate = VK_PRESENT_MODE_IMMEDIATE_KHR;

    check(Surface::Gamescope, true, {fifo, mailbox}, fifo,
          "unchecked experiment restores Gamescope WSI FIFO", false);
    check(Surface::Gamescope, false, {fifo, mailbox}, std::nullopt,
          "unchecked experiment preserves non-WSI fallback", false);
    check(Surface::Gamescope, true, {fifo, mailbox, immediate}, immediate,
          "unchecked experiment preserves Immediate priority", false);
    check(Surface::Wayland, false, {fifo, mailbox}, mailbox,
          "unchecked experiment does not affect ordinary Wayland", false);

    // Regression: Mesa Wayland surfaces may expose FIFO and Mailbox without
    // Immediate. Selecting FIFO here hands paced video to another scheduler.
    check(Surface::Gamescope, true, {fifo, mailbox}, mailbox,
          "Gamescope WSI must use exposed Mailbox before its FIFO compatibility path");
    check(Surface::Gamescope, false, {fifo, mailbox}, mailbox,
          "Gamescope Mailbox support must not depend on a WSI environment flag");
    check(Surface::Gamescope, true, {fifo, mailbox, immediate}, immediate,
          "existing Gamescope Immediate presentation must retain priority");
    check(Surface::Gamescope, true, {fifo}, fifo,
          "FIFO-only Gamescope WSI must retain adaptive-worker compatibility");
    check(Surface::Gamescope, false, {fifo}, std::nullopt,
          "Gamescope FIFO without WSI must fall back to fixed pacing");
    check(Surface::Wayland, false, {fifo, mailbox, immediate}, mailbox,
          "ordinary Wayland must retain Mailbox priority");
    check(Surface::Wayland, false, {fifo, immediate}, std::nullopt,
          "ordinary Wayland must not use Immediate as a substitute");
    check(Surface::Immediate, false, {fifo, mailbox, immediate}, immediate,
          "X11 and KMSDRM must retain Immediate priority");
    check(Surface::Immediate, true, {fifo, mailbox}, std::nullopt,
          "a WSI flag alone must not enable the Gamescope exception on ordinary X11");
    check(Surface::Unsupported, true, {fifo, mailbox, immediate}, std::nullopt,
          "an unsupported window backend must not qualify for VRR");
    return failures ? 1 : 0;
}
