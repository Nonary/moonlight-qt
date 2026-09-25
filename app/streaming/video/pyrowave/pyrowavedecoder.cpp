#include "pyrowavedecoder.h"

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <Limelight.h>
#include <SDL.h>

#include <array>
#include <deque>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// Returned to the decoder when the last reference to a frame is dropped.
struct SurfaceFreeList {
    std::mutex lock;
    // Surface index and the release fence value that must complete before the
    // decoder may overwrite it (0 if the renderer never sampled it).
    std::deque<std::pair<int, uint64_t>> free;

    void push(int surface, uint64_t releaseValue)
    {
        std::lock_guard<std::mutex> guard(lock);
        free.emplace_back(surface, releaseValue);
    }

    bool pop(int& surface, uint64_t& releaseValue)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (free.empty()) {
            return false;
        }
        surface = free.front().first;
        releaseValue = free.front().second;
        free.pop_front();
        return true;
    }
};

struct FrameOwner {
    std::shared_ptr<SurfaceFreeList> freeList;
};

void freeFrameRef(void* opaque, uint8_t* data)
{
    auto owner = static_cast<FrameOwner*>(opaque);
    auto ref = reinterpret_cast<PyroWaveFrameRef*>(data);

    owner->freeList->push(ref->surface, ref->releaseFenceValue.load(std::memory_order_acquire));

    delete ref;
    delete owner;
}

void closeOsHandle(uintptr_t handle)
{
#ifdef _WIN32
    if (handle != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(handle));
    }
#else
    (void)handle;
#endif
}

VkFormat toVkFormat(PyroWavePlaneFormat format)
{
    switch (format) {
    case PyroWavePlaneFormat::R16Unorm:
        return VK_FORMAT_R16_UNORM;
    case PyroWavePlaneFormat::R8Unorm:
    default:
        return VK_FORMAT_R8_UNORM;
    }
}

const char* resultString(pyrowave_result result)
{
    switch (result) {
    case PYROWAVE_SUCCESS: return "success";
    case PYROWAVE_TIMEOUT: return "timeout";
    case PYROWAVE_ERROR_GENERIC: return "generic error";
    case PYROWAVE_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case PYROWAVE_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
    case PYROWAVE_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
    case PYROWAVE_ERROR_NO_VULKAN: return "no Vulkan";
    case PYROWAVE_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case PYROWAVE_ERROR_UNSUPPORTED_EXTERNAL_HANDLE: return "unsupported external handle";
    case PYROWAVE_ERROR_FAILED_EXTERNAL_HANDLE: return "failed external handle";
    default: return "unknown error";
    }
}

}

struct PyroWaveDecoder::Impl {
    Config config;
    PyroWaveFraming::StreamGeometry geometry {};

    pyrowave_device device = nullptr;
    pyrowave_decoder decoder = nullptr;
    pyrowave_sync_object decodeSync = nullptr;
    pyrowave_sync_object releaseSync = nullptr;
    uint64_t decodeValue = 0;

    struct Surface {
        std::array<pyrowave_image, 3> images {};
        pyrowave_gpu_buffers buffers {};
    };
    std::vector<Surface> surfaces;
    std::shared_ptr<SurfaceFreeList> freeList = std::make_shared<SurfaceFreeList>();

    PyroWaveFraming::Frame parsed;

    ~Impl()
    {
        // Destroying the decoder waits for its GPU work to finish.
        if (decoder != nullptr) {
            pyrowave_decoder_destroy(decoder);
        }
        for (auto& surface : surfaces) {
            for (auto image : surface.images) {
                if (image != nullptr) {
                    pyrowave_image_destroy(image);
                }
            }
        }
        if (decodeSync != nullptr) {
            pyrowave_sync_object_destroy(decodeSync);
        }
        if (releaseSync != nullptr) {
            pyrowave_sync_object_destroy(releaseSync);
        }
        if (device != nullptr) {
            pyrowave_device_destroy(device);
        }
    }

