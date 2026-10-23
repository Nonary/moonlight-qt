// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
// Before SDL, whose headers can lower WINVER below the CM property APIs.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <devpropdef.h>
#include <cfgmgr32.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <future>
#include <vector>
#endif
#include "dualsensehaptics.h"
#include <ControllerHaptics.h>
#include <ControllerSpeakerPcm.h>

#if (defined(__linux__) || defined(_WIN32) || defined(MOONLIGHT_HAPTICS_TEST)) && SDL_VERSION_ATLEAST(2, 24, 0)
#include "dualsensehid.h"
#include "../../../third-party/saxense/packet.h"
#include <array>
#include <atomic>
#include <limits>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto staleChunk = std::chrono::milliseconds(40);
constexpr auto idleTimeout = std::chrono::milliseconds(60);
constexpr auto period = std::chrono::nanoseconds(32000000000LL / SAXENSE_RATE);

struct Chunk {
    uint32_t sequence;
    uint16_t frames;
    Clock::time_point received;
    std::array<uint8_t, ML_HAPTICS_MAX_FRAMES * ML_HAPTICS_FRAME_BYTES> pcm;
};

struct SpeakerChunk {
  uint32_t generation;
  uint32_t sequence;
  uint64_t samplePosition;
  uint16_t frames;
  uint16_t flags;
  Clock::time_point received;
  std::array<uint8_t, ML_SPEAKER_PCM_MAX_FRAMES * ML_SPEAKER_PCM_FRAME_BYTES>
      pcm;
};

// Input side shared by every transport. Workers own conversion and pacing.
std::atomic<uint64_t> nextSpeakerOwnerToken{0};
uint64_t allocateSpeakerOwner(std::atomic<uint64_t>& counter) {
  auto previous = counter.load(std::memory_order_relaxed);
  for (;;) {
    if (previous == std::numeric_limits<uint64_t>::max()) return 0;
    if (counter.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed))
      return previous + 1;
  }
}

struct Backend {
    std::atomic<uint64_t> speakerOwnerToken{allocateSpeakerOwner(nextSpeakerOwnerToken)};
    // Dedicated device-submission gate. Never acquire it under registryMutex.
    std::mutex speakerSubmissionMutex;
    std::mutex mutex;
    std::deque<Chunk> queue;
    std::deque<SpeakerChunk> speakerQueue;
    bool stopped = false;
    bool failed = false;
    bool waveformMode = false;
    Clock::time_point lastNonzero {};
    uint32_t latestSequence = 0;
    bool haveInputSequence = false;
    uint32_t speakerGeneration = 0;
    uint32_t speakerLatestSequence = 0;
    uint64_t speakerNextSamplePosition = 0;
    bool haveSpeakerSequence = false;
    bool haveSpeakerSamplePosition = false;
    Clock::time_point lastSpeakerPacket{};

    virtual ~Backend() = default;
    virtual void notify() = 0; // Called with mutex held.
    virtual bool speakerOutputReady() const { return false; }

    template<class Submit>
    bool submitSpeakerFrame(uint64_t owner, Submit submit) {
        std::lock_guard<std::mutex> submission(speakerSubmissionMutex);
        // The callback performs the actual device submission while this gate
        // is held. A stale owner must render no speaker samples, but may still
        // submit independent actuator output.
        return submit(owner != 0 && owner == speakerOwnerToken.load());
    }

    void resetSpeakerOwner() {
        std::lock_guard<std::mutex> submission(speakerSubmissionMutex);
        std::lock_guard<std::mutex> guard(mutex);
        speakerOwnerToken = allocateSpeakerOwner(nextSpeakerOwnerToken);
        speakerQueue.clear();
        speakerGeneration = 0;
        haveSpeakerSequence = false;
        haveSpeakerSamplePosition = false;
        notify();
    }

    bool playing() {
        std::lock_guard<std::mutex> guard(mutex);
        return !failed && (waveformMode || !queue.empty());
    }

