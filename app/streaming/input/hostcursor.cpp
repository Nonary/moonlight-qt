#include "input.h"
#include "hostcursormath.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include "streaming/streamutils.h"

// Client-side cursor (Vibeshine extension). The host reports its cursor's shape,
// visibility, clip and host-originated moves; in absolute mouse mode we draw it as
// the local OS cursor and the host leaves it out of the video.

// Shapes kept per session. Each unique shape is sent once per subscription.
#define HOST_CURSOR_MAX_SHAPES 64

// How often we may ask the host again for a shape a state names but we lack.
#define HOST_CURSOR_RESEND_INTERVAL_MS 1000

static HostCursorMath::Rect toMathRect(const SDL_Rect& rect)
{
    return { rect.x, rect.y, rect.w, rect.h };
}

void SdlInputHandler::startHostCursor()
{
    if (!(LiGetHostFeatureFlags() & LI_FF_HOST_CURSOR)) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Host supports the client-side cursor; subscribing");
    m_HostCursorSupported = true;
    updateHostCursorControl();
}

SDL_Rect SdlInputHandler::getVideoRegion()
{
    SDL_Rect src, dst;

    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;

    dst.x = dst.y = 0;
    SDL_GetWindowSize(m_Window, &dst.w, &dst.h);

    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
    return dst;
}

bool SdlInputHandler::isHostCursorDrawingWanted()
{
    // We draw the cursor whenever we are the one positioning it with absolute input.
    return m_HostCursorSupported && m_HostCursorReady && m_AbsoluteMouseMode && isCaptureActive() &&
           !(m_HostCursorState.flags & LI_HOST_CURSOR_FALLBACK);
}

bool SdlInputHandler::isHostCursorDrawnLocally()
{
    // While the host still composites the cursor (it has not processed our request
    // yet, or another client needs it in the video), the video cursor is the one shown.
    return isHostCursorDrawingWanted() &&
           (m_HostCursorState.flags & LI_HOST_CURSOR_VISIBLE) &&
           !(m_HostCursorState.flags & LI_HOST_CURSOR_COMPOSITED);
}

void SdlInputHandler::updateHostCursorControl()
{
    if (!m_HostCursorSupported) {
        return;
    }

    int flags = LI_HOST_CURSOR_CONTROL_SUBSCRIBE;
    if (isHostCursorDrawingWanted()) {
        flags |= LI_HOST_CURSOR_CONTROL_CLIENT_DRAWS;
    }

    if (flags != m_HostCursorControlSent && LiSendHostCursorControl((uint8_t)flags, 0) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Host cursor: %s draws the cursor",
                    (flags & LI_HOST_CURSOR_CONTROL_CLIENT_DRAWS) ? "client" : "host");
        m_HostCursorControlSent = flags;
    }
}

void SdlInputHandler::updateLocalCursor(int mouseInVideoRegion)
{
    if (!m_HostCursorSupported || !m_AbsoluteMouseMode || !isCaptureActive() || SDL_GetRelativeMouseMode()) {
        return;
    }

    if (mouseInVideoRegion < 0) {
        int mouseX, mouseY;
        SDL_GetMouseState(&mouseX, &mouseY);
        mouseInVideoRegion = SDL_GetMouseFocus() == m_Window && isMouseInVideoRegion(mouseX, mouseY);
    }

    if (isHostCursorDrawnLocally()) {
        SDL_Cursor* cursor = mouseInVideoRegion ? getHostSdlCursor() : nullptr;
        SDL_SetCursor(cursor != nullptr ? cursor : SDL_GetDefaultCursor());
        SDL_ShowCursor(SDL_ENABLE);
    }
    else {
        SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_ShowCursor((mouseInVideoRegion && m_MouseCursorCapturedVisibilityState == SDL_DISABLE) ? SDL_DISABLE : SDL_ENABLE);
    }
}

void SdlInputHandler::warpToHostCursor()
{
    // Only move the user's cursor when it is ours to move: the window is focused
    // and the cursor is over the video.
    if (!m_HostCursorHasPosition || !(SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS) ||
            SDL_GetMouseFocus() != m_Window) {
        return;
    }

    const LI_HOST_CURSOR_STATE& state = m_HostCursorState;
    if (m_HostCursorX < 0 || m_HostCursorY < 0 ||
            m_HostCursorX >= state.referenceWidth || m_HostCursorY >= state.referenceHeight) {
        // On another host monitor; absolute input cannot follow it there.
        return;
    }

    int mouseX, mouseY;
    SDL_GetMouseState(&mouseX, &mouseY);
    if (!isMouseInVideoRegion(mouseX, mouseY)) {
        return;
    }

    HostCursorMath::Point target = HostCursorMath::streamToWindow(toMathRect(getVideoRegion()),
                                                                  state.referenceWidth, state.referenceHeight,
                                                                  m_HostCursorX, m_HostCursorY);
    if (qAbs(target.x - mouseX) <= 1 && qAbs(target.y - mouseY) <= 1) {
        return;
    }

    SDL_WarpMouseInWindow(m_Window, target.x, target.y);
}

