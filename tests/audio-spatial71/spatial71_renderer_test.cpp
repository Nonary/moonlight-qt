#define SDL_MAIN_HANDLED
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <Windows.h>
#include <SDL.h>
#include "../../app/streaming/audio/renderers/spatialaudio.h"
#include "../../app/streaming/audio/renderers/spatialaudioqueue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int FramesPerPacket = 240;
constexpr int PacketBytes = FramesPerPacket * 8 * static_cast<int>(sizeof(float));

void json(const QJsonObject& object)
{
    const auto line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::printf("%s\n", line.constData());
    std::fflush(stdout);
}

void help()
{
    std::puts("Spatial71RendererTest - actual optional Moonlight renderer, no host required\n"
              "  --help                         Does not open any audio device\n"
              "  --list-endpoints               Read-only HDMI/DisplayPort endpoint inventory\n"
              "  --invalid-endpoint-test        Tests rejected configuration/device ID only\n"
              "  --endpoint-id ID --silence|--tones [--seconds 60] [--cycles 1]\n"
              "  --default-endpoint --silence|--tones [--seconds 60] [--cycles 1]\n"
              "      [--gap-at-ms 10000 --gap-ms 500]\n"
              "Native tests require an explicit endpoint ID or --default-endpoint. The latter\n"
              "uses the current default only if it is HDMI/DisplayPort; it never guesses another\n"
              "device. No Windows default or system setting is changed. Try silence first. Tones\n"
              "are one channel/second: FL FR FC LFE BL BR SL SR, peak -30 dBFS.\n"
              "stdout contains producer JSON; stderr contains actual renderer diagnostics.\n"
              "A PASS proves API execution, not audibility, Dolby/MAT, physical latency or A/V sync.");
}

int listEndpoints()
{
    QString error;
    const auto endpoints = SpatialAudioRenderer::enumerateEndpoints(&error);
    QJsonArray devices;
    for (const auto& endpoint : endpoints) {
        devices.append(QJsonObject{{"id", endpoint.id}, {"name", endpoint.name}, {"is_default", endpoint.isDefault}});
    }
    json({{"event", "endpoint_inventory"}, {"result", error.isEmpty() ? "PASS" : "FAIL"},
          {"endpoints", devices}, {"error", error}, {"active_endpoint_opened", false}});
    return error.isEmpty() ? 0 : 1;
}

OPUS_MULTISTREAM_CONFIGURATION configuration()
{
    OPUS_MULTISTREAM_CONFIGURATION result{};
    result.sampleRate = 48000;
    result.channelCount = 8;
    result.samplesPerFrame = FramesPerPacket;
    // The harness supplies already-decoded PCM, exactly like the IAudioRenderer
    // boundary. No Opus decoder, network stack, Session or video path is simulated.
    return result;
}

int invalidEndpointTest()
{
    const QString invalidId = QStringLiteral("Spatial71-test-invalid-endpoint-not-a-device");
    auto config = configuration();
    constexpr int caseCount = 7;
    for (int test = 0; test < caseCount; ++test) {
        auto candidate = config;
        if (test == 1) candidate.channelCount = 2;
        if (test == 2) candidate.channelCount = 6;
        if (test == 3) candidate.sampleRate = 44100;
        if (test == 4) candidate.samplesPerFrame = 0;
        if (test == 5) candidate.samplesPerFrame = static_cast<int>(Spatial71::MaxDecodeFrames + 1);
        // Unsupported configurations must be rejected before resolving a
        // default device. The valid-format case uses an impossible device ID.
        SpatialAudioRenderer renderer(test == 0 ? invalidId : QString{});
        if (renderer.prepareForPlayback(test == 6 ? nullptr : &candidate)) {
            json({{"event", "invalid_endpoint_test"}, {"result", "FAIL"}, {"case", test}});
            return 1;
        }
        int bytes = PacketBytes;
        if (renderer.getAudioBuffer(&bytes) == nullptr || renderer.submitAudio(0)) {
            json({{"event", "failure_propagation_test"}, {"result", "FAIL"}, {"case", test}});
            return 1;
        }
    }
    json({{"event", "invalid_endpoint_test"}, {"result", "PASS"}, {"cases", caseCount},
          {"active_endpoint_opened", false}, {"limitation", "Does not exercise Session factory fallback or native Spatial playback"}});
    return 0;
}

