#include "pyrowavecalibrator.h"

#include "backend/networkbuffers.h"
#include "backend/computermanager.h"
#include "backend/nvhttp.h"
#include "streaming/session.h"
#include "streaming/video/pyrowave/pyrowavebitrate.h"

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

#if defined(HAVE_PYROWAVE) && (defined(Q_OS_LINUX) || defined(Q_OS_WIN32))
#include "streaming/video/pyrowave/pyrowavedecoder.h"
#include "streaming/video/pyrowave/pyrowaveframing.h"
#include <vulkan/vulkan.h>
#ifdef Q_OS_LINUX
#include "streaming/video/pyrowave/pyrowaveplacebo.h"
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
// Record padding, FEC parity, RTP/UDP/IP headers, audio and input ride on top
// of the codec's byte budget, so the bitrate may use this share of the link.
constexpr double kLinkShare = 0.8;
// Bisection steps between the floor and the highest bitrate tried.
constexpr int kBitrateSearchSteps = 1;
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
// A lower bitrate is only searched when it cuts the GPU time per frame by at
// least this share; otherwise passing at it would be run-to-run noise at the
// edge of the frame period, bought with picture quality.
constexpr double kMinimumBitrateSaving = 0.1;

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
    int bitrateKbps = 0;
    double qualityDb = 0;
    // At bitrateKbps or, when the format can't keep up, the lowest bitrate
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

#if defined(HAVE_PYROWAVE) && (defined(Q_OS_LINUX) || defined(Q_OS_WIN32))

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
                 bool chroma444, bool hdr, const std::atomic<bool>& cancelled)
        : m_Renderer(renderer), m_Width(width), m_Height(height), m_Fps(fps),
          m_Chroma444(chroma444), m_Hdr(hdr), m_Cancelled(cancelled), m_Source(width, height, chroma444)
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
#ifdef Q_OS_LINUX
        config.vulkanPool = renderer.pool.get();
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
        const size_t shard = 1392 - 16;
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
    const std::atomic<bool>& m_Cancelled;
    Planes m_Source;
    pyrowave_encoder m_Encoder = nullptr;
    PyroWaveDecoder m_Decoder;
    std::vector<uint8_t> m_Framed;
    QString m_Error;
};

// The highest bitrate up to the author's recommendation (and the link cap)
// at which the kCostPercentile frame still fits the frame period. If the top bitrate
// misses, the floor is tried; if that keeps up, bisection on the quality scale
// finds the highest bitrate that does.
Sample calibrateFormat(pyrowave_device device, Renderer& renderer, int width, int height, int fps,
                       bool chroma444, bool hdr, int linkCapKbps, const std::atomic<bool>& cancelled)
{
    Sample sample;
    sample.width = width;
    sample.height = height;
    sample.chroma444 = chroma444;
    sample.hdr = hdr;
    sample.guideKbps = pyroWaveRecommendedKbps(width, height, fps, chroma444, hdr);
    int topKbps = sample.guideKbps;
    if (linkCapKbps > 0 && linkCapKbps < topKbps) {
        topKbps = linkCapKbps;
        sample.linkLimited = true;
    }
    const auto quality = [&](int kbps) {
        return pyroWaveQualityDb(width, height, fps, chroma444, hdr, kbps);
    };
    const double availableMs = 1000.0 / fps * kPeriodShare;
    const auto keepsUp = [&](const Probe& probe) {
        return probe.ok && !probe.overloaded && probe.frameMs <= availableMs;
    };

    FormatTester tester(device, renderer, width, height, fps, chroma444, hdr, cancelled);
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
    Probe best = measure(topKbps);
    if (!best.ok) {
        sample.error = best.error;
        return sample;
    }
    sample.valid = true;
    int bestKbps = topKbps;
    if (!keepsUp(best)) {
        const int floorKbps = roundDownKbps(pyroWaveKbpsForQuality(width, height, fps, chroma444, hdr,
                                                                   kFloorQualityDb));
        // Still selectable: a lower bitrate doesn't help, so it keeps the top one
        const auto cantKeepUp = [&](const Probe& probe) {
            sample.bitrateKbps = topKbps;
            sample.qualityDb = quality(topKbps);
            sample.frameMs = probe.frameMs;
            sample.load = probe.load;
            return sample;
        };
        if (floorKbps >= topKbps) {
            return cantKeepUp(best);
        }
        const Probe floorCost = tester.probe(floorKbps, true);
        if (!floorCost.ok || floorCost.meanMs > best.meanMs * (1.0 - kMinimumBitrateSaving)) {
            return cantKeepUp(best);
        }
        const Probe floor = floorCost.overloaded ? floorCost : measure(floorKbps);
        if (!keepsUp(floor)) {
            return cantKeepUp(floor.ok ? floor : best);
        }
        sample.deviceLimited = true;
        best = floor;
        bestKbps = floorKbps;
        double passingDb = kFloorQualityDb;
        double failingDb = quality(topKbps);
        for (int step = 0; step < kBitrateSearchSteps; ++step) {
            const double middleDb = (passingDb + failingDb) / 2;
            const int kbps = roundDownKbps(pyroWaveKbpsForQuality(width, height, fps, chroma444, hdr, middleDb));
            if (kbps <= bestKbps || kbps >= topKbps) break;
            const Probe middle = measure(kbps);
            if (keepsUp(middle)) {
                passingDb = middleDb;
                best = middle;
                bestKbps = kbps;
            }
            else {
                failingDb = middleDb;
            }
        }
    }
    sample.keepsUp = true;
    sample.share = best.frameMs / availableMs;
    sample.bitrateKbps = bestKbps;
    sample.qualityDb = quality(bestKbps);
    sample.frameMs = best.frameMs;
    sample.load = best.load;
    return sample;
}