SDL_Cursor* SdlInputHandler::getHostSdlCursor()
{
    const uint32_t shapeId = m_HostCursorState.shapeId;
    auto shape = m_HostCursorShapes.constFind(shapeId);
    if (shapeId == 0 || shape == m_HostCursorShapes.constEnd()) {
        return nullptr;
    }

    // Match the cursor's size in the stream: host shape pixels -> stream pixels ->
    // window units. A new scale (resize, fullscreen) rebuilds the cursors.
    double scale = HostCursorMath::cursorScale(m_HostCursorState.scaleQ16, toMathRect(getVideoRegion()),
                                               m_HostCursorState.referenceWidth);
    if (scale != m_HostSdlCursorScale) {
        freeHostSdlCursors();
        m_HostSdlCursorScale = scale;
    }

    auto existing = m_HostSdlCursors.constFind(shapeId);
    if (existing != m_HostSdlCursors.constEnd()) {
        return existing.value();
    }

    int width = HostCursorMath::scaledDimension(shape->width, scale);
    int height = HostCursorMath::scaledDimension(shape->height, scale);
    std::vector<uint8_t> pixels = HostCursorMath::scaleBgra(shape->pixels.data(), shape->width, shape->height, width, height);
    int hotspotX = qBound(0, (int)qRound((double)shape->hotspotX * width / shape->width), width - 1);
    int hotspotY = qBound(0, (int)qRound((double)shape->hotspotY * height / shape->height), height - 1);

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), width, height, 32, width * 4,
                                                              SDL_PIXELFORMAT_BGRA32);
    if (surface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateRGBSurfaceWithFormatFrom() failed: %s",
                    SDL_GetError());
        return nullptr;
    }

    SDL_Cursor* cursor = SDL_CreateColorCursor(surface, hotspotX, hotspotY);
    SDL_FreeSurface(surface);
    if (cursor == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateColorCursor() failed: %s",
                    SDL_GetError());
        return nullptr;
    }

    m_HostSdlCursors.insert(shapeId, cursor);
    return cursor;
}

void SdlInputHandler::freeHostSdlCursor(uint32_t shapeId)
{
    auto it = m_HostSdlCursors.find(shapeId);
    if (it == m_HostSdlCursors.end()) {
        return;
    }

    if (SDL_GetCursor() == it.value()) {
        SDL_SetCursor(SDL_GetDefaultCursor());
    }
    SDL_FreeCursor(it.value());
    m_HostSdlCursors.erase(it);
}

void SdlInputHandler::freeHostSdlCursors()
{
    while (!m_HostSdlCursors.isEmpty()) {
        freeHostSdlCursor(m_HostSdlCursors.constBegin().key());
    }
}

void SdlInputHandler::handleHostCursorShape(const LI_HOST_CURSOR_SHAPE* shape)
{
    if (!m_HostCursorSupported) {
        return;
    }

    HostCursorShape copy;
    copy.width = shape->width;
    copy.height = shape->height;
    copy.hotspotX = shape->hotspotX;
    copy.hotspotY = shape->hotspotY;
    copy.pixels.assign(shape->pixels, shape->pixels + (size_t)shape->width * shape->height * 4);

    if (!m_HostCursorShapes.contains(shape->shapeId)) {
        m_HostCursorShapeOrder.append(shape->shapeId);
        while (m_HostCursorShapeOrder.size() > HOST_CURSOR_MAX_SHAPES) {
            uint32_t oldest = m_HostCursorShapeOrder.takeFirst();
            m_HostCursorShapes.remove(oldest);
            freeHostSdlCursor(oldest);
        }
    }
    m_HostCursorShapes.insert(shape->shapeId, std::move(copy));
    freeHostSdlCursor(shape->shapeId);

    if (shape->shapeId == m_HostCursorState.shapeId) {
        updateLocalCursor();
    }
}

void SdlInputHandler::handleHostCursorState(const LI_HOST_CURSOR_STATE* state)
{
    if (!m_HostCursorSupported) {
        return;
    }

    if (state->flags & LI_HOST_CURSOR_RESYNC) {
        if (!m_HostCursorReady) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Host cursor: received the initial state");
        }
        m_HostCursorReady = true;
    }
    else if (!m_HostCursorReady) {
        // Nothing is trusted before the host's full resync.
        return;
    }

    m_HostCursorState = *state;
    if (state->flags & LI_HOST_CURSOR_HAS_POSITION) {
        m_HostCursorHasPosition = true;
        m_HostCursorX = state->x;
        m_HostCursorY = state->y;
    }

    // A state may name a shape we never received (or evicted); ask for it again.
    if (state->shapeId != 0 && !m_HostCursorShapes.contains(state->shapeId)) {
        Uint32 now = SDL_GetTicks();
        if (state->shapeId != m_HostCursorResendId ||
                SDL_TICKS_PASSED(now, m_HostCursorResendTime + HOST_CURSOR_RESEND_INTERVAL_MS)) {
            int flags = m_HostCursorControlSent >= 0 ? m_HostCursorControlSent : LI_HOST_CURSOR_CONTROL_SUBSCRIBE;
            LiSendHostCursorControl((uint8_t)flags, state->shapeId);
            m_HostCursorResendId = state->shapeId;
            m_HostCursorResendTime = now;
        }
    }

    updateHostCursorControl();

    if ((state->flags & LI_HOST_CURSOR_HAS_POSITION) && isHostCursorDrawnLocally()) {
        warpToHostCursor();
    }

    updateLocalCursor();
}