    bool importFence(uintptr_t handle, pyrowave_sync_object& sync)
    {
        if (handle == 0) {
            return false;
        }

        pyrowave_sync_object_create_info info = {};
        info.device = device;
        info.external_handle = handle;
        // A D3D11 fence is the same object as a D3D12 fence on Windows 10+.
        info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;

        const pyrowave_result result = pyrowave_sync_object_create(&info, &sync);
        if (result != PYROWAVE_SUCCESS) {
            // The implementation only takes ownership of the handle on success
            closeOsHandle(handle);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: importing a shared fence failed: %s",
                         resultString(result));
            return false;
        }
        return true;
    }

    bool importSurface(IPyroWaveSurfacePool* pool, int index)
    {
        PyroWaveSharedPlane planes[3];
        if (!pool->exportPyroWaveSurface(index, planes)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: renderer could not export surface %d", index);
            return false;
        }

        Surface surface;
        bool ok = true;
        for (int plane = 0; plane < 3; plane++) {
            if (!ok) {
                closeOsHandle(planes[plane].handle);
                continue;
            }

            VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = toVkFormat(planes[plane].format);
            imageInfo.extent = { planes[plane].width, planes[plane].height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            pyrowave_image_create_info info = {};
            info.device = device;
            info.external_handle = planes[plane].handle;
#ifdef _WIN32
            info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
#endif
            info.image_create_info = &imageInfo;

            pyrowave_result result = pyrowave_image_create(&info, &surface.images[plane]);
            if (result != PYROWAVE_SUCCESS) {
                closeOsHandle(planes[plane].handle);
                surface.images[plane] = nullptr;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "PyroWave: importing surface %d plane %d failed: %s",
                             index, plane, resultString(result));
                ok = false;
                continue;
            }

            result = pyrowave_image_get_image_view(surface.images[plane],
                                                   VK_IMAGE_ASPECT_COLOR_BIT,
                                                   VK_IMAGE_USAGE_STORAGE_BIT,
                                                   &surface.buffers.planes[plane]);
            if (result != PYROWAVE_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "PyroWave: creating a view of surface %d plane %d failed: %s",
                             index, plane, resultString(result));
                ok = false;
            }
        }

        if (!ok) {
            for (auto image : surface.images) {
                if (image != nullptr) {
                    pyrowave_image_destroy(image);
                }
            }
            return false;
        }

        surfaces.push_back(surface);
        return true;
    }
};

PyroWaveDecoder::PyroWaveDecoder() = default;

PyroWaveDecoder::~PyroWaveDecoder() = default;

bool PyroWaveDecoder::initialize(const Config& config, IPyroWaveSurfacePool* pool)
{
    SDL_assert(!m_Impl);

    if (pool == nullptr || config.width <= 0 || config.height <= 0) {
        return false;
    }
    if (!config.chroma444 && ((config.width | config.height) & 1)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: 4:2:0 streams need an even width and height (%dx%d)",
                     config.width, config.height);
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->geometry = { config.width, config.height, config.chroma444 };

    uint32_t major = 0, minor = 0, patch = 0;
    pyrowave_get_api_version(&major, &minor, &patch);

    uint8_t luid[8];
    if (!pool->pyroWaveAdapterLuid(luid)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: renderer adapter identity is unavailable");
        return false;
    }

    static_assert(sizeof(pyrowave_luid) == sizeof(luid), "LUID size mismatch");
    pyrowave_result result = pyrowave_create_device_by_compat(
        0, 0, nullptr, nullptr, reinterpret_cast<const pyrowave_luid*>(luid), &impl->device);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: no Vulkan device for the renderer's adapter: %s",
                     resultString(result));
        return false;
    }

    if (!pyrowave_device_confirm_interop_support(impl->device)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: the Vulkan driver cannot import D3D11 textures and fences");
        return false;
    }

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = impl->device;
    decoderInfo.width = config.width;
    decoderInfo.height = config.height;
    decoderInfo.chroma = config.chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    decoderInfo.fragment_path = pyrowave_decoder_device_prefers_fragment_path(impl->device);
    result = pyrowave_decoder_create(&decoderInfo, &impl->decoder);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: decoder creation failed: %s",
                     resultString(result));
        return false;
    }

    if (!impl->importFence(pool->exportPyroWaveDecodeFence(), impl->decodeSync) ||
            !impl->importFence(pool->exportPyroWaveReleaseFence(), impl->releaseSync)) {
        return false;
    }

    for (int i = 0; i < pool->pyroWaveSurfaceCount(); i++) {
        if (!impl->importSurface(pool, i)) {
            return false;
        }
        impl->freeList->push(i, 0);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave decoder ready: %dx%d %s %d-bit, %d surfaces, API %u.%u.%u, bitstream %s%s",
                config.width, config.height,
                config.chroma444 ? "4:4:4" : "4:2:0",
                config.tenBit ? 10 : 8,
                (int)impl->surfaces.size(),
                major, minor, patch, PYROWAVE_BITSTREAM_ID,
                decoderInfo.fragment_path ? " (fragment path)" : "");

    m_Impl = std::move(impl);
    return true;
}