    void receive(uint32_t sequence, const uint8_t* pcm, uint16_t frames) {
        std::lock_guard<std::mutex> guard(mutex);
        if (stopped || failed) return;
        // Filter before rumble priority or idle timers are changed. Ingress
        // retains sequence across idle silence, so delayed PCM cannot restart
        // an effect the host has already ended. Unsigned subtraction wraps.
        if (haveInputSequence && int32_t(sequence - latestSequence) <= 0) return;
        latestSequence = sequence;
        haveInputSequence = true;
        const auto now = Clock::now();
        const bool nonzero = std::any_of(pcm, pcm + frames * 4u,
                                        [](uint8_t sample) { return sample != 0; });
        if (nonzero) lastNonzero = now;
        // An open USB audio endpoint can send zeros indefinitely. Once the
        // effect has drained, those packets must neither acquire rumble
        // priority nor keep writing silence over ordinary rumble commands.
        if (!nonzero && !waveformMode && queue.empty()) return;
        // Overflow discards old audio; bounded memory and bounded playout latency.
        if (queue.size() >= 8) queue.clear();
        Chunk chunk {};
        chunk.sequence = sequence; chunk.frames = frames;
        chunk.received = now;
        memcpy(chunk.pcm.data(), pcm, frames * 4);
        queue.push_back(chunk);
        notify();
    }

    void receiveSpeaker(uint64_t owner, uint32_t generation, uint32_t sequence,
                        uint64_t samplePosition, uint16_t flags,
                        const uint8_t *pcm, uint16_t frames) {
      // Dropping one bounded audio block is preferable to stalling control.
      std::unique_lock<std::mutex> guard(mutex, std::try_to_lock);
      if (!guard.owns_lock()) return;
      if (!owner || owner != speakerOwnerToken.load()) return;
      if (stopped || failed || !speakerOutputReady() || !pcm || frames == 0 ||
          frames > ML_SPEAKER_PCM_MAX_FRAMES || generation == 0 ||
          (flags & ~ML_SPEAKER_PCM_FLAG_DISCONTINUITY) != 0)
        return;

      if (speakerGeneration != generation) {
        if (speakerGeneration != 0 &&
            int32_t(generation - speakerGeneration) <= 0)
          return;
        speakerGeneration = generation;
        speakerQueue.clear();
        haveSpeakerSequence = false;
        haveSpeakerSamplePosition = false;
      }
      if (haveSpeakerSequence && int32_t(sequence - speakerLatestSequence) <= 0)
        return;
      if (haveSpeakerSamplePosition &&
          samplePosition < speakerNextSamplePosition &&
          !(flags & ML_SPEAKER_PCM_FLAG_DISCONTINUITY))
        return;

      SpeakerChunk chunk{};
      chunk.generation = generation;
      chunk.sequence = sequence;
      chunk.samplePosition = samplePosition;
      chunk.frames = frames;
      chunk.flags = flags;
      chunk.received = Clock::now();
      memcpy(chunk.pcm.data(), pcm, frames * ML_SPEAKER_PCM_FRAME_BYTES);
      if ((flags & ML_SPEAKER_PCM_FLAG_DISCONTINUITY) ||
          (haveSpeakerSamplePosition &&
           samplePosition != speakerNextSamplePosition)) {
        // Keep the new packet but remove queued pre-gap samples. The render
        // worker also clears its local FIFO when it observes this boundary.
        speakerQueue.clear();
        chunk.flags |= ML_SPEAKER_PCM_FLAG_DISCONTINUITY;
      }
      if (speakerQueue.size() >= 8) {
        speakerQueue.clear();
        chunk.flags |= ML_SPEAKER_PCM_FLAG_DISCONTINUITY;
      }
      speakerLatestSequence = sequence;
      haveSpeakerSequence = true;
      speakerNextSamplePosition = samplePosition + frames;
      haveSpeakerSamplePosition = true;
      lastSpeakerPacket = chunk.received;
      speakerQueue.push_back(chunk);
      notify();
    }
};

