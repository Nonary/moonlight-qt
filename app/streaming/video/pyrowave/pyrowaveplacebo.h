#pragma once

#include "pyrowavesurfaces.h"

#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <memory>
#include <mutex>
#include <vector>

// PyroWave surfaces backed by libplacebo textures on the renderer's VkDevice.
// Decoded planes are sampled where they were written, with no readback or
// upload. hold/release run on the decoder thread; frames may be freed from any
// thread.
class PyroWavePlaceboPool : public IPyroWaveVulkanPool
{
public:
    // Device features the decoder needs beyond libplacebo's own, for
    // pl_vulkan_params.features.
    static const VkPhysicalDeviceFeatures2* requestedFeatures();

    // Whether vulkan was created with the features the decoder needs.
    static bool supported(pl_vulkan vulkan);

    // pl_swapchain_submit_frame() takes libplacebo's pending graphics command
    // outside the lock that guards command recording, so a command begun on
    // another thread at that moment corrupts the queue's sync value. The pool
    // records its holds under commandLock, which the owner must also hold
    // around every swapchain submit.
    PyroWavePlaceboPool(pl_vk_inst instance, pl_vulkan vulkan, std::mutex& commandLock);
    ~PyroWavePlaceboPool() override;

    bool pyroWaveVulkanDevice(PyroWaveVulkanDevice& device) override;
    bool holdPyroWaveSurface(int width, int height, bool chroma444, bool sixteenBit,
                             PyroWaveVulkanSurface& surface) override;
    bool releasePyroWaveSurface(const PyroWaveVulkanSurface& surface, bool decoded,
                                AVFrame* frame) override;

    bool ownsFrame(const AVFrame* frame) const;

    // Fills out with the frame's planes if the frame came from this pool.
    bool mapFrame(const AVFrame* frame, pl_frame* out) const;

private:
    struct FreeList {
        std::mutex lock;
        std::vector<bool> busy;
    };
    struct FrameRef {
        std::shared_ptr<FreeList> freeList;
        int index;
        pl_tex planes[3];
        bool sixteenBit;
    };
    struct Surface {
        pl_tex planes[3] = {};
        bool sixteenBit = false;
    };

    static void lockQueues(void* opaque);
    static void unlockQueues(void* opaque);
    static void freeFrameRef(void* opaque, uint8_t* data);
    void returnSurface(int index);

    pl_vk_inst m_Instance;
    pl_vulkan m_Vulkan;
    VkApplicationInfo m_AppInfo = {};
    VkInstanceCreateInfo m_InstanceInfo = {};
    std::vector<pl_vulkan_queue> m_SharedQueues;
    std::vector<float> m_QueuePriorities;
    std::vector<VkDeviceQueueCreateInfo> m_QueueInfos;
    VkDeviceCreateInfo m_DeviceInfo = {};

    VkSemaphore m_Ready = VK_NULL_HANDLE;
    uint64_t m_ReadyValue = 0;
    VkSemaphore m_Done = VK_NULL_HANDLE;
    uint64_t m_DoneValue = 0;
    std::vector<Surface> m_Surfaces;
    std::shared_ptr<FreeList> m_FreeList = std::make_shared<FreeList>();
    std::mutex& m_CommandLock;
};
