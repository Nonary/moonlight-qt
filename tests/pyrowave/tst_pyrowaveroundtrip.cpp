// Encodes synthetic frames with the vendored PyroWave encoder, frames them the
// way hosts do (record framing aligned to RTP payloads, and the length-prefixed
// compatibility framing), and decodes them through the client's framing parser.
// Needs a Vulkan GPU; exits 0 with a notice when none is present.

#include "../../app/streaming/video/pyrowave/pyrowaveframing.h"

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_Failures = 0;

void expect(bool condition, const std::string& description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
        ++g_Failures;
    }
}

struct Planes {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    std::vector<uint8_t> y, cb, cr;

    int chromaWidth() const { return chroma444 ? width : width / 2; }
    int chromaHeight() const { return chroma444 ? height : height / 2; }

    void allocate(int w, int h, bool c444)
    {
        width = w;
        height = h;
        chroma444 = c444;
        y.assign(size_t(w) * h, 0);
        cb.assign(size_t(chromaWidth()) * chromaHeight(), 0);
        cr.assign(size_t(chromaWidth()) * chromaHeight(), 0);
    }

    pyrowave_cpu_buffer buffer()
    {
        pyrowave_cpu_buffer buf = {};
        buf.data[0] = y.data();
        buf.data[1] = cb.data();
        buf.data[2] = cr.data();
        buf.row_stride_in_bytes[0] = size_t(width);
        buf.row_stride_in_bytes[1] = size_t(chromaWidth());
        buf.row_stride_in_bytes[2] = size_t(chromaWidth());
        buf.plane_size_in_bytes[0] = y.size();
        buf.plane_size_in_bytes[1] = cb.size();
        buf.plane_size_in_bytes[2] = cr.size();
        buf.width = width;
        buf.height = height;
        buf.format = chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        return buf;
    }
};

// Smooth gradients plus a few hard edges, varied per frame
void fillTestImage(Planes& planes, int frameIndex)
{
    for (int yy = 0; yy < planes.height; yy++) {
        for (int xx = 0; xx < planes.width; xx++) {
            int value = (xx * 3 + yy * 2 + frameIndex * 17) & 0xFF;
            if (((xx / 64) + (yy / 64) + frameIndex) % 7 == 0) {
                value = 235;
            }
            planes.y[size_t(yy) * planes.width + xx] = uint8_t(std::max(16, std::min(235, value)));
        }
    }
    for (int yy = 0; yy < planes.chromaHeight(); yy++) {
        for (int xx = 0; xx < planes.chromaWidth(); xx++) {
            const size_t index = size_t(yy) * planes.chromaWidth() + xx;
            planes.cb[index] = uint8_t(96 + ((xx + frameIndex * 5) % 64));
            planes.cr[index] = uint8_t(96 + ((yy * 2 + frameIndex * 3) % 64));
        }
    }
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double sum = 0;
    for (size_t i = 0; i < a.size(); i++) {
        const double diff = double(a[i]) - double(b[i]);
        sum += diff * diff;
    }
    const double mse = sum / double(a.size());
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void putU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        out.push_back(uint8_t(value >> (8 * i)));
    }
}

void putPadding(std::vector<uint8_t>& out, size_t bytes)
{
    putU32(out, 0xFFFFFFFFu);
    putU32(out, uint32_t((bytes - 8) / 4));
    out.resize(out.size() + (bytes - 8), 0);
}

