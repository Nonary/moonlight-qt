#include "pyrowavecalibrator.h"

#include "backend/networkbuffers.h"
#include "backend/computermanager.h"
#include "backend/nvhttp.h"
#include "streaming/session.h"
#include "settings/streamingpreferences.h"
#include "streaming/video/pyrowave/pyrowavebitrate.h"
#include "streaming/video/pyrowave/pyrowavebandwidth.h"

#include <QMetaObject>
#include <QReadWriteLock>
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(HAVE_PYROWAVE) && (defined(Q_OS_LINUX) || defined(Q_OS_WIN32) || defined(Q_OS_DARWIN))
#include "streaming/video/pyrowave/pyrowavedecoder.h"
#include "streaming/video/pyrowave/pyrowaveframing.h"
#include <vulkan/vulkan.h>
#ifdef Q_OS_LINUX
#include "streaming/video/pyrowave/pyrowaveplacebo.h"
#elif defined(Q_OS_DARWIN)
#include "streaming/video/pyrowave/pyrowavemetal.h"
#include "streaming/video/pyrowave/pyrowavemetalcalibrator.h"
#else
#include "streaming/video/ffmpeg-renderers/d3d11pyrowave.h"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif
#include <pyrowave.h>
#endif

namespace {

// Grades on the codec author's quality scale (see pyrowavebitrate.h): his
// good-quality level, and the level below which the picture is noticeably
// softer than it.
constexpr double kFullQualityDb = kPyroWaveGoodQualityDb;
constexpr double kReducedQualityDb = 32.0;
// The regression's lowest level. A format that falls behind at this bitrate
// falls behind at any useful one.
constexpr double kFloorQualityDb = PYROWAVE_REGRESSION_MIN_PSNR_HVS_M_H;
// Decoded frames waiting to be drawn, as a stream's decoder thread runs ahead
// of its renderer.
constexpr size_t kQueuedFrames = 3;
// Seconds of frames timed per probe, at least kMinimumTimedFrames, and the
// share whose cost must fit the frame period.
constexpr int kTimedSeconds = 3;
constexpr int kMinimumTimedFrames = 300;
constexpr double kCostPercentile = 0.99;
// A live stream costs about a third more than this test (presentation,
// network receive, host bursts, the shared power budget). Formats whose slow
// frames use at most this share of the frame period stay smooth on any
// display; up to the second share, VRR hides their occasional late frame;
// beyond it, only a large VRR buffer does.
constexpr double kAnyDisplayShare = 0.6;
constexpr double kVrrShare = 0.8;

struct Sample {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    bool hdr = false;
    bool valid = false;
    bool keepsUp = false;
    // Share of the frame period its kCostPercentile frame uses
    double share = 0;
    // The link capped the bitrate below the author's recommendation
    bool linkLimited = false;
    // The bitrate was lowered below the cap so this device keeps up
    bool deviceLimited = false;
    int guideKbps = 0;
    int targetKbps = 0; // Requested image bitrate before network/device limits.
    int bitrateKbps = 0; // Applied total wire budget, including FEC.
    int imageKbps = 0;
    double qualityDb = 0;
    // At imageKbps or, when the format can't keep up, the lowest bitrate
    // tried: the kCostPercentile GPU time per frame, and its share of the
    // frame period
    double frameMs = 0;
    double load = 0;
    QString error;
};

int roundDownKbps(double kbps)
{
    return int(std::floor(kbps / 5000.0)) * 5000;
}

#if defined(HAVE_PYROWAVE) && (defined(Q_OS_LINUX) || defined(Q_OS_WIN32) || defined(Q_OS_DARWIN))

constexpr int kResolutions[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080},
#ifdef Q_OS_LINUX
                                  {1280, 800},
#endif
                                  {1280, 720}};

#ifdef Q_OS_LINUX
// Decoding and drawing run on separate threads, overlapping on the GPU, and
// the pair may use the whole frame period
constexpr bool kConcurrentDraw = true;
constexpr double kPeriodShare = 1.0;

// A headless libplacebo device. The sweep decodes into the same shared
// surfaces a stream's Vulkan renderer uses and draws each frame to a
// display-sized target with the stream's render parameters.
struct Renderer {
    pl_log log = nullptr;
    pl_vk_inst instance = nullptr;
    pl_vulkan vulkan = nullptr;
    std::mutex commandLock;
    std::unique_ptr<PyroWavePlaceboPool> pool;
    pl_renderer renderer = nullptr;
    pl_tex targets[2] = {};
    int displayWidth = 0;
    int displayHeight = 0;

    bool create(int width, int height)
    {
        displayWidth = width;
        displayHeight = height;
        log = pl_log_create(PL_API_VER, nullptr);
        pl_vk_inst_params instanceParams = pl_vk_inst_default_params;
        instance = pl_vk_inst_create(log, &instanceParams);
        if (!instance) return false;
        pl_vulkan_params params = pl_vulkan_default_params;
        params.instance = instance->instance;
        params.get_proc_addr = instance->get_proc_addr;
        params.features = PyroWavePlaceboPool::requestedFeatures();
        vulkan = pl_vulkan_create(log, &params);
        if (!vulkan || !PyroWavePlaceboPool::supported(vulkan)) return false;
        pool = std::make_unique<PyroWavePlaceboPool>(instance, vulkan, commandLock);
        renderer = pl_renderer_create(log, vulkan->gpu);
        return renderer != nullptr;
    }

    // The render target matches the swapchain: 10-bit for HDR, 8-bit for SDR,
    // at the display's size, or the stream's when the display is unknown
    bool prepare(int width, int height, bool, bool hdr)
    {
        pl_tex_params params = {};
        params.w = displayWidth > 0 ? displayWidth : width;
        params.h = displayHeight > 0 ? displayHeight : height;
        params.format = pl_find_named_fmt(vulkan->gpu, hdr ? "rgb10a2" : "rgba8");
        params.renderable = true;
        params.host_readable = true;
        if (!params.format) return false;
        return pl_tex_recreate(vulkan->gpu, &targets[hdr ? 1 : 0], &params);
    }

    // Draws the frame and returns once the GPU finished it
    bool present(AVFrame* frame, bool hdr)
    {
        if (hdr) {
            // Tagged as a stream tags 10-bit PyroWave in HDR mode
            frame->color_primaries = AVCOL_PRI_BT2020;
            frame->color_trc = AVCOL_TRC_SMPTE2084;
            frame->colorspace = AVCOL_SPC_BT2020_NCL;
        }
        pl_frame source = {};
        if (!pool->mapFrame(frame, &source)) return false;
        pl_tex target = targets[hdr ? 1 : 0];
        pl_frame output = {};
        output.num_planes = 1;
        output.planes[0].texture = target;
        output.planes[0].components = 4;
        for (int component = 0; component < 4; ++component) {
            output.planes[0].component_mapping[component] = component;
        }
        output.repr = pl_color_repr_rgb;
        output.color = hdr ? pl_color_space_hdr10 : pl_color_space_srgb;
        output.crop = { 0, 0, float(target->params.w), float(target->params.h) };
        if (!pl_render_image(renderer, &source, &output, &pl_render_fast_params)) return false;
        uint32_t pixel = 0;
        pl_tex_transfer_params transfer = {};
        transfer.tex = target;
        transfer.rc = { 0, 0, 0, 1, 1, 1 };
        transfer.ptr = &pixel;
        return pl_tex_download(vulkan->gpu, &transfer);
    }