struct Playback final : Backend {
    std::unique_ptr<DualSenseHidOutput> output;
    SDL_GameController* controller = nullptr;
    SDL_AudioStream* converter = nullptr;
    std::condition_variable wake;
    std::thread worker;

    ~Playback() override {
        { std::lock_guard<std::mutex> lock(mutex); stopped = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
        if (converter) SDL_FreeAudioStream(converter);
    }

    void notify() override { wake.notify_all(); }

    bool write(uint8_t sequence, const uint8_t* samples) {
        uint8_t report[SAXENSE_REPORT_BYTES];
        saxense_packet(report, sequence, samples);
        return output->write(report, sizeof(report));
    }

    void run() {
        uint8_t sequence = 0;
        uint32_t expected = 0;
        bool haveSequence = false;
        bool active = false;
        auto deadline = Clock::now();
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopped && !failed) {
            if (!active) {
                wake.wait(lock, [&] { return stopped || !queue.empty(); });
                if (stopped) break;
                // Clear SDL's remembered emulated rumble so later LED writes
                // cannot inadvertently disable waveform playback.
                lock.unlock();
                SDL_GameControllerRumble(controller, 0, 0, 0);
                lock.lock();
                active = true;
                waveformMode = true;
                deadline = Clock::now();
            }
            const auto now = Clock::now();
            while (!queue.empty()) {
                auto chunk = queue.front();
                queue.pop_front();
                if (now - chunk.received > std::chrono::milliseconds(40)) {
                    SDL_AudioStreamClear(converter); haveSequence = false; continue;
                }
                if (haveSequence && chunk.sequence != expected) {
                    // Drop late/duplicate packets. On loss, discard resampler
                    // history rather than replaying samples from before the gap.
                    if (int32_t(chunk.sequence - expected) < 0) continue;
                    SDL_AudioStreamClear(converter);
                }
                expected = chunk.sequence + 1; haveSequence = true;
                if (SDL_AudioStreamAvailable(converter) > 4 * SAXENSE_PCM_BYTES)
                    SDL_AudioStreamClear(converter);
                if (SDL_AudioStreamPut(converter, chunk.pcm.data(), chunk.frames * 4) < 0) {
                    failed = true; break;
                }
            }
            if (failed) break;
            if (Clock::now() < deadline) {
                wake.wait_until(lock, deadline, [&] { return stopped; });
                continue;
            }
            uint8_t samples[SAXENSE_PCM_BYTES] {};
            if (SDL_AudioStreamAvailable(converter) >= SAXENSE_PCM_BYTES &&
                SDL_AudioStreamGet(converter, samples, sizeof(samples)) != sizeof(samples)) {
                failed = true; break;
            }
            const bool idle = now - lastNonzero > std::chrono::milliseconds(60);
            if (idle) {
                SDL_AudioStreamClear(converter);
                memset(samples, 0, sizeof(samples));
                active = false; haveSequence = false;
            }
            lock.unlock();
            const bool success = write(sequence++, samples);
            lock.lock();
            failed = !success;
            if (!active) waveformMode = false;
            // Do not burst old samples after a scheduler stall.
            deadline = std::max(deadline + period, Clock::now());
        }
        queue.clear();
        lock.unlock();
        uint8_t silence[SAXENSE_PCM_BYTES] {};
        if (active && !failed) write(sequence++, silence);
        if (failed) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "DualSense waveform output failed; reconnect the controller to retry");
        // std::thread does not run SDL_CreateThread's TLS cleanup wrapper.
        SDL_TLSCleanup();
    }
};

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// Defined locally so this translation unit needs no INITGUID instantiation.
const DEVPROPKEY deviceInstanceIdKey =
    {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
const DEVPROPKEY deviceContainerIdKey =
    {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const PROPERTYKEY endpointContainerIdKey =
    {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const GUID noContainer = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
const GUID floatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// The HID interface and the USB audio function share one device container.
bool containerForInterface(const char* path, GUID& container) {
    const int chars = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (chars <= 0) return false;
    std::wstring interfacePath(chars, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, &interfacePath[0], chars) != chars) return false;
    DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
    WCHAR instance[MAX_DEVICE_ID_LEN] {};
    ULONG size = sizeof(instance);
    if (CM_Get_Device_Interface_PropertyW(interfacePath.c_str(), &deviceInstanceIdKey, &type,
                                          reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING) return false;
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    size = sizeof(container);
    return CM_Get_DevNode_PropertyW(node, &deviceContainerIdKey, &type,
                                    reinterpret_cast<PBYTE>(&container), &size, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_GUID && !IsEqualGUID(container, noContainer);
}

// USB: Windows exposes the actuators as channels 3/4 of the controller's
// four-channel render endpoint, the same path native PC games use.
struct WasapiPlayback final : Backend {
    SDL_GameController* controller = nullptr;
    GUID container {};
    HANDLE dataEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker;

    ~WasapiPlayback() override {
        { std::lock_guard<std::mutex> lock(mutex); stopped = true; }
        if (dataEvent) SetEvent(dataEvent);
        if (worker.joinable()) worker.join();
        if (dataEvent) CloseHandle(dataEvent);
    }

    void notify() override { SetEvent(dataEvent); }
    bool speakerOutputReady() const override { return true; }

    bool open(ComPtr<IAudioClient>& client, ComPtr<IAudioRenderClient>& render,
              HANDLE audioEvent, UINT32& bufferFrames) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDeviceCollection> endpoints;
        UINT count = 0;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)) ||
            FAILED(endpoints->GetCount(&count))) return false;
        ComPtr<IMMDevice> device;
        for (UINT i = 0; i < count && !device; ++i) {
            ComPtr<IMMDevice> candidate;
            ComPtr<IPropertyStore> properties;
            if (FAILED(endpoints->Item(i, &candidate)) ||
                FAILED(candidate->OpenPropertyStore(STGM_READ, &properties))) continue;
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(endpointContainerIdKey, &value)) &&
                value.vt == VT_CLSID && value.puuid && IsEqualGUID(*value.puuid, container)) device = candidate;
            PropVariantClear(&value);
        }
        if (!device) return false; // Bluetooth or audio function disabled.

        WAVEFORMATEX* mix = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client.GetAddressOf()))) ||
            FAILED(client->GetMixFormat(&mix))) return false;
        const WORD channels = mix->nChannels;
        const DWORD mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ?
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask : 0;
        CoTaskMemFree(mix);
        if (channels != 4) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "DualSense audio endpoint has %u channels; set it to 4 channels in Sound settings for waveform haptics",
                        channels);
            return false;
        }

        WAVEFORMATEXTENSIBLE format {};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = 4;
        format.Format.nSamplesPerSec = 48000;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = 4 * sizeof(float);
        format.Format.nAvgBytesPerSec = 48000 * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = mask;
        format.SubFormat = floatSubtype;
        // The engine converts rate and sample format only. Matching the mix
        // channel mask means no matrixing can move haptics into channels 1/2.
        return SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                            20 * 10000, 0, &format.Format, nullptr)) &&
            SUCCEEDED(client->SetEventHandle(audioEvent)) &&
            SUCCEEDED(client->GetBufferSize(&bufferFrames)) &&
            SUCCEEDED(client->GetService(IID_PPV_ARGS(&render)));
    }

    void run(std::promise<bool>& ready) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        {
            ComPtr<IAudioClient> client;
            ComPtr<IAudioRenderClient> render;
            UINT32 bufferFrames = 0;
            const bool opened = SUCCEEDED(com) && audioEvent && dataEvent &&
                open(client, render, audioEvent, bufferFrames) && SUCCEEDED(client->Start());
            ready.set_value(opened);
            if (opened) {
                play(client.Get(), render.Get(), audioEvent, bufferFrames);
                client->Stop();
            }
        }
        if (audioEvent) CloseHandle(audioEvent);
        if (SUCCEEDED(com)) CoUninitialize();
        SDL_TLSCleanup();
    }

    void play(IAudioClient* client, IAudioRenderClient* render, HANDLE audioEvent, UINT32 bufferFrames) {
        constexpr size_t prebufferFrames = 480;   // 10 ms absorbs packet jitter.
        constexpr size_t maxFrames = 48 * 40;     // Bound queued playout latency.
        constexpr auto prebufferTimeout = std::chrono::milliseconds(10);
        std::vector<int16_t> actuatorFifo;
        std::vector<int16_t> speakerFifo;
        actuatorFifo.reserve((maxFrames + ML_HAPTICS_MAX_FRAMES) * 2);
        speakerFifo.reserve((maxFrames + ML_SPEAKER_PCM_MAX_FRAMES) * 2);
        std::vector<int16_t> actuatorFrame(bufferFrames * 2);
        std::vector<int16_t> speakerFrame(bufferFrames * 2);
        std::vector<float> outputFrame(bufferFrames * 4);
        uint32_t expected = 0;
        bool haveSequence = false;
        bool active = false;
        bool started = false;
        auto startBy = Clock::now();
        uint64_t renderedSpeakerOwner = 0;
        uint32_t speakerGeneration = 0;
        uint64_t speakerExpectedPosition = 0;
        bool haveSpeakerPosition = false;
        bool speakerActive = false;
        bool speakerStarted = false;
        auto speakerStartBy = Clock::now();
        auto speakerLastPacket = Clock::time_point{};
        const HANDLE events[] = {dataEvent, audioEvent};
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopped && !failed) {
            // Reconnect can rotate a retained endpoint without recreating this
            // worker. Discard its private FIFO at the next synchronized turn.
            if (renderedSpeakerOwner != speakerOwnerToken) {
              renderedSpeakerOwner = speakerOwnerToken;
              speakerFifo.clear();
              speakerGeneration = 0;
              haveSpeakerPosition = false;
              speakerActive = speakerStarted = false;
              speakerLastPacket = {};
            }
            if (!active && !queue.empty()) {
                // Clear SDL's remembered emulated rumble, as on Bluetooth.
                lock.unlock();
                SDL_GameControllerRumble(controller, 0, 0, 0);
                lock.lock();
                active = true; started = false;
                waveformMode = true;
                startBy = Clock::now() + prebufferTimeout;
            }
            if (!speakerActive && !speakerQueue.empty()) {
              speakerActive = true;
              speakerStarted = false;
              speakerStartBy = Clock::now() + prebufferTimeout;
            }
            const auto now = Clock::now();
            while (!queue.empty()) {
                const Chunk& chunk = queue.front();
                const bool stale = now - chunk.received > staleChunk;
                // Drop late/duplicate packets and discard buffered samples across
                // a discontinuity rather than replaying pre-loss feedback.
                const bool late = haveSequence && int32_t(chunk.sequence - expected) < 0;
                if (stale || (!late && haveSequence && chunk.sequence != expected)) {
                  actuatorFifo.clear();
                  haveSequence = false;
                }
                if (!stale && !late) {
                    expected = chunk.sequence + 1; haveSequence = true;
                    for (unsigned i = 0; i < chunk.frames * 2u; ++i)
                      actuatorFifo.push_back(
                          int16_t(MlHapticsRead16(chunk.pcm.data() + i * 2)));
                }
                queue.pop_front();
            }
            while (!speakerQueue.empty()) {
              const SpeakerChunk &chunk = speakerQueue.front();
              const bool stale = now - chunk.received > staleChunk;
              if (chunk.generation != speakerGeneration) {
                speakerFifo.clear();
                speakerGeneration = chunk.generation;
                haveSpeakerPosition = false;
              }
              const bool discontinuity =
                  (chunk.flags & ML_SPEAKER_PCM_FLAG_DISCONTINUITY) != 0;
              if (stale || discontinuity ||
                  (haveSpeakerPosition &&
                   chunk.samplePosition != speakerExpectedPosition)) {
                speakerFifo.clear();
                haveSpeakerPosition = false;
              }
              if (!stale) {
                speakerExpectedPosition = chunk.samplePosition + chunk.frames;
                haveSpeakerPosition = true;
                for (unsigned i = 0; i < chunk.frames * 2u; ++i)
                  speakerFifo.push_back(
                      int16_t(MlSpeakerRead16(chunk.pcm.data() + i * 2)));
                speakerLastPacket = chunk.received;
                speakerActive = true;
              }
              speakerQueue.pop_front();
            }
            if (actuatorFifo.size() > maxFrames * 2)
              actuatorFifo.erase(actuatorFifo.begin(),
                                 actuatorFifo.end() - prebufferFrames * 2);
            if (speakerFifo.size() > maxFrames * 2)
              speakerFifo.erase(speakerFifo.begin(),
                                speakerFifo.end() - prebufferFrames * 2);
            if (active && now - lastNonzero > idleTimeout) {
                // Already-submitted samples drain; afterwards the engine renders silence.
                actuatorFifo.clear();
                active = false; started = false; haveSequence = false;
                waveformMode = false;
            }
            if (speakerActive && speakerLastPacket != Clock::time_point{} &&
                now - speakerLastPacket > idleTimeout) {
              speakerFifo.clear();
              speakerActive = false;
              speakerStarted = false;
              haveSpeakerPosition = false;
            }
            lock.unlock();

            bool success = true;
            if (active && !started &&
                (actuatorFifo.size() >= prebufferFrames * 2 || now >= startBy))
              started = true;
            if (speakerActive && !speakerStarted &&
                (speakerFifo.size() >= prebufferFrames * 2 ||
                 now >= speakerStartBy))
              speakerStarted = true;
            success = submitSpeakerFrame(renderedSpeakerOwner, [&](bool ownerMatches) {
              if (!ownerMatches) {
                speakerFifo.clear();
                speakerActive = speakerStarted = false;
                haveSpeakerPosition = false;
              }
              if ((started && !actuatorFifo.empty()) ||
                  (speakerStarted && !speakerFifo.empty())) {
                UINT32 padding = 0;
                BYTE *buffer = nullptr;
                success = SUCCEEDED(client->GetCurrentPadding(&padding)) &&
                          padding <= bufferFrames;
                const UINT32 availableFrames = std::max<UINT32>(
                    started ? UINT32(actuatorFifo.size() / 2) : 0,
                    speakerStarted ? UINT32(speakerFifo.size() / 2) : 0);
                const UINT32 frames =
                    success ? std::min<UINT32>(bufferFrames - padding,
                                               availableFrames)
                            : 0;
                if (frames) {
                  success = SUCCEEDED(render->GetBuffer(frames, &buffer));
                  if (success) {
                    const size_t samples = frames * 2u;
                    std::fill_n(speakerFrame.begin(), samples, int16_t{});
                    std::fill_n(actuatorFrame.begin(), samples, int16_t{});
                    const size_t speakerSamples =
                        speakerStarted ? std::min(samples, speakerFifo.size())
                                       : 0;
                    const size_t actuatorSamples =
                        started ? std::min(samples, actuatorFifo.size()) : 0;
                    std::copy_n(speakerFifo.begin(), speakerSamples,
                                speakerFrame.begin());
                    std::copy_n(actuatorFifo.begin(), actuatorSamples,
                                actuatorFrame.begin());
                    MlSpeakerPcmRouteFourChannel(speakerFrame.data(),
                                                 actuatorFrame.data(),
                                                 outputFrame.data(), frames);
                    std::memcpy(buffer, outputFrame.data(),
                                frames * 4u * sizeof(float));
                    success = SUCCEEDED(render->ReleaseBuffer(frames, 0));
                    if (speakerSamples)
                      speakerFifo.erase(speakerFifo.begin(),
                                        speakerFifo.begin() + speakerSamples);
                    if (actuatorSamples)
                      actuatorFifo.erase(actuatorFifo.begin(),
                                         actuatorFifo.begin() + actuatorSamples);
                  }
                }
              }
              return success;
            });
            // While active, also wake for engine periods and idle/prebuffer
            // deadlines. Idle workers sleep until a packet or stop request.
            const DWORD wait = (active || speakerActive)
                                   ? WaitForMultipleObjects(2, events, FALSE, 5)
                                   : WaitForSingleObject(dataEvent, INFINITE);

            lock.lock();
            failed = !success || wait == WAIT_FAILED;
        }
        queue.clear();
        speakerQueue.clear();
        waveformMode = false;
        lock.unlock();
        if (failed) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "DualSense waveform output failed; reconnect the controller to retry");
    }
};
#endif