// Record framing with the host's layout guarantees but strict record order
// otherwise: the sequence header, the coarsest level, then the rest. In each
// group, records too large to share a payload with a padding record come first
// (a payload less 4 bytes could never be placed alone), then the others padded
// so that none crosses a payload boundary and no 4-byte remainder (too small for
// a padding record) is left. criticalBytes receives the end of the coarsest
// level, whose payloads the host protects with parity. The host packs more
// tightly; the parser must accept both.
std::vector<uint8_t> recordFrame(const std::vector<uint8_t>& bitstream,
                                 const std::vector<pyrowave_packet>& packets,
                                 size_t shard, uint32_t coarseBlocks, size_t& criticalBytes)
{
    // Split the encoder's packets into the sequence header and block records
    struct Record {
        size_t offset;
        size_t size;
        bool coarse;
    };
    std::vector<Record> records;
    for (const auto& packet : packets) {
        for (size_t pos = packet.offset; pos < packet.offset + packet.size;) {
            uint32_t word0, word1;
            std::memcpy(&word0, bitstream.data() + pos, 4);
            std::memcpy(&word1, bitstream.data() + pos + 4, 4);
            const bool header = (word0 & 0x80000000u) != 0;
            const size_t size = header ? 8 : size_t((word0 >> 16) & 0xFFF) * 4;
            records.push_back({pos, size, header || (word1 >> 8) < coarseBlocks});
            pos += size;
        }
    }

    std::vector<uint8_t> out;
    // Bytes left in the current payload; the first also carries the frame header
    auto remaining = [&]() { return shard - (out.size() + 8) % shard; };
    auto place = [&](const Record& record) {
        out.insert(out.end(), bitstream.begin() + record.offset,
                   bitstream.begin() + record.offset + record.size);
    };

    place(records.front());
    for (bool coarse : {true, false}) {
        for (size_t i = 1; i < records.size(); i++) {
            if (records[i].coarse == coarse && records[i].size + 8 > shard) {
                if (records[i].size % shard == remaining() - 4) {
                    putPadding(out, 8);
                }
                place(records[i]);
            }
        }
        for (size_t i = 1; i < records.size(); i++) {
            if (records[i].coarse == coarse && records[i].size + 8 <= shard) {
                if (records[i].size > remaining() || remaining() - records[i].size == 4) {
                    putPadding(out, remaining());
                }
                place(records[i]);
            }
        }
        if (coarse) {
            // Padding is only ever placed before a record, so this is the coarse end
            criticalBytes = out.size();
        }
    }
    return out;
}

std::vector<uint8_t> lengthPrefixedFrame(const std::vector<uint8_t>& bitstream,
                                         const std::vector<pyrowave_packet>& packets)
{
    std::vector<uint8_t> out;
    putU32(out, uint32_t(packets.size()));
    for (const auto& packet : packets) {
        putU32(out, uint32_t(packet.size));
        out.insert(out.end(), bitstream.begin() + packet.offset,
                   bitstream.begin() + packet.offset + packet.size);
    }
    return out;
}

bool decodeFramed(pyrowave_decoder decoder, const std::vector<uint8_t>& framed,
                  const PyroWaveFraming::StreamGeometry& geometry,
                  PyroWaveFraming::Framing expectedFraming, Planes& output, const std::string& name)
{
    PyroWaveFraming::Frame frame;
    std::string error;
    if (!PyroWaveFraming::parse(framed.data(), framed.size(), geometry, frame, error)) {
        expect(false, name + ": parse failed: " + error);
        return false;
    }
    expect(frame.framing == expectedFraming, name + ": framing detected");

    pyrowave_decoder_clear(decoder);
    for (const auto& span : frame.spans) {
        if (pyrowave_decoder_push_packet(decoder, framed.data() + span.offset, span.size) != PYROWAVE_SUCCESS) {
            expect(false, name + ": push_packet rejected a span");
            return false;
        }
    }
    if (!pyrowave_decoder_decode_is_ready(decoder, false)) {
        expect(false, name + ": frame not complete after pushing every span");
        return false;
    }

    auto buffer = output.buffer();
    if (pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) != PYROWAVE_SUCCESS) {
        expect(false, name + ": decode failed");
        return false;
    }
    return true;
}

