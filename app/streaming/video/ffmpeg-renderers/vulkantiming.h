#pragma once

#include "ivrrframepresenter.h"
#include <vulkan/vulkan.h>
#include <array>
#include <deque>

// Observe the Gamescope WSI swapchain through VK_GOOGLE_display_timing.
// The proc-address bridge lets libplacebo keep ownership of the swapchain.
// It only appends a timing ID; desiredPresentTime remains zero.
class VulkanTiming {
public:
    ~VulkanTiming();
    static PFN_vkGetInstanceProcAddr bridge(PFN_vkGetInstanceProcAddr loader);
    bool initialize(VkDevice device);
    void begin(uint64_t id) { m_ArmedId = id; m_AcceptedId = 0; }
    void finish(VrrPresentFeedback& feedback);
    void reset();
private:
    struct Pending { uint32_t token = 0; uint64_t id = 0, submitted = 0; };
    struct Completed { uint64_t id = 0, time = 0, uncertainty = 0; };
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance, const char*);
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice, const char*);
    static VKAPI_ATTR void VKAPI_CALL getQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
    static VKAPI_ATTR void VKAPI_CALL getQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
    static VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice, const VkAllocationCallbacks*);
    static VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue, const VkPresentInfoKHR*);
    static PFN_vkVoidFunction intercept(const char*, PFN_vkVoidFunction);
    VkResult presentFrame(PFN_vkQueuePresentKHR, VkQueue, const VkPresentInfoKHR*);
    void collect();
    VkDevice m_Device = VK_NULL_HANDLE;
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    PFN_vkGetPastPresentationTimingGOOGLE m_GetTimings = nullptr;
    std::array<Pending, 256> m_Pending{};
    std::deque<Completed> m_Completed;
    size_t m_Next = 0;
    uint32_t m_Token = 0;
    uint64_t m_ArmedId = 0, m_AcceptedId = 0;
    int64_t m_Offset = 0;
    bool m_HaveOffset = false, m_SkipFirst = true;
};
