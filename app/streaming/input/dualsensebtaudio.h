// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(__linux__) || defined(_WIN32)
#include <opus.h>
#include <algorithm>
#include <array>
#include <cstdint>

// Bluetooth audio uses 512 host frames per 10.67 ms report. The controller's
// speaker payload is a 480-frame Opus packet; the haptic payload remains 32
// signed stereo frames. The 0xFE configuration leaves microphone capture off.
// Protocol research: https://github.com/mrFunkill/decky-dualsense-wired/blob/main/research/FINDINGS.md
// The HID CRC follows third-party/saxense/packet.h, adapted from SAxense by
// Sdore (Egor Vorontsov); see third-party/saxense/PROVENANCE.md for its license.
class DualSenseBtAudio {
public:
    static constexpr size_t inputFrames = 512;
    static constexpr size_t reportBytes = 398;

    DualSenseBtAudio() {
        int error = 0;
        encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
        if (error != OPUS_OK || !encoder) return;
        if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(160000)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(2)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK) {
            opus_encoder_destroy(encoder);
            encoder = nullptr;
        }
    }
    ~DualSenseBtAudio() { if (encoder) opus_encoder_destroy(encoder); }
    DualSenseBtAudio(const DualSenseBtAudio&) = delete;
    DualSenseBtAudio& operator=(const DualSenseBtAudio&) = delete;
    bool ready() const { return encoder != nullptr; }

    static void checksum(uint8_t* report, size_t size) {
        uint32_t crc = ~UINT32_C(0xEADA2D49);
        for (size_t i = 0; i < size - 4; ++i) {
            crc ^= report[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1)));
        }
        crc = ~crc;
        for (unsigned i = 0; i < 4; ++i) report[size - 4 + i] = uint8_t(crc >> (8 * i));
    }

    bool make(uint8_t reportSequence, uint8_t audioSequence, const int16_t* stereo, const uint8_t* haptics,
              std::array<uint8_t, reportBytes>& out, bool includeHaptics = true) {
        if (!ready() || !stereo || (includeHaptics && !haptics)) return false;
        std::array<int16_t, 480 * 2> resampled{};
        for (size_t i = 0; i < 480; ++i) {
            const size_t numerator = i * (inputFrames - 1);
            const size_t index = numerator / 479;
            const int fraction = int(numerator % 479);
            const size_t next = std::min(index + 1, inputFrames - 1);
            for (size_t channel = 0; channel < 2; ++channel) {
                const int left = stereo[index * 2 + channel];
                const int right = stereo[next * 2 + channel];
                resampled[i * 2 + channel] = int16_t((left * (479 - fraction) + right * fraction) / 479);
            }
        }
        std::array<uint8_t, 200> encoded{};
        if (opus_encode(encoder, resampled.data(), 480, encoded.data(), encoded.size()) != int(encoded.size())) return false;
        out.fill(0);
        out[0] = 0x36; out[1] = uint8_t((reportSequence & 15) << 4);
        out[2] = 0x91; out[3] = 7; out[4] = 0xfe;
        for (size_t i = 5; i <= 9; ++i) out[i] = 0x30;
        out[10] = audioSequence;
        out[11] = 0x90; out[12] = 63;
        // Set only audio-related flags; unrelated LEDs/triggers remain owned by SDL.
        out[13] = 0xa0; out[14] = 0x80; out[18] = 0x64;
        out[20] = 0x30; out[50] = 0x02;
        size_t offset = 76;
        if (includeHaptics) {
            out[offset++] = 0x92; out[offset++] = 64;
            std::copy_n(haptics, 64, out.begin() + offset);
            offset += 64;
        }
        out[offset++] = 0x93; out[offset++] = 200;
        std::copy(encoded.begin(), encoded.end(), out.begin() + offset);
        checksum(out.data(), out.size());
        return true;
    }
private:
    OpusEncoder* encoder = nullptr;
};
#endif
