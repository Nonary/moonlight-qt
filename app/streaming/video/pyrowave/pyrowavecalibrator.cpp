#include "pyrowavecalibrator.h"

#include "streaming/session.h"
#include "streaming/video/pyrowave/pyrowavebitrate.h"

#include <QMetaObject>
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
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

struct Sample {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    bool hdr = false;
    bool valid = false;
    int requiredKbps = 0;
    int bitrateKbps = 0;
    int wireKbps = 0;
    int frames = 0;
    double meanMs = 0;
    double p95Ms = 0;
    QString error;
};

struct Sweep {
    std::vector<Sample> samples;
    QString error;
};

// The codec author's good-quality bitrate, rounded up to 5 Mbps steps
int targetBitrateKbps(int width, int height, int fps, bool chroma444, bool hdr)
{
    return int(std::ceil(pyroWaveRecommendedKbps(width, height, fps, chroma444, hdr) / 5000.0)) * 5000;
}

#if defined(HAVE_PYROWAVE) && (defined(Q_OS_LINUX) || defined(Q_OS_WIN32))

#ifdef Q_OS_LINUX
// A headless libplacebo device, so the sweep decodes into the same shared
// surfaces a stream's Vulkan renderer uses.
struct Renderer {
    pl_log log = nullptr;
    pl_vk_inst instance = nullptr;
    pl_vulkan vulkan = nullptr;
    std::mutex commandLock;
    std::unique_ptr<PyroWavePlaceboPool> pool;

    bool create()
    {
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
        return true;
    }

    bool prepare(int, int, bool, bool) { return true; }

    // Returns once the GPU finished writing the frame's planes
    bool waitForFrame(const AVFrame* frame)
    {
        pl_frame mapped = {};
        if (!pool->mapFrame(frame, &mapped)) return false;
        uint16_t pixel = 0;
        pl_tex_transfer_params transfer = {};
        transfer.tex = mapped.planes[2].texture;
        transfer.rc = { 0, 0, 0, 1, 1, 1 };
        transfer.ptr = &pixel;
        return pl_tex_download(vulkan->gpu, &transfer);
    }

    ~Renderer()
    {
        pool.reset();
        pl_vulkan_destroy(&vulkan);
        pl_vk_inst_destroy(&instance);
        pl_log_destroy(&log);
    }
};
#else
// Use the same D3D11 texture and fence sharing as a Windows stream. A one-pixel
// D3D11 readback waits for decode completion and verifies renderer access.
struct Renderer {
    Microsoft::WRL::ComPtr<ID3D11Device5> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    LUID adapterLuid = {};
    std::unique_ptr<D3D11PyroWaveSurfaces> pool;
    HANDLE event = nullptr;

