#pragma once

#include "pyrowaveframing.h"
#include "pyrowavesurfaces.h"

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// Decodes PyroWave frames on a private Vulkan device into shared surfaces owned
// by the renderer (see IPyroWaveSurfacePool). Not thread safe: decode() must be
// called from one thread. Frames it produces may be freed from any thread.
class PyroWaveDecoder
{
public:
    struct Config {
        int width = 0;
        int height = 0;
        bool chroma444 = false;
        bool tenBit = false;
    };

    PyroWaveDecoder();
    ~PyroWaveDecoder();

    PyroWaveDecoder(const PyroWaveDecoder&) = delete;
    PyroWaveDecoder& operator=(const PyroWaveDecoder&) = delete;

    bool initialize(const Config& config, IPyroWaveSurfacePool* pool);

    // Parses and decodes one frame into a free surface. The GPU work is only
    // submitted: the frame's PyroWaveFrameRef carries the decode fence value
    // to wait for. On success, frame receives the surface reference and its
    // format/size/colour metadata. Returns false if the frame was dropped.
    // packets maps the frame's RTP packets and which of them were lost (empty
    // for a frame that arrived whole); what survived is decoded when the
    // coarsest wavelet level did, which the host announces as the first
    // criticalPackets packets (0 if it did not).
    bool decode(const uint8_t* data, size_t size,
                const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                AVFrame* frame);

    // Why the last call failed, for logging.
    const std::string& lastError() const { return m_LastError; }

    // Whether the last decoded frame was missing records
    bool lastFramePartial() const { return m_LastFramePartial; }

    // Framing seen in the most recent successfully parsed frame.
    PyroWaveFraming::Framing lastFraming() const { return m_LastFraming; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    std::string m_LastError;
    PyroWaveFraming::Framing m_LastFraming = PyroWaveFraming::Framing::Records;
    bool m_LastFramePartial = false;
};
