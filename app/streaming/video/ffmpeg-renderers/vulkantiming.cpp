#include "vulkantiming.h"
#include <Limelight.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <time.h>

namespace {
std::atomic<PFN_vkGetInstanceProcAddr> loader{nullptr};
std::atomic<PFN_vkGetDeviceProcAddr> deviceLoader{nullptr};
std::mutex dispatchLock;
std::unordered_map<VkQueue, VkDevice> queueDevices;
std::unordered_map<VkDevice, VulkanTiming*> observers;
bool accepted(VkResult r) { return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR; }

#ifdef VK_EXT_present_timing
VulkanTiming* timingForDevice(VkDevice device)
{
    std::lock_guard<std::mutex> lock(dispatchLock);
    const auto observer = observers.find(device);
    return observer != observers.end() ? observer->second : nullptr;
}

#endif
}

PFN_vkGetInstanceProcAddr VulkanTiming::bridge(PFN_vkGetInstanceProcAddr source)
{
    auto expected = static_cast<PFN_vkGetInstanceProcAddr>(nullptr);
    if (!source || (!loader.compare_exchange_strong(expected, source) && expected != source)) return source;
    return instanceProc;
}

PFN_vkVoidFunction VulkanTiming::intercept(const char* name, PFN_vkVoidFunction next)
{
    if (!next) return nullptr;
#define HOOK(n, f) if (!std::strcmp(name, n)) return reinterpret_cast<PFN_vkVoidFunction>(f)
    HOOK("vkGetDeviceProcAddr", deviceProc);
    HOOK("vkGetDeviceQueue", getQueue);
    HOOK("vkGetDeviceQueue2", getQueue2);
    HOOK("vkQueuePresentKHR", present);
    HOOK("vkDestroyDevice", destroyDevice);
#ifdef VK_EXT_present_timing
    HOOK("vkCreateSwapchainKHR", createSwapchain);
    HOOK("vkDestroySwapchainKHR", destroySwapchain);
#endif
#undef HOOK
    return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VulkanTiming::instanceProc(VkInstance instance, const char* name)
{
    auto source = loader.load();
    if (instance) deviceLoader.store(reinterpret_cast<PFN_vkGetDeviceProcAddr>(source(instance, "vkGetDeviceProcAddr")));
    return intercept(name, source(instance, name));
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VulkanTiming::deviceProc(VkDevice device, const char* name)
{
    return intercept(name, deviceLoader.load()(device, name));
}

VKAPI_ATTR void VKAPI_CALL VulkanTiming::getQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
    auto fn = reinterpret_cast<PFN_vkGetDeviceQueue>(deviceLoader.load()(device, "vkGetDeviceQueue"));
    fn(device, family, index, queue);
    std::lock_guard<std::mutex> lock(dispatchLock);
    if (*queue) queueDevices[*queue] = device;
}

VKAPI_ATTR void VKAPI_CALL VulkanTiming::getQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
{
    auto fn = reinterpret_cast<PFN_vkGetDeviceQueue2>(deviceLoader.load()(device, "vkGetDeviceQueue2"));
    fn(device, info, queue);
    std::lock_guard<std::mutex> lock(dispatchLock);
    if (*queue) queueDevices[*queue] = device;
}

VKAPI_ATTR void VKAPI_CALL VulkanTiming::destroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
    auto fn = reinterpret_cast<PFN_vkDestroyDevice>(deviceLoader.load()(device, "vkDestroyDevice"));
    {
        std::lock_guard<std::mutex> lock(dispatchLock);
        observers.erase(device);
        for (auto it = queueDevices.begin(); it != queueDevices.end();) {
            if (it->second == device) it = queueDevices.erase(it);
            else ++it;
        }
    }
    fn(device, allocator);
}

bool VulkanTiming::initialize(VkDevice device)
{
    auto source = deviceLoader.load();
    if (!source) return false;
    m_GetTimings = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(source(device, "vkGetPastPresentationTimingGOOGLE"));
    if (!m_GetTimings) return false;
    return registerDevice(device);
}

bool VulkanTiming::registerDevice(VkDevice device)
{
    m_Device = device;
    std::lock_guard<std::mutex> lock(dispatchLock);
    observers[device] = this;
    return true;
}

void VulkanTiming::begin(uint64_t id, uint64_t targetUs)
{
    m_ArmedId = id;
    m_AcceptedId = 0;
#ifdef VK_EXT_present_timing
    m_RequestedTargetUs = targetUs;
    m_AcceptedTargetUs = 0;
    m_LastPresentResult = VK_ERROR_INITIALIZATION_FAILED;
#else
    (void) targetUs;
#endif
}

