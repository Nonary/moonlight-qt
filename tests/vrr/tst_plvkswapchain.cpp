#include "../../app/streaming/video/ffmpeg-renderers/plvkswapchain.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
std::vector<int> calls;
bool creationSucceeds = true;
pl_vulkan_swapchain_params observedParameters{};
pl_color_space observedColorspace{};
const pl_swapchain_t oldChain{};
const pl_swapchain_t newChain{};

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

// Interpose only the native operations used by the shared replacement boundary.
// No window or graphics device is needed for deterministic ownership coverage.
extern "C" void pl_swapchain_destroy(pl_swapchain* chain)
{
    calls.push_back(1);
    *chain = nullptr;
}

extern "C" pl_swapchain pl_vulkan_create_swapchain(pl_vulkan,
                                                  const pl_vulkan_swapchain_params* parameters)
{
    calls.push_back(2);
    observedParameters = *parameters;
    return creationSucceeds ? &newChain : nullptr;
}

extern "C" void pl_swapchain_colorspace_hint(pl_swapchain chain, const pl_color_space* colorspace)
{
    expect(chain == &newChain, "the colorspace hint must target the replacement chain");
    calls.push_back(3);
    observedColorspace = *colorspace;
}

int main()
{
    pl_vulkan_swapchain_params parameters{};
    parameters.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    parameters.swapchain_depth = 2;
    pl_color_space colorspace{};
    colorspace.primaries = PL_COLOR_PRIM_BT_2020;
    colorspace.transfer = PL_COLOR_TRC_PQ;
    colorspace.hdr.max_luma = 1000.0f;
    pl_swapchain chain = &oldChain;

    expect(!recreatePlVkSwapchain(nullptr, &chain, parameters, colorspace, true),
           "an acquired image must prevent replacement");
    expect(calls.empty() && chain == &oldChain,
           "refusing replacement must not destroy or submit the pending chain");

    for (auto mode : {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
                     VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
        calls.clear();
        parameters.present_mode = mode;
        expect(recreatePlVkSwapchain(nullptr, &chain, parameters, colorspace, false),
               "FIFO and adaptive replacements must succeed");
        expect(calls == std::vector<int>({1, 2, 3}),
               "retire the old chain, create its replacement, and restore HDR before acquisition");
        expect(chain == &newChain && observedParameters.present_mode == mode &&
               observedParameters.swapchain_depth == 2,
               "the actual native creation must receive the requested mode and existing depth");
        expect(observedColorspace.primaries == PL_COLOR_PRIM_BT_2020 &&
               observedColorspace.transfer == PL_COLOR_TRC_PQ &&
               observedColorspace.hdr.max_luma == 1000.0f,
               "replacement must retain HDR primaries, transfer, and luminance metadata");
    }

    calls.clear();
    creationSucceeds = false;
    expect(!recreatePlVkSwapchain(nullptr, &chain, parameters, colorspace, false),
           "native creation failure must propagate to preparation");
    expect(chain == nullptr && calls == std::vector<int>({1, 2}),
           "a failed replacement must not retain a retired handle or hint a missing chain");
    return failures ? 1 : 0;
}
