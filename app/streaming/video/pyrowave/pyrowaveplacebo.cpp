#include "pyrowaveplacebo.h"

#include <SDL.h>

#include <algorithm>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <libplacebo/utils/libav.h>

namespace {

// Frames held by the decode queue, the VRR queue, the frame being rendered and
// the frame on screen. Past this the decoder drops frames instead of growing.
constexpr size_t k_MaxSurfaces = 8;

template <typename T>
const T* findFeatures(const VkPhysicalDeviceFeatures2* features, VkStructureType type)
{
    for (auto* node = reinterpret_cast<const VkBaseInStructure*>(features); node; node = node->pNext) {
        if (node->sType == type) {
            return reinterpret_cast<const T*>(node);
        }
    }
    return nullptr;
}

}

const VkPhysicalDeviceFeatures2* PyroWavePlaceboPool::requestedFeatures()
{
    static VkPhysicalDeviceVulkan13Features vk13 = [] {
        VkPhysicalDeviceVulkan13Features features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features.subgroupSizeControl = VK_TRUE;
        features.computeFullSubgroups = VK_TRUE;
        return features;
    }();
    static VkPhysicalDeviceVulkan12Features vk12 = [] {
        VkPhysicalDeviceVulkan12Features features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features.pNext = &vk13;
        features.timelineSemaphore = VK_TRUE;
        features.storageBuffer8BitAccess = VK_TRUE;
        features.shaderFloat16 = VK_TRUE;
        return features;
    }();
    static VkPhysicalDeviceFeatures2 features = [] {
        VkPhysicalDeviceFeatures2 features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &vk12;
        return features;
    }();
    return &features;
}