std::mutex registryMutex;
std::array<std::shared_ptr<Backend>, 16> registry;
std::array<uint32_t, 16> speakerGenerationFloor{};
std::array<std::weak_ptr<Backend>, 16> speakerGenerationOwner{};
#ifdef _WIN32
std::shared_ptr<Backend> openUsb(SDL_GameController* controller, const char* path) {
    auto playback = std::make_shared<WasapiPlayback>();
    if (!playback->dataEvent || !containerForInterface(path, playback->container)) return nullptr;
    playback->controller = controller;
    std::promise<bool> ready;
    auto opened = ready.get_future();
    playback->worker = std::thread([p = playback.get(), ready = std::move(ready)]() mutable { p->run(ready); });
    if (!opened.get()) return nullptr; // Destructor joins the finished worker.
    return playback;
}
#endif
}

bool DualSenseHaptics::attach(unsigned slot, SDL_GameController* controller) {
    if (slot >= registry.size()) return false;
#ifdef _WIN32
    if (controller && SDL_GameControllerGetVendor(controller) == 0x054c &&
        (SDL_GameControllerGetProduct(controller) == 0x0ce6 ||
         SDL_GameControllerGetProduct(controller) == 0x0df2)) {
        const SDL_JoystickGUID guid = SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller));
        const char* path = SDL_GameControllerPath(controller);
        if ((guid.data[0] | guid.data[1] << 8) == 0x0003 && path) { // SDL_HARDWARE_BUS_USB
            {
                std::lock_guard<std::mutex> lock(registryMutex);
                if (registry[slot]) return false;
            }
            auto playback = openUsb(controller, path);
            if (!playback) return false;
            std::lock_guard<std::mutex> lock(registryMutex);
            if (registry[slot]) return false;
            registry[slot] = std::move(playback);
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "DualSense USB waveform backend ready for slot %u (WASAPI)", slot);
            return true;
        }
    }