    ~Renderer()
    {
        for (pl_tex& target : targets) {
            if (target) pl_tex_destroy(vulkan->gpu, &target);
        }
        pl_renderer_destroy(&renderer);
        pool.reset();
        pl_vulkan_destroy(&vulkan);
        pl_vk_inst_destroy(&instance);
        pl_log_destroy(&log);
    }
};
#elif defined(Q_OS_DARWIN)
// Decode and the native scaled Metal pass finish serially. Reserve the same
// conservative quarter-period headroom as the Windows serial check.
constexpr bool kConcurrentDraw = false;
constexpr double kPeriodShare = 0.75;
using Renderer = PyroWaveMetalCalibratorRenderer;
#else
// The D3D11 check only waits for decode completion, on the decoding thread,
// so decoding may use three quarters of each frame period and rendering the
// rest
constexpr bool kConcurrentDraw = false;
constexpr double kPeriodShare = 0.75;

// Use the same D3D11 texture and fence sharing as a Windows stream. A one-pixel
// D3D11 readback waits for decode completion and verifies renderer access.
struct Renderer {
    Microsoft::WRL::ComPtr<ID3D11Device5> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    LUID adapterLuid = {};
    std::unique_ptr<D3D11PyroWaveSurfaces> pool;
    HANDLE event = nullptr;

    bool create(int, int)
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        for (UINT index = 0;; ++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 description = {};
            if (FAILED(adapter->GetDesc1(&description)) ||
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            Microsoft::WRL::ComPtr<ID3D11Device> baseDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> baseContext;
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                         levels, 2, D3D11_SDK_VERSION, &baseDevice,
                                         nullptr, &baseContext)) ||
                    FAILED(baseDevice.As(&device)) || FAILED(baseContext.As(&context))) {
                device.Reset();
                context.Reset();
                continue;
            }
            static_assert(sizeof(pyrowave_luid) == sizeof(LUID), "LUID size mismatch");
            pyrowave_device decodeDevice = nullptr;
            const bool compatible = pyrowave_create_device_by_compat(
                0, 0, nullptr, nullptr,
                reinterpret_cast<const pyrowave_luid*>(&description.AdapterLuid),
                &decodeDevice) == PYROWAVE_SUCCESS;
            const bool interop = compatible && pyrowave_device_confirm_interop_support(decodeDevice);
            if (decodeDevice) pyrowave_device_destroy(decodeDevice);
            if (!interop) {
                device.Reset();
                context.Reset();
                continue;
            }
            adapterLuid = description.AdapterLuid;
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return event != nullptr;
        }
        return false;
    }

    bool prepare(int width, int height, bool chroma444, bool hdr)
    {
        pool.reset();
        staging.Reset();
        pool = std::make_unique<D3D11PyroWaveSurfaces>();
        if (!pool->initialize(device.Get(), adapterLuid, width, height, chroma444, hdr)) return false;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = desc.Height = 1;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
    }

    bool waitForFrame(const AVFrame* frame)
    {
        auto* ref = PyroWaveFrameRef::fromFrame(frame);
        if (!ref || !pool || !pool->decodeFence() ||
                FAILED(pool->decodeFence()->SetEventOnCompletion(ref->decodeFenceValue, event)) ||
                WaitForSingleObject(event, 2000) != WAIT_OBJECT_0 ||
                !pool->waitForDecode(context.Get(), ref)) return false;
        const auto* views = pool->planeViews(ref->surface);
        if (!views) return false;
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        (*views)[2]->GetResource(&resource);
        const D3D11_BOX box = {0, 0, 0, 1, 1, 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resource.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        context->Unmap(staging.Get(), 0);
        return pool->signalRelease(context.Get(), ref);
    }

    bool present(AVFrame* frame, bool)
    {
        return waitForFrame(frame);
    }

    ~Renderer()
    {
        pool.reset();
        if (event) CloseHandle(event);
    }
};
#endif

struct Planes {
    int width;
    int height;
    bool chroma444;
    std::vector<uint8_t> y, cb, cr;

    Planes(int w, int h, bool c444) : width(w), height(h), chroma444(c444)
    {
        const int cw = c444 ? w : w / 2;
        const int ch = c444 ? h : h / 2;
        y.resize(size_t(w) * h);
        cb.resize(size_t(cw) * ch);
        cr.resize(size_t(cw) * ch);
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                int value = (col * 3 + row * 2 + 17) & 255;
                if (((col / 64) + (row / 64) + 1) % 7 == 0) value = 235;
                y[size_t(row) * w + col] = uint8_t(std::clamp(value, 16, 235));
            }
        }
        for (int row = 0; row < ch; ++row) {
            for (int col = 0; col < cw; ++col) {
                cb[size_t(row) * cw + col] = uint8_t(96 + (col + 5) % 64);
                cr[size_t(row) * cw + col] = uint8_t(96 + (row * 2 + 3) % 64);
            }
        }
    }

    pyrowave_cpu_buffer buffer()
    {
        const int cw = chroma444 ? width : width / 2;
        pyrowave_cpu_buffer result = {};
        result.width = width;
        result.height = height;
        result.format = chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        result.data[0] = y.data();
        result.data[1] = cb.data();
        result.data[2] = cr.data();
        result.row_stride_in_bytes[0] = size_t(width);
        result.row_stride_in_bytes[1] = result.row_stride_in_bytes[2] = size_t(cw);
        result.plane_size_in_bytes[0] = y.size();
        result.plane_size_in_bytes[1] = cb.size();
        result.plane_size_in_bytes[2] = cr.size();
        return result;
    }
};

void appendU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

void appendPadding(std::vector<uint8_t>& out, size_t bytes)
{
    appendU32(out, 0xffffffffu);
    appendU32(out, uint32_t((bytes - 8) / 4));
    out.resize(out.size() + bytes - 8, 0);
}

