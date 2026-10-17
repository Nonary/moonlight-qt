#include "overlaymanager.h"
#include "path.h"
#include <exception>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>

using namespace Overlay;

namespace {
// Keep every row and graph lane, shrinking the complete CPU surface only when
// needed. This leaves graph-only and already-fitting overlays at native size.
SDL_Surface* fitToOutput(SDL_Surface* surface, int width, int height)
{
    if (!surface || width <= 0 || height <= 0 ||
            (surface->w <= width && surface->h <= height)) return surface;
    const double scale = std::min(double(width) / surface->w, double(height) / surface->h);
    const int fittedWidth = std::max(1, int(std::floor(surface->w * scale)));
    const int fittedHeight = std::max(1, int(std::floor(surface->h * scale)));
    auto* fitted = SDL_CreateRGBSurfaceWithFormat(0, fittedWidth, fittedHeight,
                                                  surface->format->BitsPerPixel, surface->format->format);
    if (fitted) {
        // Copy alpha together with color rather than blending into an empty
        // surface; the backend performs the final alpha composition.
        SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
        if (SDL_BlitScaled(surface, nullptr, fitted, nullptr) != 0) {
            SDL_FreeSurface(fitted);
            fitted = nullptr;
        }
    }
    SDL_FreeSurface(surface);
    return fitted;
}

// Wrap both masks with the same unoutlined font so long rows retain their
// letter shadows. Only glyphs and their narrow shadows cover the video.
SDL_Surface* renderStatsText(TTF_Font* font, const char* text, SDL_Color color, double scale)
{
    const int oldOutline = TTF_GetFontOutline(font);
    TTF_SetFontOutline(font, 0);
    const int wrap = int(std::lround(1024 * scale));
    auto* face = TTF_RenderUTF8_Blended_Wrapped(font, text, color, wrap);
    auto* shadow = TTF_RenderUTF8_Blended_Wrapped(font, text, {0, 0, 0, 255}, wrap);
    TTF_SetFontOutline(font, oldOutline);
    if (!face || !shadow) {
        SDL_FreeSurface(face); SDL_FreeSurface(shadow);
        return nullptr;
    }
    const int stroke = std::max(1, int(std::lround(scale)));
    const int offset = std::max(2, int(std::lround(2 * scale)));
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, face->w + 2 * stroke + offset,
        face->h + 2 * stroke + offset, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface) {
        SDL_FillRect(surface, nullptr, 0);
        const auto stamp = [&](SDL_Surface* mask, int x, int y) {
            SDL_Rect at{x, y, mask->w, mask->h};
            SDL_BlitSurface(mask, nullptr, surface, &at);
        };
        stamp(shadow, stroke + offset, stroke + offset);
        for (int y = -stroke; y <= stroke; y += stroke)
            for (int x = -stroke; x <= stroke; x += stroke)
                if (x || y) stamp(shadow, stroke + x, stroke + y);
        stamp(face, stroke, stroke);
    }
    SDL_FreeSurface(face); SDL_FreeSurface(shadow);
    return surface;
}

