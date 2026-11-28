#pragma once

#include "ivrrframepresenter.h"
#include <vulkan/vulkan.h>
#include <array>
#include <deque>

// Add timing to libplacebo's existing swapchain and queue operations.
class VulkanTiming {
public:
    struct Statistics {
        uint64_t submissions = 0, returned = 0, emitted = 0;
        uint64_t unmatched = 0, beforeSubmission = 0, future = 0, stale = 0;
        uint64_t invalid = 0, clockRejected = 0, warmupSkipped = 0;
        uint64_t emptyQueries = 0, queryErrors = 0;
    };
    ~VulkanTiming();
    static PFN_vkGetInstanceProcAddr bridge(PFN_vkGetInstanceProcAddr loader);
    bool initialize(VkDevice device);
    void begin(uint64_t id, uint64_t targetUs = 0);
    void finish(VrrPresentFeedback& feedback);
    void reset();
    const Statistics& statistics() const { return m_Statistics; }
#ifdef VK_EXT_present_timing
    static bool supportsScheduling(PFN_vkGetInstanceProcAddr, VkInstance,
                                   VkPhysicalDevice, VkSurfaceKHR);
    bool initializeScheduled(VkDevice device, bool waylandFramePacing = false,
                             bool gamescopePrediction = false);
    bool scheduled() const { return m_GetScheduledTimings != nullptr; }
    bool swapchainTimed() const { return m_Swapchain != VK_NULL_HANDLE && m_TimingConfigured; }
    bool slotAvailable(uint64_t paintedId = 0);
    void collectScheduledEstimate(VrrPresentFeedback& feedback);
    uint64_t pendingSinceUs() const { return m_ScheduledPresents.empty() ? 0 : m_ScheduledPresents.front().submitted; }
#endif
private:
    // Cumulative across swapchain resets; observation only, never pacing input.
    Statistics m_Statistics;
    struct Pending { uint32_t token = 0; uint64_t id = 0, submitted = 0; };
    struct Completed { uint64_t id = 0, time = 0, uncertainty = 0; };
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance, const char*);
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice, const char*);
    static VKAPI_ATTR void VKAPI_CALL getQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
    static VKAPI_ATTR void VKAPI_CALL getQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
    static VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice, const VkAllocationCallbacks*);
    static VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue, const VkPresentInfoKHR*);
#ifdef VK_EXT_present_timing
    static VKAPI_ATTR VkResult VKAPI_CALL createSwapchain(
        VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
    static VKAPI_ATTR void VKAPI_CALL destroySwapchain(
        VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
    bool configureSwapchain(VkSwapchainKHR);
    bool canSubmitScheduled() const;
    static constexpr uint32_t kWaylandTimingCapacity = 3;
    VkResult presentScheduled(PFN_vkQueuePresentKHR, VkQueue, const VkPresentInfoKHR*,
                              uint64_t id);
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT m_SetTimingQueueSize = nullptr;
    PFN_vkGetSwapchainTimeDomainPropertiesEXT m_GetTimeDomains = nullptr;
    PFN_vkGetPastPresentationTimingEXT m_GetScheduledTimings = nullptr;
    PFN_vkGetCalibratedTimestampsKHR m_GetCalibratedTimestamps = nullptr;
    uint64_t m_TimeDomainId = 0;
    VkTimeDomainKHR m_TimeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    bool sampleStageClock(uint64_t& nativeNs, uint64_t& deviationNs) const;
    uint64_t targetInSwapchainClock(uint64_t);
    struct ScheduledEstimate { uint64_t id, submittedUs, pixelOutNs; };
    ScheduledEstimate m_ScheduledEstimate{};
    uint64_t m_TimeDomainsCounter = 0;
    uint64_t m_EstimateAfterToken = 0;
    bool m_GamescopePrediction = false;
    struct ScheduledPresent { uint64_t token, id, submitted; };
    std::deque<ScheduledPresent> m_ScheduledPresents;
    bool m_TimingConfigured = false, m_WaylandFramePacing = false;
    uint64_t m_ScheduledToken = 0, m_WaylandPaintedId = 0;
    uint64_t m_RequestedTargetUs = 0, m_AcceptedTargetUs = 0;
    VkResult m_LastPresentResult = VK_SUCCESS;
#endif
    static PFN_vkVoidFunction intercept(const char*, PFN_vkVoidFunction);
    VkResult presentFrame(PFN_vkQueuePresentKHR, VkQueue, const VkPresentInfoKHR*);
    void collect();
    bool registerDevice(VkDevice);
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
