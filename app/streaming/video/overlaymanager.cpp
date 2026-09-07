#include "overlaymanager.h"
#include "path.h"
#include <exception>

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
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

std::string OverlayManager::getOverlayText(OverlayType type)
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[type];
        SDL_utf8strlcpy(overlay.text, text, sizeof(overlay.text));
        ++overlay.revision;
        overlay.dirty = overlay.dirty || overlay.enabled;
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
    return m_Overlays[type].fontSize;
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
        if (overlay.enabled == enabled) return;
        overlay.enabled = enabled;
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
    for (;;) {
        OverlayType type;
        char text[1024];
        bool enabled;
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
            revision = overlay.revision;
            queued = overlay.queued;
            SDL_memcpy(text, overlay.text, sizeof(text));
        }
        const auto started = Clock::now();
        auto& overlay = m_Overlays[type];
        SDL_Surface* surface = nullptr;
        if (enabled && text[0]) {
            if (!overlay.font && !m_FontData.isEmpty()) {
                overlay.font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                              1, overlay.fontSize);
            }
            if (overlay.font) surface = RenderTextOutlinedWrapped(overlay.font, text, overlay.color,
                                                                  {0, 0, 0, 255}, 4, 1024);
            if (!surface) continue; // Keep the last successful overlay on failure.
        }
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

