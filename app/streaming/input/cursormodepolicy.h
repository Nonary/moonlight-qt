#pragma once

// Automatic mouse mode: picks absolute input with a local cursor while the host
// cursor is visible and free, and relative input while a game hides or locks it.
// Pure logic with explicit timestamps (no SDL/Qt), unit tested in
// tests/input/cursormodepolicy.pro. See Vibeshine's docs/client-cursor-design.md.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace CursorModePolicy {

// All tuning lives here.
// The host cursor must look hidden/locked this long before we switch to relative
// input, and visible and free this long before we switch back. Games that hide the
// cursor for a moment (an MMO click, a loading flicker) never switch. A drag is the
// exception: see setMouseButtonsHeld().
constexpr uint32_t kEnterRelativeMs = 60;
constexpr uint32_t kExitRelativeMs = 50;
// Minimum time between automatic switches, whatever the host does.
constexpr uint32_t kMinDwellMs = 100;
// Windows can hide the pointer while the user types ("Hide pointer while typing").
// A hide that starts within this long of a key press is not a game capturing the
// mouse until the mouse moves and the cursor stays hidden for kTypingRevealMs: the
// host shows the pointer again on the first move.
constexpr uint32_t kTypingHideWindowMs = 1000;
constexpr uint32_t kTypingRevealMs = 120;
// A clip smaller than this in both dimensions is a lock (FPS games), not a
// confinement (RTS games keep a visible cursor inside their window).
constexpr double kTightClipFraction = 0.10;
constexpr int kTightClipMinPixels = 32;
// A visible cursor the host keeps putting back on the same spot is being recentred.
constexpr uint32_t kRecentreWindowMs = 300;
constexpr int kRecentreReports = 3;
constexpr double kRecentreToleranceFraction = 0.002;
constexpr int kRecentreToleranceMinPixels = 2;

enum class Mode {
    Fallback,   // the user's regular mouse setting; the cursor is in the video
    Absolute,   // absolute input with the host cursor drawn locally
    Relative,   // relative input; the local cursor is hidden
};

enum class Source {
    Fallback,   // automatic mode unavailable (unsupported host, host fallback, no state yet)
    Automatic,
    Locked,     // the user locked a mode for the rest of the session
};

struct Decision {
    Mode mode;
    Source source;
    // Absolute time at which update() must run again even without new input, or 0.
    uint32_t nextUpdateMs;
};

class Policy {
public:
    // The latest host state. `available` is false until the host's first full state
    // and while it reports FALLBACK. Clip sizes are in stream pixels.
    void setHostState(uint32_t nowMs, bool available, bool visible,
                      bool hasClip, int clipWidth, int clipHeight,
                      int referenceWidth, int referenceHeight)
    {
        if (visible) {
            m_TypingHide = false;
        }
        else if (m_Visible || !m_Available) {
            // The cursor just disappeared. Was the user typing? Not if they are
            // holding a mouse button: the application took the mouse for a drag.
            m_TypingHide = !m_ButtonsHeld && m_HasKey && nowMs - m_LastKeyMs <= kTypingHideWindowMs;
            m_TypingHideMoved = false;
        }
        m_Available = available;
        m_Visible = visible;
        m_HasClip = hasClip;
        m_ClipWidth = clipWidth;
        m_ClipHeight = clipHeight;
        m_ReferenceWidth = referenceWidth;
        m_ReferenceHeight = referenceHeight;
    }

    // Local input. Key presses mark hides that may be the OS hiding the pointer while
    // typing; mouse motion ends that doubt, and marks a held button as a drag.
    // Returns true when update() must run now, before the motion is sent.
    void noteLocalKey(uint32_t nowMs)
    {
        m_HasKey = true;
        m_LastKeyMs = nowMs;
    }

    bool noteLocalMouseMotion(uint32_t nowMs)
    {
        (void)nowMs;
        bool changed = false;
        if (m_ButtonsHeld && !m_Dragging) {
            m_Dragging = true;
            changed = true;
        }
        if (m_TypingHide && !m_TypingHideMoved) {
            m_TypingHideMoved = true;
            changed = true;
        }
        return changed;
    }

    // A position the host reported while its cursor was visible.
    void noteHostPosition(uint32_t nowMs, int x, int y)
    {
        m_Reports[m_NextReport] = { nowMs, x, y, true };
        m_NextReport = (m_NextReport + 1) % m_Reports.size();
    }

    // Mouse buttons decide how drags switch (an MMO camera drag, a 3D viewport):
    // - Once the mouse moves with a button held, a hide or lock switches to relative
    //   input at once, skipping the delay and the dwell. Until the switch, our absolute
    //   positions land on a cursor the game keeps putting back, and every one of them
    //   turns the camera by the whole distance moved so far.
    // - The return to absolute input waits for the release: switching mid-drag turns
    //   the rest of the drag into a jump. That also stops a drag from flapping.
    // A click that doesn't move keeps the usual delays. Call update() again after the
    // release.
    void setMouseButtonsHeld(bool held)
    {
        if (held != m_ButtonsHeld) {
            m_ButtonsHeld = held;
            m_Dragging = false;
        }
    }

    void lock(Mode mode)
    {
        m_Locked = true;
        m_LockedMode = mode;
    }

    void unlock()
    {
        m_Locked = false;
    }

    bool isLocked() const
    {
        return m_Locked;
    }

    Mode mode() const
    {
        return m_Mode;
    }

