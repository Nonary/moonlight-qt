#include "../../app/streaming/video/ffmpeg-renderers/plvkswapchain.h"

#include <libplacebo/renderer.h>

#include <cstdio>
#include <vector>

namespace {
int errors = 0;

void logMessage(void*, pl_log_level level, const char* message)
{
    if (level <= PL_LOG_ERR) {
        ++errors;
        std::fprintf(stderr, "%s\n", message);
    }
}

bool renderBorder(pl_gpu gpu, pl_renderer renderer, pl_fmt format,
                  int width, int height, pl_rect2df crop)
{
    // A previous red image makes a missing border clear observable on readback.
    std::vector<unsigned char> red(width * height * 4);
    std::vector<unsigned char> green(64 * 36 * 4);
    for (size_t i = 0; i < red.size(); i += 4) red[i] = red[i + 3] = 255;
    for (size_t i = 0; i < green.size(); i += 4) green[i + 1] = green[i + 3] = 255;

    pl_tex_params sourceParams = {};
    sourceParams.w = 64;
    sourceParams.h = 36;
    sourceParams.format = format;
    sourceParams.sampleable = true;
    sourceParams.initial_data = green.data();
    pl_tex source = pl_tex_create(gpu, &sourceParams);

    // Use the production target builder shared by both offscreen VRR paths.
    pl_tex_t swapchainTemplate = {};
    swapchainTemplate.params.w = width;
    swapchainTemplate.params.h = height;
    swapchainTemplate.params.format = format;
    auto targetParams = plVkOffscreenRenderTargetParams(&swapchainTemplate);
    targetParams.host_readable = true;
    targetParams.initial_data = red.data();
    pl_tex target = pl_tex_create(gpu, &targetParams);
    bool passed = false;
    if (source && target) {
        pl_swapchain_frame sourceBuffer = {}, targetBuffer = {};
        sourceBuffer.fbo = source;
        targetBuffer.fbo = target;
        sourceBuffer.color_repr = targetBuffer.color_repr = pl_color_repr_rgb;
        sourceBuffer.color_space = targetBuffer.color_space = pl_color_space_srgb;
        pl_frame input = {}, output = {};
        pl_frame_from_swapchain(&input, &sourceBuffer);
        pl_frame_from_swapchain(&output, &targetBuffer);
        output.crop = crop;
        auto params = pl_render_fast_params;
        params.dither_params = nullptr;
        const int previousErrors = errors;
        const bool rendered = pl_render_image(renderer, &input, &output, &params);
        std::vector<unsigned char> pixels(red.size());
        pl_tex_transfer_params transfer = {};
        transfer.tex = target;
        transfer.ptr = pixels.data();
        const bool downloaded = pl_tex_download(gpu, &transfer);
        passed = rendered && downloaded && errors == previousErrors;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool border = x < crop.x0 || x >= crop.x1 ||
                                    y < crop.y0 || y >= crop.y1;
                const auto* pixel = &pixels[(y * width + x) * 4];
                passed = passed && pixel[0] < 3 && pixel[2] < 3 &&
                    (border ? pixel[1] < 3 : pixel[1] > 250);
            }
        }
    }
    std::printf("Offscreen %dx%d border clear and image readback: %s\n",
                width, height, passed ? "PASS" : "FAIL");
    pl_tex_destroy(gpu, &source);
    pl_tex_destroy(gpu, &target);
    return passed;
}
}

int main()
{
    pl_log_params logging = {};
    logging.log_cb = logMessage;
    logging.log_level = PL_LOG_ERR;
    pl_log log = pl_log_create(PL_API_VER, &logging);
    auto vkParams = pl_vulkan_default_params;
    vkParams.allow_software = true;
    pl_vulkan vk = pl_vulkan_create(log, &vkParams);
    if (!vk) {
        std::fprintf(stderr, "Offscreen Vulkan initialization failed\n");
        pl_log_destroy(&log);
        return 1;
    }
    pl_fmt format = pl_find_named_fmt(vk->gpu, "rgba8");
    pl_renderer renderer = pl_renderer_create(log, vk->gpu);
    bool passed = format && renderer;
    if (passed) {
        // Exact aspect ratio, letterbox and pillarbox. Odd sizes represent
        // rounded drawable extents without assuming any compositor behavior.
        passed &= renderBorder(vk->gpu, renderer, format, 64, 36, {0, 0, 64, 36});
        passed &= renderBorder(vk->gpu, renderer, format, 64, 39, {0, 1, 64, 37});
        passed &= renderBorder(vk->gpu, renderer, format, 67, 36, {1, 0, 65, 36});
    }
    passed = passed && errors == 0;
    pl_renderer_destroy(&renderer);
    pl_vulkan_destroy(&vk);
    pl_log_destroy(&log);
    return passed ? 0 : 1;
}