// CPU rasterization runs on the overlay worker. All backends receive the same
// composed surface through their current overlay upload path; no GPU API work
// or text rendering happens on the frame path.
// Draws the lanes above the optional stats text surface, which it consumes.
SDL_Surface* composeTimingGraph(TTF_Font* font, SDL_Surface* text, const TimingGraphSnapshot& snapshot, double scale)
{
    const auto& points = snapshot.points;
    using Layout = TimingGraphLayout;
    const size_t first = points.size() > size_t(Layout::Frames) ? points.size() - Layout::Frames : 0;
    const size_t count = points.size() - first;
    if (count < 2) return text;
    const auto S = [scale](double value) { return int(std::lround(value * scale)); };

    std::array<uint64_t, Layout::Frames> planned{};
    size_t plannedCount = 0;
    for (size_t i = first; i < points.size(); ++i) {
        uint64_t interval;
        if (timingGraphInterval(points, i, TimingGraphLane::Target, interval)) planned[plannedCount++] = interval;
    }
    uint64_t nominal = 8333;
    if (plannedCount) {
        auto middle = planned.begin() + plannedCount / 2;
        std::nth_element(planned.begin(), middle, planned.begin() + plannedCount);
        nominal = *middle; // Axis placement only; plotted intervals stay raw.
    }
    const double minimum = std::max(0.0, double(nominal) - Layout::RadiusUs);
    const double maximum = double(nominal) + Layout::RadiusUs;

    const int textWidth = text ? text->w : 0, textHeight = text ? text->h : 0;
    const int width = std::max(textWidth, S(Layout::Width)), height = S(Layout::Height);
    auto surface = SDL_CreateRGBSurfaceWithFormat(0, width, textHeight + height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return text;
    SDL_FillRect(surface, nullptr, 0);
    if (text) {
        SDL_Rect textPosition{0, height, text->w, text->h};
        SDL_BlitSurface(text, nullptr, surface, &textPosition);
        SDL_FreeSurface(text);
    }
    SDL_Rect background{0, 0, S(Layout::Width), height};
    SDL_FillRect(surface, &background, SDL_MapRGBA(surface->format, 8, 12, 18, 235));

    const SDL_Color planColor{190, 198, 210, 255}, submitColor{65, 215, 250, 255};
    const SDL_Color displayColor{245, 110, 220, 255}, labelColor{225, 230, 235, 255};
    const SDL_Color gridColor{45, 53, 65, 255}, referenceColor{105, 113, 125, 255}, clipColor{255, 95, 65, 255};
    const auto label = [&](const char* value, int x, int y, SDL_Color c) {
        auto words = TTF_RenderUTF8_Blended(font, value, c);
        if (words) { SDL_Rect dest{x, y, words->w, words->h}; SDL_BlitSurface(words, nullptr, surface, &dest); SDL_FreeSurface(words); }
    };
    const auto line = [&](int x0, int y0, int x1, int y1, SDL_Color c, int thickness = 1) {
        const Uint32 pixel = SDL_MapRGBA(surface->format, c.r, c.g, c.b, c.a);
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            for (int stroke = 0; stroke < thickness; ++stroke) {
                const int y = y0 + stroke;
                if (x0 >= 0 && x0 < surface->w && y >= 0 && y < height)
                    reinterpret_cast<Uint32*>(static_cast<Uint8*>(surface->pixels) + y * surface->pitch)[x0] = pixel;
            }
            if (x0 == x1 && y0 == y1) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
    };

    char caption[160];
    std::snprintf(caption, sizeof(caption), "Frametimes: %zu frames, reference %.2f ms", count, nominal / 1000.0);
    label(caption, S(10), S(4), labelColor);
    label("ms", S(14), S(28), labelColor);

    const int left = S(Layout::Left), right = S(Layout::Width - Layout::RightMargin);
    const int stroke = std::max(1, S(1.5));
    const auto xAt = [&](size_t i) {
        return left + int((i - first + Layout::Frames - count) * size_t(right - left) / (Layout::Frames - 1));
    };
    const TimingGraphLane lanes[Layout::Lanes] = {TimingGraphLane::Target, TimingGraphLane::Submit, TimingGraphLane::Display};
    const SDL_Color colors[Layout::Lanes] = {planColor, submitColor, displayColor};
    const char* names[Layout::Lanes] = {"Planned cadence", "Client submissions", "Display events"};
    // DXGI's matched refresh is useful measured timing, but it does not
    // timestamp an interval-zero flip in the middle of active scanout.
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        if (it->generation != points.back().generation) break;
        if (!it->displayUs) continue;
        if (it->displayKind == TimingGraphDisplayKind::ConfirmedRefresh)
            names[2] = "Display refresh (DXGI)";
        break;
    }
    for (int lane = 0; lane < Layout::Lanes; ++lane) {
        const int top = S(Layout::plotTop(lane)), bottom = S(Layout::plotBottom(lane));
        const auto yAt = [&](double v) {
            return int(std::lround(bottom - (std::clamp(v, minimum, maximum) - minimum) / (maximum - minimum) * (bottom - top)));
        };
        size_t valid = 0;
        double latest = 0, peak = 0;
        for (size_t i = first; i < points.size(); ++i) {
            uint64_t interval;
            if (!timingGraphInterval(points, i, lanes[lane], interval)) continue;
            latest = double(interval); peak = std::max(peak, latest); ++valid;
        }
        if (!valid) std::snprintf(caption, sizeof(caption), "%s: unavailable", names[lane]);
        else if (lanes[lane] == TimingGraphLane::Display)
            std::snprintf(caption, sizeof(caption), "%s: %.2f ms, %zu matched, peak %.2f", names[lane], latest / 1000.0, valid, peak / 1000.0);
        else std::snprintf(caption, sizeof(caption), "%s: %.2f ms, peak %.2f", names[lane], latest / 1000.0, peak / 1000.0);
        label(caption, left, S(Layout::titleTop(lane)), colors[lane]);
        for (int tick = 0; tick <= 2; ++tick) {
            const double v = minimum + (maximum - minimum) * tick / 2;
            const int y = yAt(v);
            line(left, y, right, y, gridColor);
            char number[32]; std::snprintf(number, sizeof(number), "%.1f", v / 1000.0);
            label(number, S(8), y - S(8), labelColor);
        }
        const int referenceY = yAt(double(nominal));
        for (int x = left; x < right; x += S(8)) line(x, referenceY, std::min(x + S(3), right), referenceY, referenceColor);

        bool havePrevious = false;
        int previousX = 0, previousY = 0;
        for (size_t i = first; i < points.size(); ++i) {
            uint64_t interval;
            if (!timingGraphInterval(points, i, lanes[lane], interval)) { havePrevious = false; continue; }
            const double v = double(interval);
            const bool flat = std::abs(v - double(nominal)) <= Layout::FlatUs;
            const int x = xAt(i), y = yAt(flat ? double(nominal) : v);
            if (havePrevious) line(previousX, previousY, x, y, colors[lane], stroke);
            else line(x, y, x + 1, y, colors[lane], stroke);
            if (v < minimum || v > maximum) {
                const int inward = v > maximum ? S(4) : -S(4);
                line(x - S(3), y, x, y + inward, clipColor);
                line(x, y + inward, x + S(3), y, clipColor);
            }
            previousX = x; previousY = y; havePrevious = true;
        }
    }
    // The input lane uses signed offsets from the cadence slot rather than
    // intervals. Keep its own axis: network delivery can be several ms early,
    // and the actual buffer allowance varies independently of frame rate.
    const SDL_Color networkColor{255, 185, 70, 255}, decodeColor{160, 145, 255, 255};
    const SDL_Color bufferColor{85, 225, 135, 255}, outputColor{135, 145, 160, 255};
    double inputMinimum = -2000, inputMaximum = 2000;
    const auto& latestPoint = points.back();
    const bool adaptiveInput = latestPoint.inputPercentileEnabled;
    const auto latestInput = bufferGraphSample(latestPoint);
    const SDL_Color targetColor{245, 230, 90, 255};
    for (size_t i = first; i < points.size(); ++i) {
        const auto s = bufferGraphSample(points[i]);
        if (!s.valid) continue;
        inputMaximum = std::max(inputMaximum, s.bufferLimitUs);
        if (points[i].inputPercentileEnabled) inputMaximum = std::max(inputMaximum, double(points[i].inputTargetUs));
        const auto include = [&](double v) {
            inputMinimum = std::min(inputMinimum, v);
            inputMaximum = std::max(inputMaximum, v);
        };
        if (s.producerValid) include(s.producerUs);
        if (s.networkValid) include(s.networkUs);
        if (s.readyValid) include(s.decoderReadyUs);
        else if (s.outputValid) include(s.decoderOutputUs);
    }
    inputMinimum = std::floor(inputMinimum / 1000) * 1000;
    inputMaximum = std::ceil(inputMaximum / 1000) * 1000;
    const int inputTop = S(Layout::plotTop(Layout::BufferLane));
    const int inputBottom = S(Layout::plotBottom(Layout::BufferLane));
    const auto inputY = [&](double v) {
        return int(std::lround(inputBottom - (std::clamp(v, inputMinimum, inputMaximum) - inputMinimum) /
                               (inputMaximum - inputMinimum) * (inputBottom - inputTop)));
    };
    if (adaptiveInput) {
        if (!latestInput.producerValid)
            std::snprintf(caption, sizeof(caption), latestInput.valid ? "Frame lateness: not measured (timing/capacity unqualified)" :
                          "Frame lateness: not measured (source timing unavailable)");
        else if (latestInput.producerAbsorbed())
            std::snprintf(caption, sizeof(caption), "Frame lateness: covered (%.2f / %.2f ms)",
                          latestInput.producerUs / 1000, latestInput.bufferLimitUs / 1000);
        else
            std::snprintf(caption, sizeof(caption), "Frame lateness: exceeds allowance by %.2f ms",
                          (latestInput.producerUs - latestInput.bufferLimitUs) / 1000);
    }
    else if (!latestInput.valid)
        std::snprintf(caption, sizeof(caption), "Stage readiness / buffer: unavailable");
    else if (!latestInput.readyValid)
        std::snprintf(caption, sizeof(caption), "Stage readiness: decoder completion unknown");
    else if (latestInput.absorbed())
        std::snprintf(caption, sizeof(caption), "Stage readiness: covered (%.2f / %.2f ms)",
                      latestInput.decoderReadyUs / 1000, latestInput.bufferUs / 1000);
    else
        std::snprintf(caption, sizeof(caption), "Stage readiness: exceeds buffer by %.2f ms",
                      (latestInput.decoderReadyUs - latestInput.bufferUs) / 1000);
    label(caption, left, S(Layout::titleTop(Layout::BufferLane)), labelColor);
    for (int tick = 0; tick <= 2; ++tick) {
        const double v = inputMinimum + (inputMaximum - inputMinimum) * tick / 2;
        const int y = inputY(v);
        line(left, y, right, y, gridColor);
        char number[32]; std::snprintf(number, sizeof(number), "%.1f", v / 1000);
        label(number, S(8), y - S(8), labelColor);
    }
    for (int x = left; x < right; x += S(8))
        line(x, inputY(0), std::min(x + S(3), right), inputY(0), referenceColor);

    const SDL_Color allowanceColor{145, 225, 180, 255};
    const SDL_Color inputColors[] = {networkColor, decodeColor, outputColor, targetColor, bufferColor, allowanceColor};
    const auto drawInputSeries = [&](int series) {
        bool havePrevious = false;
        int previousX = 0, previousY = 0;
        for (size_t i = first; i < points.size(); ++i) {
            const auto& point = points[i];
            const auto sample = bufferGraphSample(point);
            const bool adaptive = point.inputPercentileEnabled;
            const bool valid = sample.valid && (series == 0 ? sample.networkValid :
                series == 1 ? (adaptive ? sample.producerValid : sample.readyValid) :
                series == 2 ? (adaptive ? sample.readyValid || sample.outputValid : sample.outputValid && !sample.readyValid) :
                series == 3 ? adaptive && point.inputTargetValid :
                series == 5 ? adaptive && point.inputToleranceUs != 0 : true);
            if (!valid) { havePrevious = false; continue; }
            if (point.breakBefore) havePrevious = false;
            const double v = series == 0 ? sample.networkUs :
                series == 1 ? (adaptive ? sample.producerUs : sample.decoderReadyUs) :
                series == 2 ? (sample.readyValid ? sample.decoderReadyUs : sample.decoderOutputUs) :
                series == 3 ? double(point.inputTargetUs) : series == 5 ? sample.bufferLimitUs : sample.bufferUs;
            const int x = xAt(i), y = inputY(v);
            if (series != 5 || (i - first) % 2 == 0) {
                if (havePrevious) line(previousX, previousY, x, y, inputColors[series], series == 5 ? 1 : stroke);
                else line(x, y, x + 1, y, inputColors[series], series == 5 ? 1 : stroke);
            }
            previousX = x; previousY = y; havePrevious = true;
        }
    };
    for (int series = 0; series < 4; ++series) drawInputSeries(series);
    drawInputSeries(5); // Tolerance allowance is distinct from actual buffer delay.
    // Exact eligible producer crossings in adaptive mode. Preserve raw stage
    // coverage in legacy mode and in the independently labelled history.
    for (size_t i = first; i < points.size(); ++i) {
        const auto& point = points[i];
        const auto sample = bufferGraphSample(point);
        if (point.inputPercentileEnabled ? !sample.producerMissed() : !sample.readyValid || sample.absorbed()) continue;
        const int x = xAt(i), y = inputY(point.inputPercentileEnabled ? sample.producerUs : sample.decoderReadyUs);
        line(x - S(2), y - S(2), x + S(2), y + S(2), clipColor, stroke);
        line(x - S(2), y + S(2), x + S(2), y - S(2), clipColor, stroke);
    }
    // Dense exact crossings can round to the same pixel as the buffer.
    // Keep the allowance visible without erasing the X arms above/below it.
    drawInputSeries(4);
    const int legendY = inputBottom + S(8);
    label("Network", left, legendY, networkColor);
    label(adaptiveInput ? "Frame lateness" : "Decode ready", left + S(130), legendY, decodeColor);
    label(adaptiveInput ? "Ready/output" : "Output only", left + S(325), legendY, outputColor);
    label("Buffer", left + S(490), legendY, bufferColor);
    if (adaptiveInput) label("Target", left + S(590), legendY, targetColor);
    std::snprintf(caption, sizeof(caption), adaptiveInput ?
        "Buffer budget %.2f ms | Learned target %.2f ms%s%s" : "Buffer budget %.2f ms | Request %.2f ms%s%s",
        latestPoint.bufferUs / 1000.0,
        (adaptiveInput ? latestPoint.inputTargetUs : latestPoint.requestedBufferUs) / 1000.0,
        adaptiveInput && !latestPoint.inputTargetValid ? " (learning/unknown)" : "",
        latestPoint.inputTargetCapped ? " (capped)" : "");
    label(caption, left, height - S(180), labelColor);
    std::snprintf(caption, sizeof(caption), "Retiming budget %.2f ms | Combined budget %.2f ms",
        latestPoint.extraDelayUs / 1000.0,
        (latestPoint.bufferUs + latestPoint.extraDelayUs) / 1000.0);
    label(caption, left, height - S(160), labelColor);
    if (adaptiveInput) {
        std::snprintf(caption, sizeof(caption), "Tolerance %.2f ms | Dashed buffer + tolerance %.2f ms",
            latestPoint.inputToleranceUs / 1000.0, latestInput.bufferLimitUs / 1000.0);
        label(caption, left, height - S(140), labelColor);
        std::snprintf(caption, sizeof(caption), "Coverage target %.2f%% / %llu s | Pacing carry %.2f ms",
            latestPoint.inputProbabilityPpm / 10000.0,
            static_cast<unsigned long long>(latestPoint.inputWindowUs / 1000000),
            latestPoint.trajectoryCarryUs / 1000.0);
        label(caption, left, height - S(120), labelColor);
        char coverage[32];
        if (latestPoint.inputSamples)
            std::snprintf(coverage, sizeof(coverage), "%.2f%%",
                100.0 * (latestPoint.inputSamples - std::min(latestPoint.inputSamples, latestPoint.inputMissed)) / latestPoint.inputSamples);
        else std::snprintf(coverage, sizeof(coverage), "N/A");
        std::snprintf(caption, sizeof(caption), "Coverage (%llus) %s | frames %llu | absorbed %llu | misses %llu",
            static_cast<unsigned long long>((latestPoint.inputCoverageWindowUs ?
                latestPoint.inputCoverageWindowUs : latestPoint.inputWindowUs) / 1000000), coverage,
            static_cast<unsigned long long>(latestPoint.inputSamples),
            static_cast<unsigned long long>(latestPoint.inputAbsorbed),
            static_cast<unsigned long long>(latestPoint.inputMissed));
        label(caption, left, height - S(100), labelColor);
    }
    const auto& hitches = snapshot.hitches;
    std::snprintf(caption, sizeof(caption), "Stage >%.2f ms (2m): covered %llu | late %llu",
        hitches.toleranceUs / 1000.0,
        static_cast<unsigned long long>(hitches.absorbed), static_cast<unsigned long long>(hitches.missed));
    label(caption, left, height - S(80), labelColor);
    std::snprintf(caption, sizeof(caption), "Stage unknown %llu | Worst excess %.2f ms | Measured %.0f%%",
        static_cast<unsigned long long>(hitches.unknown), hitches.worstExcessUs / 1000.0,
        hitches.observedFrames ? hitches.measuredFrames * 100.0 / hitches.observedFrames : 0.0);
    label(caption, left, height - S(60), labelColor);
    if (adaptiveInput && latestPoint.inputVrrHeadroomUs) {
        std::snprintf(caption, sizeof(caption), "VRR credit %.2f ms | Red X: beyond buffer + tolerance",
            latestPoint.inputVrrHeadroomUs / 1000.0);
        label(caption, left, height - S(40), labelColor);
    }
    else label(adaptiveInput ? "Red X: frame lateness > buffer + tolerance. Gray: GPU/queue." :
          "Raw stages include queue/GPU waits. Red X: stage > buffer.", left, height - S(40), labelColor);
    label("Extra = smoothing/recovery; total = buffer + extra.", left, height - S(20), labelColor);
    return surface;
}

}

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    m_Overlays[OverlayType::OverlayDebug].color = {0xD4, 0xFF, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
    m_TtfInitialized = true;
    try { m_Worker = std::thread(&OverlayManager::run, this); }
    catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Overlay worker creation failed: %s", e.what());
    }
}