class DeadlineTimer {
public:
    DeadlineTimer()
    {
        m_Handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        m_HighResolution = m_Handle != nullptr;
        if (!m_Handle) m_Handle = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~DeadlineTimer() { if (m_Handle) CloseHandle(m_Handle); }
    bool valid() const { return m_Handle != nullptr; }
    bool highResolution() const { return m_HighResolution; }
    bool until(Clock::time_point deadline)
    {
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) return true;
            LARGE_INTEGER due{};
            due.QuadPart = -(std::max)(1LL, static_cast<long long>(remaining / 100));
            if (!SetWaitableTimer(m_Handle, &due, 0, nullptr, nullptr, FALSE)) return false;
            if (WaitForSingleObject(m_Handle, INFINITE) != WAIT_OBJECT_0) return false;
        }
    }
private:
    HANDLE m_Handle = nullptr;
    bool m_HighResolution = false;
};

struct Options {
    QString endpoint;
    bool useDefault = false;
    bool tones = false;
    int seconds = 60;
    int cycles = 1;
    int gapAtMs = 0;
    int gapMs = 0;
};

bool parse(const QStringList& args, Options& options)
{
    bool modeSet = false;
    for (int index = 1; index < args.size(); ++index) {
        const auto option = args[index];
        if (option == "--silence" || option == "--tones") {
            if (modeSet) return false;
            modeSet = true;
            options.tones = option == "--tones";
        }
        else if (option == "--endpoint-id") {
            if (++index >= args.size() || !options.endpoint.isEmpty() || options.useDefault) return false;
            options.endpoint = args[index];
            if (options.endpoint.trimmed().isEmpty()) return false;
        }
        else if (option == "--default-endpoint") {
            if (options.useDefault || !options.endpoint.isEmpty()) return false;
            options.useDefault = true;
        }
        else {
            if (++index >= args.size()) return false;
            bool ok = false;
            const int value = args[index].toInt(&ok);
            if (!ok) return false;
            if (option == "--seconds" && value >= 1 && value <= 3600) options.seconds = value;
            else if (option == "--cycles" && value >= 1 && value <= 20) options.cycles = value;
            else if (option == "--gap-at-ms" && value >= 1 && value <= 3599999) options.gapAtMs = value;
            else if (option == "--gap-ms" && value >= 1 && value <= 30000) options.gapMs = value;
            else return false;
        }
    }
    return modeSet && (options.useDefault || !options.endpoint.trimmed().isEmpty()) &&
            ((options.gapAtMs == 0 && options.gapMs == 0) ||
             (options.gapAtMs > 0 && options.gapMs > 0 && options.gapAtMs + options.gapMs < options.seconds * 1000));
}

void fill(float* destination, std::uint64_t firstFrame, bool tones)
{
    std::fill_n(destination, FramesPerPacket * 8, 0.0f);
    if (!tones) return;
    constexpr double pi = 3.14159265358979323846;
    constexpr double gain = 0.03162277660168379; // -30 dBFS peak
    for (std::uint64_t frame = 0; frame < FramesPerPacket; ++frame) {
        const auto absolute = firstFrame + frame;
        const auto channel = (absolute / 48000) % 8;
        const auto phaseFrame = absolute % 48000;
        const double fade = (std::min)(1.0, static_cast<double>((std::min)(phaseFrame, 47999 - phaseFrame)) / 480.0);
        const double frequency = channel == 3 ? 60.0 : 440.0;
        destination[frame * 8 + channel] = static_cast<float>(gain * fade * std::sin(2.0 * pi * frequency * static_cast<double>(phaseFrame) / 48000.0));
    }
}