// Loses one RTP payload of a record-framed frame the way moonlight-common-c
// delivers it (zero-filled, marked lost), with the payloads that start with a
// record flagged and the critical payloads announced as vibeshine does, and
// decodes what the parser salvages. Returns whether the decoder accepted the
// partial frame; its luma PSNR goes to quality.
bool decodeWithLostPayload(pyrowave_decoder decoder, std::vector<uint8_t> framed, size_t shard,
                           size_t lostPayload, size_t criticalPayloads,
                           const PyroWaveFraming::StreamGeometry& geometry,
                           const Planes& source, Planes& output, double& quality, const std::string& name)
{
    std::vector<bool> recordStart(framed.size(), false);
    for (size_t pos = 0; pos + 8 <= framed.size();) {
        recordStart[pos] = true;
        uint32_t word0, word1;
        std::memcpy(&word0, framed.data() + pos, 4);
        std::memcpy(&word1, framed.data() + pos + 4, 4);
        pos += word0 == 0xFFFFFFFFu ? 8 + size_t(word1) * 4 :
               (word0 & 0x80000000u) ? 8 : size_t((word0 >> 16) & 0xFFF) * 4;
    }

    std::vector<PyroWaveFraming::Segment> segments;
    for (size_t offset = 0, index = 0; offset < framed.size(); index++) {
        const size_t size = std::min(framed.size() - offset, index == 0 ? shard - 8 : shard);
        segments.push_back({offset, size, index == lostPayload, bool(recordStart[offset])});
        if (index == lostPayload) {
            std::fill(framed.begin() + offset, framed.begin() + offset + size, uint8_t(0));
        }
        offset += size;
    }

    PyroWaveFraming::Frame frame;
    std::string error;
    if (!PyroWaveFraming::parse(framed.data(), framed.size(), segments, criticalPayloads, geometry, frame, error)) {
        expect(false, name + ": parse failed: " + error);
        return false;
    }
    expect(frame.partial, name + ": the frame is partial");
    expect(frame.blockRecords < frame.announcedBlocks, name + ": records were lost");

    pyrowave_decoder_clear(decoder);
    for (const auto& span : frame.spans) {
        if (pyrowave_decoder_push_packet(decoder, framed.data() + span.offset, span.size) != PYROWAVE_SUCCESS) {
            expect(false, name + ": push_packet rejected a span");
            return false;
        }
    }
    // As PyroWaveDecoder does: the parser vouches for the coarsest level
    if (!frame.coarseLevelIntact ||
            !pyrowave_decoder_decode_is_ready_with_sideband(decoder, true, 0, 0.9f, nullptr, 0)) {
        return false;
    }

    auto buffer = output.buffer();
    if (pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) != PYROWAVE_SUCCESS) {
        expect(false, name + ": decode failed");
        return false;
    }
    quality = psnr(source.y, output.y);
    return true;
}