// Repack the codec's records as a host-style frame, with the coarsest level
// first and no record boundary crossing an RTP payload.
std::vector<uint8_t> frameRecords(const std::vector<uint8_t>& bitstream,
                                  const std::vector<pyrowave_packet>& packets,
                                  size_t shard, uint32_t coarseBlocks)
{
    struct Record { size_t offset; size_t size; bool coarse; };
    std::vector<Record> records;
    for (const auto& packet : packets) {
        for (size_t pos = packet.offset; pos < packet.offset + packet.size;) {
            uint32_t first = 0, second = 0;
            std::memcpy(&first, bitstream.data() + pos, 4);
            std::memcpy(&second, bitstream.data() + pos + 4, 4);
            const bool header = (first & 0x80000000u) != 0;
            const size_t size = header ? 8 : size_t((first >> 16) & 0xfff) * 4;
            if (size < 8 || pos + size > packet.offset + packet.size) return {};
            records.push_back({pos, size, header || (second >> 8) < coarseBlocks});
            pos += size;
        }
    }
    if (records.empty()) return {};
    std::vector<uint8_t> out;
    auto remaining = [&] { return shard - (out.size() + 8) % shard; };
    auto place = [&](const Record& record) {
        out.insert(out.end(), bitstream.begin() + record.offset,
                   bitstream.begin() + record.offset + record.size);
    };
    place(records.front());
    for (bool coarse : {true, false}) {
        for (size_t i = 1; i < records.size(); ++i) {
            if (records[i].coarse == coarse && records[i].size + 8 > shard) {
                if (records[i].size % shard == remaining() - 4) appendPadding(out, 8);
                place(records[i]);
            }
        }
        for (size_t i = 1; i < records.size(); ++i) {
            if (records[i].coarse == coarse && records[i].size + 8 <= shard) {
                if (records[i].size > remaining() || remaining() - records[i].size == 4)
                    appendPadding(out, remaining());
                place(records[i]);
            }
        }
    }
    return out;
}

struct Probe {
    bool ok = false;
    // Mean GPU time per frame, used to stop a format early and to compare
    // bitrates
    double meanMs = 0;
    // The kCostPercentile GPU time per frame, and its share of the frame period
    double frameMs = 0;
    double load = 0;
    // Frames are served slower than they arrive, so the queue only grows
    bool overloaded = false;
    QString error;
};

double percentile(std::vector<double> values, double share)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t index = (std::min)(values.size() - 1,
                                    size_t(std::ceil(double(values.size()) * share)) - 1);
    return values[index];
}

// One format's encoder, decoder and render path; probe() measures one bitrate.
class FormatTester {
public:
    FormatTester(pyrowave_device device, Renderer& renderer, int width, int height, int fps,
                 bool chroma444, bool hdr, int packetSize, const std::atomic<bool>& cancelled)
        : m_Renderer(renderer), m_Width(width), m_Height(height), m_Fps(fps),
          m_Chroma444(chroma444), m_Hdr(hdr), m_PacketSize(packetSize), m_Cancelled(cancelled), m_Source(width, height, chroma444)
    {
        pyrowave_encoder_create_info info = {};
        info.device = device;
        info.width = width;
        info.height = height;
        info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
        if (pyrowave_encoder_create(&info, &m_Encoder) != PYROWAVE_SUCCESS) {
            m_Error = QStringLiteral("Could not initialize encoder");
            return;
        }
        if (!renderer.prepare(width, height, chroma444, hdr)) {
            m_Error = QStringLiteral("Could not create renderer surfaces");
            return;
        }
        PyroWaveDecoder::Config config;
        config.width = width;
        config.height = height;
        config.chroma444 = chroma444;
        config.tenBit = hdr;
#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
#ifdef Q_OS_DARWIN
        config.vulkanPool = renderer.pool();
        config.requireSharedOutput = true;
#else
        config.vulkanPool = renderer.pool.get();
#endif
        const bool initialized = m_Decoder.initialize(config, nullptr);
#else
        const bool initialized = m_Decoder.initialize(config, renderer.pool.get());
#endif
        if (!initialized) {
            m_Error = QStringLiteral("Could not initialize decoder");
        }
    }

    ~FormatTester()
    {
        if (m_Encoder) pyrowave_encoder_destroy(m_Encoder);
    }

    QString error() const { return m_Error; }

    // A synthetic frame encoded at the bitrate's per-frame budget, decoded and
    // drawn back to back. A short run warms clocks and compiles shaders and
    // stops a format that can't sustain the frame rate at all; unless quick,
    // a timed run follows.
    Probe probe(int bitrateKbps, bool quick = false)
    {
        Probe probe;
        if (!encode(bitrateKbps)) {
            probe.error = m_Error;
            return probe;
        }
        const double periodMs = 1000.0 / m_Fps;
        std::vector<double> serviceMs;
        if (!run((std::max)(24, m_Fps / 2), serviceMs)) {
            probe.error = runError();
            return probe;
        }
        auto summarize = [&] {
            probe.meanMs = std::accumulate(serviceMs.begin(), serviceMs.end(), 0.0) / serviceMs.size();
            probe.frameMs = percentile(serviceMs, kCostPercentile);
            probe.load = probe.frameMs / periodMs;
            probe.overloaded = probe.meanMs >= periodMs * kPeriodShare;
        };
        summarize();
        probe.ok = true;
        if (probe.overloaded || quick) {
            return probe;
        }
        if (!run((std::max)(kMinimumTimedFrames, m_Fps * kTimedSeconds), serviceMs)) {
            probe.ok = false;
            probe.error = runError();
            return probe;
        }
        summarize();
        return probe;
    }

private:
    bool encode(int bitrateKbps)
    {
        auto input = m_Source.buffer();
        pyrowave_rate_control rate = { size_t(double(bitrateKbps) * 1000.0 / m_Fps / 8.0) };
        if (pyrowave_encoder_encode_cpu_synchronous(m_Encoder, &input, &rate) != PYROWAVE_SUCCESS) {
            m_Error = QStringLiteral("Could not encode test image");
            return false;
        }
        size_t packetCount = 0;
        const size_t shard = size_t(m_PacketSize - 16);
        if (pyrowave_encoder_compute_num_packets_with_padding(m_Encoder, shard, 8, &packetCount) != PYROWAVE_SUCCESS) {
            m_Error = QStringLiteral("Could not packetize test image");
            return false;
        }
        std::vector<pyrowave_packet> packets(packetCount);
        std::vector<uint8_t> bitstream(rate.maximum_bitstream_size + 1024 * 1024);
        size_t written = 0;
        if (pyrowave_encoder_packetize_with_padding(m_Encoder, packets.data(), shard, 8, &written,
                                                    bitstream.data(), bitstream.size()) != PYROWAVE_SUCCESS) {
            m_Error = QStringLiteral("Could not packetize test image");
            return false;
        }
        packets.resize(written);
        m_Framed = frameRecords(bitstream, packets, shard,
                                PyroWaveFraming::coarseBlockCount({m_Width, m_Height, m_Chroma444}));
        if (m_Framed.empty()) {
            m_Error = QStringLiteral("Could not packetize test image");
            return false;
        }
        return true;
    }

    QString runError() const
    {
        const QString error = QString::fromStdString(m_Decoder.lastError());
        return error.isEmpty() ? QStringLiteral("Could not draw the decoded frame") : error;
    }