QString runSweep(int fps, int displayWidth, int displayHeight, int linkCapKbps,
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
            const int resolutions[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080},
#ifdef Q_OS_LINUX
                                          {1280, 800},
#endif
                                          {1280, 720}};
            for (const auto& resolution : resolutions) {
                for (bool chroma444 : {true, false}) {
                    for (bool hdr : {true, false}) {
                        if (cancelled.load()) break;
                        const Sample sample = calibrateFormat(device, renderer, resolution[0], resolution[1],
                                                              fps, chroma444, hdr, linkCapKbps, cancelled);
                        // A format cut short by cancellation has no result
                        if (cancelled.load()) break;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "PyroWave calibration: %dx%d %s %d-bit %d FPS: %s, %d kbps (guide %d, link cap %d), "
                                    "%.1f dB, GPU p99 %.2f ms/frame (%.0f%% of the frame)%s%s",
                                    sample.width, sample.height, sample.chroma444 ? "4:4:4" : "4:2:0",
                                    sample.hdr ? 10 : 8, fps,
                                    !sample.valid ? "error" : !sample.keepsUp ? "falls behind" :
                                    sample.share > kVrrShare ? "needs a large VRR buffer" :
                                    sample.share > kAnyDisplayShare ? "needs VRR" : "keeps up on any display",
                                    sample.bitrateKbps, sample.guideKbps, linkCapKbps, sample.qualityDb,
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
            {QStringLiteral("bitrateKbps"), sample.bitrateKbps},
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

void PyroWaveCalibrator::start(ComputerManager* manager, const QString& hostUuid, int fps,
                               int displayWidth, int displayHeight)
{
    if (m_Running || (m_Worker && m_Worker->isRunning())) return;
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

    const int linkMbps = NetworkBuffers::routedWiredLinkMbps(QHostAddress(hostAddress.address()));
    m_LinkSummary = tr("Measuring bandwidth from %1 to this PC…").arg(hostName);

#if !defined(HAVE_PYROWAVE) || (!defined(Q_OS_LINUX) && !defined(Q_OS_WIN32))
    Q_UNUSED(displayWidth);
    Q_UNUSED(displayHeight);
    m_Results.clear();
    m_Message = tr("Local PyroWave calibration requires a supported GPU decoder.");
    emit changed();
#else
    m_Running = true;
    m_Results.clear();
    m_Message = tr("Testing host-to-client bandwidth…");
    emit changed();

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_Cancel = cancelled;
    m_Worker = QThread::create([this, fps, displayWidth, displayHeight, linkMbps,
                                hostAddress, hostHttpsPort, hostCertificate, hostName,
                                useTrueUid, cancelled] {
        int measuredMbps = 0;
        int hostLinkMbps = 0;
        try {
            NvHTTP http(hostAddress, hostHttpsPort, hostCertificate, useTrueUid);
            const QString serverInfo = http.getServerInfo(NvHTTP::NVLL_ERROR);
            bool validBytes = false;
            const auto probeBytes = NvHTTP::getXmlString(serverInfo, "PyroWaveBandwidthProbeBytes").toUInt(&validBytes);
            if (!validBytes || probeBytes != 32U * 1024U * 1024U) {
                throw std::runtime_error("This host does not support PyroWave bandwidth calibration");
            }
            hostLinkMbps = NvHTTP::getXmlString(serverInfo, "PyroWaveHostLinkMbps").toInt();
            // The first transfer warms TLS and the receiver. The slowest of
            // three subsequent transfers is a conservative bulk-throughput cap.
            http.probePyroWaveDownloadMbps();
            for (int i = 0; i < 3 && !cancelled->load(); ++i) {
                const int sampleMbps = http.probePyroWaveDownloadMbps();
                measuredMbps = measuredMbps == 0 ? sampleMbps : (std::min)(measuredMbps, sampleMbps);
            }
            if (!cancelled->load() && measuredMbps <= 0) {
                throw std::runtime_error("PyroWave bandwidth probe returned no usable speed");
            }
        }
        catch (const std::exception& e) {
            const QString error = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(this, [this, error] {
                m_Running = false;
                m_Message = error;
                m_LinkSummary = tr("Host-to-client bandwidth could not be measured.");
                emit changed();
            }, Qt::QueuedConnection);
            return;
        }
        if (cancelled->load()) {
            QMetaObject::invokeMethod(this, [this] {
                m_Running = false;
                m_Message = tr("Calibration stopped.");
                emit changed();
            }, Qt::QueuedConnection);
            return;
        }
        int availableMbps = measuredMbps;
        if (hostLinkMbps > 0) availableMbps = (std::min)(availableMbps, hostLinkMbps);
        if (linkMbps > 0) availableMbps = (std::min)(availableMbps, linkMbps);
        const int linkCapKbps = roundDownKbps(availableMbps * 1000.0 * kLinkShare);
        if (linkCapKbps < 5000) {
            QMetaObject::invokeMethod(this, [this] {
                m_Running = false;
                m_Message = tr("This route is too slow for the minimum PyroWave bitrate.");
                m_LinkSummary = tr("No usable PyroWave bitrate could be recommended.");
                emit changed();
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this, fps, hostName, measuredMbps, hostLinkMbps,
                                         linkMbps, linkCapKbps] {
            m_LinkSummary = tr("%1 → this PC: measured %2 Mbps; host link %3 Mbps; client link %4 Mbps. Video bitrate cap: %5 Mbps. 4K 4:4:4 at %6 FPS: SDR %7, HDR %8 by network capacity.")
                .arg(hostName).arg(measuredMbps)
                .arg(hostLinkMbps > 0 ? QString::number(hostLinkMbps) : tr("unknown"))
                .arg(linkMbps > 0 ? QString::number(linkMbps) : tr("unknown"))
                .arg(linkCapKbps / 1000).arg(fps)
                .arg(linkCapKbps >= pyroWaveRecommendedKbps(3840, 2160, fps, true, false) ? tr("supported") : tr("bandwidth-limited"))
                .arg(linkCapKbps >= pyroWaveRecommendedKbps(3840, 2160, fps, true, true) ? tr("supported") : tr("bandwidth-limited"));
            m_Message = tr("Timing each format on this device at %1 FPS…").arg(fps);
            emit changed();
        }, Qt::QueuedConnection);
        const QString error = runSweep(fps, displayWidth, displayHeight, linkCapKbps, *cancelled,
                                       [this](const Sample& sample) {
            const QVariantMap result = toMap(sample);
            QMetaObject::invokeMethod(this, [this, result] {
                m_Results.append(result);
                emit changed();
            }, Qt::QueuedConnection);
        });
        const bool stopped = cancelled->load();
        QMetaObject::invokeMethod(this, [this, error, stopped] {
            m_Running = false;
            if (!error.isEmpty()) {
                m_Message = error;
            }
            else if (stopped) {
                m_Message = tr("Calibration stopped.");
            }
            else {
                m_Message = tr("Done. Click a format to use it with the bitrate shown.");
            }
            emit changed();
        }, Qt::QueuedConnection);
    });
    QThread* worker = m_Worker;
    connect(worker, &QThread::finished, this, [this, worker] {
        if (m_Worker == worker) m_Worker = nullptr;
        worker->deleteLater();
    });
    m_Worker->start();
#endif
}
