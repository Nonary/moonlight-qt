#include "pyrowaveframing.h"

#include <algorithm>
#include <cstring>

namespace PyroWaveFraming {

namespace {

// Bitstream constants (pyrowave bitstream/bitstream.md)
constexpr int k_DecompositionLevels = 5;
constexpr int k_BlockSize = 32;
constexpr int k_MinimumImageSize = 128;
constexpr size_t k_HeaderBytes = 8;
constexpr uint32_t k_HeaderWords = 2;

uint32_t readU32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

int alignedExtent(int extent)
{
    int aligned = (extent + k_BlockSize - 1) / k_BlockSize * k_BlockSize;
    return std::max(aligned, k_MinimumImageSize);
}

// Walks one run of records. With allowPadding, padding records split the run
// into spans; otherwise any padding magic is an error (length-prefixed packets
// never contain padding).
bool walkRecords(const uint8_t* data, size_t base, size_t size,
                 const StreamGeometry& geometry, uint32_t maxBlocks,
                 bool allowPadding, Frame& frame, std::string& error)
{
    size_t pos = 0;
    size_t spanStart = 0;

    auto closeSpan = [&](size_t end) {
        if (end > spanStart) {
            frame.spans.push_back({base + spanStart, end - spanStart});
        }
    };

    while (pos < size) {
        if (size - pos < k_HeaderBytes) {
            error = "trailing bytes shorter than a record header";
            return false;
        }

        const uint32_t word0 = readU32(data + base + pos);
        const uint32_t word1 = readU32(data + base + pos + 4);

        if (word0 == k_PaddingMagic) {
            if (!allowPadding) {
                error = "padding record inside a length-prefixed packet";
                return false;
            }

            const uint64_t padBytes = k_HeaderBytes + uint64_t(word1) * 4;
            if (padBytes > size - pos) {
                error = "padding record runs past the end of the frame";
                return false;
            }

            closeSpan(pos);
            pos += size_t(padBytes);
            spanStart = pos;
            frame.paddingBytes += uint32_t(padBytes);
            continue;
        }

        if (word0 & 0x80000000u) {
            // BitstreamSequenceHeader
            const int width = int(word0 & 0x3FFF) + 1;
            const int height = int((word0 >> 14) & 0x3FFF) + 1;
            const uint32_t totalBlocks = word1 & 0xFFFFFF;
            const uint32_t code = (word1 >> 24) & 0x3;
            const bool chroma444 = ((word1 >> 26) & 0x1) != 0;

            if (code != 0) {
                // Code 1 is the Solarflare "keep previous coefficients" frame,
                // which upstream PyroWave cannot decode.
                error = "unsupported sequence header code " + std::to_string(code);
                return false;
            }
            if (width != geometry.width || height != geometry.height) {
                error = "sequence header size " + std::to_string(width) + "x" + std::to_string(height) +
                        " differs from the stream";
                return false;
            }
            if (chroma444 != geometry.chroma444) {
                error = "sequence header chroma subsampling differs from the stream";
                return false;
            }
            if (totalBlocks > maxBlocks) {
                error = "sequence header announces more blocks than the frame can hold";
                return false;
            }

            frame.sequenceHeaderSeen = true;
            frame.announcedBlocks = totalBlocks;
            pos += k_HeaderBytes;
            continue;
        }

        // BitstreamHeader
        const uint32_t payloadWords = (word0 >> 16) & 0xFFF;
        const uint32_t blockIndex = word1 >> 8;

        if (payloadWords < k_HeaderWords) {
            error = "block record shorter than its header";
            return false;
        }
        if (uint64_t(payloadWords) * 4 > size - pos) {
            error = "block record runs past the end of the frame";
            return false;
        }
        if (blockIndex >= maxBlocks) {
            error = "block index " + std::to_string(blockIndex) + " is out of range";
            return false;
        }

        frame.blockRecords++;
        pos += size_t(payloadWords) * 4;
    }

    closeSpan(pos);
    return true;
}

}

uint32_t maxBlockCount(const StreamGeometry& geometry)
{
    const int alignedWidth = alignedExtent(geometry.width);
    const int alignedHeight = alignedExtent(geometry.height);
    uint32_t blocks = 0;

    for (int level = 0; level < k_DecompositionLevels; level++) {
        const int bandWidth = alignedWidth >> (level + 1);
        const int bandHeight = alignedHeight >> (level + 1);
        const uint32_t perBand = uint32_t((bandWidth + k_BlockSize - 1) / k_BlockSize) *
                                 uint32_t((bandHeight + k_BlockSize - 1) / k_BlockSize);
        const uint32_t bands = level == k_DecompositionLevels - 1 ? 4 : 3;
        const uint32_t components = (level == 0 && !geometry.chroma444) ? 1 : 3;
        blocks += perBand * bands * components;
    }

    return blocks;
}

bool parse(const uint8_t* data, size_t size, const StreamGeometry& geometry,
           Frame& frame, std::string& error)
{
    frame = Frame();

    if (data == nullptr || size < k_HeaderBytes || (size % 4) != 0) {
        error = "frame is empty or not word aligned";
        return false;
    }

    const uint32_t maxBlocks = maxBlockCount(geometry);
    const uint32_t firstWord = readU32(data);

    // A record-framed frame starts with a sequence header (extended bit set,
    // which a padding record also has); a packet count never sets bit 31.
    if (firstWord & 0x80000000u) {
        frame.framing = Framing::Records;
        if (!walkRecords(data, 0, size, geometry, maxBlocks, true, frame, error)) {
            return false;
        }
    }
    else {
        frame.framing = Framing::LengthPrefixed;
        const uint32_t count = firstWord;
        if (count == 0 || count > (size - 4) / 8) {
            error = "invalid packet count " + std::to_string(count);
            return false;
        }

        size_t pos = 4;
        for (uint32_t i = 0; i < count; i++) {
            if (size - pos < 4) {
                error = "packet length runs past the end of the frame";
                return false;
            }
            const uint32_t packetSize = readU32(data + pos);
            pos += 4;
            if (packetSize == 0 || (packetSize % 4) != 0 || packetSize > size - pos) {
                error = "invalid packet length " + std::to_string(packetSize);
                return false;
            }
            if (!walkRecords(data, pos, packetSize, geometry, maxBlocks, false, frame, error)) {
                return false;
            }
            pos += packetSize;
        }

        if (pos != size) {
            error = "bytes after the last packet";
            return false;
        }
    }

    if (!frame.sequenceHeaderSeen) {
        error = "frame has no sequence header";
        return false;
    }
    if (frame.blockRecords != frame.announcedBlocks) {
        error = "frame carries " + std::to_string(frame.blockRecords) + " of " +
                std::to_string(frame.announcedBlocks) + " announced blocks";
        return false;
    }

    return true;
}

}