#endif
    auto playback = std::make_shared<Playback>();
    playback->output = openDualSenseBluetoothOutput(controller);
    if (!playback->output) return false;
    playback->converter = SDL_NewAudioStream(AUDIO_S16LSB, 2, 48000, AUDIO_S8, 2, SAXENSE_RATE);
    if (!playback->converter) return false;
    playback->controller = controller;
    std::lock_guard<std::mutex> lock(registryMutex);
    if (registry[slot]) return false; // Single-controller merging is ambiguous.
    playback->worker = std::thread([p = playback.get()] { p->run(); });
    registry[slot] = std::move(playback);
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "DualSense Bluetooth waveform backend ready for slot %u (SAxense)", slot);
    return true;
}

void DualSenseHaptics::detach(unsigned slot) {
    std::shared_ptr<Backend> retired;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        if (slot < registry.size()) retired = std::move(registry[slot]);
    }
    // Join before the SDL controller is closed. receive() never retains owners.
}

bool DualSenseHaptics::playing(unsigned slot) {
    std::lock_guard<std::mutex> lock(registryMutex);
    if (slot >= registry.size() || !registry[slot]) return false;
    return registry[slot]->playing();
}

uint64_t DualSenseHaptics::speakerOwner(unsigned slot) {
  std::lock_guard<std::mutex> lock(registryMutex);
  if (slot >= registry.size() || !registry[slot]) return 0;
  return registry[slot]->speakerOwnerToken;
}