OverlayManager::~OverlayManager()
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        m_Stopping = true;
    }
    m_WorkReady.notify_one();
    if (m_Worker.joinable()) m_Worker.join();
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    if (m_TtfInitialized) TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_Overlays[type].enabled;
}

bool OverlayManager::isStatsEnabled()
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_StatsEnabled;
}

bool OverlayManager::isTimingGraphEnabled()
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_TimingGraphEnabled;
}

void OverlayManager::setTimingGraphState(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        if (m_TimingGraphEnabled == enabled) return;
        m_TimingGraphEnabled = enabled;
        if (!enabled) m_TimingGraph.reset();
        auto& overlay = m_Overlays[OverlayDebug];
        overlay.enabled = m_StatsEnabled || m_TimingGraphEnabled;
        ++overlay.revision;
        overlay.dirty = true;
        overlay.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

void OverlayManager::setStatusMessage(StatusSource source, const std::string& text)
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& message = m_StatusMessages[static_cast<int>(source)];
        if (message == text) return;
        message = text;
        std::string combined = m_StatusMessages[static_cast<int>(StatusSource::Mouse)];
        if (combined.empty()) {
            combined = m_StatusMessages[static_cast<int>(StatusSource::Network)];
            const auto& packetLoss = m_StatusMessages[static_cast<int>(StatusSource::PacketLoss)];
            if (!packetLoss.empty() && packetLoss != combined) {
                if (!combined.empty()) combined += "\n\n";
                combined += packetLoss;
            }
            const auto& client = m_StatusMessages[static_cast<int>(StatusSource::ClientPacing)];
            if (!combined.empty() && !client.empty()) combined += "\n\n";
            combined += client;
        }
        auto& overlay = m_Overlays[OverlayStatusUpdate];
        if (combined == overlay.text && overlay.enabled == !combined.empty()) return;
        SDL_utf8strlcpy(overlay.text, combined.c_str(), sizeof(overlay.text));
        overlay.enabled = !combined.empty();
        ++overlay.revision;
        overlay.dirty = true;
        overlay.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

std::string OverlayManager::getOverlayText(OverlayType type)
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text, TimingGraphSnapshot graph)
{
    const auto snapshot = graph.points.empty() ? nullptr : std::make_shared<const TimingGraphSnapshot>(std::move(graph));
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[type];
        if (type == OverlayDebug && m_TimingGraphEnabled) m_TimingGraph = snapshot;
        SDL_utf8strlcpy(overlay.text, text, sizeof(overlay.text));
        ++overlay.revision;
        overlay.dirty = overlay.dirty || overlay.enabled;
        overlay.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

void OverlayManager::updateTimingGraph(TimingGraphSnapshot graph)
{
    if (graph.points.empty()) return;
    auto snapshot = std::make_shared<const TimingGraphSnapshot>(std::move(graph));
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[OverlayDebug];
        if (!m_TimingGraphEnabled) return;
        m_TimingGraph = std::move(snapshot);
        ++overlay.revision;
        overlay.dirty = true;
        overlay.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_Overlays[type].fontSize;
}

void OverlayManager::setOutputSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    const int fontSize = std::max(width, height) >= 3840 && std::min(width, height) >= 2160 ? 26 : 20;
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& stats = m_Overlays[OverlayDebug];
        if (stats.fontSize == fontSize && stats.outputWidth == width && stats.outputHeight == height) return;
        stats.fontSize = fontSize;
        stats.outputWidth = width;
        stats.outputHeight = height;
        ++stats.revision;
        stats.dirty = true;
        stats.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[type];
        if (type == OverlayDebug) {
            if (m_StatsEnabled == enabled) return;
            m_StatsEnabled = enabled;
            overlay.enabled = m_StatsEnabled || m_TimingGraphEnabled;
        }
        else {
            if (overlay.enabled == enabled) return;
            overlay.enabled = enabled;
        }
        if (!enabled) overlay.text[0] = 0;
        ++overlay.revision;
        overlay.dirty = true;
        overlay.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    // Wait for any callback into the previous renderer before allowing its
    // destruction. An in-progress CPU raster job is invalidated by revision.
    std::lock_guard<std::mutex> rendererLock(m_RendererLock);
    {
        std::lock_guard<std::mutex> stateLock(m_StateLock);
        m_Renderer = renderer;
        m_HaveRenderer = renderer != nullptr;
        for (auto& overlay : m_Overlays) {
            ++overlay.revision;
            overlay.dirty = m_HaveRenderer;
            overlay.queued = std::chrono::steady_clock::now();
        }
    }
    for (int i = 0; i < OverlayMax; ++i) SDL_FreeSurface(getUpdatedOverlaySurface(OverlayType(i)));
    m_WorkReady.notify_one();
}

