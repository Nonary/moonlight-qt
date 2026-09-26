#include "pyrowavecalibrator.h"

#include "streaming/session.h"
#include "streaming/video/pyrowave/pyrowavebitrate.h"

#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

#if defined(HAVE_PYROWAVE) && defined(Q_OS_LINUX)
#include "streaming/video/pyrowave/pyrowavedecoder.h"
#include "streaming/video/pyrowave/pyrowaveplacebo.h"

#include <vulkan/vulkan.h>
#include <pyrowave.h>
#endif

namespace {

struct Sample {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    bool hdr = false;
    bool valid = false;
    bool sustainable = false;
    bool linkFit = false;
    bool qualityFit = false;
    int requiredKbps = 0;
    int bitrateKbps = 0;
    int wireKbps = 0;
    int frames = 0;
    double meanMs = 0;
    double p95Ms = 0;
    double queueMaxMs = 0;
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

#if defined(HAVE_PYROWAVE) && defined(Q_OS_LINUX)

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
    // This flag is only a UI warning. A short synthetic decode test cannot
    // decide whether a lower bitrate looks good for the user's game.
    sample.qualityFit = bitrateKbps >= pyroWaveRecommendedKbps(width, height, fps, chroma444, hdr);

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
    // The user asked for decoder calibration with a nominal 1 Gbps link.
    // Bound the selected bitrate, but do not reject based on a synthetic
    // frame's encoded size or an assumed host/FEC overhead percentage.
    sample.linkFit = sample.bitrateKbps <= 900000;
    PyroWaveDecoder decoder;
    PyroWaveDecoder::Config config;
    config.width = width;
    config.height = height;
    config.chroma444 = chroma444;
    config.tenBit = hdr;
    config.vulkanPool = renderer.pool.get();
    if (!decoder.initialize(config, nullptr)) {
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
        sample.queueMaxMs = std::max(sample.queueMaxMs,
                                     std::chrono::duration<double, std::milli>(start - arrival).count());
        if (!decodeOne()) {
            sample.error = QString::fromStdString(decoder.lastError());
            return sample;
        }
        service.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        sample.frames++;
        // Obvious overload needs no longer run. This also bounds the UI wait.
        if (i >= 7 && sample.queueMaxMs > 3.0 * period.count()) break;
    }
    sample.meanMs = std::accumulate(service.begin(), service.end(), 0.0) / service.size();
    std::sort(service.begin(), service.end());
    sample.p95Ms = service[size_t(std::ceil(service.size() * .95) - 1)];
    sample.valid = true;
    // The stream's renderer shares this GPU and frames arrive in bursts, so
    // decode must leave a quarter of each frame period to spare.
    sample.sustainable = sample.p95Ms <= period.count() * 0.75 &&
                         sample.queueMaxMs < period.count() / 2.0;
    return sample;
}

Sample calibrateSample(pyrowave_device device, Renderer& renderer, int width, int height, int fps,
                       bool chroma444, bool hdr)
{
    const int startingKbps = targetBitrateKbps(width, height, fps, chroma444, hdr);
    Sample first = runSample(device, renderer, width, height, fps, chroma444, hdr,
                             std::min(startingKbps, 900000));
    if (!first.valid || (first.sustainable && first.linkFit)) return first;

    const double periodMs = 1000.0 / fps;
    if (first.p95Ms > 2.0 * periodMs) return first;

    // One lower-rate probe is enough to find a useful fallback without
    // turning calibration into a long bitrate search.
    const int lowerKbps = (first.bitrateKbps * 3 / 4) / 5000 * 5000;
    if (lowerKbps >= first.bitrateKbps) return first;
    return runSample(device, renderer, width, height, fps, chroma444, hdr, lowerKbps);
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
        result.error = QStringLiteral("The Vulkan device cannot decode into renderer surfaces");
        return result;
    }
    const int resolutions[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080},
                                  {1280, 800}, {1280, 720}};
    for (const auto& resolution : resolutions) {
        for (bool chroma444 : {true, false}) {
            for (bool hdr : {true, false}) {
                result.samples.push_back(calibrateSample(device, renderer, resolution[0], resolution[1], fps,
                                                         chroma444, hdr));
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
            {QStringLiteral("sustainable"), sample.sustainable},
            {QStringLiteral("linkFit"), sample.linkFit},
            {QStringLiteral("qualityFit"), sample.qualityFit},
            {QStringLiteral("requiredKbps"), sample.requiredKbps},
            {QStringLiteral("bitrateKbps"), sample.bitrateKbps},
            {QStringLiteral("wireKbps"), sample.wireKbps},
            {QStringLiteral("frames"), sample.frames},
            {QStringLiteral("meanMs"), sample.meanMs},
            {QStringLiteral("p95Ms"), sample.p95Ms},
            {QStringLiteral("queueMaxMs"), sample.queueMaxMs},
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
#if !defined(HAVE_PYROWAVE) || !defined(Q_OS_LINUX)
    m_Results.clear();
    m_Message = tr("Local PyroWave calibration is available on Linux with Vulkan decoding.");
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
                m_Message = tr("Dimmed options missed the short decode test. You can still select one and check its live frame rate.");
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