bool DualSenseHaptics::speakerReady(unsigned slot) {
  std::lock_guard<std::mutex> lock(registryMutex);
  if (slot >= registry.size() || !registry[slot])
    return false;
  return registry[slot]->speakerOutputReady();
}

void DualSenseHaptics::resetSpeakerGenerationFloor() {
  std::array<std::shared_ptr<Backend>, 16> retained;
  {
    std::lock_guard<std::mutex> lock(registryMutex);
    retained = registry;
  }
  // Waiting for in-flight device I/O must not hold the global registry lock.
  // Each retained owner survives this wait through its shared_ptr snapshot.
  for (auto &backend : retained) {
    if (backend) backend->resetSpeakerOwner();
  }
  // Clear admission floors only after rotations. Packets arriving during the
  // reset cannot leave an old-generation floor behind for the new owner.
  std::lock_guard<std::mutex> lock(registryMutex);
  speakerGenerationFloor.fill(0);
  speakerGenerationOwner.fill({});
}

void DualSenseHaptics::receive(uint16_t slot, uint32_t sequence, const uint8_t* pcm, uint16_t frames) {
    if (!pcm || frames == 0 || frames > ML_HAPTICS_MAX_FRAMES) return;
    std::lock_guard<std::mutex> lock(registryMutex);
    if (slot >= registry.size() || !registry[slot]) return;
    registry[slot]->receive(sequence, pcm, frames);
}