void OverlayManager::run()
{
    using Clock = std::chrono::steady_clock;
    auto ns = [](Clock::duration d) { return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(); };
    unsigned next = 0;
    std::array<int, OverlayMax> rasterFontSizes{}; // Worker-owned font cache sizes.
    for (;;) {
        OverlayType type;
        char text[1024];
        bool enabled, drawText;
        int fontSize, outputWidth, outputHeight;
        std::shared_ptr<const TimingGraphSnapshot> graph;
        uint64_t revision;
        Clock::time_point queued;
        {
            std::unique_lock<std::mutex> lock(m_StateLock);
            m_WorkReady.wait(lock, [&] {
                if (m_Stopping) return true;
                if (!m_HaveRenderer) return false;
                for (const auto& overlay : m_Overlays) if (overlay.dirty) return true;
                return false;
            });
            if (m_Stopping) return;
            // One coalesced request per overlay; rotate to avoid starving status
            // messages if diagnostic text changes faster than it can be drawn.
            while (!m_Overlays[next].dirty) next = (next + 1) % OverlayMax;
            type = OverlayType(next);
            next = (next + 1) % OverlayMax;
            auto& overlay = m_Overlays[type];
            overlay.dirty = false;
            enabled = overlay.enabled;
            drawText = enabled && overlay.text[0] && (type != OverlayDebug || m_StatsEnabled);
            fontSize = overlay.fontSize;
            outputWidth = overlay.outputWidth;
            outputHeight = overlay.outputHeight;
            revision = overlay.revision;
            queued = overlay.queued;
            SDL_memcpy(text, overlay.text, sizeof(text));
            if (type == OverlayDebug && m_TimingGraphEnabled) graph = m_TimingGraph;
        }
        const auto started = Clock::now();
        auto& overlay = m_Overlays[type];
        SDL_Surface* surface = nullptr;
        const bool drawGraph = enabled && graph && graph->points.size() >= 2;
        if (drawText || drawGraph) {
            if ((!overlay.font || rasterFontSizes[type] != fontSize) && !m_FontData.isEmpty()) {
                auto resized = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()), 1, fontSize);
                if (!resized) continue;
                if (overlay.font) TTF_CloseFont(overlay.font);
                overlay.font = resized;
                rasterFontSizes[type] = fontSize;
            }
            const double scale = type == OverlayDebug ? fontSize / 20.0 : 1.0;
            if (drawText) {
                if (overlay.font) {
                    surface = type == OverlayDebug ? renderStatsText(overlay.font, text, overlay.color, scale) :
                        RenderTextOutlinedWrapped(overlay.font, text, overlay.color,
                            {0, 0, 0, 255}, int(std::lround(4 * scale)), int(std::lround(1024 * scale)));
                }
                if (!surface) continue; // Keep the last successful overlay on failure.
            }
            if (drawGraph && overlay.font) surface = composeTimingGraph(overlay.font, surface, *graph, scale);
        }
        if (type == OverlayDebug) surface = fitToOutput(surface, outputWidth, outputHeight);
        const auto rasterized = Clock::now();
        std::lock_guard<std::mutex> rendererLock(m_RendererLock);
        bool publish;
        {
            std::lock_guard<std::mutex> stateLock(m_StateLock);
            publish = !m_Stopping && m_Renderer && overlay.revision == revision;
            if (publish) surface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&overlay.surface, surface);
        }
        SDL_FreeSurface(surface); // Superseded result or previous unconsumed surface.
        if (publish) {
            const auto dispatch = Clock::now();
            m_Renderer->notifyOverlayUpdated(type);
            m_Renderer->recordOverlayTiming({type, revision, ns(started - queued),
                                            ns(rasterized - started), ns(Clock::now() - dispatch)});
        }
    }
}

