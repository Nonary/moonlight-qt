#include "assertions.h"
#include "../../app/streaming/video/ffmpeg-renderers/vulkantiming.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>
#include <time.h>

namespace {
uint64_t monotonicNs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
const auto device = reinterpret_cast<VkDevice>(uintptr_t(1));
const auto queue = reinterpret_cast<VkQueue>(uintptr_t(2));
const auto swapchain = (VkSwapchainKHR)uintptr_t(3);
const auto otherDevice = reinterpret_cast<VkDevice>(uintptr_t(4));
const auto otherQueue = reinterpret_cast<VkQueue>(uintptr_t(5));
std::vector<VkPastPresentationTimingGOOGLE> history;
bool hold = false;
int64_t timingOffsetNs = 0;
VkResult presentResult = VK_SUCCESS;
unsigned tagged = 0, forwarded = 0, destroyed = 0;
const void* expectedChain = nullptr;
const VkSemaphore* expectedSemaphores = nullptr;

#ifdef VK_EXT_present_timing
std::vector<uint64_t> pendingTokens;
uint64_t nativeTarget = 0;
bool nativeComplete = false;
bool nativeSurfaceSupported = true;
bool stageLocal = false;
unsigned nativeSubmissions = 0;
uint32_t nativeCapacity = 1;

VKAPI_ATTR VkResult VKAPI_CALL createSwapchain(VkDevice, const VkSwapchainCreateInfoKHR* info,
        const VkAllocationCallbacks*, VkSwapchainKHR* out)
{
    assert(info->presentMode == VK_PRESENT_MODE_FIFO_KHR);
    assert(info->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT);
    assert(info->flags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR);
    *out = swapchain;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroySwapchain(VkDevice, VkSwapchainKHR handle,
        const VkAllocationCallbacks*)
{
    assert(handle == swapchain);
    pendingTokens.clear();
}

VKAPI_ATTR VkResult VKAPI_CALL surfaceCapabilities(VkPhysicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR* caps)
{
    auto* timing = static_cast<VkPresentTimingSurfaceCapabilitiesEXT*>(caps->pNext);
    timing->presentTimingSupported = VK_TRUE;
    timing->presentAtAbsoluteTimeSupported = nativeSurfaceSupported;
    timing->presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL timeDomains(VkDevice, VkSwapchainKHR handle,
        VkSwapchainTimeDomainPropertiesEXT* props, uint64_t* counter)
{
    assert(handle == swapchain);
    *counter = 1;
    if (props->pTimeDomains) {
        assert(props->timeDomainCount == 1);
        props->pTimeDomains[0] = stageLocal ? VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT :
            VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
        props->pTimeDomainIds[0] = 42;
    }
    props->timeDomainCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL calibrate(VkDevice, uint32_t count,
        const VkCalibratedTimestampInfoKHR* info, uint64_t* output, uint64_t* deviation)
{
    assert(stageLocal && count == 1 && info->timeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
    const auto* clock = static_cast<const VkSwapchainCalibratedTimestampInfoEXT*>(info->pNext);
    assert(clock->swapchain == swapchain && clock->timeDomainId == 42);
    assert(clock->presentStage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
    *deviation = 0;
    *output = monotonicNs();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL sizeTimingQueue(VkDevice, VkSwapchainKHR handle, uint32_t size)
{
    assert(handle == swapchain && size == nativeCapacity);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL scheduledPast(VkDevice, const VkPastPresentationTimingInfoEXT* info,
        VkPastPresentationTimingPropertiesEXT* props)
{
    assert(info->swapchain == swapchain && props->presentationTimingCount == pendingTokens.size());
    props->presentationTimingCount = nativeComplete ? pendingTokens.size() : 0;
    if (nativeComplete) {
        for (size_t i = 0; i < pendingTokens.size(); ++i) {
            auto& timing = props->pPresentationTimings[i];
            assert(timing.presentStageCount == 1 && timing.pPresentStages);
            timing.presentId = pendingTokens[i];
            timing.reportComplete = VK_TRUE;
            timing.pPresentStages[0] = {VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT, 0};
        }
        pendingTokens.clear();
        nativeComplete = false;
    }
    return VK_SUCCESS;
}
#endif

VKAPI_ATTR void VKAPI_CALL getQueue(VkDevice d, uint32_t, uint32_t, VkQueue* q) {
    *q = d == device ? queue : otherQueue;
}
VKAPI_ATTR void VKAPI_CALL getQueue2(VkDevice d, const VkDeviceQueueInfo2*, VkQueue* q) {
    getQueue(d, 0, 0, q);
}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice, const VkAllocationCallbacks*) { ++destroyed; }
VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue, const VkPresentInfoKHR* info) {
    // Leave a real interval after the recorded submission boundary. An
    // instantaneous mock can round a correlated nanosecond timestamp below
    // that boundary by one microsecond and be correctly rejected as stale.
    const timespec delay{0, 100000};
    nanosleep(&delay, nullptr);
    const auto* p = static_cast<const VkBaseInStructure*>(info->pNext);
#ifdef VK_EXT_present_timing
    if (p && p->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) {
        const auto* times = reinterpret_cast<const VkPresentTimingsInfoEXT*>(p);
        const auto* ids = static_cast<const VkPresentId2KHR*>(times->pNext);
        assert(pendingTokens.size() < nativeCapacity && times->swapchainCount == 1 && ids->swapchainCount == 1);
        assert(ids->pNext == expectedChain && info->pWaitSemaphores == expectedSemaphores);
        assert(times->pTimingInfos[0].timeDomainId == 42);
        assert(times->pTimingInfos[0].presentStageQueries == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
        assert(times->pTimingInfos[0].targetTimeDomainPresentStage ==
            (stageLocal ? VkPresentStageFlagsEXT(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) : 0u));
        nativeTarget = times->pTimingInfos[0].targetTime;
        ++nativeSubmissions;
        if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR)
            pendingTokens.push_back(ids->pPresentIds[0]);
    }
    else
#endif
    if (p && p->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE) {
        auto* times = reinterpret_cast<const VkPresentTimesInfoGOOGLE*>(p);
        assert(times->swapchainCount == 1 && times->pTimes[0].desiredPresentTime == 0);
        assert(times->pNext == expectedChain && info->pWaitSemaphores == expectedSemaphores);
        ++tagged;
        if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR)
            history.push_back({times->pTimes[0].presentID, 0,
                               uint64_t(int64_t(monotonicNs()) + timingOffsetNs), 0, 0});
    }
    else ++forwarded;
    if (info->pResults) info->pResults[0] = presentResult;
    return presentResult;
}
VKAPI_ATTR VkResult VKAPI_CALL past(VkDevice, VkSwapchainKHR, uint32_t* count, VkPastPresentationTimingGOOGLE* out) {
    if (!out) {
        assert(*count == 0); // Gamescope consumes the INPUT count, even on a size query.
        *count = hold ? 0 : uint32_t(history.size());
        return VK_SUCCESS;
    }
    assert(*count <= history.size());
    std::copy_n(history.begin(), *count, out);
    history.erase(history.begin(), history.begin() + *count);
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice, const char* name) {
#define PROC(n, f) if (!std::strcmp(name, n)) return reinterpret_cast<PFN_vkVoidFunction>(f)
    PROC("vkQueuePresentKHR", present);
    PROC("vkGetDeviceQueue", getQueue);
    PROC("vkGetDeviceQueue2", getQueue2);
    PROC("vkDestroyDevice", destroyDevice);
    PROC("vkGetPastPresentationTimingGOOGLE", past);
#ifdef VK_EXT_present_timing
    PROC("vkCreateSwapchainKHR", createSwapchain);
    PROC("vkDestroySwapchainKHR", destroySwapchain);
    PROC("vkGetPhysicalDeviceSurfaceCapabilities2KHR", surfaceCapabilities);
    PROC("vkGetSwapchainTimeDomainPropertiesEXT", timeDomains);
    PROC("vkSetSwapchainPresentTimingQueueSizeEXT", sizeTimingQueue);
    PROC("vkGetPastPresentationTimingEXT", scheduledPast);
    PROC("vkGetCalibratedTimestampsKHR", calibrate);
#endif
#undef PROC
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance, const char* name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(deviceProc);
    return deviceProc(device, name);
}
}

extern "C" uint64_t LiGetMicroseconds() { return monotonicNs() / 1000 - 100000; }

int main()
{
    auto proxy = VulkanTiming::bridge(instanceProc);
    auto instance = reinterpret_cast<VkInstance>(uintptr_t(6));
    auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(proxy(instance, "vkGetDeviceProcAddr"));
    auto get = reinterpret_cast<PFN_vkGetDeviceQueue>(getDevice(device, "vkGetDeviceQueue"));
    auto submit = reinterpret_cast<PFN_vkQueuePresentKHR>(getDevice(device, "vkQueuePresentKHR"));
    VkQueue q;
    get(device, 0, 0, &q);
    assert(q == queue);
    VulkanTiming timing;
    assert(timing.initialize(device));
    const uint32_t index = 0;
    VkSemaphore semaphore = (VkSemaphore)uintptr_t(7);
    VkResult result;
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &semaphore,
                         1, &swapchain, &index, &result};
    expectedSemaphores = &semaphore;
    auto frame = [&](uint64_t id) {
        timing.begin(id);
        const auto r = submit(queue, &info);
        assert(r == presentResult && result == presentResult);
        VrrPresentFeedback feedback;
        feedback.presented = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
        timing.finish(feedback);
        assert(info.pNext == expectedChain && info.pSwapchains == &swapchain);
        return feedback;
    };
    auto first = frame(10);
    assert(first.submissionIdValid && first.submissionId == 10 && !first.latchSampleValid);
    auto second = frame(20);
    assert(second.latchSampleValid && second.latchSubmissionId == 20);
    assert(second.latchTimeKind == Vrr13::PresentationTimeKind::DisplayEvent);
    assert(second.latchTimeUs <= LiGetMicroseconds() && LiGetMicroseconds() - second.latchTimeUs < 10000);
    assert(second.presentationUncertaintyUs <= 500);
    // Existing extensions and semaphore waits must survive the timing tag.
    VkBaseInStructure original{VK_STRUCTURE_TYPE_PRESENT_ID_KHR, nullptr};
    info.pNext = expectedChain = &original;
    assert(frame(30).latchSubmissionId == 30);
    info.pNext = expectedChain = nullptr;
    hold = true;
    assert(!frame(40).latchSampleValid);
    hold = false;
    assert(frame(50).latchSubmissionId == 40);
    assert(frame(60).latchSubmissionId == 50);
    // Failed presents cannot claim feedback or leave pending IDs to reuse.
    presentResult = VK_ERROR_OUT_OF_DATE_KHR;
    auto failed = frame(70);
    assert(!failed.submissionIdValid && !failed.latchSampleValid);
    presentResult = VK_SUBOPTIMAL_KHR;
    assert(frame(80).submissionIdValid);
    presentResult = VK_SUCCESS;
    assert(frame(90).latchSubmissionId == 90);
    // Cancellation/legacy calls and unrelated devices forward unchanged.
    assert(submit(queue, &info) == VK_SUCCESS);
    auto get2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(proxy(instance, "vkGetDeviceQueue2"));
    VkDeviceQueueInfo2 queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, nullptr, 0, 0, 0};
    get2(otherDevice, &queueInfo, &q);
    assert(q == otherQueue && submit(q, &info) == VK_SUCCESS);
    assert(forwarded == 2 && tagged == 9);
    // Invalid compositor timestamps stay rejected, with a reason available
    // to distinguish poor timing coverage from frames not being displayed.
    const auto emitted = timing.statistics().emitted;
    timingOffsetNs = -1000000000;
    assert(!frame(100).latchSampleValid);
    assert(timing.statistics().beforeSubmission == 1);
    timingOffsetNs = 1000000000;
    assert(!frame(110).latchSampleValid);
    assert(timing.statistics().future == 1);
    timingOffsetNs = 0;
    assert(frame(120).latchSampleValid);
    assert(timing.statistics().emitted == emitted + 1);
    const auto returned = timing.statistics().returned;
    timing.reset();
    assert(timing.statistics().returned == returned);

#ifdef VK_EXT_present_timing
    assert(VulkanTiming::supportsScheduling(instanceProc, instance, VK_NULL_HANDLE, VK_NULL_HANDLE));
    nativeSurfaceSupported = false;
    assert(!VulkanTiming::supportsScheduling(instanceProc, instance, VK_NULL_HANDLE, VK_NULL_HANDLE));
    VulkanTiming scheduled;
    assert(scheduled.initializeScheduled(device));
    auto create = reinterpret_cast<PFN_vkCreateSwapchainKHR>(getDevice(device, "vkCreateSwapchainKHR"));
    auto destroyChain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(getDevice(device, "vkDestroySwapchainKHR"));
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR handle;
    assert(create(device, &createInfo, nullptr, &handle) == VK_SUCCESS);
    assert(handle == swapchain && scheduled.swapchainTimed() && scheduled.slotAvailable());
    info.pNext = expectedChain = &original;
    const uint64_t target = LiGetMicroseconds() + 12000;
    scheduled.begin(401, target);
    assert(submit(queue, &info) == VK_SUCCESS);
    assert(nativeTarget >= (target + 100000) * 1000 - 500000);
    assert(nativeTarget <= (target + 100000) * 1000 + 500000);
    assert(info.pNext == &original && nativeSubmissions == 1);
    VrrPresentFeedback native;
    scheduled.finish(native);
    assert(native.presented && native.submissionId == 401 && native.nativeTargetUs == target);
    assert(native.nativeOutstandingPresents == 1);
    assert(native.nativeTargetValid && !native.latchSampleValid);
    assert(!scheduled.slotAvailable());
    scheduled.reset();
    assert(!scheduled.slotAvailable());

    scheduled.begin(402, target);
    assert(submit(queue, &info) == VK_ERROR_OUT_OF_DATE_KHR);
    scheduled.finish(native);
    assert(!native.presented && !native.nativeTargetValid && nativeSubmissions == 1);
    nativeComplete = true;
    assert(scheduled.slotAvailable());

    // A rejected present must leave the slot free.
    presentResult = VK_ERROR_OUT_OF_DATE_KHR;
    scheduled.begin(403, target);
    assert(submit(queue, &info) == VK_ERROR_OUT_OF_DATE_KHR);
    scheduled.finish(native);
    assert(!native.presented && scheduled.slotAvailable());
    assert(native.nativeOutstandingPresents == 0);
    // An already elapsed target is an immediate, tracked present.
    presentResult = VK_SUCCESS;
    scheduled.begin(404, LiGetMicroseconds() - 1000);
    assert(submit(queue, &info) == VK_SUCCESS && nativeTarget == 0);
    scheduled.finish(native);
    assert(native.presented && !native.nativeTargetValid && !scheduled.slotAvailable());
    destroyChain(device, handle, nullptr);
    assert(!scheduled.swapchainTimed() && scheduled.slotAvailable());
    stageLocal = true;
    assert(create(device, &createInfo, nullptr, &handle) == VK_SUCCESS);
    assert(scheduled.swapchainTimed() && scheduled.slotAvailable());
    const uint64_t gamescopeTarget = LiGetMicroseconds() + 12000;
    scheduled.begin(405, gamescopeTarget);
    assert(submit(queue, &info) == VK_SUCCESS);
    assert(nativeTarget >= (gamescopeTarget + 100000) * 1000 - 500000);
    assert(nativeTarget <= (gamescopeTarget + 100000) * 1000 + 500000);
    scheduled.finish(native);
    assert(native.presented && native.nativeTargetUs == gamescopeTarget);
    destroyChain(device, handle, nullptr);

    stageLocal = false;
    nativeCapacity = 3;
    VulkanTiming wayland;
    assert(wayland.initializeScheduled(device, true));
    assert(create(device, &createInfo, nullptr, &handle) == VK_SUCCESS);
    wayland.begin(406);
    assert(submit(queue, &info) == VK_SUCCESS && nativeTarget != 0);
    wayland.finish(native);
    assert(native.nativeOutstandingPresents == 1 && !native.nativeTargetValid);
    assert(!wayland.slotAvailable(405));
    assert(wayland.slotAvailable(406));
    wayland.reset();
    assert(!wayland.slotAvailable());
    assert(wayland.slotAvailable(406));
    wayland.begin(407);
    assert(submit(queue, &info) == VK_SUCCESS);
    wayland.finish(native);
    assert(native.nativeOutstandingPresents == 2 && !wayland.slotAvailable());
    assert(wayland.slotAvailable(407));
    wayland.begin(408);
    assert(submit(queue, &info) == VK_SUCCESS);
    wayland.finish(native);
    assert(native.nativeOutstandingPresents == 3 && !wayland.slotAvailable(408));
    nativeComplete = true;
    assert(wayland.slotAvailable() && wayland.statistics().returned == 3);
    destroyChain(device, handle, nullptr);
#endif
    auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(getDevice(device, "vkDestroyDevice"));
    destroy(otherDevice, nullptr);
    destroy(device, nullptr);
    assert(destroyed == 2);
}