bool PyroWaveDecoder::decode(const uint8_t* data, size_t size,
                             const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                             AVFrame* frame)
{
    Impl& impl = *m_Impl;

    m_LastFramePartial = false;
    if (!PyroWaveFraming::parse(data, size, packets, criticalPackets, impl.geometry, impl.parsed, m_LastError)) {
        return false;
    }
    m_LastFraming = impl.parsed.framing;

    // Every frame is independent. Clearing first keeps the 3-bit sequence
    // counter from treating a frame after a long drop as stale.
    pyrowave_decoder_clear(impl.decoder);
    for (const auto& span : impl.parsed.spans) {
        const pyrowave_result result = pyrowave_decoder_push_packet(impl.decoder, data + span.offset, span.size);
        if (result != PYROWAVE_SUCCESS) {
            m_LastError = std::string("decoder rejected a packet: ") + resultString(result);
            return false;
        }
    }

    if (!impl.parsed.partial) {
        if (!pyrowave_decoder_decode_is_ready(impl.decoder, false)) {
            m_LastError = "frame is incomplete";
            return false;
        }
    }
    else {
        // A partial frame decodes if its coarsest wavelet level is intact and
        // more than 90% of its blocks arrived; missing detail decodes as blur.
        // The parser checks the coarsest level: PyroWave's own check cannot tell
        // a lost block from an all-zero one that was never sent.
        if (!impl.parsed.coarseLevelIntact) {
            m_LastError = "part of the coarsest wavelet level was lost";
            return false;
        }
        if (!pyrowave_decoder_decode_is_ready_with_sideband(impl.decoder, true, 0, 0.9f, nullptr, 0)) {
            m_LastError = "too little of the frame arrived (" + std::to_string(impl.parsed.blockRecords) +
                          " of " + std::to_string(impl.parsed.announcedBlocks) + " blocks)";
            return false;
        }
    }
    m_LastFramePartial = impl.parsed.partial;

    int surface;
    uint64_t releaseValue;
    if (!impl.freeList->pop(surface, releaseValue)) {
        m_LastError = "no free output surface";
        return false;
    }

    std::array<pyrowave_gpu_external_reference, 3> acquireImages;
    std::array<pyrowave_gpu_external_reference, 3> releaseImages;
    for (int plane = 0; plane < 3; plane++) {
        // The previous contents are overwritten, so discard them
        acquireImages[plane] = { impl.surfaces[surface].images[plane], VK_QUEUE_FAMILY_IGNORED };
        releaseImages[plane] = { impl.surfaces[surface].images[plane], VK_QUEUE_FAMILY_EXTERNAL };
    }

    pyrowave_gpu_sync_operation acquire = {};
    acquire.images = acquireImages.data();
    acquire.num_images = acquireImages.size();
    if (releaseValue != 0) {
        // Wait on the GPU until the renderer finished its last read
        acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(impl.releaseSync);
        acquire.sync.value = releaseValue;
    }

    const uint64_t decodeValue = impl.decodeValue + 1;
    pyrowave_gpu_sync_operation release = {};
    release.images = releaseImages.data();
    release.num_images = releaseImages.size();
    release.sync.semaphore = pyrowave_sync_object_get_semaphore(impl.decodeSync);
    release.sync.value = decodeValue;

    const pyrowave_result result = pyrowave_decoder_decode_gpu_buffer(
        impl.decoder, &acquire, &release, &impl.surfaces[surface].buffers);
    if (result != PYROWAVE_SUCCESS) {
        impl.freeList->push(surface, releaseValue);
        m_LastError = std::string("decode submission failed: ") + resultString(result);
        return false;
    }
    impl.decodeValue = decodeValue;

    auto ref = new PyroWaveFrameRef();
    ref->surface = surface;
    ref->decodeFenceValue = decodeValue;
    auto owner = new FrameOwner { impl.freeList };

    frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(ref), sizeof(*ref),
                                     freeFrameRef, owner, 0);
    if (frame->buf[0] == nullptr) {
        // The GPU will still write this surface; the release value it needs
        // is unchanged because nobody sampled it.
        impl.freeList->push(surface, releaseValue);
        delete ref;
        delete owner;
        m_LastError = "out of memory";
        return false;
    }

    frame->width = impl.config.width;
    frame->height = impl.config.height;
    if (impl.config.tenBit) {
        frame->format = impl.config.chroma444 ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV420P10;
    }
    else {
        frame->format = impl.config.chroma444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P;
    }
    // Hosts average each 2x2 quad for 4:2:0 chroma
    frame->chroma_location = AVCHROMA_LOC_CENTER;
    frame->flags |= AV_FRAME_FLAG_KEY;

    return true;
}