VulkanTiming::~VulkanTiming()
{
    // The renderer stops its worker before destroying this observer.
    std::lock_guard<std::mutex> lock(dispatchLock);
    auto it = observers.find(m_Device);
    if (it != observers.end() && it->second == this) observers.erase(it);
}

VKAPI_ATTR VkResult VKAPI_CALL VulkanTiming::present(VkQueue queue, const VkPresentInfoKHR* info)
{
    VkDevice device;
    VulkanTiming* observer = nullptr;
    {
        std::lock_guard<std::mutex> lock(dispatchLock);
        auto it = queueDevices.find(queue);
        if (it == queueDevices.end()) return VK_ERROR_INITIALIZATION_FAILED;
        device = it->second;
        auto found = observers.find(device);
        if (found != observers.end()) observer = found->second;
    }
    auto next = reinterpret_cast<PFN_vkQueuePresentKHR>(deviceLoader.load()(device, "vkQueuePresentKHR"));
    return observer ? observer->presentFrame(next, queue, info) : next(queue, info);
}

void VulkanTiming::reset()
{
#ifdef VK_EXT_present_timing
    // A pause or output epoch does not retire a Vulkan present. The next
    // acquire must still observe completion, or destroy the old swapchain.
    if (scheduled()) {
        m_WaylandPaintedId = 0;
        return;
    }
#endif
    m_Pending = {};
    m_Completed.clear();
    m_Swapchain = VK_NULL_HANDLE;
    m_Next = 0;
    m_SkipFirst = true;
}

VkResult VulkanTiming::presentFrame(PFN_vkQueuePresentKHR next, VkQueue queue, const VkPresentInfoKHR* info)
{
    const uint64_t id = m_ArmedId;
    m_ArmedId = 0;
#ifdef VK_EXT_present_timing
    if (scheduled()) return presentScheduled(next, queue, info, id);
#endif
    if (!id || info->swapchainCount != 1) return next(queue, info);
    // Preserve extensions already owned by libplacebo or another caller.
    for (auto* p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE) return next(queue, info);
    if (m_Swapchain != info->pSwapchains[0]) {
        reset();
        m_Swapchain = info->pSwapchains[0];
    }
    if (!++m_Token) ++m_Token;
    VkPresentTimeGOOGLE time{m_Token, 0};
    VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, info->pNext, 1, &time};
    VkPresentInfoKHR copy = *info;
    copy.pNext = &times;
    const uint64_t submitted = LiGetMicroseconds();
    const VkResult result = next(queue, &copy);
    if (accepted(result) && (!info->pResults || accepted(info->pResults[0]))) {
        ++m_Statistics.submissions;
        m_Pending[m_Next++ % m_Pending.size()] = {m_Token, id, submitted};
        m_AcceptedId = id;
    }
    else reset();
    return result;
}

void VulkanTiming::collect()
{
    if (!m_Swapchain) return;
    // Gamescope consumes returned records. Query the count with zero input,
    // then request exactly that count, never an oversized capacity.
    uint32_t count = 0;
    if (m_GetTimings(m_Device, m_Swapchain, &count, nullptr) != VK_SUCCESS || count > 256) {
        ++m_Statistics.queryErrors;
        return;
    }
    if (!count) { ++m_Statistics.emptyQueries; return; }
    std::array<VkPastPresentationTimingGOOGLE, 256> times{};
    const auto result = m_GetTimings(m_Device, m_Swapchain, &count, times.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        ++m_Statistics.queryErrors;
        return;
    }
    count = std::min<uint32_t>(count, times.size());
    m_Statistics.returned += count;
    const uint64_t before = LiGetMicroseconds();
    timespec clock{};
    if (clock_gettime(CLOCK_MONOTONIC, &clock) != 0) {
        m_Statistics.clockRejected += count;
        return;
    }
    const uint64_t observed = LiGetMicroseconds();
    if (observed < before || observed - before > 998) {
        m_Statistics.clockRejected += count;
        return;
    }
    const int64_t offset = int64_t(before + (observed - before) / 2) -
        (int64_t(clock.tv_sec) * 1000000 + clock.tv_nsec / 1000);
    const bool clockJump = m_HaveOffset && std::abs(offset - m_Offset) > 1000;
    m_Offset = offset;
    m_HaveOffset = true;
    if (clockJump) { m_Statistics.clockRejected += count; reset(); return; }
    std::sort(times.begin(), times.begin() + count, [](const auto& a, const auto& b) {
        return a.actualPresentTime < b.actualPresentTime;
    });
    for (uint32_t i = 0; i < count; ++i) {
        const auto& t = times[i];
        auto p = std::find_if(m_Pending.begin(), m_Pending.end(), [&](const auto& pending) {
            return pending.token && pending.token == t.presentID;
        });
        if (p == m_Pending.end()) { ++m_Statistics.unmatched; continue; }
        const auto pending = *p;
        *p = {};
        const int64_t converted = int64_t(t.actualPresentTime / 1000) + offset;
        if (!t.actualPresentTime || converted <= 0) { ++m_Statistics.invalid; continue; }
        if (uint64_t(converted) < pending.submitted) { ++m_Statistics.beforeSubmission; continue; }
        if (uint64_t(converted) > observed) { ++m_Statistics.future; continue; }
        if (observed - uint64_t(converted) > 100000) { ++m_Statistics.stale; continue; }
        if (m_SkipFirst) { m_SkipFirst = false; ++m_Statistics.warmupSkipped; continue; }
        if (m_Completed.size() == 64) m_Completed.pop_front();
        m_Completed.push_back({pending.id, uint64_t(converted), (observed - before + 1) / 2 + 1});
    }
}

