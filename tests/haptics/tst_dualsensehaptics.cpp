// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual resampler, worker, bounded input queue and stop behavior
// with an in-memory HID output. No physical device is opened.
#include "../../app/streaming/input/dualsensehaptics.cpp"
#include "../../app/streaming/input/dualsensetriggers.h"
#include <ControllerSpeakerPcm.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <future>

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

// Use the production ingress and priority logic without a worker. USB/IP
// delivers silent audio continuously while a game's audio endpoint is open.
static void testAdmission() {
    struct InputBackend final : Backend { void notify() override {} } backend;
    std::array<uint8_t, 960> pcm {};
    backend.receive(UINT32_MAX - 1, pcm.data(), 240);
    require(!backend.playing() && backend.queue.empty(), "silent endpoint must not suppress ordinary rumble");
    MlHapticsWrite16(pcm.data(), 0x8000);
    backend.receive(UINT32_MAX, pcm.data(), 240);
    require(backend.playing() && backend.queue.size() == 1, "nonzero waveform takes priority");
    backend.receive(UINT32_MAX, pcm.data(), 240);
    backend.receive(UINT32_MAX - 1, pcm.data(), 240);
    require(backend.queue.size() == 1, "late and duplicate packets rejected before playback");
    backend.receive(0, pcm.data(), 240);
    require(backend.queue.size() == 2, "sequence wrap accepts the next waveform");
    backend.queue.clear();
    pcm.fill(0);
    backend.receive(1, pcm.data(), 240);
    backend.receive(0, pcm.data(), 240);
    require(!backend.playing() && backend.queue.empty(), "idle silence stays inactive across sequence wrap");
    MlHapticsWrite16(pcm.data(), 1);
    backend.receive(0, pcm.data(), 240);
    require(backend.queue.empty(), "idle silence still advances sequence; late waveform cannot restart playback");
}

static void testSpeakerWireAndNegotiation() {
  std::array<uint8_t, ML_SPEAKER_PCM_MAX_PAYLOAD> packet{};
  packet[0] = 2;
  packet[1] = 1;
  MlSpeakerWrite16(packet.data() + 2, 3);
  MlSpeakerWrite32(packet.data() + 4, 7);
  MlSpeakerWrite32(packet.data() + 8, 12);
  MlSpeakerWrite64(packet.data() + 12, 2400);
  MlSpeakerWrite16(packet.data() + 20, ML_SPEAKER_PCM_MAX_FRAMES);
  MlSpeakerWrite16(packet.data() + 22, ML_SPEAKER_PCM_FLAG_DISCONTINUITY);
  MlSpeakerWrite64(packet.data() + 24, 0x123456789abcdef0ULL);
  require(MlSpeakerPcmValidate(packet.data(), packet.size()),
          "speaker maximum packet validates");
  require(MlSpeakerRead32(packet.data() + 4) == 7 &&
              MlSpeakerRead64(packet.data() + 12) == 2400,
          "speaker generation and sample position round trip");

  auto invalid = packet;
  invalid[0] = 1;
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()),
          "speaker version rejected");
  invalid = packet;
  MlSpeakerWrite64(invalid.data() + 24, 0);
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()), "unbound speaker packet rejected");
  invalid = packet;
  MlSpeakerWrite64(invalid.data() + 12, UINT64_MAX);
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()), "sample position overflow rejected");
  invalid = packet;
  invalid[1] = 2;
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()),
          "speaker format rejected");
  invalid = packet;
  MlSpeakerWrite16(invalid.data() + 2, 16);
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()),
          "speaker controller range rejected");
  invalid = packet;
  MlSpeakerWrite32(invalid.data() + 4, 0);
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()),
          "speaker zero generation rejected");
  invalid = packet;
  MlSpeakerWrite16(invalid.data() + 22, 2);
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()),
          "speaker reserved flags rejected");
  invalid = packet;
  MlSpeakerWrite16(invalid.data() + 20, 0);
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()),
          "speaker zero frames rejected");
  invalid = packet;
  MlSpeakerWrite16(invalid.data() + 20, ML_SPEAKER_PCM_MAX_FRAMES + 1);
  require(!MlSpeakerPcmValidate(invalid.data(), invalid.size()),
          "speaker oversized frames rejected");
  require(!MlSpeakerPcmValidate(packet.data(), packet.size() - 1),
          "speaker payload length must be exact");
  require(!MlSpeakerPcmValidate(nullptr, packet.size()),
          "speaker null payload rejected");

  require(!MlSpeakerPcmCanSend(ML_FF_CONTROLLER_SPEAKER_PCM, 4, LI_CCAP_PS_SPEAKER_PCM, 1),
          "Steam attributes capability must not enable speaker PCM");
  for (int session = 0; session != 2; ++session) {
    for (int controller = 0; controller != 2; ++controller) {
      for (int source = 0; source != 2; ++source) {
        const bool expected = session && controller && source;
        require(
            MlSpeakerPcmCanSend(session ? ML_FF_CONTROLLER_SPEAKER_PCM : 0, 2,
                                controller ? LI_CCAP_PS_SPEAKER_PCM : 0,
                                source) == expected,
            "speaker session/controller/source gates are independent");
      }
    }
  }
}