void runCase(pyrowave_device device, int width, int height, bool chroma444, size_t budget)
{
    const std::string name = std::to_string(width) + "x" + std::to_string(height) +
                             (chroma444 ? " 4:4:4" : " 4:2:0");
    const PyroWaveFraming::StreamGeometry geometry {width, height, chroma444};

    pyrowave_encoder_create_info encoderInfo = {};
    encoderInfo.device = device;
    encoderInfo.width = width;
    encoderInfo.height = height;
    encoderInfo.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_encoder encoder = nullptr;
    if (pyrowave_encoder_create(&encoderInfo, &encoder) != PYROWAVE_SUCCESS) {
        expect(false, name + ": encoder creation");
        return;
    }

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = device;
    decoderInfo.width = width;
    decoderInfo.height = height;
    decoderInfo.chroma = encoderInfo.chroma;
    pyrowave_decoder decoder = nullptr;
    if (pyrowave_decoder_create(&decoderInfo, &decoder) != PYROWAVE_SUCCESS) {
        expect(false, name + ": decoder creation");
        pyrowave_encoder_destroy(encoder);
        return;
    }

    Planes source, decoded;
    source.allocate(width, height, chroma444);
    decoded.allocate(width, height, chroma444);

    const size_t shard = 1392 - 16;
    size_t recordBytes = 0, paddingBytes = 0;

    // Frames 1-4 are encoded but never decoded: the decoder must still accept
    // frame 5 even though its 3-bit sequence counter skipped ahead.
    for (int frameIndex = 0; frameIndex < 6; frameIndex++) {
        fillTestImage(source, frameIndex);
        auto input = source.buffer();
        pyrowave_rate_control rate = { budget };
        if (pyrowave_encoder_encode_cpu_synchronous(encoder, &input, &rate) != PYROWAVE_SUCCESS) {
            expect(false, name + ": encode");
            break;
        }
        if (frameIndex != 0 && frameIndex != 5) {
            continue;
        }

        size_t packetCount = 0;
        pyrowave_encoder_compute_num_packets_with_padding(encoder, shard, 8, &packetCount);
        std::vector<pyrowave_packet> packets(packetCount);
        std::vector<uint8_t> bitstream(budget + 1024 * 1024);
        size_t written = 0;
        if (pyrowave_encoder_packetize_with_padding(encoder, packets.data(), shard, 8, &written,
                                                    bitstream.data(), bitstream.size()) != PYROWAVE_SUCCESS) {
            expect(false, name + ": packetize");
            break;
        }
        packets.resize(written);

        size_t criticalBytes = 0;
        const auto records = recordFrame(bitstream, packets, shard,
                                         PyroWaveFraming::coarseBlockCount(geometry), criticalBytes);
        recordBytes += records.size();
        if (decodeFramed(decoder, records, geometry, PyroWaveFraming::Framing::Records, decoded, name + " records")) {
            const double quality = psnr(source.y, decoded.y);
            expect(quality > 30.0, name + ": record-framed luma PSNR " + std::to_string(quality));
            PyroWaveFraming::Frame frame;
            std::string error;
            PyroWaveFraming::parse(records.data(), records.size(), geometry, frame, error);
            paddingBytes += frame.paddingBytes;

            // Lose one payload at a time across the frame. The payloads holding the
            // coarsest level carry parity (moonlight-common-c recovers them), so only
            // the others can reach the parser with a hole; each only blurs its area.
            const size_t payloads = (records.size() + 8 + shard - 1) / shard;
            const size_t criticalPayloads = (criticalBytes + 8 + shard - 1) / shard;
            int salvaged = 0, tried = 0;
            double worstQuality = 99;
            for (size_t lost = criticalPayloads; lost < payloads;
                 lost += std::max<size_t>(1, (payloads - criticalPayloads) / 24)) {
                const std::string lossName = name + " lost payload " + std::to_string(lost) +
                                             " of " + std::to_string(payloads);
                double lossyQuality = 0;
                tried++;
                if (decodeWithLostPayload(decoder, records, shard, lost, criticalPayloads, geometry, source,
                                          decoded, lossyQuality, lossName)) {
                    salvaged++;
                    worstQuality = std::min(worstQuality, lossyQuality);
                    // Losing the next-coarsest level right after the critical payloads
                    // blurs a large area for the frame (about 22 dB at 4:4:4 here)
                    expect(lossyQuality > 20.0 && lossyQuality <= quality + 0.01,
                           lossName + ": luma PSNR " + std::to_string(lossyQuality) +
                           " against " + std::to_string(quality) + " whole");
                }
            }
            std::printf("%s frame %d: %zu of %zu payloads critical (%.1f%% of the frame); %d of %d "
                        "single-payload losses elsewhere decoded, luma PSNR %.1f dB whole, %.1f dB worst salvaged\n",
                        name.c_str(), frameIndex, criticalPayloads, payloads,
                        100.0 * double(criticalBytes) / double(records.size()), salvaged, tried, quality, worstQuality);
            expect(salvaged == tried, name + ": every loss outside the critical payloads decodes");
        }

        // Re-packetize with the compatibility boundary for length-prefixed framing
        pyrowave_encoder_compute_num_packets(encoder, 1024, &packetCount);
        packets.resize(packetCount);
        pyrowave_encoder_packetize(encoder, packets.data(), 1024, &written, bitstream.data(), bitstream.size());
        packets.resize(written);
        const auto prefixed = lengthPrefixedFrame(bitstream, packets);
        if (decodeFramed(decoder, prefixed, geometry, PyroWaveFraming::Framing::LengthPrefixed, decoded,
                         name + " length-prefixed")) {
            const double quality = psnr(source.y, decoded.y);
            expect(quality > 30.0, name + ": length-prefixed luma PSNR " + std::to_string(quality));
        }
    }

    std::printf("%s: record framing %zu bytes, %.1f%% padding (strict order)\n",
                name.c_str(), recordBytes, recordBytes ? 100.0 * double(paddingBytes) / double(recordBytes) : 0.0);

    pyrowave_decoder_destroy(decoder);
    pyrowave_encoder_destroy(encoder);
}

}

int main()
{
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) {
        std::printf("No Vulkan device for PyroWave; skipping the round trip\n");
        return 0;
    }

    // Budgets around 1.6 bits per pixel
    runCase(device, 1280, 720, false, 180 * 1024);
    runCase(device, 1920, 1080, false, 400 * 1024);
    runCase(device, 1920, 1080, true, 650 * 1024);

    pyrowave_device_destroy(device);

    if (g_Failures == 0) {
        std::printf("PyroWave round trip: all checks passed\n");
    }
    return g_Failures ? 1 : 0;
}