void DualSenseHaptics::receiveSpeaker(uint16_t slot, uint64_t owner, uint32_t generation,
                                      uint32_t sequence,
                                      uint64_t samplePosition, uint16_t flags,
                                      const uint8_t *pcm, uint16_t frames) {
  if (!pcm || frames == 0 || frames > ML_SPEAKER_PCM_MAX_FRAMES)
    return;
  std::unique_lock<std::mutex> lock(registryMutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  if (slot >= registry.size() || !registry[slot])
    return;
  if (!owner || registry[slot]->speakerOwnerToken != owner) return;
  auto &floor = speakerGenerationFloor[slot];
  if (speakerGenerationOwner[slot].lock() != registry[slot]) floor = 0;
  if (floor != 0) {
    if (int32_t(generation - floor) < 0)
      return;
    if (generation == floor &&
        speakerGenerationOwner[slot].lock() != registry[slot])
      return;
  }
  floor = generation;
  speakerGenerationOwner[slot] = registry[slot];
  registry[slot]->receiveSpeaker(owner, generation, sequence, samplePosition, flags,
                                 pcm, frames);
}
#else
bool DualSenseHaptics::attach(unsigned, SDL_GameController*) { return false; }
void DualSenseHaptics::detach(unsigned) {}
bool DualSenseHaptics::playing(unsigned) { return false; }
void DualSenseHaptics::receive(uint16_t, uint32_t, const uint8_t*, uint16_t) {}
bool DualSenseHaptics::speakerReady(unsigned) { return false; }
void DualSenseHaptics::resetSpeakerGenerationFloor() {}
uint64_t DualSenseHaptics::speakerOwner(unsigned) { return 0; }
void DualSenseHaptics::receiveSpeaker(uint16_t, uint64_t, uint32_t, uint32_t, uint64_t,
                                      uint16_t, const uint8_t *, uint16_t) {}
#endif