    // Decodes feed a queue of kQueuedFrames that is drawn in order, on a
    // second thread when kConcurrentDraw. The decoder blocks while the queue
    // is full, so the interval between two frames finishing is the second
    // one's cost to the pipeline.
    bool run(int frames, std::vector<double>& serviceMs)
    {
        using Clock = std::chrono::steady_clock;
        std::mutex lock;
        std::condition_variable wake;
        std::deque<AVFrame*> queued;
        bool decoding = true;
        std::atomic<bool> ok {true};
        std::vector<Clock::time_point> finished;
        finished.reserve(size_t(frames));

        // Only one thread draws at a time: the drawer, then this one
        auto draw = [&](AVFrame* frame) {
            if (!m_Renderer.present(frame, m_Hdr)) ok = false;
            av_frame_free(&frame);
            finished.push_back(Clock::now());
        };
        auto drawQueued = [&] {
            for (;;) {
                AVFrame* frame = nullptr;
                {
                    std::unique_lock<std::mutex> guard(lock);
                    wake.wait(guard, [&] { return !decoding || !queued.empty(); });
                    if (queued.empty()) return;
                    frame = queued.front();
                    queued.pop_front();
                }
                wake.notify_all();
                draw(frame);
            }
        };

        std::thread drawer;
        if (kConcurrentDraw) drawer = std::thread(drawQueued);
        for (int i = 0; i < frames && ok; ++i) {
            if (m_Cancelled.load()) {
                ok = false;
                break;
            }
            AVFrame* frame = av_frame_alloc();
            if (!frame || !m_Decoder.decode(m_Framed.data(), m_Framed.size(), {}, 0, frame)) {
                av_frame_free(&frame);
                ok = false;
                break;
            }
            if (kConcurrentDraw) {
                std::unique_lock<std::mutex> guard(lock);
                wake.wait(guard, [&] { return queued.size() < kQueuedFrames; });
                queued.push_back(frame);
                guard.unlock();
                wake.notify_all();
            }
            else {
                queued.push_back(frame);
                if (queued.size() > kQueuedFrames) {
                    draw(queued.front());
                    queued.pop_front();
                }
            }
        }
        {
            std::lock_guard<std::mutex> guard(lock);
            decoding = false;
        }
        wake.notify_all();
        if (drawer.joinable()) {
            drawer.join();
        }
        while (!queued.empty()) {
            draw(queued.front());
            queued.pop_front();
        }
        serviceMs.clear();
        for (size_t i = 1; i < finished.size(); ++i) {
            serviceMs.push_back(std::chrono::duration<double, std::milli>(finished[i] - finished[i - 1]).count());
        }
        return ok && !serviceMs.empty();
    }

    Renderer& m_Renderer;
    int m_Width;
    int m_Height;
    int m_Fps;
    bool m_Chroma444;
    bool m_Hdr;
    int m_PacketSize;
    const std::atomic<bool>& m_Cancelled;
    Planes m_Source;
    pyrowave_encoder m_Encoder = nullptr;
    PyroWaveDecoder m_Decoder;
    std::vector<uint8_t> m_Framed;
    QString m_Error;
};

// Test the selected target directly, lowering it only when fresh measurements
// establish a useful reduction in device cost.
Sample calibrateFormat(pyrowave_device device, Renderer& renderer, int width, int height, int fps,
                       bool chroma444, bool hdr, int linkCapKbps, int linkSpeedKbps,
                       PyroWaveCalibration::Target target,
                       const pyrowave::bandwidth::transport_t& transport,
                       const std::atomic<bool>& cancelled)
{
    Sample sample;
    sample.width = width;
    sample.height = height;
    sample.chroma444 = chroma444;
    sample.hdr = hdr;
    sample.guideKbps = pyroWaveRecommendedKbps(width, height, fps, chroma444, hdr);
    const int wireCapKbps = PyroWaveCalibration::wireTarget(target, linkCapKbps, linkSpeedKbps,
        PyroWaveCalibration::roundUp(pyrowave::bandwidth::total_kbps(sample.guideKbps, fps, transport)));
    const int imageCapKbps = PyroWaveCalibration::imageCapacity(wireCapKbps, fps, transport);
    if (imageCapKbps < 5000) {
        sample.error = QStringLiteral("Not enough stable bandwidth after FEC and packet overhead");
        return sample;
    }
    sample.targetKbps = PyroWaveCalibration::imageTarget(target, sample.guideKbps,
                                                        PyroWaveLink::maximumKbps);
    const int topKbps = PyroWaveCalibration::imageTarget(target, sample.guideKbps, imageCapKbps);
    if (target > PyroWaveCalibration::Recommended) sample.targetKbps = topKbps;
    sample.linkLimited = topKbps < (target <= PyroWaveCalibration::Recommended ?
                                   sample.targetKbps : sample.guideKbps);
    const auto wireKbps = [&](int imageKbps) {
        return PyroWaveCalibration::roundUp(pyrowave::bandwidth::total_kbps(imageKbps, fps, transport));
    };
    const auto quality = [&](int kbps) {
        return pyroWaveQualityDb(width, height, fps, chroma444, hdr, kbps);
    };
    const double availableMs = 1000.0 / fps * kPeriodShare;
    const auto keepsUp = [&](const Probe& probe) {
        return probe.ok && !probe.overloaded && probe.frameMs <= availableMs;
    };

    FormatTester tester(device, renderer, width, height, fps, chroma444, hdr, transport.packetsize, cancelled);
    if (!tester.error().isEmpty()) {
        sample.error = tester.error();
        return sample;
    }
    // A stall elsewhere on the system can land in any timed run, so a miss by
    // a format whose throughput keeps up is measured once more and the better
    // run kept.
    const auto measure = [&](int kbps) {
        Probe probe = tester.probe(kbps);
        if (probe.ok && !probe.overloaded && !keepsUp(probe) && !cancelled.load()) {
            const Probe again = tester.probe(kbps);
            if (again.ok && (keepsUp(again) || again.frameMs < probe.frameMs)) probe = again;
        }
        return probe;
    };
    const int floorKbps = roundDownKbps(pyroWaveKbpsForQuality(width, height, fps, chroma444, hdr,
                                                             kFloorQualityDb));
    const auto selected = PyroWaveCalibration::searchDevice(topKbps, sample.guideKbps, floorKbps,
        measure, [&](int kbps) { return tester.probe(kbps, true); }, keepsUp,
        [&] { return cancelled.load(); });
    if (!selected.cost.ok) {
        sample.error = selected.cost.error;
        return sample;
    }
    sample.valid = true;
    sample.keepsUp = keepsUp(selected.cost);
    sample.deviceLimited = selected.deviceLimited;
    sample.share = selected.cost.frameMs / availableMs;
    sample.imageKbps = selected.imageKbps;
    sample.bitrateKbps = wireKbps(selected.imageKbps);
    sample.qualityDb = quality(selected.imageKbps);
    sample.frameMs = selected.cost.frameMs;
    sample.load = selected.cost.load;
    return sample;
}