static void testSpeakerQueueLifecycle() {
  struct TestBackend final : Backend {
    bool ready = false;
    void notify() override {}
    bool speakerOutputReady() const override { return ready; }
  } backend;
  std::array<uint8_t, ML_SPEAKER_PCM_MAX_FRAMES * ML_SPEAKER_PCM_FRAME_BYTES>
      samples{};
  backend.receiveSpeaker(backend.speakerOwnerToken, 1, 0, 0, ML_SPEAKER_PCM_FLAG_DISCONTINUITY,
                         samples.data(), 240);
  require(backend.speakerQueue.empty(),
          "speaker data drops without a ready renderer");
  backend.ready = true;
  backend.queue.push_back(Chunk{});
  backend.receiveSpeaker(backend.speakerOwnerToken, 1, 0, 0, ML_SPEAKER_PCM_FLAG_DISCONTINUITY,
                         samples.data(), 240);
  backend.receiveSpeaker(backend.speakerOwnerToken, 1, 0, 0, 0, samples.data(), 240);
  require(backend.speakerQueue.size() == 1 && backend.queue.size() == 1,
          "speaker duplicate rejected without touching actuator queue");
  backend.receiveSpeaker(backend.speakerOwnerToken, 1, 1, 240, 0, samples.data(), 240);
  require(backend.speakerQueue.size() == 2,
          "speaker accepts contiguous sample position");
  backend.receiveSpeaker(backend.speakerOwnerToken, 2, 0, 0, ML_SPEAKER_PCM_FLAG_DISCONTINUITY,
                         samples.data(), 240);
  backend.receiveSpeaker(backend.speakerOwnerToken, 1, 2, 480, 0, samples.data(), 240);
  require(backend.speakerQueue.size() == 1 &&
              backend.speakerQueue.front().generation == 2,
          "new slot generation clears old samples and rejects late generation "
          "packets");
  for (uint32_t sequence = 1; sequence <= 9; ++sequence) {
    backend.receiveSpeaker(backend.speakerOwnerToken, 2, sequence, uint64_t(sequence) * 240, 0,
                           samples.data(), 240);
  }
  require(backend.speakerQueue.size() <= 8 && backend.queue.size() == 1,
          "speaker overflow stays bounded and independent from actuator PCM");
  require(backend.speakerQueue.front().flags &
              ML_SPEAKER_PCM_FLAG_DISCONTINUITY,
          "speaker queue replacement marks a discontinuity");

  DualSenseHaptics::resetSpeakerGenerationFloor();
  auto first = std::make_shared<TestBackend>();
  const auto oldOwner = first->speakerOwnerToken.load();
  first->ready = true;
  {
    std::lock_guard<std::mutex> lock(registryMutex);
    registry[5] = first;
  }
  DualSenseHaptics::receiveSpeaker(
      5, oldOwner, 10, 0, 0, ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
  DualSenseHaptics::receiveSpeaker(5, oldOwner, 10, 1, 240, 0, samples.data(), 240);
  require(first->speakerQueue.size() == 2,
          "successive packets in the current generation are admitted");
  DualSenseHaptics::detach(5);
  auto reused = std::make_shared<TestBackend>();
  reused->ready = true;
  {
    std::lock_guard<std::mutex> lock(registryMutex);
    registry[5] = reused;
  }
  DualSenseHaptics::receiveSpeaker(5, oldOwner, 10, 1, 240, 0, samples.data(), 240);
  require(reused->speakerQueue.empty(),
          "reused client slot rejects a delayed prior generation");
  DualSenseHaptics::receiveSpeaker(
      5, reused->speakerOwnerToken, 11, 0, 0, ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
  require(reused->speakerQueue.size() == 1 &&
              reused->speakerQueue.front().generation == 11,
          "reused client slot accepts the new generation");
  DualSenseHaptics::detach(5);
}

static void testSpeakerAdmissionContention() {
  struct TestBackend final : Backend {
    void notify() override {}
    bool speakerOutputReady() const override { return true; }
  };
  DualSenseHaptics::resetSpeakerGenerationFloor();
  auto backend = std::make_shared<TestBackend>();
  registry[6] = backend;
  std::array<uint8_t, 960> samples{};
  samples[0] = 17;
  for (auto *contended : {&registryMutex, &backend->mutex}) {
    std::unique_lock<std::mutex> held(*contended);
    std::promise<void> started;
    auto delivery = std::async(std::launch::async, [&] {
      started.set_value();
      DualSenseHaptics::receiveSpeaker(6, backend->speakerOwnerToken, 10, 0, 0,
          ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
    });
    started.get_future().wait();
    const auto status = delivery.wait_for(std::chrono::milliseconds(200));
    held.unlock();
    delivery.get();
    require(status == std::future_status::ready,
            "speaker callback returns while another thread still owns either lock");
    require(backend->speakerQueue.empty(), "contention drops instead of queuing late audio");
  }
  DualSenseHaptics::receiveSpeaker(6, backend->speakerOwnerToken, 10, 1, 240, 0, samples.data(), 240);
  samples[0] = 99;
  require(backend->speakerQueue.size() == 1 &&
          backend->speakerQueue.front().pcm[0] == 17,
          "admission resumes with an owned sample copy after contention");
  DualSenseHaptics::detach(6);
}

static void testSpeakerSubmissionReset() {
  struct TestBackend final : Backend {
    void notify() override {}
    bool speakerOutputReady() const override { return true; }
  };
  auto backend = std::make_shared<TestBackend>();
  registry[8] = backend;
  const auto dequeuedOwner = backend->speakerOwnerToken.load();
  // These samples represent a slice already dequeued by the WASAPI worker.
  // Exercise its actual shared submission gate and production routing, with
  // only the device-buffer submission replaced by an in-memory sink.
  const int16_t speaker[] = {12000, -12000};
  const int16_t silence[] = {0, 0};
  const int16_t actuator[] = {8000, -8000};
  float output[4]{};
  DualSenseHaptics::resetSpeakerGenerationFloor();
  require(backend->submitSpeakerFrame(dequeuedOwner, [&](bool current) {
    MlSpeakerPcmRouteFourChannel(current ? speaker : silence, actuator, output, 1);
    return true;
  }), "stale mixed-frame submission retains successful actuator output");
  require(output[0] == 0 && output[1] == 0 && output[2] > 0 && output[3] < 0,
          "reset before device submission silences old speaker slice, not actuators");
  std::array<uint8_t, 960> staleSamples{};
  backend->receiveSpeaker(dequeuedOwner, 1, 0, 0,
      ML_SPEAKER_PCM_FLAG_DISCONTINUITY, staleSamples.data(), 240);
  require(backend->speakerQueue.empty(),
          "owner is revalidated under the queue mutex if reset wins after ingress check");
  const auto currentOwner = backend->speakerOwnerToken.load();
  backend->submitSpeakerFrame(currentOwner, [&](bool current) {
    MlSpeakerPcmRouteFourChannel(current ? speaker : silence, actuator, output, 1);
    return true;
  });
  require(output[0] > 0 && output[1] < 0, "new owner still renders speaker samples");

  std::promise<void> submitting, finishSubmission, resetting;
  auto finish = finishSubmission.get_future().share();
  auto render = std::async(std::launch::async, [&] {
    return backend->submitSpeakerFrame(currentOwner, [&](bool current) {
      submitting.set_value();
      finish.wait(); // The real worker holds the same gate through ReleaseBuffer.
      return current;
    });
  });
  submitting.get_future().wait();
  auto reset = std::async(std::launch::async, [&] {
    resetting.set_value();
    DualSenseHaptics::resetSpeakerGenerationFloor();
  });
  resetting.get_future().wait();
  const auto resetStatus = reset.wait_for(std::chrono::milliseconds(100));
  auto query = std::async(std::launch::async, [] { return DualSenseHaptics::speakerOwner(8); });
  const auto queryStatus = query.wait_for(std::chrono::milliseconds(200));
  std::array<uint8_t, 960> samples{};
  auto receive = std::async(std::launch::async, [&] {
    DualSenseHaptics::receiveSpeaker(8, currentOwner, 1, 0, 0,
        ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
  });
  const auto receiveStatus = receive.wait_for(std::chrono::milliseconds(200));
  finishSubmission.set_value();
  const bool rendered = render.get();
  reset.get(); query.get(); receive.get();
  require(resetStatus == std::future_status::timeout && rendered,
          "reset cannot return until already admitted device submission completes");
  require(queryStatus == std::future_status::ready && receiveStatus == std::future_status::ready,
          "waiting for device submission holds neither registry nor callback admission");
  require(backend->speakerOwnerToken != currentOwner && backend->speakerQueue.empty(),
          "reset rotates and clears any samples received during in-flight submission");
  DualSenseHaptics::detach(8);
}

static void testSpeakerOwnerIdentity() {
  struct TestBackend final : Backend {
    void notify() override {}
    bool speakerOutputReady() const override { return true; }
  };
  DualSenseHaptics::resetSpeakerGenerationFloor();
  auto first = std::make_shared<TestBackend>();
  registry[7] = first;
  const auto retiredOwner = first->speakerOwnerToken.load();
  // No packet has ever arrived: the generation floor is still zero.
  DualSenseHaptics::detach(7);
  auto replacement = std::make_shared<TestBackend>();
  registry[7] = replacement;
  std::array<uint8_t, 960> samples{};
  DualSenseHaptics::receiveSpeaker(7, retiredOwner, 10, 0, 0,
      ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
  DualSenseHaptics::receiveSpeaker(7, 0, 10, 0, 0,
      ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
  require(replacement->speakerQueue.empty(), "unseen delayed and unknown owners fail closed before first delivery");
  const auto currentOwner = replacement->speakerOwnerToken.load();
  require(currentOwner != retiredOwner, "slot reuse changes incarnation");
  DualSenseHaptics::receiveSpeaker(7, currentOwner, 10, 0, 0,
      ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
  require(replacement->speakerQueue.size() == 1, "current owner first packet is accepted without quarantine");
  DualSenseHaptics::resetSpeakerGenerationFloor();
  require(replacement->speakerOwnerToken != currentOwner && replacement->speakerQueue.empty(),
          "reconnect rotates even retained endpoints and clears old audio");
  DualSenseHaptics::receiveSpeaker(7, currentOwner, 20, 0, 0,
      ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
  require(replacement->speakerQueue.empty(), "prior session owner is rejected even with newer host generation");
  DualSenseHaptics::receiveSpeaker(7, replacement->speakerOwnerToken, 1, 0, 0,
      ML_SPEAKER_PCM_FLAG_DISCONTINUITY, samples.data(), 240);
  require(replacement->speakerQueue.size() == 1, "new session may restart host generation with a fresh owner");
  DualSenseHaptics::detach(7);
  std::atomic<uint64_t> nearWrap{std::numeric_limits<uint64_t>::max() - 1};
  require(allocateSpeakerOwner(nearWrap) == std::numeric_limits<uint64_t>::max(), "full64-bit owner width preserved");
  require(allocateSpeakerOwner(nearWrap) == 0 && allocateSpeakerOwner(nearWrap) == 0,
          "incarnation exhaustion stays disabled rather than wrapping");

  std::array<uint8_t, ML_SPEAKER_BIND_SIZE> binding{};
  require(MlSpeakerPcmBuildBinding(binding.data(), binding.size(), 15, UINT64_MAX), "binding builds full-width owner");
  require(binding[3] == 16 && MlSpeakerRead32(binding.data() + 4) == ML_SPEAKER_BIND_MAGIC &&
          MlSpeakerRead16(binding.data() + 8) == 15 && MlSpeakerRead64(binding.data() + 12) == UINT64_MAX,
          "binding uses exact legacy input header and owner bytes");
  require(!MlSpeakerPcmBuildBinding(binding.data(), binding.size(), 16, 1) &&
          !MlSpeakerPcmBuildBinding(binding.data(), binding.size(), 0, 0) &&
          !MlSpeakerPcmBuildBinding(binding.data(), binding.size() - 1, 0, 1),
          "binding rejects invalid slot owner and packet size");
}

static void testSpeakerRendererRouting() {
  const int16_t speaker[] = {1200, -1300, 2200, -2300};
  const int16_t actuators[] = {-3200, 3300, -4200, 4300};
  float output[8]{};
  MlSpeakerPcmRouteFourChannel(speaker, actuators, output, 2);
  require(output[0] == 1200.0f / 32768.0f && output[1] == -1300.0f / 32768.0f,
          "speaker left and right route only to endpoint channels one and two");
  require(output[2] == -3200.0f / 32768.0f && output[3] == 3300.0f / 32768.0f,
          "actuator left and right remain on endpoint channels three and four");
  require(output[4] == 2200.0f / 32768.0f && output[5] == -2300.0f / 32768.0f &&
              output[6] == -4200.0f / 32768.0f &&
              output[7] == 4300.0f / 32768.0f,
          "second frame preserves both independent streams");
  MlSpeakerPcmRouteFourChannel(nullptr, actuators, output, 2);
  require(output[0] == 0.0f && output[1] == 0.0f && output[2] != 0.0f,
          "actuator-only endpoint output silences speaker channels");
  MlSpeakerPcmRouteFourChannel(speaker, nullptr, output, 2);
  require(output[0] != 0.0f && output[1] != 0.0f && output[2] == 0.0f &&
              output[3] == 0.0f,
          "speaker-only endpoint output silences actuator channels");
}

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
    require(
        makeDualSenseTriggerReport(DS_EFFECT_LEFT_TRIGGER, 0, 0, left, right)
                .validFlag0 == 8,
        "single trigger update");

    // Exercise SDL's cross-platform SendEffect dispatch with a virtual Sony
    // gamepad. The real Windows HIDAPI driver supplies Bluetooth framing.
    std::array<uint8_t, 47> captured{};
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = 0x054c;
    desc.product_id = 0x0ce6;
    desc.name = "DualSense trigger test";
    desc.userdata = &captured;
    desc.SendEffect = [](void *data, const void *effect, int size) -> int {
      if (size != 47)
        return -1;
      memcpy(static_cast<std::array<uint8_t, 47> *>(data)->data(), effect,
             size);
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

int main(int argc, char** argv) {
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_GAMECONTROLLER);
    struct QuitSDL { ~QuitSDL() { SDL_Quit(); } } quitSDL;
    try {
        if (argc == 2 && std::strcmp(argv[1], "--submission-only") == 0) {
            testSpeakerSubmissionReset();
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--owner-only") == 0) {
            testSpeakerOwnerIdentity();
            return 0;
        }
        testAdmission();
        testSpeakerWireAndNegotiation();
        testSpeakerQueueLifecycle();
        testSpeakerRendererRouting();
        testSpeakerAdmissionContention();
        testSpeakerOwnerIdentity();
        testSpeakerSubmissionReset();
        if (argc == 2 && std::strcmp(argv[1], "--speaker-only") == 0) {
            std::puts("DualSense: speaker wire, negotiation, queue lifecycle and four-channel routing passed");
            return 0;
        }
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
        bool gotSilence = false;
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
                // Observe silence from the end of the right-channel effect.
                // Idle now follows the last nonzero sample, so Windows timer
                // granularity can put the final report before packet 110.
                if (packet >= 100 && std::all_of(report + 13, report + 77,
                                              [](uint8_t sample) { return sample == 0; }))
                    gotSilence = true;
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
        while (recorded.pop(report)) {
            gotSilence = true;
            for (unsigned i = 13; i < 77; ++i) require(report[i] == 0, "idle silence");
        }
        require(gotSilence, "stop report");
        // Exercise the Bluetooth worker with a native effect followed by an
        // endpoint that keeps submitting zeros. It must stop emitting reports
        // and release rumble priority without waiting for audio to close.
        wave.fill(0);
        for (unsigned frame = 0; frame < 240; ++frame)
            MlHapticsWrite16(wave.data() + frame * 4, 16000);
        for (unsigned packet = 120; packet < 124; ++packet) {
            DualSenseHaptics::receive(0, packet, wave.data(), 240);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        wave.fill(0);
        for (unsigned packet = 124; packet < 164; ++packet) {
            DualSenseHaptics::receive(0, packet, wave.data(), 240);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (packet == 153) {
                require(!DualSenseHaptics::playing(0), "continuous silence releases rumble priority");
                while (recorded.pop(report)) {}
            }
        }
        require(!DualSenseHaptics::playing(0) && !recorded.pop(report),
                "silent endpoint must not restart waveform output");

        // Overload without a worker: queue must remain bounded and retain latest.
        { std::lock_guard<std::mutex> lock(playback->mutex); playback->stopped = true; }
        playback->wake.notify_all(); playback->worker.join();
        { std::lock_guard<std::mutex> lock(playback->mutex); playback->stopped = false; }
        MlHapticsWrite16(wave.data(), 1);
        for (unsigned i = 0; i < 1000; ++i) DualSenseHaptics::receive(0, 164 + i, wave.data(), 240);
        require(playback->queue.size() <= 8 && playback->queue.back().sequence == 1163, "bounded backlog");
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
        std::puts("DualSense: trigger dispatch, packet CRC, stereo resampling, loss/duplicates, speaker packet/lifecycle/channel routing, silent endpoint rumble coexistence, idle/removal silence, bounded queue and output failure passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