bool PyroWavePlaceboPool::supported(pl_vulkan vulkan)
{
    auto vk12 = findFeatures<VkPhysicalDeviceVulkan12Features>(
        vulkan->features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    auto vk13 = findFeatures<VkPhysicalDeviceVulkan13Features>(
        vulkan->features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    return vk12 && vk12->timelineSemaphore &&
           vk13 && vk13->subgroupSizeControl && vk13->computeFullSubgroups &&
           pl_find_named_fmt(vulkan->gpu, "r8") && pl_find_named_fmt(vulkan->gpu, "r16");
}

PyroWavePlaceboPool::PyroWavePlaceboPool(pl_vk_inst instance, pl_vulkan vulkan, std::mutex& commandLock)
    : m_Instance(instance), m_Vulkan(vulkan), m_CommandLock(commandLock)
{
    m_AppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    m_AppInfo.apiVersion = instance->api_version;
    m_InstanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    m_InstanceInfo.pApplicationInfo = &m_AppInfo;
    m_InstanceInfo.enabledExtensionCount = uint32_t(instance->num_extensions);
    m_InstanceInfo.ppEnabledExtensionNames = instance->extensions;

    // The decoder sees the graphics family and, when libplacebo has one, its
    // separate compute family. Those are the queues lockQueues() serializes.
    m_SharedQueues.push_back(vulkan->queue_graphics);
    if (vulkan->queue_compute.count > 0 && vulkan->queue_compute.index != vulkan->queue_graphics.index) {
        m_SharedQueues.push_back(vulkan->queue_compute);
    }
    uint32_t maxQueues = 0;
    for (const auto& queue : m_SharedQueues) {
        maxQueues = std::max(maxQueues, uint32_t(queue.count));
    }
    m_QueuePriorities.assign(size_t(maxQueues), 1.0f);
    for (const auto& queue : m_SharedQueues) {
        VkDeviceQueueCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = uint32_t(queue.index);
        info.queueCount = uint32_t(queue.count);
        info.pQueuePriorities = m_QueuePriorities.data();
        m_QueueInfos.push_back(info);
    }
    m_DeviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    m_DeviceInfo.pNext = vulkan->features;
    m_DeviceInfo.queueCreateInfoCount = uint32_t(m_QueueInfos.size());
    m_DeviceInfo.pQueueCreateInfos = m_QueueInfos.data();
    m_DeviceInfo.enabledExtensionCount = uint32_t(vulkan->num_extensions);
    m_DeviceInfo.ppEnabledExtensionNames = vulkan->extensions;

    pl_vulkan_sem_params semParams = {};
    semParams.type = VK_SEMAPHORE_TYPE_TIMELINE;
    m_Ready = pl_vulkan_sem_create(vulkan->gpu, &semParams);
    m_Done = pl_vulkan_sem_create(vulkan->gpu, &semParams);
}

PyroWavePlaceboPool::~PyroWavePlaceboPool()
{
    // The semaphores may still be referenced by submitted work
    pl_gpu_finish(m_Vulkan->gpu);
    for (auto& surface : m_Surfaces) {
        for (auto& plane : surface.planes) {
            pl_tex_destroy(m_Vulkan->gpu, &plane);
        }
    }
    pl_vulkan_sem_destroy(m_Vulkan->gpu, &m_Ready);
    pl_vulkan_sem_destroy(m_Vulkan->gpu, &m_Done);
}

void PyroWavePlaceboPool::lockQueues(void* opaque)
{
    auto pool = static_cast<PyroWavePlaceboPool*>(opaque);
    for (const auto& queue : pool->m_SharedQueues) {
        for (uint32_t i = 0; i < uint32_t(queue.count); i++) {
            pool->m_Vulkan->lock_queue(pool->m_Vulkan, uint32_t(queue.index), i);
        }
    }
}

void PyroWavePlaceboPool::unlockQueues(void* opaque)
{
    auto pool = static_cast<PyroWavePlaceboPool*>(opaque);
    for (auto queue = pool->m_SharedQueues.rbegin(); queue != pool->m_SharedQueues.rend(); ++queue) {
        for (int i = int(queue->count) - 1; i >= 0; i--) {
            pool->m_Vulkan->unlock_queue(pool->m_Vulkan, uint32_t(queue->index), uint32_t(i));
        }
    }
}

bool PyroWavePlaceboPool::pyroWaveVulkanDevice(PyroWaveVulkanDevice& device)
{
    if (m_Ready == VK_NULL_HANDLE || m_Done == VK_NULL_HANDLE) {
        return false;
    }
    device.getInstanceProcAddr = m_Instance->get_proc_addr;
    device.instance = m_Instance->instance;
    device.physicalDevice = m_Vulkan->phys_device;
    device.device = m_Vulkan->device;
    device.instanceInfo = &m_InstanceInfo;
    device.deviceInfo = &m_DeviceInfo;
    device.lockQueues = lockQueues;
    device.unlockQueues = unlockQueues;
    device.userdata = this;
    device.asyncCompute = m_SharedQueues.size() > 1;
    return true;
}

void PyroWavePlaceboPool::returnSurface(int index)
{
    std::lock_guard<std::mutex> lock(m_FreeList->lock);
    m_FreeList->busy[size_t(index)] = false;
}

void PyroWavePlaceboPool::freeFrameRef(void*, uint8_t* data)
{
    auto ref = reinterpret_cast<FrameRef*>(data);
    {
        std::lock_guard<std::mutex> lock(ref->freeList->lock);
        ref->freeList->busy[size_t(ref->index)] = false;
    }
    delete ref;
}

bool PyroWavePlaceboPool::holdPyroWaveSurface(int width, int height, bool chroma444, bool sixteenBit,
                                              PyroWaveVulkanSurface& surface)
{
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(m_FreeList->lock);
        for (size_t i = 0; i < m_FreeList->busy.size(); i++) {
            if (!m_FreeList->busy[i]) {
                index = int(i);
                break;
            }
        }
        if (index < 0 && m_FreeList->busy.size() < k_MaxSurfaces) {
            index = int(m_FreeList->busy.size());
            m_FreeList->busy.push_back(false);
            m_Surfaces.emplace_back();
        }
        if (index < 0) {
            return false;
        }
        m_FreeList->busy[size_t(index)] = true;
    }

    pl_gpu gpu = m_Vulkan->gpu;
    pl_fmt format = pl_find_named_fmt(gpu, sixteenBit ? "r16" : "r8");
    Surface& target = m_Surfaces[size_t(index)];
    target.sixteenBit = sixteenBit;
    surface = {};
    surface.index = index;
    surface.ready = m_Ready;
    surface.done = m_Done;

    for (int plane = 0; plane < 3; plane++) {
        const bool subsampled = plane != 0 && !chroma444;
        pl_tex_params params = {};
        params.w = subsampled ? width / 2 : width;
        params.h = subsampled ? height / 2 : height;
        params.format = format;
        params.sampleable = true;
        params.storable = true;
        params.renderable = (format->caps & PL_FMT_CAP_RENDERABLE) != 0;
        // Lets the calibrator and tests read decoded planes back
        params.host_readable = (format->caps & PL_FMT_CAP_HOST_READABLE) != 0;
        if (!pl_tex_recreate(gpu, &target.planes[plane], &params)) {
            returnSurface(index);
            return false;
        }
        surface.widths[plane] = uint32_t(params.w);
        surface.heights[plane] = uint32_t(params.h);
    }

    // The holds are submitted in order on libplacebo's queue, so the last
    // value also covers the transitions of the earlier planes. Texture
    // creation above records no commands and can be slow, so it stays
    // outside the command lock.
    std::lock_guard<std::mutex> commandLock(m_CommandLock);
    int held = 0;
    for (; held < 3; held++) {
        pl_vulkan_hold_params hold = {};
        hold.tex = target.planes[held];
        hold.layout = VK_IMAGE_LAYOUT_GENERAL;
        hold.qf = VK_QUEUE_FAMILY_IGNORED;
        hold.semaphore = { m_Ready, ++m_ReadyValue };
        if (!pl_vulkan_hold_ex(gpu, &hold)) {
            break;
        }
        surface.images[held] = pl_vulkan_unwrap(gpu, target.planes[held], &surface.format, nullptr);
    }
    if (held != 3) {
        for (int plane = 0; plane < held; plane++) {
            pl_vulkan_release_params release = {};
            release.tex = target.planes[plane];
            release.layout = VK_IMAGE_LAYOUT_GENERAL;
            release.qf = VK_QUEUE_FAMILY_IGNORED;
            release.semaphore = { m_Ready, m_ReadyValue };
            pl_vulkan_release_ex(gpu, &release);
        }
        returnSurface(index);
        return false;
    }

    surface.readyValue = m_ReadyValue;
    surface.doneValue = ++m_DoneValue;
    return true;
}

bool PyroWavePlaceboPool::releasePyroWaveSurface(const PyroWaveVulkanSurface& surface, bool decoded,
                                                 AVFrame* frame)
{
    Surface& target = m_Surfaces[size_t(surface.index)];
    for (auto plane : target.planes) {
        pl_vulkan_release_params release = {};
        release.tex = plane;
        release.layout = VK_IMAGE_LAYOUT_GENERAL;
        release.qf = VK_QUEUE_FAMILY_IGNORED;
        // A failed decode never signals doneValue
        release.semaphore = decoded ? pl_vulkan_sem { m_Done, surface.doneValue } :
                                      pl_vulkan_sem { m_Ready, surface.readyValue };
        pl_vulkan_release_ex(m_Vulkan->gpu, &release);
    }

    if (!decoded) {
        returnSurface(surface.index);
        return false;
    }

    auto ref = new FrameRef { m_FreeList, surface.index,
                              { target.planes[0], target.planes[1], target.planes[2] },
                              target.sixteenBit };
    frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(ref), sizeof(*ref),
                                     freeFrameRef, this, 0);
    if (frame->buf[0] == nullptr) {
        delete ref;
        returnSurface(surface.index);
        return false;
    }

    const bool chroma444 = surface.widths[1] == surface.widths[0];
    frame->format = target.sixteenBit ?
        (chroma444 ? AV_PIX_FMT_YUV444P16 : AV_PIX_FMT_YUV420P16) :
        (chroma444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P);
    frame->width = int(surface.widths[0]);
    frame->height = int(surface.heights[0]);
    return true;
}

bool PyroWavePlaceboPool::ownsFrame(const AVFrame* frame) const
{
    return frame->buf[0] != nullptr && av_buffer_get_opaque(frame->buf[0]) == this;
}

bool PyroWavePlaceboPool::mapFrame(const AVFrame* frame, pl_frame* out) const
{
    if (!ownsFrame(frame)) {
        return false;
    }

    auto ref = reinterpret_cast<const FrameRef*>(frame->buf[0]->data);
    pl_frame_from_avframe(out, frame);
    out->num_planes = 3;
    for (int plane = 0; plane < 3; plane++) {
        out->planes[plane] = {};
        out->planes[plane].texture = ref->planes[plane];
        out->planes[plane].components = 1;
        out->planes[plane].component_mapping[0] = plane;
    }
    // The planes hold UNORM samples over the full texture range
    out->repr.bits.sample_depth = out->repr.bits.color_depth = ref->sixteenBit ? 16 : 8;
    out->repr.bits.bit_shift = 0;
    pl_frame_set_chroma_location(out, pl_chroma_from_av(frame->chroma_location));
    return true;
}