QString runSweep(int fps, int displayWidth, int displayHeight, int linkCapKbps, int linkSpeedKbps,
                 PyroWaveCalibration::Target target,
                 const pyrowave::bandwidth::transport_t& transport,
                 const std::atomic<bool>& cancelled, const std::function<void(const Sample&)>& report)
{
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) {
        return QStringLiteral("No usable Vulkan device");
    }
    QString error;
    {
        Renderer renderer;
        if (!renderer.create(displayWidth, displayHeight)) {
            error = QStringLiteral("Could not create a GPU renderer for PyroWave calibration");
        }
        else {
            for (const auto& resolution : kResolutions) {
                for (bool chroma444 : {true, false}) {
                    for (bool hdr : {true, false}) {
                        if (cancelled.load()) break;
                        const Sample sample = calibrateFormat(device, renderer, resolution[0], resolution[1],
                                                              fps, chroma444, hdr, linkCapKbps, linkSpeedKbps,
                                                              target, transport, cancelled);
                        // A format cut short by cancellation has no result
                        if (cancelled.load()) break;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "PyroWave calibration: %dx%d %s %d-bit %d FPS: %s, %d wire kbps (%d image, guide %d, link cap %d), "
                                    "%.1f dB, GPU p99 %.2f ms/frame (%.0f%% of the frame)%s%s",
                                    sample.width, sample.height, sample.chroma444 ? "4:4:4" : "4:2:0",
                                    sample.hdr ? 10 : 8, fps,
                                    !sample.valid ? "error" : !sample.keepsUp ? "falls behind" :
                                    sample.share > kVrrShare ? "needs a large VRR buffer" :
                                    sample.share > kAnyDisplayShare ? "needs VRR" : "keeps up on any display",
                                    sample.bitrateKbps, sample.imageKbps, sample.guideKbps, linkCapKbps, sample.qualityDb,
                                    sample.frameMs, sample.load * 100,
                                    sample.error.isEmpty() ? "" : ", error: ",
                                    sample.error.isEmpty() ? "" : sample.error.toUtf8().constData());
                        report(sample);
                    }
                }
            }
        }
    }
    pyrowave_device_destroy(device);
    return error;
}

#endif

// Smoothness risk rather than picture quality
QString tier(const Sample& sample)
{
    if (!sample.valid) return QStringLiteral("error");
    if (!sample.keepsUp) return QStringLiteral("slow");
    if (sample.share > kVrrShare) return QStringLiteral("vrrLarge");
    return sample.share > kAnyDisplayShare ? QStringLiteral("vrr") : QStringLiteral("any");
}

QString quality(const Sample& sample)
{
    // The recommendation is rounded up, so it always reaches the full level
    if (sample.qualityDb >= kFullQualityDb - 0.01) return QStringLiteral("full");
    if (sample.qualityDb >= kReducedQualityDb) return QStringLiteral("reduced");
    return QStringLiteral("low");
}

QVariantMap toMap(const Sample& sample)
{
    return {{QStringLiteral("width"), sample.width},
            {QStringLiteral("height"), sample.height},
            {QStringLiteral("chroma444"), sample.chroma444},
            {QStringLiteral("hdr"), sample.hdr},
            {QStringLiteral("valid"), sample.valid},
            {QStringLiteral("keepsUp"), sample.keepsUp},
            {QStringLiteral("tier"), tier(sample)},
            {QStringLiteral("quality"), quality(sample)},
            {QStringLiteral("linkLimited"), sample.linkLimited},
            {QStringLiteral("deviceLimited"), sample.deviceLimited},
            {QStringLiteral("guideKbps"), sample.guideKbps},
            {QStringLiteral("targetKbps"), sample.targetKbps},
            {QStringLiteral("bitrateKbps"), sample.bitrateKbps},
            {QStringLiteral("imageKbps"), sample.imageKbps},
            {QStringLiteral("qualityDb"), sample.qualityDb},
            {QStringLiteral("frameMs"), sample.frameMs},
            {QStringLiteral("loadPercent"), qRound(sample.load * 100)},
            {QStringLiteral("error"), sample.error}};
}

} // namespace

PyroWaveCalibrator::PyroWaveCalibrator(QObject* parent) : QObject(parent) {}

PyroWaveCalibrator::~PyroWaveCalibrator()
{
    if (m_Worker) {
        if (m_Cancel) m_Cancel->store(true);
        m_Worker->wait();
        delete m_Worker;
    }
}

void PyroWaveCalibrator::cancel()
{
    if (m_Cancel) m_Cancel->store(true);
}

int PyroWaveCalibrator::bandwidthTargetKbps() const
{
    if (!m_BandwidthReady) return 0;
    const auto target = static_cast<PyroWaveCalibration::Target>(m_Target);
    const int image = PyroWaveCalibration::imageTarget(
        target == Minimum ? PyroWaveCalibration::Minimum : PyroWaveCalibration::Recommended,
        m_RecommendedImageKbps, m_RecommendedImageKbps);
    return PyroWaveCalibration::roundUp(pyrowave::bandwidth::total_kbps(image, m_Fps, m_Transport));
}

QString PyroWaveCalibrator::bandwidthQuality() const
{
    if (!m_BandwidthReady) return QString();
    const auto target = static_cast<PyroWaveCalibration::Target>(m_Target);
    const int image = PyroWaveCalibration::imageCapacity(
        PyroWaveCalibration::wireTarget(target, m_LinkCapKbps, m_LinkSpeedKbps,
            PyroWaveCalibration::roundUp(pyrowave::bandwidth::total_kbps(m_RecommendedImageKbps, m_Fps, m_Transport))),
        m_Fps, m_Transport);
    switch (PyroWaveCalibration::imageQuality(target, image, m_RecommendedImageKbps)) {
    case PyroWaveCalibration::MeetsTarget: return QStringLiteral("target");
    case PyroWaveCalibration::ReducedQuality: return QStringLiteral("reduced");
    case PyroWaveCalibration::BelowMinimum: return QStringLiteral("low");
    }
    return QString();
}