void VulkanTiming::finish(VrrPresentFeedback& feedback)
{
    m_ArmedId = 0;
#ifdef VK_EXT_present_timing
    m_RequestedTargetUs = 0;
    if (scheduled()) {
        feedback.nativePresentResult = m_LastPresentResult;
        feedback.presented = m_AcceptedId != 0;
        feedback.submissionIdValid = feedback.presented;
        feedback.submissionId = m_AcceptedId;
        feedback.nativeTargetValid = feedback.presented && m_AcceptedTargetUs != 0;
        feedback.nativeTargetUs = m_AcceptedTargetUs;
        feedback.nativeOutstandingPresents = m_ScheduledPresents.size();
        m_AcceptedId = 0;
        return;
    }
#endif
    if (m_AcceptedId && feedback.presented && !feedback.cancelled) {
        feedback.submissionIdValid = true;
        feedback.submissionId = m_AcceptedId;
        collect();
    }
    m_AcceptedId = 0;
    if (m_Completed.empty()) return;
    const auto sample = m_Completed.front();
    m_Completed.pop_front();
    ++m_Statistics.emitted;
    feedback.latchSampleValid = true;
    feedback.latchTimeKind = Vrr13::PresentationTimeKind::DisplayEvent;
    feedback.latchSubmissionId = sample.id;
    feedback.latchTimeUs = sample.time;
    feedback.presentationUncertaintyUs = sample.uncertainty;
}

#ifdef VK_EXT_present_timing
bool VulkanTiming::supportsScheduling(PFN_vkGetInstanceProcAddr proc, VkInstance instance,
                                     VkPhysicalDevice device, VkSurfaceKHR surface)
{
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
        proc(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    if (!query) return false;
    VkPresentTimingSurfaceCapabilitiesEXT timing{};
    timing.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
    VkSurfaceCapabilities2KHR caps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &timing, {}};
    const VkPhysicalDeviceSurfaceInfo2KHR info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, nullptr, surface};
    return query(device, &info, &caps) == VK_SUCCESS &&
        timing.presentTimingSupported && timing.presentAtAbsoluteTimeSupported &&
        (timing.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
}

bool VulkanTiming::initializeScheduled(VkDevice device, bool waylandFramePacing)
{
    auto source = deviceLoader.load();
    if (!source) return false;
    m_GetTimeDomains = reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(
        source(device, "vkGetSwapchainTimeDomainPropertiesEXT"));
    m_SetTimingQueueSize = reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(
        source(device, "vkSetSwapchainPresentTimingQueueSizeEXT"));
    m_GetScheduledTimings = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(
        source(device, "vkGetPastPresentationTimingEXT"));
    m_GetCalibratedTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
        source(device, "vkGetCalibratedTimestampsKHR"));
    m_WaylandFramePacing = waylandFramePacing;
    return m_GetTimeDomains && m_SetTimingQueueSize && m_GetScheduledTimings &&
        registerDevice(device);
}

