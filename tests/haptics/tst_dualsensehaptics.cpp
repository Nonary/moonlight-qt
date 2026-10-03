// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual resampler, worker, bounded input queue and stop behavior
// with an in-memory HID output. No physical device is opened.
#include "../../app/streaming/input/dualsensehaptics.cpp"
#include "../../app/streaming/input/dualsensetriggers.h"
#include <cstdio>
#include <cmath>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Reports {
    std::mutex mutex;
    std::deque<std::array<uint8_t, SAXENSE_REPORT_BYTES>> packets;
    bool fail = false;

    bool pop(uint8_t* report) {
        std::lock_guard<std::mutex> lock(mutex);
        if (packets.empty()) return false;
        memcpy(report, packets.front().data(), SAXENSE_REPORT_BYTES);
        packets.pop_front();
        return true;
    }
};

class RecordingOutput final : public DualSenseHidOutput {
public:
    explicit RecordingOutput(Reports& reports) : reports(reports) {}
    bool write(const uint8_t* report, size_t size) override {
        std::lock_guard<std::mutex> lock(reports.mutex);
        if (reports.fail || size != SAXENSE_REPORT_BYTES) return false;
        std::array<uint8_t, SAXENSE_REPORT_BYTES> packet;
        memcpy(packet.data(), report, size);
        reports.packets.push_back(packet);
        return true;
    }
    Reports& reports;
};

static void testTriggers() {
    uint8_t left[DS_EFFECT_PAYLOAD_SIZE], right[DS_EFFECT_PAYLOAD_SIZE];
    for (unsigned i = 0; i < DS_EFFECT_PAYLOAD_SIZE; ++i) {
        left[i] = uint8_t(i + 1);
        right[i] = uint8_t(255 - i);
    }
    const auto report = makeDualSenseTriggerReport(0xff, 0x21, 0x26, left, right);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&report);
    require(bytes[0] == 0x0c && bytes[10] == 0x26 && bytes[21] == 0x21, "trigger flags and types");
    require(memcmp(bytes + 11, right, 10) == 0 && memcmp(bytes + 22, left, 10) == 0, "trigger payloads");
    for (unsigned i = 1; i < 47; ++i)
        if (i < 10 || i >= 32) require(bytes[i] == 0, "triggers must not change rumble, audio or LEDs");
    require(makeDualSenseTriggerReport(DS_EFFECT_LEFT_TRIGGER, 0, 0, left, right).validFlag0 == 8,
            "single trigger update");

    // Exercise SDL's cross-platform SendEffect dispatch with a virtual Sony
    // gamepad. The real Windows HIDAPI driver supplies Bluetooth framing.
    std::array<uint8_t, 47> captured {};
    SDL_VirtualJoystickDesc desc {};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = 0x054c;
    desc.product_id = 0x0ce6;
    desc.name = "DualSense trigger test";
    desc.userdata = &captured;
    desc.SendEffect = [](void* data, const void* effect, int size) -> int {
        if (size != 47) return -1;
        memcpy(static_cast<std::array<uint8_t, 47>*>(data)->data(), effect, size);
        return 0;
    };
    const int index = SDL_JoystickAttachVirtualEx(&desc);
    require(index >= 0, "virtual controller attachment");
    SDL_GameController* controller = SDL_GameControllerOpen(index);
    require(controller != nullptr, "virtual controller open");
    // Some sdl2-compat builds invert the virtual callback's success status.
    // Assert the observed dispatch here; this is not a hardware output test.
    SDL_GameControllerSendEffect(controller, &report, sizeof(report));
    SDL_GameControllerClose(controller);
    SDL_JoystickDetachVirtual(index);
    require(memcmp(captured.data(), &report, sizeof(report)) == 0, "SDL trigger dispatch");
}

int main() {
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_GAMECONTROLLER);
    struct QuitSDL { ~QuitSDL() { SDL_Quit(); } } quitSDL;
    try {
        testTriggers();
        require(!DualSenseHaptics::attach(16, nullptr), "invalid controller slot");
        require(!DualSenseHaptics::attach(0, nullptr), "missing waveform controller");
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
        Reports recorded;
        auto playback = std::make_shared<Playback>();
        playback->output = std::make_unique<RecordingOutput>(recorded);
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
            while (recorded.pop(report)) {
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
        while (recorded.pop(report)) {
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
        playback.reset();

        // Removal joins a running worker and sends a final silent report.
        playback = std::make_shared<Playback>();
        playback->output = std::make_unique<RecordingOutput>(recorded);
        playback->converter = SDL_NewAudioStream(AUDIO_S16LSB, 2, 48000, AUDIO_S8, 2, 3000);
        require(playback->converter != nullptr, "removal resampler");
        { std::lock_guard<std::mutex> lock(registryMutex); registry[0] = playback; }
        playback->worker = std::thread([p = playback.get()] { p->run(); });
        DualSenseHaptics::receive(0, 0, wave.data(), 240);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        playback.reset(); // registry is the sole owner, as in production
        DualSenseHaptics::detach(0);
        require(!DualSenseHaptics::playing(0), "removed controller is inactive");
        bool gotFinal = false;
        while (recorded.pop(report)) gotFinal = true;
        require(gotFinal, "removal final report");
        for (unsigned i = 13; i < 77; ++i) require(report[i] == 0, "removal silence");

        // A disconnected/failed HID output must release legacy rumble priority.
        playback = std::make_shared<Playback>();
        recorded.fail = true;
        playback->output = std::make_unique<RecordingOutput>(recorded);
        playback->converter = SDL_NewAudioStream(AUDIO_S16LSB, 2, 48000, AUDIO_S8, 2, 3000);
        require(playback->converter != nullptr, "failure resampler");
        { std::lock_guard<std::mutex> lock(registryMutex); registry[0] = playback; }
        playback->worker = std::thread([p = playback.get()] { p->run(); });
        DualSenseHaptics::receive(0, 0, wave.data(), 240);
        playback->worker.join();
        require(!DualSenseHaptics::playing(0), "failed output releases rumble priority");
        DualSenseHaptics::receive(0, 1, wave.data(), 240);
        require(playback->queue.empty(), "failed output rejects more PCM");
        DualSenseHaptics::detach(0);
        playback.reset();
        std::puts("DualSense: trigger dispatch, packet CRC, stereo resampling, loss/duplicates, idle/removal silence, bounded queue and output failure passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