void PyroWaveCalibrator::start(ComputerManager* manager, const QString& hostUuid, int fps,
                               int displayWidth, int displayHeight, int bitrateTarget)
{
    if (m_Running || m_Worker) return;
    m_BandwidthReady = false;
    m_LinkCapKbps = 0;
    m_LinkSpeedKbps = 0;
    m_PaceMbps = 0;
    m_LinkSummary.clear();
    if (bitrateTarget < Minimum || bitrateTarget > Maximum) {
        m_Results.clear();
        m_Message = tr("Choose a valid bitrate target first.");
        emit changed();
        return;
    }
    const auto target = static_cast<PyroWaveCalibration::Target>(bitrateTarget);
    if (fps < 10 || fps > 240) {
        m_Results.clear();
        m_Message = tr("Choose a valid frame rate first.");
        emit changed();
        return;
    }
    if (Session::get() != nullptr) {
        m_Results.clear();
        m_Message = tr("Finish the current stream before calibrating.");
        emit changed();
        return;
    }

    const auto hosts = manager ? manager->getComputers() : QVector<NvComputer*>();
    const auto host = std::find_if(hosts.begin(), hosts.end(), [&](const NvComputer* computer) {
        QReadLocker lock(&computer->lock);
        return computer->uuid == hostUuid;
    });
    if (host == hosts.end()) {
        m_Results.clear();
        m_Message = tr("Choose a paired online host before calibrating.");
        emit changed();
        return;
    }
    NvAddress hostAddress;
    uint16_t hostHttpsPort = 0;
    QSslCertificate hostCertificate;
    QString hostName;
    bool useTrueUid = false;
    {
        QReadLocker lock(&(*host)->lock);
        if ((*host)->state != NvComputer::CS_ONLINE ||
            (*host)->pairState != NvComputer::PS_PAIRED ||
            !((*host)->serverCodecModeSupport & SCM_PYROWAVE)) {
            m_Results.clear();
            m_Message = tr("Choose an online, paired host that supports PyroWave.");
            emit changed();
            return;
        }
        hostAddress = (*host)->activeAddress;
        hostHttpsPort = (*host)->activeHttpsPort;
        hostCertificate = (*host)->serverCert;
        hostName = (*host)->name;
        useTrueUid = !(*host)->isNvidiaServerSoftware;
    }

    int packetSize = StreamingPreferences::get()->packetSize;
    if (packetSize == 0) {
        packetSize = (*host)->getActiveAddressReachability() == NvComputer::RI_LAN ? 1392 : 1024;
    }
    if (packetSize < 256 || packetSize > 1392 || packetSize % 4 != 0) {
        m_Results.clear();
        m_Message = tr("Calibration supports packet sizes from 256 to 1392 bytes in multiples of four.");
        emit changed();
        return;
    }
    const int linkMbps = NetworkBuffers::routedWiredLinkMbps(QHostAddress(hostAddress.address()));
    m_LinkSummary = tr("Measuring bandwidth from %1 to this PC…").arg(hostName);

#if !defined(HAVE_PYROWAVE) || (!defined(Q_OS_LINUX) && !defined(Q_OS_WIN32) && !defined(Q_OS_DARWIN))
    Q_UNUSED(displayWidth);
    Q_UNUSED(displayHeight);
    Q_UNUSED(target);
    m_Results.clear();
    m_Message = tr("Local PyroWave calibration requires a supported GPU decoder.");
    emit changed();
#else
    m_Fps = fps;
    m_DisplayWidth = displayWidth;
    m_DisplayHeight = displayHeight;
    m_Target = bitrateTarget;
    // Step one grades the video settings selected when this test starts.
    // Step two still evaluates each format independently.
    const auto prefs = StreamingPreferences::get();
    m_RecommendedImageKbps = pyroWaveRecommendedKbps(prefs->width, prefs->height, fps,
                                                    prefs->enableYUV444, prefs->enableHdr);
    m_VideoDescription = tr("%1×%2 at %3 FPS, %4 %5")
        .arg(prefs->width).arg(prefs->height).arg(fps)
        .arg(prefs->enableYUV444 ? QStringLiteral("4:4:4") : QStringLiteral("4:2:0"))
        .arg(prefs->enableHdr ? QStringLiteral("HDR") : QStringLiteral("SDR"));
    m_HostUuid = hostUuid;
    m_Running = true;
    m_Results.clear();
    m_Message = tr("Testing host-to-client bandwidth…");
    emit changed();

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_Cancel = cancelled;
    const int recommendedImageKbps = m_RecommendedImageKbps;
    m_Worker = QThread::create([this, fps, linkMbps,
                                hostAddress, hostHttpsPort, hostCertificate, hostName,
                                useTrueUid, target, packetSize, recommendedImageKbps, cancelled] {
        int hostLinkMbps = 0;
        PyroWaveLink::Result link;
        PyroWaveLink::PaceResult pace;
        QString paceNote;
        int linkSpeedKbps = 0;
        int frameCapKbps = 0;
        pyrowave::bandwidth::transport_t transport;
        transport.packetsize = packetSize;
        try {
            NvHTTP http(hostAddress, hostHttpsPort, hostCertificate, useTrueUid);
            const QString serverInfo = http.getServerInfo(NvHTTP::NVLL_ERROR);
            if (NvHTTP::getXmlString(serverInfo, "PyroWaveUdpProbeVersion") != "1" ||
                NvHTTP::getXmlString(serverInfo, "PyroWaveWireBudgetVersion") != "1") {
                throw std::runtime_error("Update Vibeshine to support UDP loss testing and FEC-aware calibration");
            }
            const bool udpHandshake = NvHTTP::getXmlString(serverInfo, "PyroWaveUdpHandshakeVersion") == "1";
#ifdef Q_OS_WIN32
            if (!udpHandshake) {
                throw std::runtime_error("Update Vibeshine to support the PyroWave UDP handshake for Windows calibration");
            }
#endif
            bool validFec = false, validParity = false;
            const int fec = NvHTTP::getXmlString(serverInfo, "PyroWaveCriticalFecPercentage").toInt(&validFec);
            const int parity = NvHTTP::getXmlString(serverInfo, "PyroWaveMinParityShards").toInt(&validParity);
            if (!validFec || fec < 0 || fec > 100 || !validParity || parity < 0 || parity > 255) {
                throw std::runtime_error("Invalid host FEC budget");
            }
            transport.critical_fec_percentage = fec;
            transport.min_parity_shards = parity;
            hostLinkMbps = NvHTTP::getXmlString(serverInfo, "PyroWaveHostLinkMbps").toInt();
            int capKbps = PyroWaveLink::maximumKbps;
            if (hostLinkMbps > 0) capKbps = (std::min)(capKbps, int((std::min)(hostLinkMbps, 3000) * 1000));
            if (linkMbps > 0) capKbps = (std::min)(capKbps, int((std::min)(linkMbps, 3000) * 1000));
            // Quality targets need no capacity search above the largest image
            // recommendation in the matrix. Allow the existing 5% headroom.
            if (target <= PyroWaveCalibration::Recommended) {
                int neededKbps = 0;
                for (const auto& resolution : kResolutions) {
                    for (bool chroma444 : {true, false}) for (bool hdr : {true, false}) {
                        const int guide = pyroWaveRecommendedKbps(resolution[0], resolution[1], fps, chroma444, hdr);
                        const int image = PyroWaveCalibration::imageTarget(target, guide,
                            PyroWaveCalibration::imageCapacity(capKbps, fps, transport));
                        neededKbps = (std::max)(neededKbps,
                            PyroWaveCalibration::qualityProbeCeiling(image, fps, transport));
                    }
                }
                capKbps = (std::min)(capKbps, (std::max)(5000, neededKbps));
            }
            else if (target == PyroWaveCalibration::Moderate && (hostLinkMbps > 0 || linkMbps > 0)) {
                // Search only as high as the preset can use: 60% of the link
                // or the largest Recommended rate in the matrix, whichever is
                // higher, plus the 5% confirmation margin. Below it, the
                // search confirms the highest rate the path sustains.
                int neededKbps = PyroWaveCalibration::roundUp(
                    PyroWaveCalibration::roundDown(capKbps * 0.6) / 0.95);
                for (const auto& resolution : kResolutions) {
                    for (bool chroma444 : {true, false}) for (bool hdr : {true, false}) {
                        neededKbps = (std::max)(neededKbps, PyroWaveCalibration::qualityProbeCeiling(
                            pyroWaveRecommendedKbps(resolution[0], resolution[1], fps, chroma444, hdr), fps, transport));
                    }
                }
                capKbps = (std::min)(capKbps, neededKbps);
            }
            // Starting at the bounded ceiling avoids a slow upward staircase.
            // Search precision, probe duration and two confirmations are intact.
            const auto probeCapacity = [&](int kbps) {
                QMetaObject::invokeMethod(this, [this, kbps] {
                    m_Message = tr("Testing %1 Mbps including FEC: measuring throughput, loss and delivery timing…").arg(kbps / 1000);
                    emit changed();
                }, Qt::QueuedConnection);
                const auto result = http.probePyroWaveUdp(kbps, packetSize, *cancelled, udpHandshake);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave UDP probe: %d kbps, sent %u/%u, received %u, loss %.3f%%, worst 100ms %.3f%%, "
                    "delay p99 %.2f ms, growth %.2f ms, host duration %.2f ms, receiver read delay p99 %.2f ms, "
                    "kernel arrivals %d, host send retries %u, host send error %d: capacity %s, timing %s (%s)",
                    kbps, result.sent, result.expected,
                    result.received, result.lossPercent, result.worstWindowLossPercent,
                    result.delayP99Ms, result.delayGrowthMs, result.senderMs, result.receiverReadDelayP99Ms,
                    result.kernelArrivalTimestamps, result.hostSendRetries, result.hostLastSendError,
                    result.capacityQualified() ? "pass" : "fail",
                    result.stable() ? "pass" : "fail", result.failureReason());
                return result;
            };
            link = PyroWaveLink::search(capKbps, capKbps, probeCapacity, [&] { return cancelled->load(); }, false);
            if (!cancelled->load() && !link.capacityQualified()) {
                throw std::runtime_error(QString("No usable PyroWave network budget: %1. Last test %2 Mbps: received %3/%4 packets, "
                    "loss %5%, delivery variation p99 %6 ms, delay growth %7 ms, host duration %8 ms.")
                    .arg(QString::fromLatin1(link.failureReason())).arg(link.requestedKbps / 1000)
                    .arg(link.received).arg(link.expected).arg(link.lossPercent, 0, 'f', 3)
                    .arg(link.delayP99Ms, 0, 'f', 2).arg(link.delayGrowthMs, 0, 'f', 2)
                    .arg(link.senderMs, 0, 'f', 2).toStdString());
            }
            // The capacity probe spreads load evenly, but the stream sends each
            // frame back-to-back. Send the measured budget as frames at this
            // frame rate and find the fastest pace passing whole-frame and
            // tail loss checks as well as the aggregate loss limits.
            int paceLinkMbps = 0;
            for (int known : {hostLinkMbps, linkMbps}) {
                if (known > 0) paceLinkMbps = paceLinkMbps ? (std::min)(paceLinkMbps, known) : known;
            }
            paceLinkMbps = (std::min)(paceLinkMbps, 9999);
            linkSpeedKbps = paceLinkMbps * 1000;
            if (cancelled->load()) {
                throw std::runtime_error("Calibration stopped.");
            }
            if (NvHTTP::getXmlString(serverInfo, "PyroWaveUdpProbeBurstVersion") != "1") {
                throw std::runtime_error(tr("Update Vibeshine to support frame-shaped bandwidth tests before calibrating PyroWave.").toStdString());
            }
            else if (paceLinkMbps <= 0) {
                throw std::runtime_error(tr("Frame pacing cannot be calibrated because neither wired link speed is known.").toStdString());
            }
            else {
                // Probe frames as large as the selected video settings stream
                // at this target. A Moderate/Maximum budget can far exceed
                // them, and its 125% pace floor would then approach the link.
                int budgetKbps = link.requestedKbps;
                if (recommendedImageKbps > 0) {
                    const auto selected = static_cast<PyroWaveCalibration::Target>(target);
                    const int wireCap = PyroWaveCalibration::wireTarget(selected, link.requestedKbps, linkSpeedKbps,
                        PyroWaveCalibration::roundUp(pyrowave::bandwidth::total_kbps(recommendedImageKbps, fps, transport)));
                    const int image = PyroWaveCalibration::imageTarget(selected, recommendedImageKbps,
                        PyroWaveCalibration::imageCapacity(wireCap, fps, transport));
                    const int wire = PyroWaveCalibration::roundUp(pyrowave::bandwidth::total_kbps(image, fps, transport));
                    if (wire >= PyroWaveLink::minimumKbps) budgetKbps = (std::min)(budgetKbps, wire);
                }
                const int burstFps = std::clamp(fps, 10, 500);
                try {
                    pace = PyroWaveLink::searchPace(paceLinkMbps * 1000, budgetKbps / 4 * 5,
                        [&](int paceKbps) {
                        QMetaObject::invokeMethod(this, [this, paceKbps, budgetKbps] {
                            m_Message = tr("Testing frame pacing at %1 Mbps: sending %2 Mbps as whole frames and measuring packet loss…")
                                .arg(paceKbps / 1000).arg(budgetKbps / 1000);
                            emit changed();
                        }, Qt::QueuedConnection);
                        const auto result = http.probePyroWaveUdp(budgetKbps, packetSize, *cancelled, udpHandshake,
                                                                  burstFps, (std::max)(paceKbps, budgetKbps));
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PyroWave pace probe: %d kbps as %d FPS frames paced at %d kbps: sent %u/%u, received %u, "
                            "loss %.3f%%, worst 100ms %.3f%%, damaged frames %u/%u, "
                            "frames losing more than 10%% %.3f%%, missing tails %.3f%%, worst frame loss %.3f%%, "
                            "host duration %.2f ms, host send retries %u, "
                            "host send error %d: %s", budgetKbps, burstFps, paceKbps,
                            result.sent, result.expected, result.received, result.lossPercent,
                            result.worstWindowLossPercent, result.damagedFrames, result.frames,
                            result.excessiveLossFramePercent(), result.missingTailFramePercent(),
                            result.worstFrameLossPercent, result.senderMs,
                            result.hostSendRetries, result.hostLastSendError,
                            PyroWaveLink::paceQualified(result) ? "frame loss pass" : "frame loss fail");
                        return result;
                    }, [&] { return cancelled->load(); });
                    frameCapKbps = budgetKbps;
                }
                catch (const std::exception& e) {
                    if (cancelled->load()) throw;
                    throw std::runtime_error(tr("Frame pacing could not be calibrated (%1). Test bandwidth again before applying a format.")
                        .arg(QString::fromUtf8(e.what())).toStdString());
                }
                if (!cancelled->load() && !pace.paceKbps) {
                    throw std::runtime_error(tr("No usable frame pacing measurement completed. Test bandwidth again.").toStdString());
                }
                if (!cancelled->load() && pace.paceKbps) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PyroWave frame pace: %d kbps (%s, loss %.3f%%), link %d Mbps",
                                pace.paceKbps, pace.qualified ? "frame loss confirmed" : "best available; loss limits not met",
                                pace.probe.lossPercent, paceLinkMbps);
                    paceNote = (pace.qualified ?
                        tr("Frame pacing: %1 Mbps confirmed on this %2 Mbps path. Packet loss %3%; "
                           "frames losing more than 10%: %4%; missing frame tails: %5%; worst frame loss: %6%.") :
                        tr("Warning: no tested packet speed met all loss limits. Best available: %1 Mbps on this %2 Mbps path. "
                           "Packet loss %3%; frames losing more than 10%: %4%; missing frame tails: %5%; worst frame loss: %6%. Visible damage may remain."))
                            .arg(pace.paceKbps / 1000).arg(paceLinkMbps)
                            .arg(pace.probe.lossPercent, 0, 'f', 2)
                            .arg(pace.probe.excessiveLossFramePercent(), 0, 'f', 2)
                            .arg(pace.probe.missingTailFramePercent(), 0, 'f', 2)
                            .arg(pace.probe.worstFrameLossPercent, 0, 'f', 2);
                }
            }
        }
        catch (const std::exception& e) {
            const QString error = QString::fromUtf8(e.what());
            const bool stopped = cancelled->load();
            QMetaObject::invokeMethod(this, [this, error, stopped] {
                m_Message = stopped ? tr("Calibration stopped.") : error;
                m_LinkSummary = tr("Calibration could not be completed.");
                emit changed();
            }, Qt::QueuedConnection);
            return;
        }
        if (cancelled->load()) {
            QMetaObject::invokeMethod(this, [this] {
                m_Message = tr("Calibration stopped.");
                emit changed();
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this, hostName, link, pace, paceNote, linkSpeedKbps, frameCapKbps, transport, cancelled] {
            if (cancelled->load()) return;
            m_LinkCapKbps = (std::min)(link.requestedKbps, frameCapKbps);
            m_LinkSpeedKbps = linkSpeedKbps;
            m_PaceMbps = pace.paceKbps / 1000;
            m_Transport = transport;
            m_BandwidthReady = true;
            m_LinkSummary = tr("%1 → this PC: measured capacity %2 Mbps; frame-tested budget %7 Mbps including FEC and headers, with 5% headroom where available. Capacity packet loss %3%; worst 100 ms %4%; delivery variation p99 %5 ms. Critical FEC: %6%. Rates below include overhead; quality uses the remaining image bitrate.")
                .arg(hostName).arg(link.requestedKbps / 1000).arg(link.lossPercent, 0, 'f', 2)
                .arg(link.worstWindowLossPercent, 0, 'f', 2).arg(link.delayP99Ms, 0, 'f', 1)
                .arg(transport.critical_fec_percentage).arg(m_LinkCapKbps / 1000);
            if (!paceNote.isEmpty()) m_LinkSummary += QStringLiteral(" ") + paceNote;
            const auto grade = bandwidthQuality();
            if (!pace.qualified) {
                m_Message = tr("No tested packet speed met all loss limits. The best available speed is selected; visible damage may remain. Choose Next to test the decoder.");
            }
            else if (grade == QStringLiteral("target")) {
                m_Message = tr("Selected quality target met for %1. Choose Next to test the decoder.").arg(m_VideoDescription);
            }
            else if (grade == QStringLiteral("reduced")) {
                m_Message = tr("Reduced quality for %1: bandwidth is below the selected target. Choose Next to test the decoder.").arg(m_VideoDescription);
            }
            else {
                m_Message = tr("Bandwidth is below Minimum for %1. Choose Next to check lower resolutions or formats.").arg(m_VideoDescription);
            }
            emit changed();
        }, Qt::QueuedConnection);
    });
    watchWorker();
    m_Worker->start();
