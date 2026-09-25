#pragma once

// Parses one PyroWave frame as delivered by moonlight-common-c into the runs of
// PyroWave records that pyrowave_decoder_push_packet() accepts. Supports both
// framings in docs/pyrowave-protocol.md:
//
//  - Record framing: a sequence header, block records and padding records
//    (0xFFFFFFFF, N, N zero words), concatenated.
//  - Length-prefixed framing: [u32 count] { [u32 size] [packet] } * count.
//
// Everything that reaches the decoder is validated here first: a block record
// shorter than its header, a record running past the frame, a sequence header
// for another size or chroma mode, or an out-of-range block index rejects the
// whole frame. Frames are independent, so the next frame simply replaces it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PyroWaveFraming {

enum class Framing {
    Records,
    LengthPrefixed,
};

// A byte range of the input that holds whole records and no padding.
struct Span {
    size_t offset;
    size_t size;
};

struct StreamGeometry {
    int width;
    int height;
    bool chroma444;
};

struct Frame {
    Framing framing = Framing::Records;
    std::vector<Span> spans;
    // Number of block records (excluding sequence headers and padding)
    uint32_t blockRecords = 0;
    // total_blocks from the sequence header, or 0 if none was present
    uint32_t announcedBlocks = 0;
    bool sequenceHeaderSeen = false;
    uint32_t paddingBytes = 0;
};

constexpr uint32_t k_PaddingMagic = 0xFFFFFFFFu;

// Number of 32x32 blocks a frame of this geometry can index (bitstream spec:
// five wavelet levels of each component, 4:2:0 omits chroma level 0).
uint32_t maxBlockCount(const StreamGeometry& geometry);

bool parse(const uint8_t* data, size_t size, const StreamGeometry& geometry,
           Frame& frame, std::string& error);

}