VKAPI_ATTR VkResult VKAPI_CALL VulkanTiming::createSwapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain)
{
    auto create = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        deviceLoader.load()(device, "vkCreateSwapchainKHR"));
    auto observer = timingForDevice(device);
    if (!observer || !observer->scheduled()) return create(device, info, allocator, swapchain);

    VkSwapchainCreateInfoKHR copy = *info;
    copy.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT |
                  VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
    const auto result = create(device, &copy, allocator, swapchain);
    if (result != VK_SUCCESS) return result;
    if (!observer->configureSwapchain(*swapchain)) {
        auto destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            deviceLoader.load()(device, "vkDestroySwapchainKHR"));
        destroy(device, *swapchain, allocator);
        *swapchain = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    observer->m_Swapchain = *swapchain;
    observer->m_ScheduledPresents.clear();
    observer->m_WaylandPaintedId = 0;
    observer->m_TimingConfigured = true;
    return result;
}

VKAPI_ATTR void VKAPI_CALL VulkanTiming::destroySwapchain(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator)
{
    if (auto observer = timingForDevice(device);
        observer && observer->scheduled() && observer->m_Swapchain == swapchain) {
        observer->m_Swapchain = VK_NULL_HANDLE;
        observer->m_ScheduledPresents.clear();
        observer->m_WaylandPaintedId = 0;
        observer->m_TimingConfigured = false;
    }
    auto destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        deviceLoader.load()(device, "vkDestroySwapchainKHR"));
    destroy(device, swapchain, allocator);
}