SDL_Surface* OverlayManager::RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    int oldOutline = TTF_GetFontOutline(font);
    TTF_SetFontOutline(font, outlineWidth);

    // Verify that the string won't require wrapping (which could cause the outline and the text
    // to diverge due to different wrapping positions).
    //
    // FIXME: We do this rather than just disabling wrapping entirely (wrapWidth = 0) because we
    // need further testing to ensure that all renderers can handle non-NPOT overlay textures.
    for (const QString& line : QString(text).split('\n')) {
        int extent, count;
        if (TTF_MeasureUTF8(font, line.toUtf8(), wrapWidth, &extent, &count) == 0 && count < line.size()) {
            // If it requires wrapping, render it without the outline
            TTF_SetFontOutline(font, oldOutline);
            return TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
        }
    }

    // Draw text twice, but outline is a bit bigger
    auto outlineSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, outlineColor, wrapWidth);
    TTF_SetFontOutline(font, 0);
    auto textSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
    TTF_SetFontOutline(font, oldOutline);

    if (outlineSurface == nullptr || textSurface == nullptr) {
        SDL_FreeSurface(outlineSurface);
        SDL_FreeSurface(textSurface);
        return nullptr;
    }

    // Merge the texts
    SDL_Rect dst = { outlineWidth, outlineWidth, textSurface->w, textSurface->h };
    SDL_BlitSurface(textSurface, nullptr, outlineSurface, &dst);

    SDL_FreeSurface(textSurface);
    return outlineSurface;
}
