#include "streaming/video/ffmpeg.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

static bool clockReady = true;
extern "C" bool __wrap_LiGetCaptureClockOffset(int64_t* offsetUs, uint64_t* uncertaintyUs)
{
    *offsetUs = 0;
    *uncertaintyUs = 250;
    return clockReady;
}

static void checkOverlay(FFmpegVideoDecoder& decoder, VIDEO_STATS& stats, bool detailed,
                         const std::string& endpoint, bool timingVisible = true)
{
    char output[4096];
    decoder.stringifyVideoStats(stats, output, sizeof(output), detailed);
    const std::string text(output);
    const std::string label("End-to-End Latency (estimate): ");
    assert(!text.empty() && text.back() == '\n');
    const auto lastStart = text.rfind('\n', text.size() - 2);
    if (timingVisible) {
        assert(lastStart != std::string::npos);
        assert(text.compare(lastStart + 1, label.size(), label) == 0);
        assert(text.find(label) == lastStart + 1);
        assert(text.substr(lastStart + 1).find(endpoint) != std::string::npos);
        assert(text.find("Incoming smoothness: ") < lastStart);
    }
    else {
        assert(text.find(label) == std::string::npos);
    }
    if (detailed) {
        const auto advanced = text.find("GPU wait within rendering: ");
        assert(advanced != std::string::npos);
        if (timingVisible) assert(advanced <= lastStart);
    }
    else {
        assert(text.find("VRR buffer: ") != std::string::npos);
    }
}

int main()
{
    unsetenv("MOONLIGHT_CAPTURE_TIMING");
    FFmpegVideoDecoder decoder(true);
    decoder.m_VideoFormat = VIDEO_FORMAT_PYROWAVE;
    decoder.m_Pacer = new Pacer(nullptr);
    decoder.m_Pacer->m_Shutdown = true; // The fixture creates no renderer or producer threads.
    decoder.m_Pacer->m_CaptureLatencyRequested = true;
    auto& telemetry = decoder.m_Pacer->m_Telemetry;

    VIDEO_STATS stats{};
    stats.vrrTelemetryActive = true;
    stats.vrrStateSequence = 1;
    stats.vrrPresentedFrames = 1;
    stats.vrrReadiness.samples = 1;
    stats.vrrReadiness.intervalPolicy = true;
    stats.vrrOnTimeTargetPerMillion = 990000;
    for (const bool detailed : {false, true}) {
        checkOverlay(decoder, stats, detailed, "unavailable");
    }

    LiGetMicroseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    const uint64_t now = LiGetMicroseconds();
    assert(now > 24000);
    Overlay::TimingGraphInput submit;
    submit.submitted = submit.idValid = true;
    submit.submissionId = 1;
    submit.submissionUs = now - 18000;
    telemetry.recordCapturePresentation(submit, now - 24000, 0);
    for (const bool detailed : {false, true}) {
        checkOverlay(decoder, stats, detailed, "6.0 / 6.0 ms (submit only,");
    }

    Overlay::TimingGraphInput display;
    display.displayValid = true;
    display.displayId = 1;
    display.displayUs = now - 13000;
    using Vrr13::PresentationTimeKind;
    telemetry.recordCapturePresentation(display, 0, 50, PresentationTimeKind::DisplayEstimate);
    for (const bool detailed : {false, true}) {
        checkOverlay(decoder, stats, detailed, "11.0 / 11.0 ms (Gamescope predicted scanout,");
    }

    display.displayUs = now - 12000;
    telemetry.recordCapturePresentation(display, 0, 50, PresentationTimeKind::DisplayEvent);
    for (const bool detailed : {false, true}) {
        checkOverlay(decoder, stats, detailed, "12.0 / 12.0 ms (measured scanout,");
    }

    clockReady = false;
    checkOverlay(decoder, stats, true, "unavailable");
    setenv("MOONLIGHT_CAPTURE_TIMING", "0", 1);
    checkOverlay(decoder, stats, false, "disabled");
    checkOverlay(decoder, stats, true, "disabled");
    decoder.m_Pacer->m_CaptureLatencyRequested = false;
    checkOverlay(decoder, stats, false, "", false);
    std::cout << "Production video stats: exact final row and all endpoints passed (compact and advanced)\n";
}
