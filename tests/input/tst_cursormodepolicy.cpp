#include "../../app/streaming/input/cursormodepolicy.h"

#include <cstdio>

using namespace CursorModePolicy;

static int g_Failures;

static void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        g_Failures++;
    }
}

static constexpr int kWidth = 1920;
static constexpr int kHeight = 1080;

static void host(Policy& p, uint32_t now, bool visible, int clipW = -1, int clipH = -1)
{
    p.setHostState(now, true, visible, clipW >= 0, clipW, clipH, kWidth, kHeight);
}

// A policy that has seen the host's first state at `start` with the cursor visible.
static Policy readyAbsolute(uint32_t start)
{
    Policy p;
    host(p, start, true);
    p.update(start);
    return p;
}

static void testFallbackUntilAvailable()
{
    Policy p;
    Decision d = p.update(0);
    require(d.mode == Mode::Fallback && d.source == Source::Fallback && d.nextUpdateMs == 0, "no host state is fallback");

    p.setHostState(10, false, true, false, 0, 0, kWidth, kHeight);
    d = p.update(10);
    require(d.mode == Mode::Fallback, "host FALLBACK is fallback");

    // The first usable state applies at once, without hysteresis.
    host(p, 20, false);
    d = p.update(20);
    require(d.mode == Mode::Relative && d.source == Source::Automatic, "first state hidden -> relative now");

    Policy q;
    host(q, 20, true);
    require(q.update(20).mode == Mode::Absolute, "first state visible -> absolute now");
}

static void testEnterRelativeAfterHysteresis()
{
    Policy p = readyAbsolute(1000);
    host(p, 2000, false);
    Decision d = p.update(2000);
    require(d.mode == Mode::Absolute, "hidden but not yet long enough");
    require(d.nextUpdateMs == 2000 + kEnterRelativeMs, "timer due when the hide has lasted long enough");
    require(p.update(2000 + kEnterRelativeMs - 1).mode == Mode::Absolute, "one ms early");
    require(p.update(2000 + kEnterRelativeMs).mode == Mode::Relative, "relative once hidden for the delay");
}

static void testBriefHideNeverSwitches()
{
    Policy p = readyAbsolute(1000);
    host(p, 2000, false);
    p.update(2000);
    host(p, 2000 + kEnterRelativeMs - 1, true);
    Decision d = p.update(2000 + kEnterRelativeMs - 1);
    require(d.mode == Mode::Absolute && d.nextUpdateMs == 0, "a short hide is forgotten");
    require(p.update(5000).mode == Mode::Absolute, "and never switches later");
}

static void testExitRelativeAfterHysteresis()
{
    Policy p;
    host(p, 0, false);
    p.update(0);  // relative, last switch at 0
    host(p, 1000, true);
    Decision d = p.update(1000);
    require(d.mode == Mode::Relative && d.nextUpdateMs == 1000 + kExitRelativeMs, "visible, waiting to return");
    require(p.update(1000 + kExitRelativeMs - 1).mode == Mode::Relative, "one ms early");
    require(p.update(1000 + kExitRelativeMs).mode == Mode::Absolute, "absolute once visible for the delay");
}

static void testMinimumDwell()
{
    Policy p = readyAbsolute(0);
    host(p, 1000, false);
    p.update(1000);
    require(p.update(1000 + kEnterRelativeMs).mode == Mode::Relative, "switched to relative");
    const uint32_t switched = 1000 + kEnterRelativeMs;

    // Visible again straight away: the exit delay alone would allow a switch at
    // switched + 100, but the minimum dwell holds it until switched + 150.
    host(p, switched + 1, true);
    Decision d = p.update(switched + 1);
    const uint32_t due = switched + 1 + kExitRelativeMs > switched + kMinDwellMs ? switched + 1 + kExitRelativeMs : switched + kMinDwellMs;
    require(d.nextUpdateMs == due, "dwell extends the due time");
    require(p.update(due - 1).mode == Mode::Relative, "held by the dwell");
    require(p.update(due).mode == Mode::Absolute, "switches once the dwell passes");
}

static void testFlappingHostDoesNotFlap()
{
    // An ARPG/MMO hiding the cursor in 40 ms bursts: never long enough to switch.
    Policy p = readyAbsolute(0);
    int switches = 0;
    Mode last = Mode::Absolute;
    for (uint32_t t = 100; t < 3000; t += 10) {
        host(p, t, ((t / 40) % 2) == 0);
        Mode m = p.update(t).mode;
        switches += m != last;
        last = m;
    }
    require(switches == 0, "40 ms hide/show bursts never switch");

    // Long hides (camera drag) switch exactly once each way, and no faster than the dwell.
    Policy q = readyAbsolute(0);
    switches = 0;
    last = Mode::Absolute;
    for (uint32_t t = 100; t < 5000; t += 5) {
        host(q, t, ((t / 1000) % 2) == 1);
        Mode m = q.update(t).mode;
        switches += m != last;
        last = m;
    }
    require(switches == 5, "1 s hides switch once per hide and once per show");
}

