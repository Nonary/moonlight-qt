#pragma once

#include "timing.h"
#include <libplacebo/vulkan.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Vrr {
// Wait on one Vulkan timeline value for this image, not libplacebo's global
// command-retirement loop. That loop can spend milliseconds retiring unrelated
// work even when called with a 100 us timeout, obscuring GPU readiness.
class VulkanCompletion {
public:
    bool initialize(pl_vulkan vk, bool asynchronous = false) {
        m_Vulkan = vk;
        const auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk->get_proc_addr(vk->instance, "vkGetDeviceProcAddr"));
        if (!getDevice) return false;
        m_Create = reinterpret_cast<PFN_vkCreateSemaphore>(getDevice(vk->device, "vkCreateSemaphore"));
        m_Destroy = reinterpret_cast<PFN_vkDestroySemaphore>(getDevice(vk->device, "vkDestroySemaphore"));
        m_Wait = reinterpret_cast<PFN_vkWaitSemaphores>(getDevice(vk->device, "vkWaitSemaphores"));
        if (!m_Wait) m_Wait = reinterpret_cast<PFN_vkWaitSemaphores>(getDevice(vk->device, "vkWaitSemaphoresKHR"));
        if (!m_Create || !m_Destroy || !m_Wait) return false;
        VkSemaphoreTypeCreateInfo timeline{};
        timeline.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore.pNext = &timeline;
        if (m_Create(vk->device, &semaphore, nullptr, &m_Semaphore) != VK_SUCCESS) return false;
        if (asynchronous) {
            try { m_Observer = std::thread(&VulkanCompletion::observe, this); }
            catch (...) { return false; }
        }
        return true;
    }
    ~VulkanCompletion() {
        // Destroy after the dependent swapchain images, before the VkDevice.
        if (m_Semaphore) {
            finish();
            { std::lock_guard<std::mutex> lock(m_Lock); m_Stop = true; }
            m_Changed.notify_all();
            if (m_Observer.joinable()) m_Observer.join();
            m_Destroy(m_Vulkan->device, m_Semaphore, nullptr);
        }
    }
    bool submit(pl_tex image, const std::atomic<bool>& stopping, Preparation& timing, Ns earliest) {
        if (!m_Observer.joinable() || !signal(image, stopping, timing, earliest)) return false;
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Pending.push_back(timing);
        m_Changed.notify_all();
        return true;
    }
    bool poll(Preparation& timing) {
        std::lock_guard<std::mutex> lock(m_Lock);
        if (m_Completed.empty()) return false;
        timing = m_Completed.front(); m_Completed.pop_front(); return true;
    }
    void finish() {
        pl_gpu_finish(m_Vulkan->gpu);
        if (m_Observer.joinable()) {
            std::unique_lock<std::mutex> lock(m_Lock);
            m_Changed.wait(lock, [&] { return m_Pending.empty() && !m_Active; });
        }
    }
    // Overlay uploads retain the synchronous path on their background worker.
    bool wait(pl_tex image, const std::atomic<bool>& stopping, Preparation& timing, Ns earliest) {
        if (!signal(image, stopping, timing, earliest)) return false;
        const bool ready = waitValue(timing, stopping);
        timing.token = 0;
        return ready;
    }
private:
    bool signal(pl_tex image, const std::atomic<bool>& stopping, Preparation& timing, Ns earliest) {
        if (!m_Semaphore || stopping.load()) return false;
        const uint64_t value = ++m_Value;
        timing.token = value;
        pl_vulkan_hold_params hold{};
        hold.tex = image;
        // Preserve the image's layout: this helper also works for offscreen
        // images. The swapchain performs its PRESENT_SRC transition on submit.
        VkImageLayout layout;
        hold.out_layout = &layout;
        hold.qf = VK_QUEUE_FAMILY_IGNORED;
        hold.semaphore = {m_Semaphore, value};
        if (!pl_vulkan_hold_ex(m_Vulkan->gpu, &hold)) return false;
        pl_vulkan_release_params release{};
        release.tex = image; release.layout = layout;
        release.qf = VK_QUEUE_FAMILY_IGNORED;
        release.semaphore = hold.semaphore;
        // Release immediately with the dependency intact, including on timeout
        // or cancellation, so cleanup can safely submit/destroy the image.
        pl_vulkan_release_ex(m_Vulkan->gpu, &release);
        pl_gpu_flush(m_Vulkan->gpu);
        timing.commandsSubmitted = now();
        timing.gpuNotReady = earliest;
        return true;
    }
    bool waitValue(Preparation& timing, const std::atomic<bool>& stopping) {
        const uint64_t value = timing.token;
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1; wait.pSemaphores = &m_Semaphore; wait.pValues = &value;
        const auto deadline = timing.commandsSubmitted + 100 * Millisecond;
        while (!stopping.load() && now() < deadline) {
            const auto checkedAt = now();
            const auto result = m_Wait(m_Vulkan->device, &wait, 100000);
            if (result == VK_SUCCESS) { timing.gpuReady = now(); return true; }
            if (result != VK_TIMEOUT) return false;
            timing.gpuNotReady = checkedAt;
        }
        return false;
    }
    void observe() {
        for (;;) {
            Preparation timing;
            {
                std::unique_lock<std::mutex> lock(m_Lock);
                m_Changed.wait(lock, [&] { return m_Stop || !m_Pending.empty(); });
                if (m_Stop && m_Pending.empty()) return;
                timing = m_Pending.front(); m_Pending.pop_front(); m_Active = true;
            }
            waitValue(timing, m_Stop);
            {
                std::lock_guard<std::mutex> lock(m_Lock);
                m_Completed.push_back(timing); m_Active = false;
            }
            m_Changed.notify_all();
        }
    }
    std::mutex m_Lock;
    std::condition_variable m_Changed;
    std::deque<Preparation> m_Pending, m_Completed;
    std::thread m_Observer;
    std::atomic<bool> m_Stop{false};
    bool m_Active = false;
    pl_vulkan m_Vulkan = nullptr;
    VkSemaphore m_Semaphore = VK_NULL_HANDLE;
    uint64_t m_Value = 0;
    PFN_vkCreateSemaphore m_Create = nullptr;
    PFN_vkDestroySemaphore m_Destroy = nullptr;
    PFN_vkWaitSemaphores m_Wait = nullptr;
};
}
