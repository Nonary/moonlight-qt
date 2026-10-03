// SPDX-License-Identifier: GPL-3.0-or-later
#include "dualsensehaptics.h"
#include <ControllerHaptics.h>

#if (defined(__linux__) || defined(_WIN32) || defined(MOONLIGHT_HAPTICS_TEST)) && SDL_VERSION_ATLEAST(2, 24, 0)
#include "dualsensehid.h"
#include "../../../third-party/saxense/packet.h"
#include <array>
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
constexpr auto period = std::chrono::nanoseconds(32000000000LL / SAXENSE_RATE);

struct Chunk {
    uint32_t sequence;
    uint16_t frames;
    Clock::time_point received;
    std::array<uint8_t, ML_HAPTICS_MAX_FRAMES * ML_HAPTICS_FRAME_BYTES> pcm;
};

struct Playback {
    std::unique_ptr<DualSenseHidOutput> output;
    SDL_GameController* controller = nullptr;
    SDL_AudioStream* converter = nullptr;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Chunk> queue;
    std::thread worker;
    bool stopped = false;
    bool failed = false;
    bool waveformMode = false;
    Clock::time_point lastReceived {};

    ~Playback() {
        { std::lock_guard<std::mutex> lock(mutex); stopped = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
        if (converter) SDL_FreeAudioStream(converter);
    }

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
            const bool idle = now - lastReceived > std::chrono::milliseconds(60);
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

std::mutex registryMutex;
std::array<std::shared_ptr<Playback>, 16> registry;
}

bool DualSenseHaptics::attach(unsigned slot, SDL_GameController* controller) {
    if (slot >= registry.size()) return false;
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
    std::shared_ptr<Playback> retired;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        if (slot < registry.size()) retired = std::move(registry[slot]);
    }
    // Join before the SDL controller is closed. receive() never retains owners.
}

bool DualSenseHaptics::playing(unsigned slot) {
    std::lock_guard<std::mutex> lock(registryMutex);
    if (slot >= registry.size() || !registry[slot]) return false;
    auto& p = *registry[slot];
    std::lock_guard<std::mutex> guard(p.mutex);
    return !p.failed && (p.waveformMode || !p.queue.empty());
}

void DualSenseHaptics::receive(uint16_t slot, uint32_t sequence, const uint8_t* pcm, uint16_t frames) {
    if (!pcm || frames == 0 || frames > ML_HAPTICS_MAX_FRAMES) return;
    std::lock_guard<std::mutex> lock(registryMutex);
    if (slot >= registry.size() || !registry[slot]) return;
    auto& p = *registry[slot];
    std::lock_guard<std::mutex> guard(p.mutex);
    if (p.stopped || p.failed) return;
    // Overflow discards old audio; bounded memory and bounded playout latency.
    if (p.queue.size() >= 8) p.queue.clear();
    Chunk chunk {};
    chunk.sequence = sequence; chunk.frames = frames;
    chunk.received = p.lastReceived = Clock::now();
    memcpy(chunk.pcm.data(), pcm, frames * 4);
    p.queue.push_back(chunk);
    p.wake.notify_all();
}
#else
bool DualSenseHaptics::attach(unsigned, SDL_GameController*) { return false; }
void DualSenseHaptics::detach(unsigned) {}
bool DualSenseHaptics::playing(unsigned) { return false; }
void DualSenseHaptics::receive(uint16_t, uint32_t, const uint8_t*, uint16_t) {}
#endif