#endif
}

void PyroWaveCalibrator::reset()
{
    cancel();
    m_BandwidthReady = false;
    m_LinkCapKbps = 0;
    m_LinkSpeedKbps = 0;
    m_PaceMbps = 0;
    m_Results.clear();
    m_Message.clear();
    m_LinkSummary.clear();
    emit changed();
}

void PyroWaveCalibrator::watchWorker()
{
    QThread* worker = m_Worker;
    const auto cancelled = m_Cancel;
    connect(worker, &QThread::finished, this, [this, worker, cancelled] {
        if (m_Worker == worker) {
            m_Worker = nullptr;
            m_Running = false;
            if (cancelled->load()) {
                m_BandwidthReady = false;
                m_Message = tr("Test stopped. Test bandwidth again to continue.");
            }
            emit changed();
        }
        worker->deleteLater();
    });
}

void PyroWaveCalibrator::startDecoderTest(const QString& hostUuid, int bitrateTarget)
{
    if (m_Running || m_Worker) return;
    if (!m_BandwidthReady || hostUuid != m_HostUuid || bitrateTarget != m_Target) {
        m_Message = tr("Test bandwidth for this host and target before continuing.");
        emit changed();
        return;
    }
    if (Session::get() != nullptr) {
        m_Message = tr("Finish the current stream before testing the decoder.");
        emit changed();
        return;
    }
#if defined(HAVE_PYROWAVE) && (defined(Q_OS_LINUX) || defined(Q_OS_WIN32) || defined(Q_OS_DARWIN))
    m_Running = true;
    m_Results.clear();
    m_Message = tr("Stress testing decoder and rendering at %1 FPS…").arg(m_Fps);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_Cancel = cancelled;
    const int fps = m_Fps, width = m_DisplayWidth, height = m_DisplayHeight, cap = m_LinkCapKbps,
              linkSpeed = m_LinkSpeedKbps;
    const auto target = static_cast<PyroWaveCalibration::Target>(m_Target);
    const auto transport = m_Transport;
    emit changed();
    m_Worker = QThread::create([this, fps, width, height, cap, linkSpeed, target, transport, cancelled] {
        const QString error = runSweep(fps, width, height, cap, linkSpeed, target, transport, *cancelled,
                                      [this, cancelled](const Sample& sample) {
            const QVariantMap result = toMap(sample);
            QMetaObject::invokeMethod(this, [this, result, cancelled] {
                if (cancelled->load()) return;
                m_Results.append(result);
                emit changed();
            }, Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(this, [this, error, cancelled] {
            if (cancelled->load()) return;
            m_Message = error.isEmpty() ? tr("Stress test complete. Select a format to apply it.") : error;
            emit changed();
        }, Qt::QueuedConnection);
    });
    watchWorker();
    m_Worker->start();
#endif
}