bool VulkanTiming::configureSwapchain(VkSwapchainKHR swapchain)
{
    VkSwapchainTimeDomainPropertiesEXT properties{};
    properties.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
    uint64_t counter = 0;
    if (m_GetTimeDomains(m_Device, swapchain, &properties, &counter) != VK_SUCCESS ||
        !properties.timeDomainCount || properties.timeDomainCount > 32) return false;
    std::array<VkTimeDomainKHR, 32> clocks{};
    std::array<uint64_t, 32> ids{};
    properties.pTimeDomains = clocks.data();
    properties.pTimeDomainIds = ids.data();
    if (m_GetTimeDomains(m_Device, swapchain, &properties, &counter) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < properties.timeDomainCount; ++i) {
        if (clocks[i] != VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR &&
            clocks[i] != VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR &&
            !(clocks[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT && m_GetCalibratedTimestamps))
            continue;
        m_TimeDomainId = ids[i];
        m_TimeDomain = clocks[i];
        return m_SetTimingQueueSize(m_Device, swapchain, m_WaylandFramePacing ? kWaylandTimingCapacity : 1) == VK_SUCCESS;
    }
    return false;
}

uint64_t VulkanTiming::targetInSwapchainClock(uint64_t targetUs)
{
    const uint64_t beforeUs = LiGetMicroseconds();
    const bool stageLocal = m_TimeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
    if (!m_WaylandFramePacing && targetUs <= beforeUs) return 0;

    uint64_t nativeNs;
    if (stageLocal) {
        // Gamescope exposes a stage-local domain, even though its current
        // implementation uses CLOCK_MONOTONIC for all of the stages.
        const VkSwapchainCalibratedTimestampInfoEXT swapchainClock{
            VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr,
            m_Swapchain, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT, m_TimeDomainId};
        const VkCalibratedTimestampInfoKHR clock{
            VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &swapchainClock, m_TimeDomain};
        uint64_t deviationNs = 0;
        if (m_GetCalibratedTimestamps(m_Device, 1, &clock, &nativeNs, &deviationNs) != VK_SUCCESS ||
                deviationNs > 500000) return 0;
    }
    else {
        const auto clockId = m_TimeDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR ?
            CLOCK_MONOTONIC : CLOCK_MONOTONIC_RAW;
        timespec clock{};
        if (clock_gettime(clockId, &clock) != 0) return 0;
        nativeNs = uint64_t(clock.tv_sec) * 1000000000 + clock.tv_nsec;
    }

    const uint64_t afterUs = LiGetMicroseconds();
    if (afterUs < beforeUs || afterUs - beforeUs > 500 ||
            (!m_WaylandFramePacing && targetUs <= afterUs)) return 0;
    const uint64_t midpointUs = beforeUs + (afterUs - beforeUs) / 2;
    // On Mesa Wayland, a zero target selects WSI's automatic phase prediction.
    // A late frame should be eligible for the next repaint immediately.
    if (targetUs <= midpointUs) return nativeNs;
    return nativeNs + (targetUs - midpointUs) * 1000;
}

bool VulkanTiming::canSubmitScheduled() const
{
    // The frame callback arrives after KWin has consumed the commit for a
    // repaint. Let the next commit reach FIFO while that repaint is scanning
    // out. KWin can have two painted frames in flight, but only one commit
    // can be waiting to be picked up for a repaint.
    return m_ScheduledPresents.empty() ||
        (m_WaylandFramePacing && m_ScheduledPresents.size() < kWaylandTimingCapacity &&
         m_WaylandPaintedId != 0 && m_ScheduledPresents.back().id == m_WaylandPaintedId);
}

bool VulkanTiming::slotAvailable(uint64_t paintedId)
{
    if (paintedId) m_WaylandPaintedId = paintedId;
    if (m_ScheduledPresents.empty()) return true;

    // Read all possible completions in one call. Mesa can consume completed
    // entries even when the supplied result array is too small to return them.
    std::array<VkPresentStageTimeEXT, kWaylandTimingCapacity> stages{};
    std::array<VkPastPresentationTimingEXT, kWaylandTimingCapacity> timings{};
    for (size_t i = 0; i < m_ScheduledPresents.size(); ++i) {
        timings[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
        timings[i].presentStageCount = 1;
        timings[i].pPresentStages = &stages[i];
    }
    VkPastPresentationTimingPropertiesEXT properties{};
    properties.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
    properties.presentationTimingCount = static_cast<uint32_t>(m_ScheduledPresents.size());
    properties.pPresentationTimings = timings.data();
    const VkPastPresentationTimingInfoEXT info{
        VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT, nullptr, 0, m_Swapchain};
    const auto result = m_GetScheduledTimings(m_Device, &info, &properties);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        ++m_Statistics.queryErrors;
        return canSubmitScheduled();
    }
    for (size_t i = 0; i < std::min<size_t>(properties.presentationTimingCount, timings.size()); ++i) {
        const auto& timing = timings[i];
        // A completed zero timestamp can be a discard. A Gamescope timestamp
        // can be a prediction. Only the matching terminal ID retires a record.
        if (!timing.reportComplete) continue;
        const auto completed = std::find_if(m_ScheduledPresents.begin(), m_ScheduledPresents.end(),
            [&](const ScheduledPresent& pending) { return pending.token == timing.presentId; });
        if (completed != m_ScheduledPresents.end()) {
            m_ScheduledPresents.erase(completed);
            ++m_Statistics.returned;
        }
    }
    return canSubmitScheduled();
}

VkResult VulkanTiming::presentScheduled(PFN_vkQueuePresentKHR next, VkQueue queue,
                                        const VkPresentInfoKHR* info, uint64_t id)
{
    const auto targetUs = std::exchange(m_RequestedTargetUs, 0);
    if (info->swapchainCount != 1 || info->pSwapchains[0] != m_Swapchain ||
        !m_TimingConfigured || !canSubmitScheduled()) {
        if (info->pResults && info->swapchainCount == 1)
            info->pResults[0] = VK_ERROR_OUT_OF_DATE_KHR;
        return m_LastPresentResult = VK_ERROR_OUT_OF_DATE_KHR;
    }
    const uint64_t token = ++m_ScheduledToken;
    const VkPresentId2KHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, info->pNext, 1, &token};
    const uint64_t targetNs = targetInSwapchainClock(targetUs);
    const auto targetStage = m_TimeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT ?
        VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT : VkPresentStageFlagBitsEXT{};
    const VkPresentTimingInfoEXT timing{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT, nullptr,
        !m_WaylandFramePacing || targetStage ? VkPresentTimingInfoFlagsEXT{} :
            VkPresentTimingInfoFlagsEXT{VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT},
        targetNs, m_TimeDomainId, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
        static_cast<VkPresentStageFlagsEXT>(targetStage)};
    const VkPresentTimingsInfoEXT timings{
        VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, &presentId, 1, &timing};
    VkPresentInfoKHR copy = *info;
    copy.pNext = &timings;
    const uint64_t submittedUs = LiGetMicroseconds();
    const VkResult result = next(queue, &copy);
    m_LastPresentResult = accepted(result) && info->pResults ? info->pResults[0] : result;
    if (accepted(m_LastPresentResult)) {
        ++m_Statistics.submissions;
        m_ScheduledPresents.push_back({token, id, submittedUs});
        m_AcceptedId = id;
        m_AcceptedTargetUs = targetNs ? targetUs : 0;
    }
    return result;
}
#endif