int run(const Options& options)
{
    const QString selection = options.useDefault ? QStringLiteral("current-default-HDMI") : options.endpoint;
    for (int cycle = 1; cycle <= options.cycles; ++cycle) {
        DeadlineTimer timer;
        if (!timer.valid()) return 1;
        auto renderer = std::make_unique<SpatialAudioRenderer>(options.endpoint);
        auto config = configuration();
        if (!renderer->prepareForPlayback(&config)) {
            json({{"event", "renderer_start"}, {"result", "FAIL"}, {"cycle", cycle}, {"endpoint", selection}});
            return 1;
        }
        if (renderer->getAudioBufferFormat() != IAudioRenderer::AudioFormat::Float32NE) return 1;
        json({{"event", "renderer_start"}, {"result", "PASS"}, {"cycle", cycle},
              {"endpoint", selection}, {"mode", options.tones ? "tones" : "silence"},
              {"packet_frames", FramesPerPacket}, {"high_resolution_producer_timer", timer.highResolution()}});
        const auto start = Clock::now();
        auto next = start;
        const auto end = start + std::chrono::seconds(options.seconds);
        std::uint64_t sentFrames = 0;
        std::uint64_t packets = 0;
        std::uint64_t latePeriods = 0;
        double maximumLateMs = 0;
        bool gapDone = false;
        bool success = true;
        while (next < end) {
            if (!timer.until(next)) { success = false; break; }
            auto now = Clock::now();
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (!gapDone && options.gapMs && elapsedMs >= options.gapAtMs) {
                gapDone = true;
                json({{"event", "intentional_producer_gap"}, {"duration_ms", options.gapMs}, {"cycle", cycle}});
                if (!timer.until(now + std::chrono::milliseconds(options.gapMs))) { success = false; break; }
                now = Clock::now();
                next = now;
            }
            if (now >= end) break;
            const double lateMs = std::chrono::duration<double, std::milli>(now - next).count();
            maximumLateMs = (std::max)(maximumLateMs, lateMs);
            if (lateMs >= 5.0) { ++latePeriods; next = now; } // no accumulated catch-up bursts
            int size = PacketBytes;
            auto* buffer = static_cast<float*>(renderer->getAudioBuffer(&size));
            if (!buffer || size < PacketBytes) { success = false; break; }
            fill(buffer, sentFrames, options.tones);
            if (!renderer->submitAudio(PacketBytes)) { success = false; break; }
            sentFrames += FramesPerPacket;
            ++packets;
            next += std::chrono::milliseconds(5);
        }
        const double durationMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        success = renderer->submitAudio(0) && success;
        // Destructor joins the real renderer worker and prints its queue/event
        // counters. This tool does not substitute a fake COM renderer.
        const auto stopStart = Clock::now();
        renderer.reset();
        const double stopMs = std::chrono::duration<double, std::milli>(Clock::now() - stopStart).count();
        json({{"event", "producer_run"}, {"result", success ? "PASS" : "FAIL"}, {"cycle", cycle},
              {"frames_submitted", static_cast<qint64>(sentFrames)}, {"packets_submitted", static_cast<qint64>(packets)},
              {"duration_ms", durationMs}, {"max_producer_lateness_ms", maximumLateMs},
              {"late_producer_periods", static_cast<qint64>(latePeriods)}, {"stop_join_ms", stopMs},
              {"intentional_gap_ms", gapDone ? options.gapMs : 0},
              {"latency_scope", "Software producer scheduling and renderer shutdown only; no acoustic latency or A/V measurement"}});
        if (!success) return 1;
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const auto args = application.arguments();
    if (args.size() == 1 || (args.size() == 2 && args[1] == "--help")) { help(); return 0; }
    if (args.size() == 2 && args[1] == "--list-endpoints") return listEndpoints();
    if (args.size() == 2 && args[1] == "--invalid-endpoint-test") return invalidEndpointTest();
    Options options;
    if (!parse(args, options)) { help(); std::fputs("Invalid arguments; no audio device opened.\n", stderr); return 2; }
    return run(options);
}