static void testClips()
{
    // An RTS confining a visible cursor to its 800x600 window stays absolute.
    Policy p = readyAbsolute(0);
    host(p, 1000, true, 800, 600);
    require(p.update(5000).mode == Mode::Absolute, "large clip stays absolute");

    // A visible cursor clipped to a tiny rect is locked.
    host(p, 6000, true, 40, 40);
    p.update(6000);
    require(p.update(6000 + kEnterRelativeMs).mode == Mode::Relative, "tiny clip -> relative");

    // Narrow in one dimension only is not a lock.
    Policy q = readyAbsolute(0);
    host(q, 1000, true, 1920, 20);
    require(q.update(5000).mode == Mode::Absolute, "a clip tiny in one dimension only stays absolute");

    // A clip that misses the stream entirely (empty) is a lock.
    Policy r = readyAbsolute(0);
    host(r, 1000, true, 0, 0);
    r.update(1000);
    require(r.update(1000 + kEnterRelativeMs).mode == Mode::Relative, "empty clip -> relative");
}

static void testRecentring()
{
    Policy p = readyAbsolute(0);
    host(p, 1000, true);
    // The host keeps putting a visible cursor back at the centre.
    p.noteHostPosition(1000, 960, 540);
    p.noteHostPosition(1016, 961, 540);
    require(p.update(1016).mode == Mode::Absolute && p.update(1016).nextUpdateMs == 0, "two returns are not recentring");
    p.noteHostPosition(1033, 960, 541);
    Decision d = p.update(1033);
    require(d.mode == Mode::Absolute && d.nextUpdateMs == 1033 + kEnterRelativeMs, "three returns start the switch");
    require(p.update(1033 + kEnterRelativeMs).mode == Mode::Relative, "recentred cursor -> relative");

    // Recentring stops: the oldest of the three reports ages out at 1000 + window + 1,
    // then the exit delay and dwell apply.
    d = p.update(1100);
    require(d.mode == Mode::Relative && d.nextUpdateMs == 1000 + kRecentreWindowMs + 1, "timer for the recentring to expire");
    d = p.update(1000 + kRecentreWindowMs + 1);
    require(d.mode == Mode::Relative && d.nextUpdateMs != 0, "expired, waiting to return");
    require(p.update(1000 + kRecentreWindowMs + 1 + kExitRelativeMs).mode == Mode::Absolute, "free again -> absolute");

    // Reports that wander (a cursor moving slowly) are not recentring.
    Policy q = readyAbsolute(0);
    host(q, 1000, true);
    for (int i = 0; i < 10; i++) {
        q.noteHostPosition(1000 + i * 20, 900 + i * 6, 500);
    }
    require(q.update(1200).mode == Mode::Absolute && q.update(1200).nextUpdateMs == 0, "slow movement is not recentring");
}

static void testLocks()
{
    Policy p = readyAbsolute(0);
    p.lock(Mode::Relative);
    Decision d = p.update(10);
    require(d.mode == Mode::Relative && d.source == Source::Locked && d.nextUpdateMs == 0, "lock applies at once");
    host(p, 20, true);
    require(p.update(5000).mode == Mode::Relative, "lock ignores the host");

    // The host losing cursor information still falls back; the lock resumes after.
    p.setHostState(6000, false, true, false, 0, 0, kWidth, kHeight);
    require(p.update(6000).mode == Mode::Fallback, "fallback overrides a lock");
    host(p, 7000, true);
    require(p.update(7000).mode == Mode::Relative, "lock resumes");

    p.unlock();
    d = p.update(7001);
    require(d.mode == Mode::Relative && d.source == Source::Automatic && d.nextUpdateMs != 0, "unlocked, automatic again with hysteresis");
    require(p.update(7001 + kMinDwellMs + kExitRelativeMs).mode == Mode::Absolute, "returns to the host's mode");
}

static void testTickWrap()
{
    const uint32_t start = 0xFFFFFFF0u;
    Policy p = readyAbsolute(start - 1000);
    host(p, start + 5, false);
    Decision d = p.update(start + 5);
    require(d.nextUpdateMs == (uint32_t)(start + 5 + kEnterRelativeMs), "due time wraps");
    require(p.update(start + 6).mode == Mode::Absolute, "not due across the wrap");
    require(p.update((uint32_t)(start + 5 + kEnterRelativeMs)).mode == Mode::Relative, "due across the wrap");
}

int main()
{
    testFallbackUntilAvailable();
    testEnterRelativeAfterHysteresis();
    testBriefHideNeverSwitches();
    testExitRelativeAfterHysteresis();
    testMinimumDwell();
    testFlappingHostDoesNotFlap();
    testClips();
    testRecentring();
    testLocks();
    testTickWrap();
    if (g_Failures == 0) {
        std::printf("PASS cursor mode policy\n");
    }
    return g_Failures == 0 ? 0 : 1;
}
