#include "../../app/streaming/video/pyrowave/pyrowaveframing.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

using PyroWaveFraming::Frame;
using PyroWaveFraming::Framing;
using PyroWaveFraming::StreamGeometry;

int g_Failures = 0;

void expect(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_Failures;
    }
}

void putU32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 24));
}

void putSequenceHeader(std::vector<uint8_t>& out, int width, int height, uint32_t totalBlocks,
                       bool chroma444, uint32_t code = 0, uint32_t sequence = 1)
{
    putU32(out, uint32_t(width - 1) | (uint32_t(height - 1) << 14) | (sequence << 28) | 0x80000000u);
    putU32(out, totalBlocks | (code << 24) | ((chroma444 ? 1u : 0u) << 26));
}

// A block record with `words` 32-bit words including its 8-byte header
void putBlock(std::vector<uint8_t>& out, uint32_t blockIndex, uint32_t words, uint32_t sequence = 1)
{
    putU32(out, 0x1u | (words << 16) | (sequence << 28));
    putU32(out, blockIndex << 8);
    for (uint32_t i = 2; i < words; i++) {
        putU32(out, 0x5A5A5A5Au);
    }
}

void putPadding(std::vector<uint8_t>& out, uint32_t zeroWords)
{
    putU32(out, PyroWaveFraming::k_PaddingMagic);
    putU32(out, zeroWords);
    for (uint32_t i = 0; i < zeroWords; i++) {
        putU32(out, 0);
    }
}

bool parse(const std::vector<uint8_t>& data, const StreamGeometry& geometry, Frame& frame)
{
    std::string error;
    return PyroWaveFraming::parse(data.data(), data.size(), geometry, frame, error);
}

void testBlockCounts()
{
    // Values from the bitstream's block-index formula (see the PyroWave
    // integration dossier): five levels, four bands at the coarsest level.
    expect(PyroWaveFraming::maxBlockCount({1920, 1080, false}) == 3261, "1080p 4:2:0 block count");
    expect(PyroWaveFraming::maxBlockCount({1920, 1080, true}) == 6321, "1080p 4:4:4 block count");
    expect(PyroWaveFraming::maxBlockCount({1280, 720, false}) == 1473, "720p 4:2:0 block count");
    expect(PyroWaveFraming::maxBlockCount({3840, 2160, false}) == 12429, "4K 4:2:0 block count");
    expect(PyroWaveFraming::maxBlockCount({3840, 2160, true}) == 24669, "4K 4:4:4 block count");
}

void testRecordFraming()
{
    const StreamGeometry geometry {1920, 1080, false};

    std::vector<uint8_t> data;
    putSequenceHeader(data, 1920, 1080, 3, false);
    putBlock(data, 7, 5);
    putPadding(data, 3);
    putBlock(data, 0, 2);
    putBlock(data, 3260, 9);
    putPadding(data, 0);

    Frame frame;
    expect(parse(data, geometry, frame), "record framing parses");
    expect(frame.framing == Framing::Records, "record framing is detected");
    expect(frame.blockRecords == 3 && frame.announcedBlocks == 3, "record framing counts blocks");
    expect(frame.spans.size() == 2, "padding splits the records into two spans");
    expect(frame.spans.size() == 2 && frame.spans[0].offset == 0 && frame.spans[0].size == 8 + 20,
           "first span holds the sequence header and the first block");
    expect(frame.spans.size() == 2 && frame.spans[1].offset == 8 + 20 + 20 && frame.spans[1].size == 8 + 36,
           "second span starts after the padding");
    expect(frame.paddingBytes == 20 + 8, "padding bytes are counted");
}

void testLengthPrefixedFraming()
{
    const StreamGeometry geometry {1280, 720, true};

    std::vector<uint8_t> packet0;
    putSequenceHeader(packet0, 1280, 720, 2, true);
    putBlock(packet0, 0, 4);
    std::vector<uint8_t> packet1;
    putBlock(packet1, 2912, 3);

    std::vector<uint8_t> data;
    putU32(data, 2);
    putU32(data, uint32_t(packet0.size()));
    data.insert(data.end(), packet0.begin(), packet0.end());
    putU32(data, uint32_t(packet1.size()));
    data.insert(data.end(), packet1.begin(), packet1.end());

    Frame frame;
    expect(parse(data, geometry, frame), "length-prefixed framing parses");
    expect(frame.framing == Framing::LengthPrefixed, "length-prefixed framing is detected");
    expect(frame.spans.size() == 2 && frame.spans[0].offset == 8 && frame.spans[1].offset == 8 + packet0.size() + 4,
           "each packet becomes a span");

    // Padding is never valid inside a length-prefixed packet
    std::vector<uint8_t> padded;
    putU32(padded, 1);
    std::vector<uint8_t> body;
    putSequenceHeader(body, 1280, 720, 0, true);
    putPadding(body, 1);
    putU32(padded, uint32_t(body.size()));
    padded.insert(padded.end(), body.begin(), body.end());
    expect(!parse(padded, geometry, frame), "padding inside a length-prefixed packet is rejected");
}

void testRejections()
{
    const StreamGeometry geometry {1920, 1080, false};
    Frame frame;

    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 1, false);
        putBlock(data, 5, 1);
        expect(!parse(data, geometry, frame), "a block shorter than its header is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 1, false);
        putBlock(data, 5, 0);
        expect(!parse(data, geometry, frame), "a zero-length block is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 1, false);
        putBlock(data, 5, 4);
        data.resize(data.size() - 4);
        expect(!parse(data, geometry, frame), "a truncated block is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 1, false);
        putBlock(data, 3261, 2);
        expect(!parse(data, geometry, frame), "an out-of-range block index is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1280, 720, 0, false);
        expect(!parse(data, geometry, frame), "a sequence header for another size is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 0, true);
        expect(!parse(data, geometry, frame), "a sequence header for another chroma mode is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 1, false, 1);
        putBlock(data, 5, 2);
        expect(!parse(data, geometry, frame), "a keep-previous (code 1) frame is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 2, false);
        putBlock(data, 5, 2);
        expect(!parse(data, geometry, frame), "a frame missing announced blocks is rejected");
    }
    {
        std::vector<uint8_t> data;
        putBlock(data, 5, 2);
        // First word has bit 31 clear, so this is read as a packet count
        expect(!parse(data, geometry, frame), "a frame without a sequence header is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 0, false);
        putPadding(data, 100);
        data.resize(data.size() - 8);
        expect(!parse(data, geometry, frame), "padding running past the frame is rejected");
    }
    {
        std::vector<uint8_t> data;
        putSequenceHeader(data, 1920, 1080, 0, false);
        data.push_back(0);
        data.push_back(0);
        expect(!parse(data, geometry, frame), "a frame that is not word aligned is rejected");
    }
}

}

int main()
{
    testBlockCounts();
    testRecordFraming();
    testLengthPrefixedFraming();
    testRejections();

    if (g_Failures == 0) {
        std::printf("PyroWave framing: all checks passed\n");
    }
    return g_Failures ? 1 : 0;
}