    Decision update(uint32_t nowMs)
    {
        if (!m_Available) {
            m_Mode = Mode::Fallback;
            m_Pending = false;
            return { Mode::Fallback, Source::Fallback, 0 };
        }

        if (m_Locked) {
            switchTo(m_LockedMode, nowMs);
            return { m_Mode, Source::Locked, 0 };
        }

        uint32_t recentreExpiry = 0;
        const Mode desired = wantsRelative(nowMs, recentreExpiry) ? Mode::Relative : Mode::Absolute;
        uint32_t next = recentreExpiry;

        if (m_Mode == Mode::Fallback) {
            // Leaving fallback (the first host state, a host fallback ending): there is
            // nothing to debounce against, so take the host's state as it is.
            switchTo(desired, nowMs);
        }
        else if (desired == m_Mode) {
            m_Pending = false;
        }
        else {
            if (!m_Pending || m_PendingMode != desired) {
                m_Pending = true;
                m_PendingMode = desired;
                m_PendingSinceMs = nowMs;
            }
            const bool dragCapture = desired == Mode::Relative && m_Dragging && !m_TypingHide;
            const uint32_t delay = desired == Mode::Absolute ? kExitRelativeMs :
                                   m_TypingHide ? kTypingRevealMs :
                                   dragCapture ? 0 : kEnterRelativeMs;
            uint32_t due = m_PendingSinceMs + delay;
            // Elapsed time is unsigned, so this holds however long since the last switch.
            if (!dragCapture && nowMs - m_LastSwitchMs < kMinDwellMs) {
                due = latest(due, m_LastSwitchMs + kMinDwellMs);
            }
            if (reached(nowMs, due)) {
                if (desired != Mode::Absolute || !m_ButtonsHeld) {
                    switchTo(desired, nowMs);
                }
                // Otherwise due but held until the buttons come up; the caller
                // updates again on the release.
            }
            else {
                next = next ? earliest(next, due) : due;
            }
        }

        return { m_Mode, Source::Automatic, next };
    }

private:
    struct Report {
        uint32_t atMs;
        int x;
        int y;
        bool valid;
    };

    // Timestamps wrap (SDL_GetTicks() is 32-bit); compare by signed difference.
    static bool reached(uint32_t nowMs, uint32_t dueMs)
    {
        return (int32_t)(nowMs - dueMs) >= 0;
    }

    static uint32_t latest(uint32_t a, uint32_t b)
    {
        return reached(a, b) ? a : b;
    }

    static uint32_t earliest(uint32_t a, uint32_t b)
    {
        return reached(a, b) ? b : a;
    }

    void switchTo(Mode mode, uint32_t nowMs)
    {
        if (mode != m_Mode) {
            m_Mode = mode;
            m_LastSwitchMs = nowMs;
        }
        m_Pending = false;
    }

    bool tightlyClipped() const
    {
        if (!m_HasClip) {
            return false;
        }
        const int maxWidth = (std::max)(kTightClipMinPixels, (int)(m_ReferenceWidth * kTightClipFraction));
        const int maxHeight = (std::max)(kTightClipMinPixels, (int)(m_ReferenceHeight * kTightClipFraction));
        return m_ClipWidth < maxWidth && m_ClipHeight < maxHeight;
    }

    // True if at least kRecentreReports recent reports land on one spot. `expiry`
    // is when that stops being true without new reports.
    bool recentring(uint32_t nowMs, uint32_t& expiry) const
    {
        const int tolerance = (std::max)(kRecentreToleranceMinPixels,
                                       (int)((std::max)(m_ReferenceWidth, m_ReferenceHeight) * kRecentreToleranceFraction));
        bool found = false;
        for (const Report& anchor : m_Reports) {
            if (!anchor.valid || nowMs - anchor.atMs > kRecentreWindowMs) {
                continue;
            }
            std::array<uint32_t, 16> ages {};
            int count = 0;
            for (const Report& r : m_Reports) {
                if (r.valid && nowMs - r.atMs <= kRecentreWindowMs &&
                        std::abs(r.x - anchor.x) <= tolerance && std::abs(r.y - anchor.y) <= tolerance) {
                    ages[count++] = nowMs - r.atMs;
                }
            }
            if (count >= kRecentreReports) {
                // True until the kRecentreReports-th newest of these reports ages out.
                std::sort(ages.begin(), ages.begin() + count);
                const uint32_t candidate = nowMs + (kRecentreWindowMs - ages[kRecentreReports - 1]) + 1;
                expiry = found ? latest(expiry, candidate) : candidate;
                found = true;
            }
        }
        return found;
    }

    bool wantsRelative(uint32_t nowMs, uint32_t& recentreExpiry) const
    {
        recentreExpiry = 0;
        if (!m_Visible) {
            // A pointer hidden while typing counts once the mouse has moved.
            return !m_TypingHide || m_TypingHideMoved;
        }
        if (tightlyClipped()) {
            return true;
        }
        return recentring(nowMs, recentreExpiry);
    }

    bool m_Available = false;
    bool m_Visible = false;
    bool m_HasClip = false;
    int m_ClipWidth = 0;
    int m_ClipHeight = 0;
    int m_ReferenceWidth = 0;
    int m_ReferenceHeight = 0;

    std::array<Report, 16> m_Reports {};
    size_t m_NextReport = 0;

    bool m_ButtonsHeld = false;
    bool m_Dragging = false;    // the mouse moved since the buttons went down

    bool m_HasKey = false;
    uint32_t m_LastKeyMs = 0;
    bool m_TypingHide = false;
    bool m_TypingHideMoved = false;

    bool m_Locked = false;
    Mode m_LockedMode = Mode::Absolute;

    Mode m_Mode = Mode::Fallback;
    uint32_t m_LastSwitchMs = 0;
    bool m_Pending = false;
    Mode m_PendingMode = Mode::Fallback;
    uint32_t m_PendingSinceMs = 0;
};

}
