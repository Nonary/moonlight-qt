#include "vulkangpu.h"
#include <libplacebo/vulkan.h>
#include <cassert>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

int main()
{
    auto params = pl_vulkan_default_params;
    params.allow_software = true;
    pl_vulkan vk = pl_vulkan_create(nullptr, &params);
    if (!vk) { std::cerr << "No Vulkan device available\n"; return 77; }
    VkPhysicalDeviceProperties device{};
    vkGetPhysicalDeviceProperties(vk->phys_device, &device);
    std::cout << "Vulkan device: " << device.deviceName << '\n';
    const auto gpu = vk->gpu;
    auto completion = std::make_unique<Vrr::VulkanCompletion>();
    assert(completion->initialize(vk));
    pl_tex_params texture{};
    texture.w = texture.h = 128;
    // Exercise HDR-capable image storage. This is not a display/HDR metadata test.
    texture.format = pl_find_named_fmt(gpu, "rgba16f");
    assert(texture.format);
    texture.blit_dst = true;
    texture.host_readable = true;
    texture.host_writable = true;
    pl_tex image = pl_tex_create(gpu, &texture);
    assert(image);
    std::atomic<bool> stopping{false};
    for (unsigned i = 0; i < 128; ++i) {
        const float color[4] = {0.25f, 0.5f, 2.0f, 1.0f};
        const auto began = Vrr::now();
        pl_tex_clear(gpu, image, color);
        Vrr::Preparation timing{};
        assert(completion->wait(image, stopping, timing, began));
        assert(timing.commandsSubmitted >= began && timing.gpuNotReady >= began);
        assert(timing.gpuReady >= timing.gpuNotReady && timing.gpuReady >= timing.commandsSubmitted);
    }
    // libplacebo's rgba16f uses float32 host transfers with float16 storage.
    assert(texture.format->texel_size == 4 * sizeof(float) && texture.format->host_bits[0] == 32);
    std::vector<float> pixels(128 * 128 * 4);
    pl_tex_transfer_params download{};
    download.tex = image; download.ptr = pixels.data();
    assert(pl_tex_download(gpu, &download));
    for (size_t i = 0; i < pixels.size(); i += 4) {
        assert(pixels[i] == 0.25f && pixels[i + 1] == 0.5f);
        assert(pixels[i + 2] == 2.0f && pixels[i + 3] == 1.0f);
    }

    // An unsignaled imported-image dependency must keep preparation incomplete.
    // It also lets us test cancellation without depending on GPU speed.
    pl_vulkan_sem_params semaphore{};
    semaphore.type = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphore ready = pl_vulkan_sem_create(gpu, &semaphore);
    VkSemaphore gate = pl_vulkan_sem_create(gpu, &semaphore);
    assert(ready && gate);
    pl_vulkan_hold_params hold{};
    hold.tex = image; hold.layout = VK_IMAGE_LAYOUT_GENERAL;
    hold.qf = VK_QUEUE_FAMILY_IGNORED; hold.semaphore = {ready, 1};
    assert(pl_vulkan_hold_ex(gpu, &hold));
    pl_gpu_flush(gpu);
    const uint64_t one = 1;
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1; wait.pSemaphores = &ready; wait.pValues = &one;
    assert(vkWaitSemaphores(vk->device, &wait, 1000000000) == VK_SUCCESS);
    pl_vulkan_release_params release{};
    release.tex = image; release.layout = VK_IMAGE_LAYOUT_GENERAL;
    release.qf = VK_QUEUE_FAMILY_IGNORED; release.semaphore = {gate, 1};
    pl_vulkan_release_ex(gpu, &release);
    // Force the host-copy fallback: its callback may release the input before
    // the destination image is ready. Overlay publication uses the image's
    // timeline, so it must still remain incomplete behind the unsignaled gate.
    std::atomic<bool> hostReleased{false};
    pl_tex_transfer_params upload{};
    upload.tex = image; upload.ptr = pixels.data(); upload.no_import = true;
    upload.callback = [](void* p) { static_cast<std::atomic<bool>*>(p)->store(true); };
    upload.priv = &hostReleased;
    assert(pl_tex_upload(gpu, &upload));
    std::thread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        stopping.store(true);
    });
    const auto began = Vrr::now();
    Vrr::Preparation timing{};
    const bool completed = completion->wait(image, stopping, timing, began);
    const auto elapsed = Vrr::now() - began;
    cancel.join();
    VkSemaphoreSignalInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal.semaphore = gate; signal.value = 1;
    assert(vkSignalSemaphore(vk->device, &signal) == VK_SUCCESS);
    pl_gpu_finish(gpu);
    assert(hostReleased.load());
    assert(!completed && timing.gpuReady == 0 && elapsed < 100 * Vrr::Millisecond);
    pl_tex_destroy(gpu, &image);

    // The overlay worker reuses two BGRA textures and publishes only a
    // completed destination image, including when the host import is disabled.
    texture.format = pl_find_named_fmt(gpu, "bgra8");
    texture.sampleable = true;
    assert(texture.format);
    pl_tex overlays[2] = {};
    std::vector<uint8_t> overlayPixels(128 * 128 * 4), readback(overlayPixels.size());
    stopping.store(false);
    for (unsigned i = 0; i < 32; ++i) {
        auto& tex = overlays[i % 2];
        auto previous = tex;
        assert(pl_tex_recreate(gpu, &tex, &texture));
        assert(!previous || previous == tex);
        std::fill(overlayPixels.begin(), overlayPixels.end(), uint8_t(i));
        upload = {}; upload.tex = tex; upload.ptr = overlayPixels.data(); upload.no_import = true;
        assert(pl_tex_upload(gpu, &upload));
        timing = {};
        assert(completion->wait(tex, stopping, timing, Vrr::now()));
        download = {}; download.tex = tex; download.ptr = readback.data();
        assert(pl_tex_download(gpu, &download));
        assert(readback == overlayPixels);
    }
    // Asynchronous video submission must return while the imported GPU
    // dependency is still unsignaled. Polling may not manufacture completion.
    auto asynchronous = std::make_unique<Vrr::VulkanCompletion>();
    assert(asynchronous->initialize(vk, true));
    hold.tex = overlays[0]; hold.semaphore = {ready, 2};
    assert(pl_vulkan_hold_ex(gpu, &hold));
    pl_gpu_flush(gpu);
    const uint64_t two = 2; wait.pValues = &two;
    assert(vkWaitSemaphores(vk->device, &wait, 1000000000) == VK_SUCCESS);
    release.tex = overlays[0]; release.semaphore = {gate, 2};
    pl_vulkan_release_ex(gpu, &release);
    const float black[4] = {0, 0, 0, 1};
    pl_tex_clear(gpu, overlays[0], black);
    timing = {};
    assert(asynchronous->submit(overlays[0], stopping, timing, Vrr::now()));
    assert(timing.token && !timing.gpuReady);
    Vrr::Preparation observed;
    assert(!asynchronous->poll(observed));
    signal.value = 2;
    assert(vkSignalSemaphore(vk->device, &signal) == VK_SUCCESS);
    asynchronous->finish();
    assert(asynchronous->poll(observed) && observed.token == timing.token);
    assert(observed.gpuReady >= observed.commandsSubmitted && observed.gpuReady >= observed.gpuNotReady);
    assert(!asynchronous->poll(observed));
    for (auto& tex : overlays) pl_tex_destroy(gpu, &tex);
    asynchronous.reset();
    completion.reset();
    pl_vulkan_sem_destroy(gpu, &ready);
    pl_vulkan_sem_destroy(gpu, &gate);
    pl_vulkan_destroy(&vk);
    std::cout << "Vulkan HDR-format GPU completion, async submission, imported dependency and cancellation checks passed\n";
}
