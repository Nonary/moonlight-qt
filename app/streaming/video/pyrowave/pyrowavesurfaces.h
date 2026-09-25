#pragma once

// Contract between the PyroWave decoder (Vulkan) and a renderer that owns the
// decoded planes. The renderer allocates a pool of shareable plane surfaces and
// two timeline fences in its own graphics API; the decoder imports them into its
// Vulkan device, decodes into a free surface, and signals the decode fence.
// The renderer waits for that value before sampling and signals the release
// fence after its last read, which the decoder waits for before reusing the
// surface. Neither side blocks the CPU on the other.

#include <atomic>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
}

enum class PyroWavePlaneFormat {
    R8Unorm,
    R16Unorm,
};

struct PyroWaveSharedPlane {
    // OS handle (Windows NT HANDLE) that the decoder takes ownership of.
    uintptr_t handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    PyroWavePlaneFormat format = PyroWavePlaneFormat::R8Unorm;
};

class IPyroWaveSurfacePool {
public:
    virtual ~IPyroWaveSurfacePool() = default;

    // Identity of the adapter that owns the surfaces (8-byte Windows LUID).
    virtual bool pyroWaveAdapterLuid(uint8_t luid[8]) = 0;

    virtual int pyroWaveSurfaceCount() const = 0;

    // Returns fresh OS handles for the three planes (Y, Cb, Cr) of a surface.
    // The caller owns them and must close them if it does not consume them.
    virtual bool exportPyroWaveSurface(int index, PyroWaveSharedPlane planes[3]) = 0;

    // Fresh OS handles for the shared timeline fences. The decoder signals the
    // decode fence; the renderer signals the release fence.
    virtual uintptr_t exportPyroWaveDecodeFence() = 0;
    virtual uintptr_t exportPyroWaveReleaseFence() = 0;
};

// Owned by each PyroWave AVFrame through frame->buf[0]. Freeing the last
// reference returns the surface to the decoder with the release fence value
// that makes it safe to overwrite.
struct PyroWaveFrameRef {
    static constexpr uint32_t k_Magic = 0x50595257; // "PYRW"

    uint32_t magic = k_Magic;
    int surface = -1;
    // Value of the decode fence signalled when the planes are written.
    uint64_t decodeFenceValue = 0;
    // Highest release fence value the renderer signalled after sampling.
    std::atomic<uint64_t> releaseFenceValue {0};

    static PyroWaveFrameRef* fromFrame(const AVFrame* frame)
    {
        if (frame == nullptr || frame->buf[0] == nullptr ||
                frame->buf[0]->size != sizeof(PyroWaveFrameRef)) {
            return nullptr;
        }
        auto ref = reinterpret_cast<PyroWaveFrameRef*>(frame->buf[0]->data);
        return ref->magic == k_Magic ? ref : nullptr;
    }

    void noteRelease(uint64_t value)
    {
        uint64_t current = releaseFenceValue.load(std::memory_order_relaxed);
        while (current < value &&
               !releaseFenceValue.compare_exchange_weak(current, value, std::memory_order_release)) {
        }
    }
};