    bool create()
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

Sample runSample(pyrowave_device device, Renderer& renderer, int width, int height, int fps,
                 bool chroma444, bool hdr, int bitrateKbps)
{
    Sample sample;
    sample.width = width;
    sample.height = height;
    sample.chroma444 = chroma444;
    sample.hdr = hdr;
    sample.requiredKbps = targetBitrateKbps(width, height, fps, chroma444, hdr);
    sample.bitrateKbps = bitrateKbps;
    pyrowave_encoder_create_info info = {};
    info.device = device;
    info.width = width;
    info.height = height;
    info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_encoder encoder = nullptr;
    if (pyrowave_encoder_create(&info, &encoder) != PYROWAVE_SUCCESS) {
        sample.error = QStringLiteral("Could not initialize encoder");
        return sample;
    }

    std::vector<uint8_t> framed;
    do {
        Planes source(width, height, chroma444);
        auto input = source.buffer();
        pyrowave_rate_control rate = { size_t(double(sample.bitrateKbps) * 1000.0 / fps / 8.0) };
        if (pyrowave_encoder_encode_cpu_synchronous(encoder, &input, &rate) != PYROWAVE_SUCCESS) {
            sample.error = QStringLiteral("Could not encode test image");
            break;
        }
        size_t packetCount = 0;
        const size_t shard = 1392 - 16;
        if (pyrowave_encoder_compute_num_packets_with_padding(encoder, shard, 8, &packetCount) != PYROWAVE_SUCCESS) {
            sample.error = QStringLiteral("Could not packetize test image");
            break;
        }
        std::vector<pyrowave_packet> packets(packetCount);
        std::vector<uint8_t> bitstream(rate.maximum_bitstream_size + 1024 * 1024);
        size_t written = 0;
        if (pyrowave_encoder_packetize_with_padding(encoder, packets.data(), shard, 8, &written,
                                      bitstream.data(), bitstream.size()) != PYROWAVE_SUCCESS) {
            sample.error = QStringLiteral("Could not packetize test image");
            break;
        }
        packets.resize(written);
        framed = frameRecords(bitstream, packets, shard,
                              PyroWaveFraming::coarseBlockCount({width, height, chroma444}));
    } while (false);
    pyrowave_encoder_destroy(encoder);
    if (framed.empty()) return sample;

    sample.wireKbps = int(double(framed.size()) * 8.0 * fps / 1000.0);
    // This synthetic source usually compresses far below the encoder's rate
    // ceiling. Neither number measures what a host or LAN can sustain.
    PyroWaveDecoder decoder;
    PyroWaveDecoder::Config config;
    config.width = width;
    config.height = height;
    config.chroma444 = chroma444;
    config.tenBit = hdr;
    if (!renderer.prepare(width, height, chroma444, hdr)) {
        sample.error = QStringLiteral("Could not create renderer surfaces");
        return sample;
    }
#ifdef Q_OS_LINUX
    config.vulkanPool = renderer.pool.get();
    const bool initialized = decoder.initialize(config, nullptr);
#else
    const bool initialized = decoder.initialize(config, renderer.pool.get());
#endif
    if (!initialized) {
        sample.error = QStringLiteral("Could not initialize decoder");
        return sample;
    }

    auto decodeOne = [&]() {
        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;
        const bool ok = decoder.decode(framed.data(), framed.size(), {}, 0, frame) &&
                        renderer.waitForFrame(frame);
        av_frame_free(&frame);
        return ok;
    };
    for (int i = 0; i < 5; ++i) {
        if (!decodeOne()) {
            sample.error = QString::fromStdString(decoder.lastError());
            return sample;
        }
    }

    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration<double, std::milli>(1000.0 / fps);
    const auto origin = Clock::now() + std::chrono::milliseconds(5);
    std::vector<double> service;
    for (int i = 0; i < 24; ++i) {
        const auto arrival = origin + std::chrono::duration_cast<Clock::duration>(period * i);
        std::this_thread::sleep_until(arrival);
        const auto start = Clock::now();
        if (!decodeOne()) {
            sample.error = QString::fromStdString(decoder.lastError());
            return sample;
        }
        const double serviceMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        service.push_back(serviceMs);
        sample.frames++;
    }
    sample.meanMs = std::accumulate(service.begin(), service.end(), 0.0) / service.size();
    std::sort(service.begin(), service.end());
    sample.p95Ms = service[size_t(std::ceil(service.size() * .95) - 1)];
    sample.valid = true;
    return sample;
}

Sweep runSweep(int fps)
{
    Sweep result;
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) {
        result.error = QStringLiteral("No usable Vulkan device");
        return result;
    }
    Renderer renderer;
    if (!renderer.create()) {
        pyrowave_device_destroy(device);
        result.error = QStringLiteral("Could not create a GPU renderer for PyroWave calibration");
        return result;
    }
    const int resolutions[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080},
#ifdef Q_OS_LINUX
                                  {1280, 800},
#endif
                                  {1280, 720}};
    for (const auto& resolution : resolutions) {
        for (bool chroma444 : {true, false}) {
            for (bool hdr : {true, false}) {
                Sample sample = runSample(device, renderer, resolution[0], resolution[1], fps,
                                          chroma444, hdr,
                                          targetBitrateKbps(resolution[0], resolution[1], fps,
                                                            chroma444, hdr));
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PyroWave decode test: %dx%d %s %d-bit %d FPS, payload %d kbps, guide %d kbps, "
                            "p95 %.2f ms, valid %d%s%s",
                            sample.width, sample.height, sample.chroma444 ? "4:4:4" : "4:2:0",
                            sample.hdr ? 10 : 8, fps, sample.wireKbps, sample.requiredKbps,
                            sample.p95Ms, sample.valid,
                            sample.error.isEmpty() ? "" : ", error: ",
                            sample.error.isEmpty() ? "" : sample.error.toUtf8().constData());
                result.samples.push_back(std::move(sample));
            }
        }
    }
    pyrowave_device_destroy(device);
    return result;
}

#endif

QVariantMap toMap(const Sample& sample)
{
    return {{QStringLiteral("width"), sample.width},
            {QStringLiteral("height"), sample.height},
            {QStringLiteral("chroma444"), sample.chroma444},
            {QStringLiteral("hdr"), sample.hdr},
            {QStringLiteral("valid"), sample.valid},
            {QStringLiteral("requiredKbps"), sample.requiredKbps},
            {QStringLiteral("bitrateKbps"), sample.bitrateKbps},
            {QStringLiteral("wireKbps"), sample.wireKbps},
            {QStringLiteral("frames"), sample.frames},
            {QStringLiteral("meanMs"), sample.meanMs},
            {QStringLiteral("p95Ms"), sample.p95Ms},
            {QStringLiteral("error"), sample.error}};
}

} // namespace

PyroWaveCalibrator::PyroWaveCalibrator(QObject* parent) : QObject(parent) {}

PyroWaveCalibrator::~PyroWaveCalibrator()
{
    if (m_Worker) {
        m_Worker->wait();
        delete m_Worker;
    }
}

void PyroWaveCalibrator::start(int fps)
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
#if !defined(HAVE_PYROWAVE) || (!defined(Q_OS_LINUX) && !defined(Q_OS_WIN32))
    m_Results.clear();
    m_Message = tr("Local PyroWave calibration requires a supported GPU decoder.");
    emit changed();
#else
    m_Running = true;
    m_Results.clear();
    m_Message = tr("Testing resolutions and PyroWave formats on this device…");
    emit changed();

    m_Worker = QThread::create([this, fps] {
        const Sweep sweep = runSweep(fps);
        QMetaObject::invokeMethod(this, [this, sweep] {
            m_Running = false;
            for (const auto& sample : sweep.samples) m_Results.append(toMap(sample));
            if (!sweep.error.isEmpty()) {
                m_Message = sweep.error;
            }
            else {
                m_Message = tr("Local decode test complete. Results do not certify a streaming profile or change settings.");
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
