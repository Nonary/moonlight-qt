#include "vulkantiming.h"
#include <Limelight.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <time.h>

namespace {
std::atomic<PFN_vkGetInstanceProcAddr> loader{nullptr};
std::atomic<PFN_vkGetDeviceProcAddr> deviceLoader{nullptr};
std::mutex dispatchLock;
std::unordered_map<VkQueue, VkDevice> queueDevices;
std::unordered_map<VkDevice, VulkanTiming*> observers;
bool accepted(VkResult r) { return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR; }
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
    m_Device = device;
    std::lock_guard<std::mutex> lock(dispatchLock);
    observers[device] = this;
    return true;
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
