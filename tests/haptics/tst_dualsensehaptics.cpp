// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual resampler, worker, bounded input queue and stop behavior
// with a datagram socket standing in for hidraw. No physical device is opened.
#include "../../app/streaming/input/dualsensehaptics.cpp"
#include <sys/socket.h>
#include <poll.h>
#include <cstdio>
#include <cmath>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    SDL_Init(0);
    struct QuitSDL { ~QuitSDL() { SDL_Quit(); } } quitSDL;
    try {
        // Compare CRC against an independent bit-at-a-time reference including
        // the HID output prefix. Protect report length, padding and signed PCM.
        uint8_t report[SAXENSE_REPORT_BYTES], pcm[SAXENSE_PCM_BYTES];
        for (unsigned i = 0; i < sizeof(pcm); ++i) pcm[i] = uint8_t(i * 7);
        saxense_packet(report, 255, pcm);
        uint32_t crc = UINT32_MAX;
        for (unsigned n = 0; n < sizeof(report) - 3; ++n) {
            uint8_t byte = n ? report[n - 1] : 0xa2;
            for (unsigned bit = 0; bit < 8; ++bit) {
                bool low = (crc ^ (byte >> bit)) & 1;
                crc >>= 1;
                if (low) crc ^= 0xedb88320;
            }
        }
        require(MlHapticsRead32(report + 138) == ~crc, "CRC");
        require(report[0] == 0x32 && report[10] == 255 && report[12] == 64, "layout");
        require(memcmp(report + 13, pcm, 64) == 0, "waveforms");
        for (unsigned i = 77; i < 138; ++i) require(report[i] == 0, "padding");
        int sockets[2];
        require(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0, "socketpair");
        auto playback = std::make_shared<Playback>();
        playback->hid = sockets[0];
        playback->converter = SDL_NewAudioStream(AUDIO_S16LSB, 2, 48000, AUDIO_S8, 2, 3000);
        require(playback->converter != nullptr, "resampler");
        playback->worker = std::thread([p = playback.get()] { p->run(); });
        { std::lock_guard<std::mutex> lock(registryMutex); registry[0] = playback; }
        bool leftNonzero = false, rightNonzero = false;
        unsigned reports = 0;
        std::array<uint8_t, 960> wave {};
        // 120 Hz left-only tone, then right-only tone, separated by silence.
        for (unsigned packet = 0; packet < 120; ++packet) {
            for (unsigned frame = 0; frame < 240; ++frame) {
                int16_t tone = int16_t(16000 * std::sin((packet * 240 + frame) * 6.283185307179586 * 120 / 48000));
                MlHapticsWrite16(wave.data() + frame * 4, packet < 40 ? uint16_t(tone) : 0);
                MlHapticsWrite16(wave.data() + frame * 4 + 2, packet >= 60 && packet < 100 ? uint16_t(tone) : 0);
            }
            if (packet != 20) DualSenseHaptics::receive(0, packet, wave.data(), 240); // packet loss
            if (packet == 10) {
                auto stale = wave;
                for (unsigned frame = 0; frame < 240; ++frame)
                    MlHapticsWrite16(stale.data() + frame * 4 + 2, 16384);
                // A late duplicate must not inject right-channel feedback into
                // the left-only effect verified below.
                DualSenseHaptics::receive(0, 9, stale.data(), 240);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            while (recv(sockets[1], report, sizeof(report), 0) > 0) {
                ++reports;
                for (unsigned i = 0; i < 32; ++i) {
                    if (packet < 40) {
                        leftNonzero |= report[13 + i * 2] != 0;
                        require(report[14 + i * 2] == 0, "left leaks into right");
                    }
                    if (packet > 70 && packet < 100) {
                        rightNonzero |= report[14 + i * 2] != 0;
                        require(report[13 + i * 2] == 0, "right leaks into left");
                    }
                }
            }
        }
        require(leftNonzero && rightNonzero && reports > 20, "rendered both actuators");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(!DualSenseHaptics::playing(0), "idle timeout");
        bool gotSilence = false;
        while (recv(sockets[1], report, sizeof(report), 0) > 0) {
            gotSilence = true;
            for (unsigned i = 13; i < 77; ++i) require(report[i] == 0, "idle silence");
        }
        require(gotSilence, "stop report");
        // Overload without a worker: queue must remain bounded and retain latest.
        { std::lock_guard<std::mutex> lock(playback->mutex); playback->stopped = true; }
        playback->wake.notify_all(); playback->worker.join();
        { std::lock_guard<std::mutex> lock(playback->mutex); playback->stopped = false; }
        for (unsigned i = 0; i < 1000; ++i) DualSenseHaptics::receive(0, i, wave.data(), 240);
        require(playback->queue.size() <= 8 && playback->queue.back().sequence == 999, "bounded backlog");
        DualSenseHaptics::detach(0);
        DualSenseHaptics::receive(0, 1001, wave.data(), 240); // harmless after removal
        playback.reset(); close(sockets[1]);
        std::puts("DualSense haptics: packet CRC, stereo resampling, packet loss/duplicates, idle silence and bounded queue passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
