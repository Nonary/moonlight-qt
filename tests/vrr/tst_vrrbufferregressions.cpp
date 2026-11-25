// Production-policy regression gate for the adaptive VRR buffer.
//
// Most contracts here guard a buffer regression that already reached a live
// build; the rest pin documented policy nothing else exercised live. The
// tests written when those bugs were fixed pinned the policy revision current
// at the time (input hitch policy 0-9, stable adaptation 1/2, bounded catch-up
// off), so every later default revision started with no coverage for them. Here every controller is built exactly
// as a live session builds it (vrrproductionsession.h, compared with the
// worker by tst_vrrpacingworker) and nothing pins a revision: whatever policy
// becomes the default inherits every contract the moment it is selected.
//
// Bounds come from the documented policy (release law, rise rate, profile
// allowance, retiming budgets) plus explicit slack, not from whatever the
// implementation happened to produce. Frames follow the worker's call order.
// Presented stutter is measured from the simulated submissions themselves,
// never from the controller's own miss counters. Every scenario runs for a
// presenter that can latch and one that cannot ("@no-latch").
//
// Run with scenario names to select a subset, e.g.
//   tst_vrrbufferregressions start-release rate-round-trip

#include "capturespans.h"
#include "slowdecodespan.h"
#include "vrrproductionsession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

std::atomic<int> g_Failures{0};
std::atomic<int> g_Checks{0};
bool g_Strict = false;
std::mutex g_KnownLock;

// Scenarios run concurrently; each collects its lines and main prints them
// in scenario order. The bool marks a failure line for stderr.
using Log = std::vector<std::pair<bool, std::string>>;
thread_local Log* t_Log = nullptr;
// Every scenario runs for presenters that can latch (Windows D3D11) and that
// cannot (Linux Vulkan present modes without latch protection).
thread_local bool t_CanLatch = true;

std::string scenarioName(const char* scenario)
{
    return std::string(scenario) + (t_CanLatch ? "" : "@no-latch");
}

void emit(bool failure, const char* format, va_list args)
{
    char line[1024] = {};
    std::vsnprintf(line, sizeof(line), format, args);
    if (t_Log) t_Log->emplace_back(failure, line);
    else std::fputs(line, failure ? stderr : stdout);
}

void report(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    emit(false, format, args);
    va_end(args);
}

void reportLine(bool failure, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    emit(failure, format, args);
    va_end(args);
}

// Contracts the shipped policy violates today, each with its evidence. They
// print on every run without failing the gate (--strict fails them). Once
// none of an entry's checks fail any more, the gate fails until the entry is
// deleted, so a fix hardens the contract instead of leaving it advisory.
// Example entry:
//   {"rate-drop/", {"a rate drop gives back the moved mapping promptly", nullptr},
//    "what fails, since which commit, and the evidence", 0, 0},
struct KnownRegression {
    const char* scenarioPrefix;
    const char* contracts[4];
    const char* evidence;
    size_t failed, passed;
};
std::vector<KnownRegression> g_Known = {
    {"display-ceiling@no-latch/",
     {"a released starting delay actually lowers latency at the ceiling",
      "late frames leave no more standing phase than they forced at the ceiling",
      "planned retiming at the display ceiling stays inside its budget",
      "on-screen retiming at the display ceiling stays inside its budget"},
     "A presenter that cannot latch must stay a guard above its previous present, so at exactly its maximum refresh "
     "every frame is pushed later and the interval buffer reports service overload. That blocks all input evidence "
     "(0 eligible observations), so the starting delay never releases (+6.9 ms Balanced), and phase sawtooths between "
     "replacements (retiming p50 1.8-2.8 ms). Latching presenters (Windows D3D11) are unaffected.", 0, 0},
    {"versus-fixed-refresh@no-latch/",
     {"no more visible stutter than fixed refresh at the display ceiling", nullptr},
     "Same cause as display-ceiling@no-latch: a guard above every previous present means the presenter cannot show "
     "every frame at exactly its maximum refresh, so it replaces one about every 48 frames (about 150 skips a minute, "
     "where fixed refresh's FIFO vsync shows 0-14). Ceiling policy 2 trims policy 1's count by a fifth; curing it needs "
     "a latched or vsync-locked present at the ceiling. Latching presenters (Windows D3D11) are unaffected.", 0, 0},
    {"versus-fixed-refresh-stall@no-latch/",
     {"no more visible stutter than fixed refresh at the display ceiling", nullptr},
     "As versus-fixed-refresh@no-latch: the guard, not the stalls, sets the replacement rate.", 0, 0},
    {"versus-fixed-refresh-captures@no-latch/",
     {"no more visible stutter than fixed refresh on a recorded capture span",
      "no more stutter or replacements than ceiling policy 1 on a recorded capture span", nullptr},
     "As versus-fixed-refresh@no-latch, on recorded arrivals: the guard spacing replaces frames at the ceiling.", 0, 0},
    {"versus-fixed-refresh-captures/0420-lossy",
     {"no more visible stutter than fixed refresh on a recorded capture span", nullptr},
     "In the lossy 04:20 span (61-364 lost packets per frame, 20-46 ms late) Balanced shows about 54 visible events "
     "where fixed refresh's deeper queue shows 28, at 5.5 ms more latency. Ceiling policy 1 shows 56: the adaptive "
     "buffer's rise, not ceiling replacement, decides that trade, and it does not rise fast enough for chaos.", 0, 0},
    {"versus-fixed-refresh@no-latch/smooth/reduce-judder/",
     {"less latency than fixed refresh at the display ceiling", nullptr},
     "Smooth with Reduce judder on a presenter that cannot latch averages 13.58 ms where fixed refresh's "
     "common-budget queue averages 13.51 ms, in the late-4ms and frequent-2ms scenarios.", 0, 0},
};

KnownRegression* knownRegression(const std::string& scenario, const char* contract)
{
    // An entry without a variant covers both presenter variants; one naming
    // "@no-latch" covers only that variant.
    std::string base = scenario;
    const auto variant = base.find("@no-latch");
    if (variant != std::string::npos) base.erase(variant, std::strlen("@no-latch"));
    for (auto& known : g_Known) {
        const std::string& subject = std::strstr(known.scenarioPrefix, "@") ? scenario : base;
        if (subject.compare(0, std::strlen(known.scenarioPrefix), known.scenarioPrefix) != 0) continue;
        for (const char* listed : known.contracts)
            if (listed && std::strcmp(listed, contract) == 0) return &known;
    }
    return nullptr;
}

void check(bool ok, const std::string& scenario, const char* contract, const char* format, ...)
{
    ++g_Checks;
    KnownRegression* known = knownRegression(scenario, contract);
    if (known) {
        std::lock_guard<std::mutex> lock(g_KnownLock);
        ++(ok ? known->passed : known->failed);
    }
    if (ok) return;
    char detail[512] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    if (known && !g_Strict) {
        reportLine(false, "KNOWN [%s] %s: %s\n", scenario.c_str(), contract, detail);
        return;
    }
    ++g_Failures;
    reportLine(true, "FAIL [%s] %s: %s\n", scenario.c_str(), contract, detail);
}

struct Preset { int mode; const char* name; };
constexpr Preset kPresets[] = {{2, "low-latency"}, {1, "balanced"}, {0, "smooth"}};

struct Smoothing { bool smooth; const char* name; };
// The two smoothing settings available to a live session.
constexpr Smoothing kSmoothing[] = {
    {false, "smoothing-off"}, {true, "reduce-judder"}};

std::string label(const char* scenario, const Preset& preset, const Smoothing& smoothing)
{
    return scenarioName(scenario) + "/" + preset.name + "/" + smoothing.name;
}

// xorshift64*: identical sequences on every standard library.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) {}
    uint64_t next()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ULL;
    }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
};

constexpr uint64_t kOriginUs = 10000000;
// Network, reassembly and decode of an on-time frame. The mapping absorbs it.
constexpr uint64_t kBaseLatencyUs = 1000;
constexpr uint32_t k116Ticks = 776; // 90 kHz RTP ticks per frame
constexpr uint32_t k30Ticks = 3000;

struct Frame {
    // submittedUs is the present call; shownUs is when it reaches the screen.
    // A latched (synchronized) present waits in the flip queue for one refresh
    // after the previous flip, exactly as the controller's spacing anchor models.
    uint64_t sourceUs = 0, decodedUs = 0, targetUs = 0, submittedUs = 0, shownUs = 0;
    uint64_t appliedUs = 0, recoveryUs = 0, carryUs = 0, extraUs = 0;
    // The smoothed slot before floors, and the smoothing retiming inside it.
    uint64_t originalUs = 0;
    int64_t smoothingUs = 0;
    // Retiming as it reaches the screen: a capped plan cannot hide flip-queue phase.
    uint64_t shownExtraUs() const {
        return (shownUs > originalUs ? shownUs - originalUs : 0) + uint64_t(std::max<int64_t>(0, smoothingUs));
    }
    // Replaced by a ready successor (worker trajectory resync); never shown.
    bool dropped = false;
};

struct Input {
    uint64_t latenessUs = 0;
    uint32_t lostPackets = 0;
    uint64_t prepareUs = 500;
    // Fixed wake-up/present-call delay after the target (3c32553e: 0.28 ms).
    uint64_t presentLatencyUs = 0;
    // Wait between reassembly and decode submission. Real clients hand every
    // frame over in about 0.1 ms (2026-10-19 capture: p10 75, p50 97, p99
    // 207 us); larger waits are client decoder backlog.
    uint64_t decoderQueueUs = 100;
    // The next frame's lateness, which decides whether a replacement finds a
    // ready successor. Late input that persists (a latency step) sets it.
    uint64_t successorLatenessUs = 0;
    bool hasDeliveryTimeline = true;
};

// One production controller driven through the worker's per-frame calls.
class Stream {
public:
    explicit Stream(const VrrSessionConfig& session, uint64_t seedUs = 0, int64_t driftPpm = 0) :
        Stream(session, vrrTimingParametersForSession(session), seedUs, driftPpm)
    {}

    Stream(const VrrSessionConfig& session, const VrrTimingParameters& parameters,
           uint64_t seedUs, int64_t driftPpm = 0) :
        m_Session(session),
        m_Controller(session, t_CanLatch, parameters),
        m_DriftPpm(driftPpm)
    {
        // Same call the worker makes with the previous session's settled delay.
        if (seedUs) m_Controller.seedPlayoutDelayStart(seedUs);
    }

    const Frame& step(uint32_t ticks, const Input& input = {})
    {
        m_Ticks += ticks;
        const uint64_t elapsedUs = m_Ticks * 1000000 / 90000;
        const int64_t driftUs = int64_t(elapsedUs) * m_DriftPpm / 1000000;
        Frame f;
        f.sourceUs = uint64_t(int64_t(kOriginUs + elapsedUs) + driftUs);
        f.decodedUs = f.sourceUs + kBaseLatencyUs + input.latenessUs + input.decoderQueueUs;
        // Start near the RTP wrap so every long scenario crosses it.
        PacedFrame frame(nullptr, m_FrameNumber++, uint32_t(m_Ticks + 0xFFF00000u), true, f.decodedUs);
        // Input accounting excludes decoder backlog. The steady handoff is part
        // of readiness, like the mapping that measures it; excluding it would
        // under-protect every frame by its length.
        const uint64_t submitUs = f.decodedUs - 600;
        if (input.hasDeliveryTimeline)
            frame.setDeliveryTimeline(submitUs - input.decoderQueueUs - 200, submitUs - input.decoderQueueUs, submitUs);
        frame.setLostPackets(input.lostPackets);
        const uint64_t now = std::max(f.decodedUs, m_LastSubmittedUs);
        const auto d = m_Controller.schedule(frame, now);
        // Frame-level decisions are compared independently of the scoring counters.
        f.targetUs = d.targetUs;
        f.appliedUs = d.playoutDelayUs;
        f.recoveryUs = d.recoveryAddedUs;
        f.carryUs = d.trajectory.carryUs;
        f.extraUs = d.plannedExtraDelayUs();
        f.originalUs = d.originalTargetUs;
        f.smoothingUs = d.cadenceSmoothingUs;
        uint64_t startUs = now;
        if (d.trajectory.resyncRecommended) {
            // The worker waits at most two source periods (20 ms cap), or the
            // decision's shorter wait, in whole milliseconds, for a proven-ready
            // successor, then discards this frame for it. Steady input decodes
            // the successor one period later.
            const uint64_t waitUs = (((d.replacementWaitUs ? d.replacementWaitUs :
                std::min<uint64_t>(20000, 2 * d.sourcePeriodUs)) + 999) / 1000) * 1000;
            const uint64_t successorUs = f.sourceUs + d.sourcePeriodUs + kBaseLatencyUs +
                input.decoderQueueUs + input.successorLatenessUs + 400;
            if (successorUs <= now + waitUs) {
                m_Controller.noteTrajectoryResyncDrop();
                m_Controller.noteSubmission(false, false, 0);
                f.dropped = true;
                ++m_Replacements;
                m_Frames.push_back(f);
                return m_Frames.back();
            }
            // Without one, the frame goes out only after the whole wait.
            startUs = now + waitUs;
            ++m_Deferrals;
            m_Controller.noteTrajectoryDeferred();
        }
        const uint64_t ready = std::max(startUs, d.renderStartUs) + input.prepareUs;
        f.submittedUs = std::max(ready, d.targetUs) + input.presentLatencyUs;
        f.shownUs = d.latchedPresentation && m_LastShownUs ?
            std::max(f.submittedUs, m_LastShownUs + m_Controller.displayPeriodUs()) : f.submittedUs;
        m_Controller.notePreparationDuration(input.prepareUs, 0, ready);
        m_Controller.noteSchedulerDelays(0, 0, true);
        m_Controller.noteSubmission(true, false, f.submittedUs, f.submittedUs);
        m_LastSubmittedUs = f.submittedUs;
        m_LastShownUs = f.shownUs;
        m_Frames.push_back(f);
        return m_Frames.back();
    }

    // Frames discarded for a ready successor at the display ceiling, and
    // replacement requests that found none and cost the worker its wait.
    size_t replacements() const { return m_Replacements; }
    size_t deferrals() const { return m_Deferrals; }

    void rebase() { m_Controller.rebase(); }
    const std::vector<Frame>& frames() const { return m_Frames; }
    size_t size() const { return m_Frames.size(); }
    const VrrTimingController& controller() const { return m_Controller; }
    Vrr13::IntervalBuffer::Stats interval() const { return m_Controller.intervalStats(); }
    uint64_t toleranceUs() const { return uint64_t(m_Session.timingOptions.toleranceUs); }
    uint64_t windowUs() const { return uint64_t(m_Session.timingOptions.historySeconds) * 1000000; }
    // Profile allowance: the share of frames the selected smoothness may miss.
    uint64_t allowancePpm() const { return uint64_t(10000 - m_Session.timingOptions.targetHundredths) * 100; }
    uint64_t fallUsPerSecond() const { return m_Controller.parameters().playoutMeanMissReleaseUsPerSecond; }
    // Only Balanced tapers release below the refresh ceiling.
    uint64_t releaseTauUs() const { return m_Controller.parameters().playoutIntervalReleaseTauUs; }
private:
    VrrSessionConfig m_Session;
    VrrTimingController m_Controller;
    int64_t m_DriftPpm = 0;
    uint64_t m_Ticks = 0, m_LastSubmittedUs = 0, m_LastShownUs = 0;
    int m_FrameNumber = 0;
    size_t m_Replacements = 0, m_Deferrals = 0;
    std::vector<Frame> m_Frames;
};

// Presented interval stretched beyond the tolerance relative to the source
// interval: what the viewer sees as a hitch. Catch-up compressions are not.
// A replaced frame is never shown; the next shown frame spans both intervals.
size_t hitches(const std::vector<Frame>& f, size_t begin, size_t end, uint64_t toleranceUs)
{
    size_t count = 0;
    size_t previous = SIZE_MAX;
    for (size_t i = begin; i < std::min(end, f.size()); ++i) {
        if (f[i].dropped) continue;
        if (previous != SIZE_MAX) {
            const int64_t source = int64_t(f[i].sourceUs) - int64_t(f[previous].sourceUs);
            const int64_t presented = int64_t(f[i].shownUs) - int64_t(f[previous].shownUs);
            count += presented - source > int64_t(toleranceUs);
        }
        previous = i;
    }
    return count;
}

// Aggregates over decisions that reached the screen.
template<class Value>
uint64_t maximum(const std::vector<Frame>& f, size_t begin, size_t end, Value value)
{
    uint64_t result = 0;
    for (size_t i = begin; i < std::min(end, f.size()); ++i)
        if (!f[i].dropped) result = std::max(result, value(f[i]));
    return result;
}

template<class Value>
uint64_t minimum(const std::vector<Frame>& f, size_t begin, size_t end, Value value)
{
    uint64_t result = UINT64_MAX;
    for (size_t i = begin; i < std::min(end, f.size()); ++i)
        if (!f[i].dropped) result = std::min(result, value(f[i]));
    return result;
}

template<class Value>
uint64_t mean(const std::vector<Frame>& f, size_t begin, size_t end, Value value)
{
    uint64_t total = 0, count = 0;
    for (size_t i = begin; i < std::min(end, f.size()); ++i)
        if (!f[i].dropped) { total += value(f[i]); ++count; }
    return count ? total / count : 0;
}

uint64_t applied(const Frame& f) { return f.appliedUs; }
uint64_t latency(const Frame& f) { return f.shownUs - f.sourceUs; }

// First index from which the applied buffer stays at or below the limit for
// the rest of the run; f.size() when it never settles.
size_t settledAtOrBelow(const std::vector<Frame>& f, size_t begin, uint64_t limit)
{
    size_t settled = f.size();
    for (size_t i = f.size(); i-- > begin;) {
        if (f[i].appliedUs > limit) break;
        settled = i;
    }
    return settled;
}

// Largest buffer removal between consecutive frames, and over any window.
uint64_t largestDrop(const std::vector<Frame>& f, size_t begin, size_t end, size_t span = 1)
{
    uint64_t result = 0;
    for (size_t i = begin; i + span < std::min(end, f.size()); ++i)
        if (f[i].appliedUs > f[i + span].appliedUs) result = std::max(result, f[i].appliedUs - f[i + span].appliedUs);
    return result;
}

// Changes of direction in the applied buffer, ignoring movement below the
// hysteresis. A held disturbance should give a held buffer, not probes.
size_t reversals(const std::vector<Frame>& f, size_t begin, size_t end, uint64_t hysteresisUs)
{
    size_t count = 0;
    int direction = 0;
    uint64_t anchor = begin < f.size() ? f[begin].appliedUs : 0;
    for (size_t i = begin; i < std::min(end, f.size()); ++i) {
        const uint64_t value = f[i].appliedUs;
        const int now = value >= anchor + hysteresisUs ? 1 : value + hysteresisUs <= anchor ? -1 : 0;
        if (!now) continue;
        if (direction && now != direction) ++count;
        direction = now;
        anchor = value;
    }
    return count;
}

uint64_t percentile(std::vector<uint64_t> values, unsigned perMille)
{
    if (values.empty()) return 0;
    const size_t rank = std::max<size_t>(1, (values.size() * perMille + 999) / 1000);
    std::nth_element(values.begin(), values.begin() + rank - 1, values.end());
    return values[rank - 1];
}

// Change in presented interval from one pair to the next.
std::vector<uint64_t> presentedJerk(const std::vector<Frame>& f, size_t begin, size_t end)
{
    std::vector<uint64_t> jerk;
    std::vector<uint64_t> shown;
    for (size_t i = begin; i < std::min(end, f.size()); ++i)
        if (!f[i].dropped) shown.push_back(f[i].shownUs);
    for (size_t i = 2; i < shown.size(); ++i) {
        const int64_t a = int64_t(shown[i - 1]) - int64_t(shown[i - 2]);
        const int64_t b = int64_t(shown[i]) - int64_t(shown[i - 1]);
        jerk.push_back(uint64_t(a > b ? a - b : b - a));
    }
    return jerk;
}

size_t frames90(double seconds) { return size_t(seconds * 90); }
size_t frames116(double seconds) { return size_t(seconds * 90000 / k116Ticks); }
double seconds116(size_t frames) { return double(frames) * k116Ticks / 90000; }

// 1. Clean input must drain the cold-start or cached starting delay at the
// release law and then never buy delay again. Regression: Codex "paced
// attack revision 2" floated at its starting delay; 2ccca878 and 0d2f9adc
// left stale plateaus. The worker seeds each session from the previous
// session's settled delay, so a buffer that cannot leave its seed ratchets
// across sessions.
void testStartingDelayReleasesOnCleanInput()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) for (uint64_t seed : {uint64_t(0), uint64_t(4000)}) {
        const std::string name = label("start-release", preset, smoothing) + (seed ? "/cached-4ms" : "/cold");
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth), seed);
        Rng rng(11 + uint64_t(preset.mode) * 3 + seed);
        const auto& policy = s.controller().parameters();
        const double holdSeconds = double(policy.playoutMeanMissHoldUs) / 1e6;
        const size_t total = frames116(holdSeconds + 8334.0 / s.fallUsPerSecond() + 4.0 * s.releaseTauUs() / 1e6 + 5);
        for (size_t i = 0; i < total; ++i) s.step(k116Ticks, {rng.below(400)});
        const auto& f = s.frames();
        const auto in = s.interval();
        const uint64_t start = f[0].appliedUs, floor = in.update.minimumUs + 250;
        const size_t settled = settledAtOrBelow(f, 0, floor);
        const double boundSeconds = double(start > floor ? start - floor : 0) / double(s.fallUsPerSecond()) +
            3.0 * double(s.releaseTauUs()) / 1e6 + holdSeconds + 5.0;
        const uint64_t perSecond = largestDrop(f, 0, total, frames116(1));
        const size_t visible = hitches(f, frames116(2), total, s.toleranceUs());
        report("%-52s start %5" PRIu64 " floor %5" PRIu64 " settled %5.1fs (bound %4.1fs) drop/s %4" PRIu64 " hitches %zu missed %" PRIu64 "\n",
            name.c_str(), start, floor, seconds116(settled), boundSeconds, perSecond, visible, in.failedUs);
        if (seed) check(start + 250 >= std::min(seed, in.update.maximumUs), name, "a cached starting delay is honored",
            "first frame %" PRIu64 " us for a %" PRIu64 " us seed", start, seed);
        check(settled < total && seconds116(settled) <= boundSeconds, name,
            "clean input drains the starting delay to the floor at the release law",
            "start %" PRIu64 " us reached floor+250 at %.1f s; release law allows %.1f s", start,
            settled < total ? seconds116(settled) : -1.0, boundSeconds);
        check(perSecond <= s.fallUsPerSecond() + 50, name, "release never outruns the fall rate (no visible step)",
            "dropped %" PRIu64 " us within one second; fall rate %" PRIu64 " us/s", perSecond, s.fallUsPerSecond());
        check(visible == 0 && in.evaluatedUs > 0 && in.lossFraction() <= double(s.allowancePpm()) / 1e6, name, "clean input follows source cadence and meets the configured timing target",
            "%zu presented hitches, %" PRIu64 " failed interval microseconds", visible, in.failedUs);
        check(s.replacements() == 0, name, "no frame is replaced away from the display ceiling",
            "%zu frames replaced at 116 FPS on a 120 Hz display", s.replacements());
    }
}

// 2. Misses that stay inside the selected allowance must not buy latency:
// the profile promises e.g. 99.5% smooth intervals, not zero misses.
// Regression class: buffers that "looked too high" (3c32553e, 0d2f9adc).
void testMissesWithinAllowanceBuyNoDelay()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("within-allowance", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth), 1000);
        // A quarter of each profile's allowance: 0.25%, 0.125%, 0.0125%.
        const size_t spacing = size_t(4000000 / s.allowancePpm());
        Rng rng(23 + uint64_t(preset.mode));
        const size_t total = frames116(180);
        for (size_t i = 0; i < total; ++i)
            s.step(k116Ticks, {i % spacing == spacing / 2 ? 3000 : rng.below(400)});
        const auto& f = s.frames();
        const auto in = s.interval();
        const uint64_t peak = maximum(f, frames116(1), total, applied);
        report("%-52s spike every %5zu peak %5" PRIu64 " floor %5" PRIu64 " missed %" PRIu64 "/%" PRIu64 "\n",
            name.c_str(), spacing, peak, in.update.minimumUs, in.failedUs, in.evaluatedUs);
        check(peak <= in.update.minimumUs + 250, name, "misses inside the profile allowance never buy latency",
            "buffer reached %" PRIu64 " us (floor %" PRIu64 ") for one 3 ms spike every %zu frames",
            peak, in.update.minimumUs, spacing);
    }
}

// 2b. Loss no permitted buffer can remove, and a late frame that cannot yet
// establish a rate, must not buy latency either; and a release must wait out
// its hold. Regression: the goal solver merged in c822745f. It weighted a lost
// interval by its presented length, so buffer shortened a freeze it could not
// remove: after each 30 ms stall Smooth rose from its floor to as much as
// 10.75 ms for the same hitches (cb7dcec7 covered only stalls that fitted the
// allowance). It applied the allowance to however little of the window was
// filled, so one 6 ms frame 8 s into a session took Smooth to 4.25 ms. And it
// followed every step of the solved target down, turning the buffer around
// within a second of raising it.
void testUnabsorbableLossBuysNoDelay()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("unabsorbable-loss", preset, smoothing);
        const auto session = vrrProductionSession(preset.mode, 116, 120, smoothing.smooth);
        {
            // A 30 ms delivery stall every 45 s: beyond every profile's cap.
            Stream s(session, 1000);
            Rng rng(131 + uint64_t(preset.mode));
            const size_t total = frames116(300), every = frames116(45);
            for (size_t i = 0; i < total; ++i) s.step(k116Ticks, {i % every == every / 2 ? 30000 : rng.below(400)});
            const uint64_t floor = s.interval().update.minimumUs, peak = maximum(s.frames(), frames116(1), total, applied);
            report("%-52s stalls: peak %5" PRIu64 " floor %5" PRIu64 "\n", name.c_str(), peak, floor);
            check(peak <= floor + 250, name, "a stall beyond the cap never buys latency",
                "buffer reached %" PRIu64 " us (floor %" PRIu64 ") for a 30 ms stall every 45 s", peak, floor);
        }
        {
            // One 6 ms frame 8 s into the session, then clean input.
            Stream s(session, 1000);
            Rng rng(137 + uint64_t(preset.mode));
            const size_t total = frames116(90), late = frames116(8);
            for (size_t i = 0; i < total; ++i) s.step(k116Ticks, {i == late ? 6000 : rng.below(400)});
            const uint64_t floor = s.interval().update.minimumUs, peak = maximum(s.frames(), late, total, applied);
            report("%-52s one late frame: peak %5" PRIu64 " floor %5" PRIu64 "\n", name.c_str(), peak, floor);
            check(peak <= floor + 250, name, "one late frame in a window that has only begun to fill never buys latency",
                "buffer reached %" PRIu64 " us (floor %" PRIu64 ") for one 6 ms frame 8 s into the session", peak, floor);
        }
        {
            // Heavy-tailed lateness (exponential, mean 0.6 ms): the solved
            // target steps between neighbouring values every few seconds.
            Stream s(session, 1000);
            Rng rng(139 + uint64_t(preset.mode));
            const size_t total = frames116(180);
            for (size_t i = 0; i < total; ++i) {
                const double unit = double(rng.below(1000000) + 1) / 1000001.0;
                s.step(k116Ticks, {uint64_t(-600.0 * std::log(unit))});
            }
            const auto& f = s.frames();
            const auto& policy = s.controller().parameters();
            const uint64_t holdUs = policy.playoutIntervalTaperHoldUs ?
                policy.playoutIntervalTaperHoldUs : policy.playoutMeanMissHoldUs;
            // A fall sooner after a rise than the hold, less one evaluation.
            uint64_t lastRiseUs = 0, soonestUs = UINT64_MAX;
            size_t early = 0;
            for (size_t i = frames116(10); i < total; ++i) {
                if (f[i].appliedUs > f[i - 1].appliedUs) lastRiseUs = f[i].sourceUs;
                else if (f[i].appliedUs < f[i - 1].appliedUs && lastRiseUs) {
                    const uint64_t sinceUs = f[i].sourceUs - lastRiseUs;
                    soonestUs = std::min(soonestUs, sinceUs);
                    early += sinceUs + 1000000 < holdUs;
                }
            }
            report("%-52s target steps: reversals %3zu soonest release %5.1f s after a rise (hold %.0f s)\n", name.c_str(),
                reversals(f, frames116(10), total, 200), soonestUs == UINT64_MAX ? -1.0 : double(soonestUs) / 1e6,
                double(holdUs) / 1e6);
            check(early == 0, name, "a release waits out the hold after a rise",
                "%zu releases began within %.1f s of a rise; the hold is %.0f s", early,
                soonestUs == UINT64_MAX ? 0.0 : double(soonestUs) / 1e6, double(holdUs) / 1e6);
        }
    }
}

// Interval feedback buys protection for persistent, attributable timing errors.
// Drive the same 3 ms delivery excursion at two frequencies: isolated misses
// should stay inexpensive, while repeated late/early pairs should earn delay.
void testIntervalPressureAndRelease()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const auto name = label("interval-pressure", preset, smoothing);
        const auto session = vrrProductionSession(preset.mode, 90, 120, smoothing.smooth);
        Stream stream(session, 1000);
        for (size_t i = 0; i < frames90(5); ++i) stream.step(1000);
        const auto floor = stream.interval().update.minimumUs;
        const size_t burst = stream.size();
        for (size_t i = 0; i < frames90(35); ++i)
            stream.step(1000, {i % 2 ? 3000u : 0u});
        const auto peak = maximum(stream.frames(), burst, stream.size(), applied);
        check(peak > floor && peak <= 4000, name, "repeated 3 ms excursions earn bounded protection",
              "floor %" PRIu64 " us, peak %" PRIu64 " us", floor, peak);
        const auto historicalCoverage = stream.interval().evaluatedUs;
        const auto disturbedScore = stream.interval().qualityPercent();
        const size_t clean = stream.size();
        const double cleanSeconds = double(stream.windowUs()) / 1e6 + 5;
        for (size_t i = 0; i < frames90(cleanSeconds); ++i) stream.step(1000);
        const auto stats = stream.interval();
        check(stats.scoreWindowUs == stream.windowUs() && historicalCoverage > 0 &&
                  stats.evaluatedUs > stream.windowUs() - 2000000 && stats.qualityPercent() > disturbedScore,
              name, "timing history retains its selected duration and expires old misses",
              "coverage %" PRIu64 " us, score %.2f -> %.2f", stats.evaluatedUs, disturbedScore, stats.qualityPercent());
        const auto& frames = stream.frames();
        check(frames.back().appliedUs <= floor + 250 && reversals(frames, clean + frames90(2), frames.size(), 200) == 0,
              name, "clean intervals release earned protection without rebuilding it",
              "peak %" PRIu64 " us, final %" PRIu64 " us", peak, frames.back().appliedUs);
    }
}

// A few costly long/short pairs per second can exceed the selected loss
// allowance even when their one-second mean error stays inside tolerance.
void testSparseDeliveryPressure()
{
    for (const int fps : {60, 120}) {
        const auto name = scenarioName("sparse-delivery") + "/" + std::to_string(fps) + "fps";
        auto session = vrrProductionSession(1, fps, 2 * fps, false);
        session.timingOptions.toleranceUs = fps == 120 ? 500 : 750;
        session.timingOptions.historySeconds = 15;
        Stream delivery(session, 1000), lateWake(session, 1000);
        const uint32_t ticks = 90000 / fps;
        const int interval = fps / 3;
        constexpr uint64_t delayUs = 5000;
        for (int i = 0; i < 3 * fps; ++i) {
            delivery.step(ticks);
            lateWake.step(ticks);
        }
        const uint64_t floor = delivery.interval().update.minimumUs;
        const size_t disturbed = delivery.size();
        for (int i = 0; i < fps * 60; ++i) {
            const uint64_t excursion = i % interval == 0 ? delayUs : 0;
            delivery.step(ticks, {excursion});
            Input wake;
            wake.presentLatencyUs = excursion;
            lateWake.step(ticks, wake);
        }
        const auto stats = delivery.interval();
        const auto& frames = delivery.frames();
        const size_t settled = disturbed + 30 * fps;
        const uint64_t low = minimum(frames, settled, frames.size(), applied);
        const uint64_t high = maximum(frames, settled, frames.size(), applied);
        const auto score = stats.qualityPercent();
        const auto goal = session.timingOptions.targetHundredths / 100.0;
        report("%-52s settled %.3f%%, goal %.2f%%, buffer %" PRIu64 " to %" PRIu64 " us\n",
               name.c_str(), score, goal, low, high);
        check(stats.evaluatedUs > 14000000 && score >= goal, name,
              "occasional costly delivery pairs meet the configured pacing goal",
              "score %.4f%% over %" PRIu64 " us, goal %.2f%%", score, stats.evaluatedUs, goal);
        check(low > floor && high <= floor + delayUs && high - low <= delivery.toleranceUs(),
              name, "recurring delivery pressure keeps a stable bounded reserve",
              "settled buffer %" PRIu64 " to %" PRIu64 " us, floor %" PRIu64, low, high, floor);
        const auto wakeStats = lateWake.interval();
        const auto wakePeak = maximum(lateWake.frames(), disturbed, lateWake.size(), applied);
        check(wakeStats.qualityPercent() < goal && wakePeak == floor, name,
              "late handoffs with already-ready frames cannot buy input protection",
              "score %.4f%%, peak %" PRIu64 " us for a %" PRIu64 " us floor",
              wakeStats.qualityPercent(), wakePeak, floor);

        // Keep observing handoffs for two entire score windows without
        // producer timing. Late wakeups cannot renew input growth. Once
        // visible, fresh clean delivery must drain the learned protection.
        const size_t missing = delivery.size();
        for (int i = 0; i < fps * session.timingOptions.historySeconds * 2; ++i) {
            Input wake;
            wake.hasDeliveryTimeline = false;
            wake.presentLatencyUs = i % interval == 0 ? delayUs : 0;
            delivery.step(ticks, wake);
        }
        const auto unseenPeak = maximum(delivery.frames(), missing, delivery.size(), applied);
        check(unseenPeak <= high, name, "hand-off errors with missing producer timing cannot grow stale demand",
              "previous maximum %" PRIu64 " us, peak after losing visibility %" PRIu64, high, unseenPeak);
        for (int i = 0; i < fps * 90; ++i) delivery.step(ticks);
        const auto cleanStats = delivery.interval();
        check(delivery.frames().back().appliedUs <= floor + 250 && cleanStats.qualityPercent() >= goal &&
                  cleanStats.evaluatedUs > 14000000, name,
              "fresh clean input drains learned protection after prior evidence expires",
              "last buffer %" PRIu64 " us, floor %" PRIu64 ", score %.4f%% over %" PRIu64 " us",
              delivery.frames().back().appliedUs, floor, cleanStats.qualityPercent(), cleanStats.evaluatedUs);
    }
}

// Keep the revision 6 comparison reproducible. Its advance between misses
// must improve subsequent handoff spacing over revision 5.
void testRecurringHitchForecast()
{
    using Action = Vrr13::IntervalBuffer::Action;
    const auto name = scenarioName("recurring-hitch-forecast");
    auto session = vrrProductionSession(1, 100, 240, false);
    session.timingOptions.historySeconds = 15;
    const uint32_t ticks = 900;
    auto historical = vrrTimingParametersForSession(session);
    historical.playoutVrr18IntervalFeedback = Vrr18GoalIntervalFeedback;
    auto forecast = historical;
    forecast.playoutVrr18IntervalFeedback = Vrr18PredictiveIntervalFeedback;
    Stream revision6(session, forecast, 1000), revision5(session, historical, 1000);
    for (size_t i = 0; i < 300; ++i) {
        revision6.step(ticks);
        revision5.step(ticks);
    }
    size_t cleanPairForecasts = 0;
    uint64_t largestForecastUs = 0;
    uint64_t forecastExcessUs = 0, baselineExcessUs = 0;
    uint64_t forecastLatencyUs = 0, baselineLatencyUs = 0;
    const auto stretch = [&](const Stream& stream) {
        const auto& frames = stream.frames();
        const auto& current = frames.back();
        const auto& previous = frames[frames.size() - 2];
        const auto actual = current.submittedUs - previous.submittedUs;
        const auto allowed = current.sourceUs - previous.sourceUs + stream.toleranceUs();
        return actual - std::min(actual, allowed);
    };
    constexpr size_t disturbedFrames = 2000;
    for (size_t i = 0; i < disturbedFrames; ++i) {
        Input input;
        if (i % 11 == 3) input.latenessUs = 5000;
        const auto& newFrame = revision6.step(ticks, input);
        const auto& oldFrame = revision5.step(ticks, input);
        forecastLatencyUs += newFrame.submittedUs - newFrame.sourceUs;
        baselineLatencyUs += oldFrame.submittedUs - oldFrame.sourceUs;
        forecastExcessUs += stretch(revision6);
        baselineExcessUs += stretch(revision5);
        const auto update = revision6.interval().update;
        if (update.action == Action::ForecastGrow && update.requestedUs > update.beforeUs) {
            largestForecastUs = std::max(largestForecastUs, update.requestedUs - update.beforeUs);
            if (i % 11 != 3 && i % 11 != 4 && update.intervalErrorUs <= revision6.toleranceUs())
                ++cleanPairForecasts;
        }
    }
    report("%-52s %zu clean-pair forecasts, largest %" PRIu64 " us, cumulative stretch %" PRIu64
           " -> %" PRIu64 " us, mean handoff latency %.1f -> %.1f us\n", name.c_str(), cleanPairForecasts,
           largestForecastUs, baselineExcessUs, forecastExcessUs,
           double(baselineLatencyUs) / disturbedFrames, double(forecastLatencyUs) / disturbedFrames);
    check(cleanPairForecasts > 0 && largestForecastUs <= revision6.toleranceUs(), name,
          "recoverable recurring misses earn a bounded advance on a clean adjacent pair",
          "%zu clean-pair forecasts, largest %" PRIu64 " us", cleanPairForecasts, largestForecastUs);
    check(forecastExcessUs < baselineExcessUs && !revision6.replacements() && !revision5.replacements(), name,
          "anticipation reduces measured stretch without shedding frames",
          "revision 5 %" PRIu64 " us, revision 6 %" PRIu64 " us, replacements %zu -> %zu",
          baselineExcessUs, forecastExcessUs, revision5.replacements(), revision6.replacements());
    check(forecastLatencyUs <= baselineLatencyUs + disturbedFrames * revision6.toleranceUs(), name,
          "the average latency cost of earlier protection stays within one tolerance",
          "revision 5 %.1f us, revision 6 %.1f us",
          double(baselineLatencyUs) / disturbedFrames, double(forecastLatencyUs) / disturbedFrames);

    // Each interrupted burst is long enough to fill one evidence window.
    // Two such windows separated by a source epoch cannot predict a miss.
    Stream interrupted(session, forecast, 1000), unmeasured(session, forecast, 1000);
    Stream wake(session, forecast, 1000), tooLate(session, forecast, 1000);
    auto lenient = session;
    lenient.timingOptions.targetHundredths = 9900;
    auto rareParameters = vrrTimingParametersForSession(lenient);
    rareParameters.playoutVrr18IntervalFeedback = Vrr18PredictiveIntervalFeedback;
    Stream rare(lenient, rareParameters, 1000);
    size_t interruptedForecasts = 0, unmeasuredForecasts = 0, wakeForecasts = 0, overCapForecasts = 0, rareForecasts = 0;
    for (size_t i = 0; i < 1500; ++i) {
        const uint64_t delayUs = i % 11 == 3 ? 5000 : 0;
        Input wakeOnly;
        wakeOnly.presentLatencyUs = delayUs;
        wake.step(ticks, wakeOnly);
        tooLate.step(ticks, {delayUs ? 25000u : 0u});
        rare.step(ticks, {i % 73 == 3 ? 3000u : 0u});
        interrupted.step(ticks, {delayUs});
        Input partialEvidence;
        partialEvidence.latenessUs = delayUs;
        partialEvidence.hasDeliveryTimeline = i % 150 != 149;
        unmeasured.step(ticks, partialEvidence);
        interruptedForecasts += interrupted.interval().update.action == Action::ForecastGrow;
        unmeasuredForecasts += unmeasured.interval().update.action == Action::ForecastGrow;
        wakeForecasts += wake.interval().update.action == Action::ForecastGrow;
        overCapForecasts += tooLate.interval().update.action == Action::ForecastGrow;
        rareForecasts += rare.interval().update.action == Action::ForecastGrow;
        if (i % 150 == 149) interrupted.rebase();
    }
    check(!interruptedForecasts, name, "a source rebase discards incomplete forecast evidence",
          "%zu forecasts across separate epochs", interruptedForecasts);
    check(!unmeasuredForecasts, name, "missing independent readiness breaks forecast exposure",
          "%zu forecasts bridged a missing producer timestamp", unmeasuredForecasts);
    check(!wakeForecasts && !overCapForecasts, name, "wake delays and over-cap stalls cannot authorize a forecast",
          "%zu wake forecasts, %zu over-cap forecasts", wakeForecasts, overCapForecasts);
    check(!rareForecasts, name, "within-budget recurring misses cannot authorize a forecast",
          "%zu forecasts at a %.2f%% goal", rareForecasts, lenient.timingOptions.targetHundredths / 100.0);
}

// Drive both policies with identical frames. Measure the last score violation
// so briefly crossing the goal cannot be mistaken for sustained recovery.
void testForecastGoalRecovery()
{
    const auto name = scenarioName("forecast-goal-recovery");
    constexpr size_t fps = 120;
    constexpr uint32_t ticks = 750;
    // At 120 Hz the unlatched presenter's spacing guard blocks input
    // observations. Give it display headroom to exercise the same predictor.
    auto session = vrrProductionSession(1, fps, t_CanLatch ? 120 : 240, false);
    session.timingOptions.historySeconds = 15;
    const double goal = session.timingOptions.targetHundredths / 100.0;
    auto baseline = vrrTimingParametersForSession(session);
    baseline.playoutVrr18IntervalFeedback = Vrr18PredictiveIntervalFeedback;
    Stream revision6(session, baseline, 1000), live(session, 1000);

    struct Result {
        size_t recoveryFrame = 0, hitches = 0;
        uint64_t excessUs = 0, meanUs = 0, p99Us = 0, peakUs = 0;
    };
    const auto measure = [&](Stream& stream) {
        for (size_t i = 0; i < 3 * fps; ++i) stream.step(ticks);
        const size_t onset = stream.size();
        Result result;
        Frame previous = stream.frames().back();
        std::vector<uint64_t> handoffLatency;
        for (size_t i = 0; i < 90 * fps; ++i) {
            const auto& frame = stream.step(ticks, {i % 11 == 3 ? 5000u : 0u});
            if (stream.interval().qualityPercent() < goal) result.recoveryFrame = i + 1;
            if (frame.dropped) continue;
            handoffLatency.push_back(frame.submittedUs - frame.decodedUs);
            if (i < 20 * fps) {
                const int64_t stretch = int64_t(frame.submittedUs - previous.submittedUs) -
                                        int64_t(frame.sourceUs - previous.sourceUs);
                result.hitches += stretch > 2000;
                result.excessUs += uint64_t(std::max<int64_t>(0, stretch - int64_t(stream.toleranceUs())));
            }
            previous = frame;
        }
        const auto latencyUs = [](const Frame& f) { return f.submittedUs - f.decodedUs; };
        result.meanUs = mean(stream.frames(), onset, stream.size(), latencyUs);
        result.peakUs = maximum(stream.frames(), onset, stream.size(), latencyUs);
        result.p99Us = percentile(handoffLatency, 990);
        return result;
    };
    const auto old = measure(revision6);
    const auto now = measure(live);
    report("%-52s sustained goal %.2f -> %.2f s, first 20 s hitches %zu -> %zu, stretch %" PRIu64
           " -> %" PRIu64 " us, handoff mean/p99/max %" PRIu64 "/%" PRIu64 "/%" PRIu64
           " -> %" PRIu64 "/%" PRIu64 "/%" PRIu64 " us\n", name.c_str(),
           double(old.recoveryFrame) / fps, double(now.recoveryFrame) / fps,
           old.hitches, now.hitches, old.excessUs, now.excessUs,
           old.meanUs, old.p99Us, old.peakUs, now.meanUs, now.p99Us, now.peakUs);
    check(live.interval().qualityPercent() >= goal && revision6.interval().qualityPercent() >= goal &&
              now.recoveryFrame + fps <= old.recoveryFrame,
          name, "persistent delivery errors reach and retain the configured goal sooner",
          "goal %.2f%%, sustained after %.2f -> %.2f s", goal,
          double(old.recoveryFrame) / fps, double(now.recoveryFrame) / fps);
    check(now.hitches < old.hitches && now.excessUs < old.excessUs &&
              live.replacements() <= revision6.replacements(),
          name, "quicker recovery reduces both costly handoff hitches and stretch",
          "2 ms hitches %zu -> %zu, excess %" PRIu64 " -> %" PRIu64 " us, replacements %zu -> %zu",
          old.hitches, now.hitches, old.excessUs, now.excessUs, revision6.replacements(), live.replacements());
    check(now.meanUs <= old.meanUs + 1000 && now.p99Us <= old.p99Us + 1000 && now.peakUs <= old.peakUs + 1000,
          name, "mean, p99 and peak decode-to-handoff latency each stay within 1 ms of revision 6",
          "mean/p99/peak %" PRIu64 "/%" PRIu64 "/%" PRIu64 " -> %" PRIu64 "/%" PRIu64 "/%" PRIu64 " us",
          old.meanUs, old.p99Us, old.peakUs, now.meanUs, now.p99Us, now.peakUs);

    const uint64_t oldReserveUs = live.frames().back().appliedUs;
    for (size_t i = 0; i < 90 * fps; ++i) live.step(ticks);
    const uint64_t floorUs = live.interval().update.minimumUs;
    check(live.frames().back().appliedUs <= floorUs + 250 && live.interval().qualityPercent() >= goal, name,
          "clean delivery releases the reserve earned by recurring hitches",
          "reserve after recovery %" PRIu64 " us, last %" PRIu64 " us, floor %" PRIu64 " us",
          oldReserveUs, live.frames().back().appliedUs, floorUs);
    const size_t nextOnset = live.size();
    for (size_t i = 0; i < 60 * fps; ++i) live.step(ticks, {i % 11 == 3 ? 3000u : 0u});
    const auto high = maximum(live.frames(), nextOnset, live.size(), applied);
    check(live.interval().qualityPercent() >= goal && high > floorUs && high + 500 <= oldReserveUs, name,
          "a later smaller delivery excursion earns a correspondingly smaller reserve",
          "3 ms excursion peak %" PRIu64 " us, previous 5 ms reserve %" PRIu64 " us, score %.3f%%",
          high, oldReserveUs, live.interval().qualityPercent());
}

void testForecastRequiresRecurringDelivery()
{
    using Action = Vrr13::IntervalBuffer::Action;
    const auto name = scenarioName("forecast-evidence");
    auto session = vrrProductionSession(1, 120, 240, false);
    session.timingOptions.historySeconds = 15;
    Stream tooLate(session, 1000), wake(session, 1000);
    Stream irregular(session, 1000), missing(session, 1000), interrupted(session, 1000);
    auto lenient = session;
    lenient.timingOptions.targetHundredths = 9900;
    Stream rare(lenient, 1000);
    size_t capForecasts = 0, wakeForecasts = 0, cadenceForecasts = 0;
    size_t missingForecasts = 0, interruptedForecasts = 0, rareForecasts = 0;
    const auto forecast = [](const Stream& stream) { return stream.interval().update.action == Action::ForecastGrow; };
    for (size_t i = 0; i < 3600; ++i) {
        const auto delayUs = i % 11 == 3 ? 5000u : 0u;
        tooLate.step(750, {delayUs ? 25000u : 0u});
        Input wakeOnly;
        wakeOnly.presentLatencyUs = delayUs;
        wake.step(750, wakeOnly);
        irregular.step(i % 2 ? 600 : 900);
        Input invisible;
        invisible.latenessUs = delayUs;
        invisible.hasDeliveryTimeline = false;
        missing.step(750, invisible);
        interrupted.step(750, {delayUs});
        if (i % 11 == 10) interrupted.rebase();
        rare.step(750, {i % 97 == 3 ? 3000u : 0u});
        capForecasts += forecast(tooLate);
        wakeForecasts += forecast(wake);
        cadenceForecasts += forecast(irregular);
        missingForecasts += forecast(missing);
        interruptedForecasts += forecast(interrupted);
        rareForecasts += forecast(rare);
    }
    // Enter each run below goal because of presenter wakes. Slide the
    // delivery burst across window boundaries to check both an isolated
    // stretch/catch-up pair and two genuinely late frames only 150 ms apart.
    for (size_t delayedFrames : {1u, 2u}) {
        size_t forecasts = 0, belowGoal = 0;
        for (size_t phase = 0; phase < 120; ++phase) {
            Stream shortBurst(session, 1000);
            const size_t first = 780 + phase;
            for (size_t i = 0; i < 1200; ++i) {
                Input input;
                if (i < 720 && i % 11 == 3) input.presentLatencyUs = 5000;
                if (i == first || (delayedFrames == 2 && i == first + 18)) input.latenessUs = 5000;
                shortBurst.step(750, input);
                if (i == first) belowGoal += shortBurst.interval().qualityPercent() <
                                             session.timingOptions.targetHundredths / 100.0;
                forecasts += forecast(shortBurst);
            }
        }
        check(belowGoal == 120 && !forecasts, name,
              "a brief episode cannot corroborate itself across observation windows",
              "%zu late frames, %zu of 120 phases below goal, %zu forecasts", delayedFrames, belowGoal, forecasts);
    }
    check(!capForecasts && !wakeForecasts && !cadenceForecasts, name,
          "over-cap stalls, presenter wakes and irregular source intervals cannot earn forecasts",
          "over-cap %zu, wake-only %zu, irregular cadence %zu", capForecasts, wakeForecasts, cadenceForecasts);
    check(!missingForecasts && !interruptedForecasts, name,
          "forecasts require adjacent measured deliveries from the same source epoch",
          "missing readiness %zu, rebased %zu", missingForecasts, interruptedForecasts);
    check(!rareForecasts && rare.interval().qualityPercent() >= 99.0, name,
          "recurring deliveries inside the configured budget cannot earn forecasts",
          "%zu forecasts, final score %.3f%%", rareForecasts, rare.interval().qualityPercent());
}

// Normally ready frames wake 1.9 ms after their deadline. That wait masks
// most of the alternating 3 ms input delays. Protecting occasional 6 ms
// deliveries more aggressively would compress the masked handoffs.
void testForecastChargesInducedCompression()
{
    using Buffer = Vrr13::IntervalBuffer;
    const auto name = scenarioName("forecast-mixed-wakes");
    struct Result { size_t forecasts = 0; uint64_t peakUs = 0; double score = 0; };
    const auto run = [](bool costAware) {
        Buffer observer;
        Result result;
        for (uint64_t i = 1; i < 4500; ++i) {
            Buffer::Sample sample;
            sample.frame = i;
            sample.smoothedRtpSlotUs = sample.intended = kOriginUs + i * 10000;
            sample.sourcePeriodUs = 10000;
            sample.buffer = observer.demand(1000);
            sample.deadline = sample.intended + sample.buffer;
            uint64_t deliveryUs = 0, wakeUs = 0;
            if (i > 350) {
                if (i % 47 == 1) deliveryUs = 6000;
                else if (i % 2 == 0) deliveryUs = 3000;
                else wakeUs = 1900;
            }
            sample.independentReadyUs = sample.ready = sample.intended + deliveryUs;
            sample.submitted = std::max(sample.ready, sample.deadline) + wakeUs;
            sample.absorbable = sample.valid = true;
            sample.serialService = 500;
            observer.observe(sample, 1000, 8333, 3000000, 500, true, 995000,
                500, 15000000, 1000000, 2, true, 0, true, true, true, true, costAware);
            result.forecasts += observer.stats().update.action == Buffer::Action::ForecastGrow;
            result.peakUs = std::max(result.peakUs, sample.buffer);
        }
        result.score = observer.stats().qualityPercent();
        return result;
    };
    const auto baseline = run(false), live = run(true);
    check(!live.forecasts && live.peakUs <= baseline.peakUs && live.score + 0.001 >= baseline.score, name,
          "shadow reserves pay for the compressions they induce on masked handoffs",
          "%zu forecasts, revision 6 to 7 peak %" PRIu64 " -> %" PRIu64 " us, score %.3f -> %.3f%%",
          live.forecasts, baseline.peakUs, live.peakUs, baseline.score, live.score);
}

// The same 5 ms delivery excursions fit the available reserve on both
// clocks. Alternate 7/13 ms source intervals around a 10 ms nominal period
// to keep source deviation above twice the 500 us pacing tolerance.
void testForecastRequiresBoundedSourceWander()
{
    using Buffer = Vrr13::IntervalBuffer;
    const auto name = scenarioName("forecast-erratic-source");
    for (bool erratic : {false, true}) {
        Buffer observer;
        uint64_t slotUs = kOriginUs;
        size_t forecasts = 0, belowGoal = 0;
        for (uint64_t frame = 1; frame <= 1800; ++frame) {
            slotUs += erratic ? (frame % 2 ? 7000 : 13000) : 10000;
            Buffer::Sample sample;
            sample.frame = frame;
            sample.smoothedRtpSlotUs = sample.intended = slotUs;
            sample.sourcePeriodUs = 10000;
            sample.buffer = observer.demand(1000);
            sample.deadline = slotUs + sample.buffer;
            sample.ready = sample.independentReadyUs = slotUs + (frame > 300 && frame % 11 == 3 ? 5000 : 0);
            sample.submitted = std::max(sample.ready, sample.deadline);
            sample.valid = sample.absorbable = true;
            sample.serialService = 500;
            observer.observe(sample, 1000, 8333, 3000000, 500, true, 995000,
                500, 15000000, 1000000, 2, true, 0, true, true, true, true, true);
            const auto stats = observer.stats();
            belowGoal += stats.averageValid && stats.qualityPercent() < 99.5;
            forecasts += stats.update.action == Buffer::Action::ForecastGrow;
        }
        check(belowGoal && (erratic ? forecasts == 0 : forecasts > 0), name,
              "recurring late input forecasts only while source deviation remains bounded",
              "%s source, %zu below-goal intervals, %zu forecasts", erratic ? "alternating" : "steady",
              belowGoal, forecasts);
    }
}

// A 6 ms starting buffer covers all input variation. If a
// real 4 ms spacing floor also covers it, removing that buffer is free.
// Use the same ready times without a floor to check the inverse decision.
void testReleaseBehindSpacingFloor()
{
    const auto name = scenarioName("release-with-spacing-floor");

    // The first image was ready at 8 ms but a fixed floor held it to 10 ms.
    // Its successor was ready at 16 ms; its 8 ms spacing floor still puts
    // the handoff at 18 ms, even if either frame's 3 ms buffer drops to 1 ms.
    Vrr13::IntervalBuffer::Sample first;
    first.ready = first.independentReadyUs = 8000;
    first.submitted = first.spacingFloor = 10000;
    first.buffer = 3000;
    auto successor = first;
    successor.ready = successor.independentReadyUs = 16000;
    successor.submitted = successor.spacingFloor = 18000;
    successor.floorFollowsPrevious = true;
    successor.predecessorBufferUs = first.buffer;
    successor.predecessorAdvanceUs = first.maximumSubmissionAdvanceUs(1000);
    check(successor.maximumSubmissionAdvanceUs(1000) == 0, name,
          "an immovable earlier floor still holds the next early-ready handoff",
          "predicted advance %" PRIu64 " us", successor.maximumSubmissionAdvanceUs(1000));

    // A genuinely movable earlier handoff can buy back 2 ms, up to the
    // successor's own independent floor. Preserve this partial release.
    first.spacingFloor = 0;
    successor.predecessorAdvanceUs = first.maximumSubmissionAdvanceUs(1000);
    successor.scanoutFloor = 17000;
    check(successor.maximumSubmissionAdvanceUs(1000) == 1000, name,
          "a moving predecessor never moves the successor's independent floor",
          "predicted advance %" PRIu64 " us", successor.maximumSubmissionAdvanceUs(1000));
    const auto run = [](uint64_t floorOffsetUs, uint64_t occasionalReadinessUs) {
        const auto policy = vrrTimingParametersForSession(vrrProductionSession(1, 100, 144, false));
        Vrr13::IntervalBuffer observer;
        for (uint64_t frame = 1; frame <= 6000; ++frame) {
            Vrr13::IntervalBuffer::Sample sample;
            sample.frame = frame;
            sample.intended = kOriginUs + frame * 10000;
            sample.buffer = observer.demand(6000);
            sample.deadline = sample.intended + sample.buffer;
            sample.ready = sample.intended + (frame % 5 ? 0 : occasionalReadinessUs);
            sample.independentReadyUs = sample.ready;
            sample.valid = sample.absorbable = true;
            sample.serialService = 500;
            sample.smoothedRtpSlotUs = sample.intended;
            sample.sourcePeriodUs = 10000;
            sample.spacingFloor = floorOffsetUs ? sample.intended + floorOffsetUs : 0;
            sample.submitted = std::max({sample.deadline, sample.ready, sample.spacingFloor});
            observer.observe(sample, 1000, 10000, policy.playoutMeanMissHoldUs,
                policy.playoutMeanMissReleaseUsPerSecond, true, policy.playoutOnTimeTargetPerMillion,
                policy.playoutIntervalToleranceUs, 15000000, policy.playoutIntervalInitialWarmupUs,
                policy.playoutIntervalInitialMinimumSamples, true, 0, true, true,
                policy.playoutVrr18IntervalFeedback >= Vrr18GoalIntervalFeedback,
                policy.playoutVrr18IntervalFeedback >= Vrr18PredictiveIntervalFeedback,
                policy.playoutVrr18IntervalFeedback >= Vrr18CostAwareIntervalFeedback,
                policy.playoutVrr18IntervalFeedback >= Vrr18AnticipatoryIntervalFeedback,
                policy.playoutVrr18IntervalFeedback >= Vrr18TargetAwareRelease,
                policy.playoutVrr18IntervalFeedback);
        }
        return observer.stats().update.requestedUs;
    };
    const uint64_t covered = run(4000, 3500), unconstrained = run(0, 3500);
    check(covered == 1000 && unconstrained > covered && unconstrained <= 3500, name,
          "a real pacing floor can cover input even when earlier than the currently buffered deadline",
          "requested %" PRIu64 " us with a hard floor and %" PRIu64 " us without", covered, unconstrained);
    const uint64_t withinTolerance = run(4000, 4300);
    check(withinTolerance == 1000, name,
          "an excursion just past the floor costs only its remaining lateness",
          "requested %" PRIu64 " us despite only 300 us beyond the floor", withinTolerance);
    const uint64_t beyondTolerance = run(4000, 6000);
    check(beyondTolerance > 4000 && beyondTolerance <= 6000, name,
          "repeated misses beyond the floor can still earn protection",
          "requested %" PRIu64 " us for recurring 2 ms overruns", beyondTolerance);
}

// Use identical sender timestamps in a source-only and a mixed impairment.
// Late delivery also stretches the interval ending at a late frame and
// compresses its successor. Neither interval can erase a host-only gap.
void testSourceCadenceBufferGrowth()
{
    for (const auto& smoothing : kSmoothing) {
        const auto name = scenarioName("source-cadence-buffer") + "/" + smoothing.name;
        auto session = vrrProductionSession(1, 120, 120, smoothing.smooth);
        session.timingOptions = {2000, 9950, 120, 500};
        Stream sourceOnly(session, 1000), mixed(session, 1000);
        for (size_t i = 0; i < 400; ++i) {
            sourceOnly.step(1500);
            mixed.step(1500);
        }
        constexpr uint32_t staggeredTicks[] = {2040, 1500, 960, 1500};
        const size_t start = sourceOnly.size();
        for (size_t i = 0; i < 1200; ++i) {
            const auto ticks = staggeredTicks[i % 4];
            sourceOnly.step(ticks);
            // The independent 4 ms delivery miss coincides with a long host interval.
            mixed.step(ticks, {i % 4 == 0 ? 4000u : 0u});
        }
        const auto hostPeak = maximum(sourceOnly.frames(), start, sourceOnly.size(), applied);
        const auto mixedPeak = maximum(mixed.frames(), start, mixed.size(), applied);
        check(hostPeak == 1000, name, "host stagger alone cannot grow the buffer",
              "host-only peak %" PRIu64 " us", hostPeak);
        check(mixedPeak > hostPeak && mixedPeak <= hostPeak + 4000, name,
              "independent delivery jitter during host stagger earns bounded protection",
              "host-only peak %" PRIu64 " us, mixed peak %" PRIu64 " us", hostPeak, mixedPeak);

        for (size_t i = 0; i < 1200; ++i) sourceOnly.step(1500, {i % 4 == 1 ? 4000u : 0u});
        check(sourceOnly.frames().back().appliedUs > 1000, name,
              "steady 60 FPS delivery can requalify at its own rate",
              "final %" PRIu64 " us", sourceOnly.frames().back().appliedUs);
    }
}

void testClientTimingAttribution()
{
    const auto name = scenarioName("client-timing-attribution");
    const auto session = vrrProductionSession(1, 100, 120, false);
    Stream delivery(session, 1000), client(session, 1000);
    for (size_t i = 0; i < 400; ++i) {
        delivery.step(1000);
        client.step(1000);
    }
    const auto initial = delivery.interval();
    check(initial.clientEvaluatedUs > 0 && initial.clientQualityPercent() == 100.0,
          name, "timely 90 FPS input scores cleanly in a 100 FPS session",
          "scored %" PRIu64 " us at %.4f%%", initial.clientEvaluatedUs, initial.clientQualityPercent());

    delivery.step(1000, {4000});
    const auto lateDelivery = delivery.interval();
    delivery.step(1000);
    const auto afterDelivery = delivery.interval();
    check(lateDelivery.qualityPercent() < initial.qualityPercent() &&
              lateDelivery.clientEvaluatedUs == initial.clientEvaluatedUs &&
              afterDelivery.clientEvaluatedUs == initial.clientEvaluatedUs &&
              !lateDelivery.update.clientTimingHitch && !afterDelivery.update.clientTimingHitch,
          name, "late delivery still counts for pacing, while both affected intervals skip client timing",
          "pacing %.4f%%, client scored %" PRIu64 " -> %" PRIu64 " -> %" PRIu64 " us",
          lateDelivery.qualityPercent(), initial.clientEvaluatedUs,
          lateDelivery.clientEvaluatedUs, afterDelivery.clientEvaluatedUs);

    Input backlog;
    backlog.decoderQueueUs += 4000;
    client.step(1000, backlog);
    const auto lateClient = client.interval();
    check(lateClient.clientEvaluatedUs > initial.clientEvaluatedUs &&
              lateClient.clientQualityPercent() < initial.clientQualityPercent() &&
              lateClient.update.clientTimingHitch,
          name, "the same delay after reassembly lowers client timing and marks the stretched interval",
          "client scored %" PRIu64 " us at %.4f%%, marker %d", lateClient.clientEvaluatedUs,
          lateClient.clientQualityPercent(), lateClient.update.clientTimingHitch);

    Stream varying(session, 1000);
    for (size_t i = 0; i < 400; ++i) varying.step(1000);
    const auto beforeRateChange = varying.interval().clientEvaluatedUs;
    for (size_t i = 0; i < 400; ++i) varying.step(i % 2 ? 900 : 1000);
    const auto variableRate = varying.interval();
    check(variableRate.clientEvaluatedUs > beforeRateChange &&
              variableRate.clientQualityPercent() >= 99.95,
          name, "variable 90 to 100 FPS input remains eligible and does not create client timing errors",
          "client score %.4f%%", variableRate.clientQualityPercent());
}

// Source cadence, not requested FPS, determines the interval being followed.
// Returning to 90 FPS must not restore an obsolete predictor layer when the
// new arrivals are clean. A short ramp tests the transitions as well as dwell.
void testRateRoundTrip()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const auto name = label("rate-round-trip", preset, smoothing);
        const auto session = vrrProductionSession(preset.mode, 90, 120, smoothing.smooth);
        Stream stream(session, 1000);
        for (size_t i = 0; i < frames90(35); ++i)
            stream.step(1000, {i % 2 ? 3000u : 0u});
        const size_t change = stream.size();
        const uint64_t learned = stream.frames().back().appliedUs;
        for (int rate = 89; rate >= 30; --rate) stream.step(90000 / rate);
        for (int i = 0; i < 30 * 30; ++i) stream.step(k30Ticks);
        const size_t returning = stream.size();
        for (int rate = 31; rate <= 90; ++rate) stream.step(90000 / rate);
        for (size_t i = 0; i < frames90(120); ++i) stream.step(1000);
        const auto& frames = stream.frames();
        const auto floor = stream.interval().update.minimumUs;
        check(learned > floor, name, "rate change starts with earned protection",
              "learned %" PRIu64 " us, floor %" PRIu64 " us", learned, floor);
        check(maximum(frames, change, returning, applied) <= learned + 250, name,
              "the slower cadence alone does not build a new reserve", "prior buffer %" PRIu64 " us", learned);
        check(frames.back().appliedUs <= floor + 250 && hitches(frames, frames.size() - frames90(10),
              frames.size(), stream.toleranceUs()) == 0, name, "a clean return settles at the floor and follows source cadence",
              "final buffer %" PRIu64 " us, floor %" PRIu64 " us", frames.back().appliedUs, floor);
    }
}

// Match the original sparse delivery excursions at steady and varying source
// cadence. Measure pacing of the displayed frames and continuity of the reserve.
void testOrdinaryVariation()
{
    const auto ticksForRate = [](double fps) { return uint32_t(90000.0 / fps + 0.5); };
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("ordinary-variation", preset, smoothing);
        const auto session = vrrProductionSession(preset.mode, 116, 120, smoothing.smooth);
        // The same disturbance sequence at a varying and at a steady rate.
        struct Run { uint64_t largestRampDrop; size_t hitches; };
        const auto run = [&](bool varying) {
            Stream s(session, 1000);
            Rng rng(113 + uint64_t(preset.mode)), rateRng(127);
            uint64_t spikeUs = 3000, elapsedTicks = 0;
            size_t counter = 0;
            const auto step = [&](uint32_t ticks) {
                s.step(ticks, {rng.below(400) + (counter++ % 20 == 10 ? spikeUs : 0)});
                elapsedTicks += ticks;
            };
            const auto jittered = [&](double fps) {
                return varying ? uint32_t(int64_t(ticksForRate(fps)) + int64_t(rateRng.below(55)) - 27) : k116Ticks;
            };
            while (elapsedTicks < 20 * 90000) step(k116Ticks);
            const size_t rampBegin = s.size();
            // A smooth 116 -> 90 -> 116 FPS ramp with 0.3 ms of host jitter.
            for (uint64_t begin = elapsedTicks; elapsedTicks - begin < 20 * 90000;) {
                const double x = double(elapsedTicks - begin) / (20 * 90000);
                step(jittered(116 - 26 * (x < 0.5 ? 2 * x : 2 - 2 * x)));
            }
            const size_t wanderBegin = s.size();
            // Random dwell across the band; at its lower end, the 12.6/13.3/
            // 13.6 ms tails that once switched a 91 FPS layer. The need grows.
            spikeUs = preset.mode == 2 ? 4000 : 4500;
            size_t nextTail = 300;
            for (uint64_t begin = elapsedTicks; elapsedTicks - begin < 20 * 90000;) {
                const double fps = 90 + double(rateRng.below(27));
                const size_t dwell = 30 + size_t(rateRng.below(200));
                for (size_t i = 0; i < dwell && elapsedTicks - begin < 20 * 90000; ++i) {
                    step(jittered(fps));
                    if (--nextTail == 0) {
                        if (varying && fps <= 100) for (uint32_t tail : {1138u, 1198u, 1224u}) step(tail);
                        nextTail = 300;
                    }
                }
            }
            const auto& f = s.frames();
            size_t lastTen = f.size();
            while (lastTen > wanderBegin && f.back().sourceUs - f[lastTen - 1].sourceUs < 10000000) --lastTen;
            return Run{largestDrop(f, rampBegin, wanderBegin), hitches(f, lastTen, f.size(), s.toleranceUs())};
        };
        const Run varying = run(true), steady = run(false);
        const size_t hitchBound = steady.hitches * 3 / 2 + 5;
        check(varying.largestRampDrop <= uint64_t(session.timingOptions.toleranceUs), name,
              "a gradual rate change does not discard protection in one step",
              "largest single-frame buffer reduction %" PRIu64 " us", varying.largestRampDrop);
        check(varying.hitches <= hitchBound, name, "rate variation covers the disturbance like a steady rate",
            "%zu hitches in the last 10 s while varying, %zu at a steady rate", varying.hitches, steady.hitches);
        // Three long intervals at 116 FPS look like a brief 76 FPS layer. The
        // detector may switch, but the excursion must cost no protection.
        Stream excursion(session, 1000);
        Rng excursionRng(131 + uint64_t(preset.mode));
        for (size_t i = 0; i < frames116(20); ++i)
            excursion.step(k116Ticks, {excursionRng.below(400) + (i % 20 == 10 ? 3000 : 0)});
        const uint64_t excursionLearned = excursion.frames().back().appliedUs;
        const size_t excursionBegin = excursion.size();
        for (int stall = 0; stall < 6; ++stall) {
            for (uint32_t tail : {1138u, 1198u, 1224u}) excursion.step(tail, {excursionRng.below(400)});
            for (size_t i = 0; i < frames116(5); ++i)
                excursion.step(k116Ticks, {excursionRng.below(400) + (excursion.size() % 20 == 10 ? 3000 : 0)});
        }
        const auto& e = excursion.frames();
        const uint64_t excursionMinimum = minimum(e, excursionBegin, e.size(), applied);
        uint64_t largestRise = 0;
        for (size_t i = excursionBegin + 1; i < e.size(); ++i)
            if (e[i].appliedUs > e[i - 1].appliedUs) largestRise = std::max(largestRise, e[i].appliedUs - e[i - 1].appliedUs);
        report("%-52s stall excursions: min %5" PRIu64 " of %5" PRIu64 ", largest restore step %4" PRIu64 "\n",
            name.c_str(), excursionMinimum, excursionLearned, largestRise);
        check(excursionMinimum + 250 >= excursionLearned, name, "a three-frame stall excursion costs no protection",
            "fell to %" PRIu64 " us from %" PRIu64 " us around a brief long-interval run", excursionMinimum, excursionLearned);
        check(largestRise <= excursion.toleranceUs(), name, "returning from a stall excursion needs no visible restore step",
            "buffer jumped %" PRIu64 " us in one frame", largestRise);
    }
}

// 5c. At the display ceiling no presenter can repay phase by showing frames
// faster: released input buffer and late-frame carry become standing ownership
// phase, reported as Retiming budget, unless one frame is replaced by a ready
// successor. A release must be shed: the buffer said that protection is
// excess. Phase a late frame forced is kept instead, up to a refresh, as fixed
// refresh keeps its queued frame (versus-fixed-refresh). Regressions: the
// 2026-10-20 03:58 capture (120 FPS on 120 Hz, DXGI, Absorb) held about 5 ms of
// retiming for minutes after its buffer released, the replacement existing
// only for native scheduled presentation; ceiling policy 1 then replaced a
// frame after every late frame.
void testDisplayCeilingShedsStandingPhase()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("display-ceiling", preset, smoothing);
        const auto session = vrrProductionSession(preset.mode, 120, 120, smoothing.smooth);
        const uint64_t tolerance = uint64_t(session.timingOptions.toleranceUs);
        const uint64_t budget = !smoothing.smooth ? 0 : tolerance;
        // Score after even Smooth (50 us/s) can shed its cold reserve.
        const size_t total = 120 * 240, settledFrom = total - 20 * 120;
        constexpr uint64_t kLateUs = 4000, kJitterUs = 400, kRefreshUs = 8333;
        // One late frame every 5 s adds carry that running faster cannot repay.
        const auto run = [&](uint64_t seedUs) {
            auto stream = std::make_unique<Stream>(session, seedUs);
            Rng rng(137 + uint64_t(preset.mode));
            for (size_t i = 0; i < total; ++i)
                stream->step(750, {rng.below(kJitterUs) + (i % 600 == 300 ? kLateUs : 0)});
            return stream;
        };
        // Cold start releases the starting delay at the ceiling; the reference
        // starts at the floor with the same input.
        const auto cold = run(0), floor = run(1000);
        const auto& f = cold->frames();
        std::vector<uint64_t> planned, shown;
        for (size_t i = settledFrom; i < total; ++i) {
            if (f[i].dropped) continue;
            planned.push_back(f[i].extraUs);
            shown.push_back(f[i].shownExtraUs());
        }
        const uint64_t plannedP50 = percentile(planned, 500), shownP50 = percentile(shown, 500);
        const uint64_t coldLatency = mean(f, settledFrom, total, latency);
        const uint64_t floorLatency = mean(floor->frames(), settledFrom, total, latency);
        // The input buffer settles at its floor in both runs, so on-screen
        // latency differs from it only by retiming and readiness.
        const uint64_t floorBuffer = floor->frames().back().appliedUs;
        const uint64_t start = f[0].appliedUs;
        // The most a late frame overran its protection, plus the 0.5 ms
        // handoff and preparation it reaches the screen with; never more than
        // a refresh is kept.
        const uint64_t forced = std::min(kRefreshUs,
            kLateUs + kJitterUs + 500 - std::min(kLateUs + kJitterUs + 500, floorBuffer));
        const uint64_t kept = std::max(budget, forced);
        // Each replacement sheds up to one period: the starting delay, in up to
        // two steps while the release is still converging. Late frames under
        // a refresh cost none. A presenter that cannot latch must stay a guard
        // above the previous present, so at exactly its maximum refresh it
        // cannot show every frame; bound it to 4 per second.
        // A replacement cannot reclaim a whole refresh while the intentional
        // input buffer fills that time again. Bound startup using the standing
        // phase threshold beyond the retained retiming budget, less the known
        // source jitter; then require no replacements after cold release.
        constexpr uint64_t kSourceDisplacementUs = 1000;
        const uint64_t shedCredit = tolerance + kSourceDisplacementUs - kJitterUs;
        const uint64_t releasable = start - std::min(start, floorBuffer);
        const size_t allowed = t_CanLatch ? size_t((releasable + shedCredit - 1) / shedCredit) + 1 : total / 30;
        const double coldReleaseSeconds = double(releasable) / double(cold->fallUsPerSecond()) +
            3.0 * double(cold->releaseTauUs()) / 1e6 + 5.0;
        const size_t coldReleaseEnd = std::min(total, size_t(coldReleaseSeconds * 120) + 1);
        const size_t settledReplacements = size_t(std::count_if(f.begin() + coldReleaseEnd, f.end(),
            [](const Frame& frame) { return frame.dropped; }));
        report("%-52s start %5" PRIu64 " retiming p50 planned %4" PRIu64 " shown %4" PRIu64 " (kept %4" PRIu64 ") latency %5" PRIu64 " (floor start %5" PRIu64 ") replacements %zu (allowed %zu)\n",
            name.c_str(), start, plannedP50, shownP50, kept, coldLatency, floorLatency, cold->replacements(), allowed);
        check(plannedP50 <= kept + tolerance + 50, name, "planned retiming at the display ceiling stays inside its budget",
            "planned retiming p50 %" PRIu64 " us over the last 20 s; budget %" PRIu64 " us, late-frame phase %" PRIu64
            " us, plus T %" PRIu64 " us", plannedP50, budget, forced, tolerance);
        check(shownP50 <= kept + tolerance + 50, name, "on-screen retiming at the display ceiling stays inside its budget",
            "frames reached the screen %" PRIu64 " us (p50) after their smoothed slot; budget %" PRIu64
            " us, late-frame phase %" PRIu64 " us, plus T %" PRIu64 " us", shownP50, budget, forced, tolerance);
        check(coldLatency <= floorLatency + tolerance, name, "a released starting delay actually lowers latency at the ceiling",
            "latency %" PRIu64 " us after releasing the cold start, %" PRIu64 " us when started at the floor",
            coldLatency, floorLatency);
        check(floorLatency <= kBaseLatencyUs + 100 + floorBuffer + kept + tolerance + 1000, name,
            "late frames leave no more standing phase than they forced at the ceiling",
            "latency %" PRIu64 " us from a floor start with a %" PRIu64 " us buffer, %" PRIu64 " us budget and %" PRIu64
            " us late-frame phase", floorLatency, floorBuffer, budget, forced);
        check(cold->replacements() <= allowed, name, "late frames under a refresh cost no replacement at the ceiling",
            "%zu replacements in 90 s; allowed %zu for the starting delay", cold->replacements(), allowed);
        if (t_CanLatch)
            check(settledReplacements == 0, name, "recurring late frames cost no replacements after the cold release bound",
                "%zu replacements after %.1f seconds", settledReplacements, coldReleaseSeconds);
    }
}

// Moonlight's fixed-refresh V-Sync pacer (Pacer::handleVsync) on the same
// input: the baseline the ceiling policy has to beat. Vsync ticks at phase +
// k * D. Each tick drops queued frames beyond its target (one, or three while
// any of the last 500 ms of queue samples held at most one frame), then takes
// the oldest queued frame or waits up to 1000/Hz - 3 ms for one to arrive.
// The frame it takes is shown on the next vsync; the pacing queue holds three.
std::vector<Frame> fixedRefresh(const std::vector<Frame>& input, uint64_t phaseUs, int displayHz)
{
    const double periodUs = 1000000.0 / displayHz;
    const uint64_t waitUs = uint64_t(std::max(1000 / displayHz, 3) - 3) * 1000;
    std::vector<Frame> out(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        out[i].sourceUs = input[i].sourceUs;
        out[i].decodedUs = input[i].decodedUs;
        out[i].dropped = true;
    }
    // Frames reach the pacing queue in decode order; a late frame holds back
    // its successors.
    std::vector<size_t> queue, history;
    size_t next = 0;
    uint64_t releasedUs = 0;
    const auto arrival = [&](size_t i) { return std::max(input[i].decodedUs, releasedUs); };
    const uint64_t originUs = input.front().decodedUs + phaseUs;
    for (uint64_t tick = 0; next < input.size() || !queue.empty(); ++tick) {
        const uint64_t vsyncUs = originUs + uint64_t(double(tick) * periodUs);
        for (; next < input.size() && arrival(next) <= vsyncUs; ++next) {
            releasedUs = arrival(next);
            if (queue.size() == 3) queue.erase(queue.begin());
            queue.push_back(next);
        }
        const size_t target = std::any_of(history.begin(), history.end(), [](size_t depth) { return depth <= 1; }) ? 3 : 1;
        if (history.size() == size_t(displayHz / 2)) history.erase(history.begin());
        history.push_back(queue.size());
        while (queue.size() > target) queue.erase(queue.begin());
        size_t taken = SIZE_MAX;
        if (!queue.empty()) {
            taken = queue.front();
            queue.erase(queue.begin());
        }
        else if (next < input.size() && arrival(next) <= vsyncUs + waitUs) {
            releasedUs = arrival(next);
            taken = next++;
        }
        if (taken != SIZE_MAX) {
            out[taken].shownUs = originUs + uint64_t(double(tick + 1) * periodUs);
            out[taken].dropped = false;
        }
    }
    return out;
}

// What a viewer can see: shown frames whose presented interval departs from
// the source interval by more than 1 ms, or that follow a skipped frame.
// Sub-millisecond flip-queue steps are neither a repeat nor a drop.
struct Disturbances {
    size_t events = 0, skips = 0;
    // Longest wait for a new frame on screen: one disturbance can hide a freeze.
    uint64_t worstStepUs = 0, worstGapUs = 0, meanLatencyUs = 0;
};

Disturbances disturbances(const std::vector<Frame>& f, size_t begin, size_t end)
{
    constexpr int64_t kVisibleStepUs = 1000;
    Disturbances d;
    size_t previous = SIZE_MAX, shown = 0;
    uint64_t latencyUs = 0;
    for (size_t i = begin; i < std::min(end, f.size()); ++i) {
        if (f[i].dropped) continue;
        latencyUs += latency(f[i]);
        ++shown;
        if (previous != SIZE_MAX) {
            const int64_t source = int64_t(f[i].sourceUs) - int64_t(f[previous].sourceUs);
            const int64_t presented = int64_t(f[i].shownUs) - int64_t(f[previous].shownUs);
            const uint64_t step = uint64_t(presented > source ? presented - source : source - presented);
            const bool skipped = i - previous > 1;
            d.skips += skipped;
            d.events += skipped || step > uint64_t(kVisibleStepUs);
            d.worstStepUs = std::max(d.worstStepUs, step);
            d.worstGapUs = std::max(d.worstGapUs, uint64_t(presented));
        }
        previous = i;
    }
    d.meanLatencyUs = shown ? latencyUs / shown : 0;
    return d;
}

struct LateInput {
    const char* name;
    Input (*make)(size_t frame, Rng& rng);
    // Lateness that persists (a latency step): it stalls any presenter by
    // itself less its buffer, and fixed refresh hides more of it behind its
    // extra latency, so the stall is bounded by the step instead.
    uint64_t sustainedUs = 0;
};

// 5d. At the display ceiling VRR must show no more visible stutter than the
// fixed-refresh pacer on the same input, and less latency. Fixed refresh
// carries about a queued refresh of latency, so it rides out a late frame
// under a refresh without any repeat. Regression: ceiling policy 1 shed the
// phase every late frame forced with a skip, 12 a minute at one 4 ms late
// frame per 5 s where fixed refresh showed none. Its vsync phase is
// arbitrary, so it is scored as the mean over 16 phases.
void versusFixedRefresh(const char* scenario, const std::vector<LateInput>& inputs,
                        bool comparableBudget = true)
{
    // Score the last minute: a cold start's release settles in the first,
    // and display-ceiling bounds what that release may cost.
    constexpr size_t kTotal = 120 * 120, kFrom = 60 * 120;
    constexpr int kPhases = 16;
    for (const auto& input : inputs) for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        if (!comparableBudget && preset.mode != 0) continue;
        const std::string name = label(scenario, preset, smoothing) + "/" + input.name +
            (comparableBudget ? "/common-budget" : "/default-four-frame");
        auto session = vrrProductionSession(preset.mode, 120, 120, smoothing.smooth);
        // Compare latency with at most the fixed pacer's one-frame delivery
        // budget. Smooth's default four-frame budget deliberately trades more
        // latency for fewer disturbances; exercise that separately below.
        if (comparableBudget)
            session.timingOptions.bufferPerMille = std::min(session.timingOptions.bufferPerMille, 1000);
        Stream stream(session);
        Rng rng(211 + uint64_t(preset.mode));
        for (size_t i = 0; i < kTotal; ++i) stream.step(750, input.make(i, rng));
        const auto vrr = disturbances(stream.frames(), kFrom, kTotal);
        double fixedEvents = 0, fixedLatency = 0;
        uint64_t fixedWorstStep = 0, fixedWorstGap = 0;
        for (int phase = 0; phase < kPhases; ++phase) {
            const auto fixed = disturbances(fixedRefresh(stream.frames(), uint64_t(phase) * 8333 / kPhases, 120), kFrom, kTotal);
            fixedEvents += double(fixed.events) / kPhases;
            fixedLatency += double(fixed.meanLatencyUs) / kPhases;
            fixedWorstStep = std::max(fixedWorstStep, fixed.worstStepUs);
            fixedWorstGap = std::max(fixedWorstGap, fixed.worstGapUs);
        }
        report("%-62s vrr events %3zu (skips %3zu, deferred %3zu) latency %5" PRIu64 " | fixed refresh events %5.1f latency %5.0f\n",
            name.c_str(), vrr.events, vrr.skips, stream.deferrals(), vrr.meanLatencyUs, fixedEvents, fixedLatency);
        // Each request that finds no successor holds the frame for the
        // worker's whole wait; asking on every frame compounded those waits.
        check(stream.deferrals() <= kTotal / 120, name, "replacement requests without a ready successor stay rare",
            "%zu requests found no successor in 120 s", stream.deferrals());
        check(double(vrr.events) <= fixedEvents, name, "no more visible stutter than fixed refresh at the display ceiling",
            "%zu visible events (%zu skips) over 60 s; fixed refresh %.1f", vrr.events, vrr.skips, fixedEvents);
        // Discarding every frame of a latency step is one disturbance on the
        // event count; the screen still froze for the whole step.
        check(vrr.worstGapUs <= fixedWorstGap + 8334, name, "no freeze longer than fixed refresh's worst hold plus a refresh",
            "%" PRIu64 " us without a new frame; fixed refresh %" PRIu64 " us", vrr.worstGapUs, fixedWorstGap);
        if (comparableBudget)
            check(double(vrr.meanLatencyUs) < fixedLatency, name, "less latency than fixed refresh at the display ceiling",
                "mean latency %" PRIu64 " us; fixed refresh %.0f us", vrr.meanLatencyUs, fixedLatency);
        else {
            const uint64_t selectedBudgetUs = uint64_t(session.timingOptions.bufferPerMille) * 8333 / 1000;
            check(vrr.meanLatencyUs <= selectedBudgetUs + kBaseLatencyUs + 100 + stream.toleranceUs(),
                name, "default Smooth latency remains within its selected four-frame delivery budget",
                "mean latency %" PRIu64 " us; selected buffer budget %" PRIu64 " us", vrr.meanLatencyUs, selectedBudgetUs);
        }
        if (input.sustainedUs)
            check(vrr.worstStepUs <= input.sustainedUs + 8333, name, "a latency step stalls for at most the step plus a refresh",
                "worst presented step %" PRIu64 " us for a %" PRIu64 " us step; fixed refresh %" PRIu64 " us",
                vrr.worstStepUs, input.sustainedUs, fixedWorstStep);
        else
            check(vrr.worstStepUs <= std::max<uint64_t>(fixedWorstStep, 8334), name,
                "no disturbance larger than fixed refresh's at the display ceiling",
                "worst presented step %" PRIu64 " us; fixed refresh %" PRIu64 " us", vrr.worstStepUs, fixedWorstStep);
    }
}

void testVersusFixedRefreshLateFrames()
{
    versusFixedRefresh("versus-fixed-refresh", {
        // One late frame every 5 s: under half, near and beyond a refresh.
        {"late-4ms", [](size_t i, Rng& rng) { return Input{rng.below(400) + (i % 600 == 300 ? 4000 : 0)}; }},
        {"late-8ms", [](size_t i, Rng& rng) { return Input{rng.below(400) + (i % 600 == 300 ? 8000 : 0)}; }},
        {"late-10ms", [](size_t i, Rng& rng) { return Input{rng.below(400) + (i % 600 == 300 ? 10000 : 0)}; }},
        // 2% of frames 2 ms late: above every profile's allowance.
        {"frequent-2ms", [](size_t i, Rng& rng) { return Input{rng.below(400) + (i % 60 == 30 ? 2000 : 0)}; }},
    });
}

void testVersusFixedRefreshStalls()
{
    const std::vector<LateInput> inputs = {
        // A 25 ms network stall every 10 s delivers three frames together.
        {"stall-25ms", [](size_t i, Rng& rng) {
            const size_t k = i % 1200;
            return Input{rng.below(400) + (k == 600 ? 25000 : k == 601 ? 16700 : k == 602 ? 8300 : 0)};
        }},
        // Every frame 20 ms late for 2 s every 20 s: a route change, not a
        // burst. Successors are as late, so replacing them sheds nothing.
        {"latency-step-20ms", [](size_t i, Rng& rng) {
            const auto inStep = [](size_t frame) { const size_t k = frame % 2400; return k >= 1200 && k < 1440; };
            Input input{rng.below(400) + (inStep(i) ? 20000 : 0)};
            input.successorLatenessUs = inStep(i + 1) ? 20000 : 0;
            return input;
        }, 20000},
        // The same step opening with a hiccup: its first frame arrives with
        // the second, so the first replacement finds a ready successor and
        // every later successor is as late. Shedding must stop there.
        {"bunched-latency-step-20ms", [](size_t i, Rng& rng) {
            const auto lateness = [](size_t frame) -> uint64_t {
                const size_t k = frame % 2400;
                return k == 1200 ? 28300 : k > 1200 && k < 1440 ? 20000 : 0;
            };
            Input input{rng.below(400) + lateness(i)};
            input.successorLatenessUs = lateness(i + 1);
            return input;
        }, 28300},
    };
    versusFixedRefresh("versus-fixed-refresh-stall", inputs);
    versusFixedRefresh("versus-fixed-refresh-stall", inputs, false);
}

// The live policy on a recorded arrival timeline, through the same worker
// model as Stream. A frame that needed no native decode wait was ready at
// decoder output: the observed completion also holds the live session's own
// queue residence. Source times follow the RTP clock, aligned to the first
// receive, so VRR and fixed refresh share one latency reference.
struct CaptureRun {
    std::vector<Frame> frames;
    size_t replacements = 0, deferrals = 0;
};

// ceilingPolicy 0 runs the live policy; otherwise it selects a ceiling policy.
template<size_t N>
CaptureRun runCapture(const CaptureArrival (&span)[N], uint64_t ceilingPolicy = 0)
{
    const auto session = vrrProductionSession(1, 120, 120, true);
    auto parameters = vrrTimingParametersForSession(session);
    if (ceilingPolicy) parameters.playoutCeilingPhaseReplacement = ceilingPolicy;
    VrrTimingController controller(session, t_CanLatch, parameters);
    const auto ready = [&](size_t i) {
        return kOriginUs + (span[i].syncWait == 0 ? span[i].output : std::max(span[i].output, span[i].complete));
    };
    CaptureRun run;
    uint64_t lastSubmittedUs = 0, lastShownUs = 0, rtpTicks = 0;
    for (size_t i = 0; i < N; ++i) {
        const auto& a = span[i];
        if (i) rtpTicks += uint32_t(a.rtp - span[i - 1].rtp);
        Frame f;
        f.sourceUs = kOriginUs + span[0].receive + rtpTicks * 1000000 / 90000;
        f.decodedUs = ready(i);
        PacedFrame frame(nullptr, int(a.frame), a.rtp, a.valid != 0, kOriginUs + a.output);
        frame.noteGpuReadyUs(f.decodedUs);
        frame.setDeliveryTimeline(kOriginUs + a.receive, kOriginUs + a.reassembled, kOriginUs + a.submit);
        frame.setLostPackets(a.lost);
        const auto dequeuedUs = std::max(kOriginUs + a.output, lastSubmittedUs);
        const auto now = std::max(f.decodedUs, dequeuedUs);
        frame.noteDecodeSyncWaitUs(now - dequeuedUs);
        const auto d = controller.schedule(frame, now);
        f.originalUs = d.originalTargetUs;
        uint64_t startUs = now;
        if (d.trajectory.resyncRecommended) {
            const uint64_t waitUs = (((d.replacementWaitUs ? d.replacementWaitUs :
                std::min<uint64_t>(20000, 2 * d.sourcePeriodUs)) + 999) / 1000) * 1000;
            if (i + 1 < N && ready(i + 1) <= now + waitUs) {
                controller.noteTrajectoryResyncDrop();
                controller.noteSubmission(false, false, 0);
                f.dropped = true;
                ++run.replacements;
                run.frames.push_back(f);
                continue;
            }
            startUs = now + waitUs;
            ++run.deferrals;
            controller.noteTrajectoryDeferred();
        }
        const uint64_t readyUs = std::max(startUs, d.renderStartUs) + 500;
        f.submittedUs = std::max(readyUs, d.targetUs);
        f.shownUs = d.latchedPresentation && lastShownUs ?
            std::max(f.submittedUs, lastShownUs + controller.displayPeriodUs()) : f.submittedUs;
        controller.notePreparationDuration(500, 0, readyUs);
        controller.noteSchedulerDelays(0, 0, true);
        controller.noteSubmission(true, false, f.submittedUs, f.submittedUs);
        lastSubmittedUs = f.submittedUs;
        lastShownUs = f.shownUs;
        run.frames.push_back(f);
    }
    return run;
}

// A real game does not hold 120 FPS, and fixed refresh judders at every lower
// rate by construction; score visible events where the source ran at the
// ceiling for the interval (7.9-8.8 ms), and skips anywhere.
Disturbances ceilingDisturbances(const std::vector<Frame>& f)
{
    Disturbances d;
    size_t previous = SIZE_MAX, shown = 0;
    uint64_t latencyUs = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i].dropped) continue;
        latencyUs += latency(f[i]);
        ++shown;
        if (previous != SIZE_MAX) {
            const bool skipped = i - previous > 1;
            const int64_t source = int64_t(f[i].sourceUs) - int64_t(f[previous].sourceUs);
            const int64_t presented = int64_t(f[i].shownUs) - int64_t(f[previous].shownUs);
            const bool ceiling = !skipped && source >= 7900 && source <= 8800;
            const uint64_t step = uint64_t(presented > source ? presented - source : source - presented);
            d.skips += skipped;
            d.events += skipped || (ceiling && step > 1000);
        }
        previous = i;
    }
    d.meanLatencyUs = shown ? latencyUs / shown : 0;
    return d;
}

// 5e. The fixed-refresh comparison on recorded arrival timelines from the
// 2026-10-20 captures (capturespans.h), and against ceiling policy 1, the policy
// this one replaced. Regressions: policy 1 replaced 10 frames in the 03:58 span,
// 31 visible events where fixed refresh shows 6; without the decode guard the
// span showed 14; shedding every step past a refresh, without the 32-frame rate
// limit, replaced 49 frames in the lossy 04:20 span (93 events where policy 1
// shows 56).
void testVersusFixedRefreshCaptures()
{
    const auto compare = [](const char* spanName, const CaptureRun& run, const CaptureRun& policy1) {
        const std::string name = scenarioName("versus-fixed-refresh-captures") + "/" + spanName;
        const auto vrr = ceilingDisturbances(run.frames);
        double fixedEvents = 0, fixedLatency = 0;
        constexpr int kPhases = 16;
        for (int phase = 0; phase < kPhases; ++phase) {
            const auto fixed = ceilingDisturbances(fixedRefresh(run.frames, uint64_t(phase) * 8333 / kPhases, 120));
            fixedEvents += double(fixed.events) / kPhases;
            fixedLatency += double(fixed.meanLatencyUs) / kPhases;
        }
        const auto previous = ceilingDisturbances(policy1.frames);
        report("%-62s vrr events %3zu (skips %3zu, replaced %3zu, deferred %3zu) latency %5" PRIu64
            " | policy 1 events %3zu (replaced %3zu) | fixed refresh events %5.1f latency %5.0f\n", name.c_str(),
            vrr.events, vrr.skips, run.replacements, run.deferrals, vrr.meanLatencyUs, previous.events,
            policy1.replacements, fixedEvents, fixedLatency);
        check(vrr.events <= previous.events && run.replacements <= policy1.replacements, name,
            "no more stutter or replacements than ceiling policy 1 on a recorded capture span",
            "%zu visible events and %zu replacements; policy 1 %zu and %zu", vrr.events, run.replacements,
            previous.events, policy1.replacements);
        check(double(vrr.events) <= fixedEvents, name, "no more visible stutter than fixed refresh on a recorded capture span",
            "%zu visible events (%zu replacements); fixed refresh %.1f", vrr.events, run.replacements, fixedEvents);
        check(double(vrr.meanLatencyUs) < fixedLatency, name, "less latency than fixed refresh on a recorded capture span",
            "mean latency %" PRIu64 " us; fixed refresh %.0f us", vrr.meanLatencyUs, fixedLatency);
    };
    compare("0358-sustained-120", runCapture(kCapture0358), runCapture(kCapture0358, 1));
    compare("0420-lossy", runCapture(kCapture0420), runCapture(kCapture0420, 1));
}

// 6. An abrupt rate drop that moves the source mapping later must not leave
// the old protection standing on top of it. 116 FPS with one frame in ten
// arriving E early holds the mapping minimum on the early frames, so the
// buffer covers E. At 30 FPS the early frames stop: the mapping slews E later
// and the evidence needs E less buffer. Regression: f96b0524 (about 6 ms of
// excess latency for 25 s after a 105 -> 30 FPS drop).
void testRateDropReleasesMappingExcess()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("rate-drop", preset, smoothing);
        const auto session = vrrProductionSession(preset.mode, 116, 120, smoothing.smooth);
        // The largest early offset each profile's buffer ceiling can protect.
        const uint64_t early = preset.mode == 2 ? 3000 : preset.mode == 1 ? 6000 : 8000;
        Stream s(session, 1000);
        Rng rng(61 + uint64_t(preset.mode));
        for (size_t i = 0; i < 3000; ++i) s.step(k116Ticks, {i % 10 == 0 ? 0 : early + rng.below(600)});
        const size_t drop = s.size();
        const uint64_t protection = s.frames().back().appliedUs;
        const size_t recoveryFrames = 30 * (15 + int(protection / s.fallUsPerSecond()) +
            int(4 * s.releaseTauUs() / 1000000));
        for (size_t i = 0; i < recoveryFrames; ++i) s.step(k30Ticks, {early + rng.below(600)});
        // The same 30 FPS input from a session that never ran faster.
        Stream reference(session, 1000);
        Rng referenceRng(67 + uint64_t(preset.mode));
        for (size_t i = 0; i < 1050; ++i) reference.step(k30Ticks, {early + referenceRng.below(600)});
        const auto age = [](const Frame& f) { return f.shownUs - f.decodedUs; };
        const auto& f = s.frames();
        const uint64_t settledAge = mean(reference.frames(), 600, 1050, age);
        const uint64_t firstAge = mean(f, drop, drop + 450, age);
        const uint64_t lateAge = mean(f, f.size() - 150, f.size(), age);
        const uint64_t firstExcess = firstAge > settledAge ? firstAge - settledAge : 0;
        const uint64_t lateExcess = lateAge > settledAge ? lateAge - settledAge : 0;
        report("%-52s protection %5" PRIu64 " decode-to-present after drop %5" PRIu64 " (0-15 s) %5" PRIu64 " (settled) reference %5" PRIu64 "\n",
            name.c_str(), protection, firstAge, lateAge, settledAge);
        check(firstExcess <= protection + s.toleranceUs(), name,
            "rate change does not add another standing reserve on top of the earlier buffer",
            "first 15 s excess %" PRIu64 " us with prior protection %" PRIu64 " us", firstExcess, protection);
        check(lateExcess <= 500, name, "no excess remains after the release period at the lower rate",
            "%" PRIu64 " us excess after the release period", lateExcess);
    }
}

// 7. Host/client clock drift must neither ratchet retained protection up nor
// erode it. Regression: 409a5661 (drift x age moved every retained
// requirement; a slow client clock ratcheted the buffer, a fast one eroded it).
void testClockDriftNeitherRatchetsNorErodes()
{
    for (const auto& smoothing : kSmoothing) {
        const Preset& preset = kPresets[1];
        struct Run { uint64_t meanUs, trendUs; size_t hitches; };
        const auto run = [&](int64_t ppm) {
            Stream s(vrrProductionSession(preset.mode, 90, 120, smoothing.smooth), 1000, ppm);
            Rng rng(71);
            const size_t total = frames90(240);
            // Alternating 3 ms excursions establish interval pressure, so real
            // protection is retained while the clocks drift 12 ms apart.
            for (size_t i = 0; i < total; ++i)
                s.step(1000, {rng.below(600) + (i % 2 ? 3000 : 0)});
            const auto& f = s.frames();
            const uint64_t early = mean(f, frames90(120), frames90(160), applied);
            const uint64_t late = mean(f, frames90(200), total, applied);
            return Run{mean(f, frames90(120), total, applied), late > early ? late - early : early - late,
                       hitches(f, frames90(60), total, s.toleranceUs())};
        };
        const Run control = run(0);
        check(control.meanUs >= 2000, label("clock-drift", preset, smoothing),
            "the drift fixture retains real protection", "mean buffer %" PRIu64 " us",
            control.meanUs);
        for (int64_t ppm : {int64_t(50), int64_t(-50)}) {
            const std::string name = label("clock-drift", preset, smoothing) + (ppm > 0 ? "/+50ppm" : "/-50ppm");
            const Run drifted = run(ppm);
            const uint64_t difference = drifted.meanUs > control.meanUs ? drifted.meanUs - control.meanUs :
                control.meanUs - drifted.meanUs;
            report("%-52s mean %5" PRIu64 " (no drift %5" PRIu64 ") trend %4" PRIu64 " hitches %3zu (no drift %3zu)\n",
                name.c_str(), drifted.meanUs, control.meanUs, drifted.trendUs, drifted.hitches, control.hitches);
            // The original defect moved retained requirements by drift x age:
            // about 3 ms here. Ordinary mapping catch-up is a small fraction.
            check(difference <= std::max<uint64_t>(300, control.meanUs / 8), name,
                "clock drift neither ratchets nor erodes retained protection",
                "mean buffer %" PRIu64 " us with drift, %" PRIu64 " us without", drifted.meanUs, control.meanUs);
            check(drifted.trendUs <= 300, name, "retained protection does not trend with drift",
                "mean buffer moved %" PRIu64 " us between minutes 2-3 and 3.3-4", drifted.trendUs);
            check(drifted.hitches <= control.hitches * 2 + 10, name, "clock drift does not erode coverage",
                "%zu presented hitches with drift, %zu without", drifted.hitches, control.hitches);
        }
    }
}

// 8. Late frames that lost packets, slow preparation after input, and time
// spent in the client's own decoder queue must not buy input buffer.
// Regression: 39d10abf (loss must cost detail, not timing), post-input
// render/scheduler work relabeled as input misses, and the buffer growing
// from its own backlog.
void testLossAndDownstreamWorkBuyNoInputDelay()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const auto session = vrrProductionSession(preset.mode, 116, 120, smoothing.smooth);
        const size_t total = frames116(60);
        {
            const std::string name = label("lossy-late-frames", preset, smoothing);
            Stream s(session, 1000);
            Rng rng(83 + uint64_t(preset.mode));
            for (size_t i = 0; i < total; ++i) {
                const bool lossy = i % 20 == 10;
                s.step(k116Ticks, {rng.below(400) + (lossy ? 3000 : 0), lossy ? 4u : 0u});
            }
            const auto in = s.interval();
            const uint64_t peak = maximum(s.frames(), frames116(1), total, applied);
            report("%-52s peak %5" PRIu64 " floor %5" PRIu64 " missed %" PRIu64 "\n", name.c_str(), peak, in.update.minimumUs, in.failedUs);
            check(peak <= in.update.minimumUs + 250 && in.evaluatedUs > 0, name,
                "late frames with lost packets never buy input buffer",
                "buffer peaked at %" PRIu64 " us (floor %" PRIu64 "), %" PRIu64 " failed interval microseconds", peak, in.update.minimumUs, in.failedUs);
        }
        {
            // Intermittent service that fits the cadence can be protected. A
            // persistently overloaded decoder or renderer cannot catch up by
            // delaying future frames. Exercise both sides of that boundary.
            for (bool decoder : {false, true}) for (bool overloaded : {false, true}) {
                const auto name = label(decoder ? "decoder-backlog" : "slow-preparation", preset, smoothing)
                    + (overloaded ? "/overloaded" : "/intermittent");
                Stream s(session, 1000);
                Rng rng(79 + uint64_t(preset.mode));
                for (size_t i = 0; i < frames116(10); ++i) s.step(k116Ticks);
                const size_t start = s.size();
                const size_t length = frames116(overloaded ? 4 : 50);
                size_t overloadedSamples = 0, overloadedAttacks = 0;
                for (size_t i = 0; i < length; ++i) {
                    Input input{rng.below(400)};
                    const uint64_t extra = overloaded ? 12000 : (i % 20 == 10 ? 3000 : 0);
                    if (decoder) input.decoderQueueUs += extra;
                    else input.prepareUs += extra;
                    s.step(k116Ticks, input);
                    const auto sample = s.interval();
                    if (sample.serviceOverloaded) {
                        ++overloadedSamples;
                        if (sample.update.attemptedIncreaseUs) ++overloadedAttacks;
                    }
                }
                const auto stats = s.interval();
                const auto peak = maximum(s.frames(), start, s.size(), applied);
                const auto floor = stats.update.minimumUs;
                if (overloaded) {
                    check(overloadedSamples > 0 && overloadedAttacks == 0, name,
                          "qualified downstream overload cannot authorize a buffer increase",
                          "%zu attacks during %zu overloaded intervals", overloadedAttacks, overloadedSamples);
                }
                else {
                    check(stats.evaluatedUs > 0 && peak <= floor + 3000, name,
                          "intermittent downstream work never buys more than its recoverable duration",
                          "peak %" PRIu64 " us, floor %" PRIu64 " us for 3000 us excursions", peak, floor);
                }
            }
        }
    }
}

// 9. Between late frames the schedule must return to its plan: catch-up
// carry must drain despite a normal wake-up overshoot on every frame.
// Regression: 3c32553e (the VRR17 floor re-armed under bounded catch-up and
// carried 4-8 ms on top of the buffer; 81% of frames were pushed past plan).
void testCatchUpCarryDrainsBetweenLateFrames()
{
    // Low Latency cannot protect a 6 ms spike (ceiling half a frame), and
    // one spike in 400 is inside Balanced's allowance: neither grows the
    // buffer, so recovery alone must absorb every spike.
    for (const auto& smoothing : kSmoothing) for (int index : {0, 1}) {
        const Preset& preset = kPresets[index];
        const std::string name = label("catch-up-carry", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth), 1000);
        const size_t spacing = index == 0 ? 200 : 400;
        Rng rng(97 + uint64_t(index));
        const size_t total = frames116(60);
        for (size_t i = 0; i < total; ++i) {
            Input input{rng.below(300) + (i % spacing == spacing / 2 ? 6000 : 0)};
            // Latching presenters: the constant 0.28 ms wake-up overshoot that
            // exposed 3c32553e. A presenter that cannot latch has only 188 us
            // of slack at 116/120 FPS, so it gets the residual measured after
            // the worker's wake compensation instead (2026-10-19 capture:
            // p10 12, p50 62, p90 86, p99 185 us after the target).
            input.presentLatencyUs = t_CanLatch ? 280 : 12 + rng.below(100) + (rng.below(100) == 0 ? 120 : 0);
            s.step(k116Ticks, input);
        }
        const auto& f = s.frames();
        std::vector<uint64_t> carry, recovery;
        size_t tail = 0, tailPushed = 0;
        for (size_t i = frames116(10); i < total; ++i) {
            carry.push_back(f[i].carryUs);
            recovery.push_back(f[i].recoveryUs);
            if ((i + spacing / 2) % spacing >= spacing * 9 / 10) {
                // A latching presenter returns exactly to plan. Without latch
                // the residual's own tail can leave a sub-tolerance carry; the
                // 3c32553e defect left 4 ms on every frame.
                const uint64_t allowed = t_CanLatch ? 0 : 250;
                ++tail;
                tailPushed += f[i].carryUs > allowed || f[i].recoveryUs > allowed;
            }
        }
        const uint64_t carryLimit = s.controller().catchupCarryLimitUs();
        const uint64_t peakApplied = maximum(f, frames116(10), total, applied);
        report("%-52s carry p50 %4" PRIu64 " p90 %4" PRIu64 " (limit %4" PRIu64 ") recovery p50 %4" PRIu64 " tail pushed %zu/%zu buffer %5" PRIu64 "\n",
            name.c_str(), percentile(carry, 500), percentile(carry, 900), carryLimit, percentile(recovery, 500),
            tailPushed, tail, peakApplied);
        check(percentile(carry, 500) == 0 && percentile(recovery, 500) == 0, name,
            "between late frames the schedule returns to its plan",
            "carry p50 %" PRIu64 " us, recovery p50 %" PRIu64 " us", percentile(carry, 500), percentile(recovery, 500));
        check(tail > 0 && tailPushed == 0, name, "all carry drains before the next late frame",
            "%zu of %zu frames just before a spike still carried delay", tailPushed, tail);
        check(percentile(carry, 900) <= carryLimit, name, "carry stays within the catch-up ceiling",
            "carry p90 %" PRIu64 " us, limit %" PRIu64 " us", percentile(carry, 900), carryLimit);
    }
}

// Invalid samples must not authorize growth. Resumed clean submissions release
// the reserve even after loss and repeated changes of source epoch.
void testExpiryThroughInvalidEvidence()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("invalid-expiry", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 90, 120, smoothing.smooth), 1000);
        Rng rng(101 + uint64_t(preset.mode));
        const size_t earned = frames90(30);
        for (size_t i = 0; i < earned; ++i) s.step(1000, {rng.below(400) + (i % 2 ? 3000 : 0)});
        const uint64_t learned = s.frames().back().appliedUs;
        const size_t total = earned + frames90(double(s.windowUs()) / 1e6 + 60);
        for (size_t i = earned; i < total; ++i) {
            if (i % 60 == 0) s.rebase();
            s.step(1000, {rng.below(400), 1});
        }
        const auto in = s.interval();
        const uint64_t afterInvalid = s.frames().back().appliedUs;
        check(afterInvalid <= learned + 250, name, "invalid observations cannot buy protection",
              "buffer grew from %" PRIu64 " to %" PRIu64 " us", learned, afterInvalid);
        // Invalid input can hold an interval buffer. Only fresh, clean intervals
        // authorize release, even when the old score has expired.
        for (size_t i = 0; i < frames90(120); ++i) s.step(1000);
        const uint64_t last = s.frames().back().appliedUs;
        report("%-52s learned %5" PRIu64 " after history %5" PRIu64 " floor %5" PRIu64 "\n",
            name.c_str(), learned, last, in.update.minimumUs);
        check(learned >= 2000, name, "the disturbance establishes protection",
            "buffer %" PRIu64 " us", learned);
        check(last <= in.update.minimumUs + 500, name, "fresh clean intervals release protection after invalid observations",
            "still %" PRIu64 " us (floor %" PRIu64 ") a full history after the last valid input", last, in.update.minimumUs);
    }
}

// Reduce Judder may retime frames within tolerance but must not buy an input buffer or a standing delay
// beyond that budget, and Reduce judder must actually even out short/long
// source pairs. Guards the current revisions 8/9 (immediate fixed-reserve
// smoothing) and their predecessors' T-per-second actuator, which passed
// short/long pairs through unchanged.
void testSmoothingStaysInsideItsBudget()
{
    for (int rate : {90, 116}) {
        const uint32_t ticks = uint32_t(90000 / rate);
        struct Run { uint64_t peakApplied, floor, extraP50, latency, jerkP95; };
        Run runs[std::size(kSmoothing)] = {};
        for (size_t mode = 0; mode < std::size(kSmoothing); ++mode) {
            const auto& smoothing = kSmoothing[mode];
            Stream s(vrrProductionSession(1, rate, 120, smoothing.smooth), 1000);
            Rng sourceRng(103), inputRng(107);
            const size_t total = size_t(40 * rate);
            for (size_t i = 0; i < total; ++i) {
                // 0.5-1 ms of host jitter on every interval, alternating short/long.
                const uint32_t jitter = uint32_t(45 + sourceRng.below(46));
                s.step(i % 2 ? ticks + jitter : ticks - jitter, {inputRng.below(300)});
            }
            const auto& f = s.frames();
            const size_t from = size_t(10 * rate);
            std::vector<uint64_t> extra;
            for (size_t i = from; i < total; ++i) extra.push_back(f[i].extraUs);
            const auto interval = s.controller().intervalStats();
            const uint64_t floorUs = interval.inputPercentileEnabled ?
                interval.inputPercentile.minimumUs : interval.update.minimumUs;
            runs[mode] = {maximum(f, from, total, applied), floorUs, percentile(extra, 500),
                          mean(f, from, total, latency), percentile(presentedJerk(f, from, total), 950)};
        }
        const uint64_t tolerance = uint64_t(vrrProductionSession(1, rate, 120).timingOptions.toleranceUs);
        for (size_t mode = 0; mode < std::size(kSmoothing); ++mode) {
            const auto& smoothing = kSmoothing[mode];
            const std::string name = label("smoothing-budget", kPresets[1], smoothing) + "/" + std::to_string(rate) + "fps";
            const Run& r = runs[mode];
            const uint64_t budget = !smoothing.smooth ? 0 : tolerance;
            report("%-52s buffer %5" PRIu64 " extra p50 %4" PRIu64 " (budget %4" PRIu64 ") latency %5" PRIu64 " (off %5" PRIu64 ") jerk p95 %4" PRIu64 " (off %4" PRIu64 ")\n",
                name.c_str(), r.peakApplied, r.extraP50, budget, r.latency, runs[0].latency, r.jerkP95, runs[0].jerkP95);
            check(r.peakApplied <= r.floor + 250, name, "smoothing never buys input buffer",
                "input buffer peaked at %" PRIu64 " us (floor %" PRIu64 ")", r.peakApplied, r.floor);
            // Mandatory display-floor phase (a presenter that cannot latch at
            // 116/120 FPS) is not smoothing's to spend; charge only what it adds.
            check(r.extraP50 <= runs[0].extraP50 + budget + 50, name, "typical retiming stays inside the smoothing budget",
                "planned extra delay p50 %" PRIu64 " us vs %" PRIu64 " us with smoothing off, budget %" PRIu64 " us",
                r.extraP50, runs[0].extraP50, budget);
            check(r.latency <= runs[0].latency + budget + 250, name, "smoothing adds no standing delay beyond its budget",
                "mean latency %" PRIu64 " us vs %" PRIu64 " us with smoothing off", r.latency, runs[0].latency);
            // Reduce Judder must improve output cadence on imperfect input.
            if (smoothing.smooth)
                check(r.jerkP95 * 5 <= runs[0].jerkP95 * 4, name, "smoothing actually evens out short/long source pairs",
                    "presented jerk p95 %" PRIu64 " us vs %" PRIu64 " us with smoothing off", r.jerkP95, runs[0].jerkP95);
        }
    }
}

// 13. A client whose GPU decode runs close to the source interval must hold
// a steady buffer and a steady source clock once it has settled, including
// above one source interval of buffer. Regression: found 2026-11-13 on a
// recorded 4K 138 FPS HEVC stream on a Radeon 780M (slowdecodespan.h). Above
// 7.25 ms of buffer the worker reached each fast frame after its decode had
// finished and could report only its own arrival time, so the source mapping
// rose 2 ms while the solved buffer fell 2 ms and both swung back every 10 to
// 15 s; and Smooth's aim inside its allowance stepped the buffer between 8.25
// and 9.0 ms as single 15 ms frames entered and left the window, although
// 8.25 ms met the goal. vrr18 held 6.4 to 6.65 ms on the same input. The
// decoder is one serial queue and the worker one serial thread, as live; the
// D3D11 presenter now timestamps each decode completion itself.
void testSlowGpuDecodeHoldsASteadyBuffer()
{
    struct Run { uint64_t bufferLow, bufferHigh; int64_t mappingLow, mappingHigh; size_t late, frames; uint64_t repeatedSpanDriftUs; };
    const auto run = [&](const VrrSessionConfig& session, bool timestamped) {
        VrrTimingController controller(session, t_CanLatch, vrrTimingParametersForSession(session));
        controller.seedPlayoutDelayStart(1000);
        constexpr size_t kLoops = 14; // 8.75 minutes
        const size_t span = sizeof(kSlowDecodeSpan) / sizeof(kSlowDecodeSpan[0]), total = span * kLoops;
        const uint64_t tolerance = uint64_t(session.timingOptions.toleranceUs);
        uint64_t ticks = 0, lastSubmittedUs = 0, decoderFreeUs = 0;
        Run result{UINT64_MAX, 0, INT64_MAX, INT64_MIN, 0, 0, 0};
        std::vector<uint64_t> previousSpan(span);
        int64_t secondSum = 0;
        size_t secondCount = 0;
        for (size_t i = 0; i < total; ++i) {
            const auto& in = kSlowDecodeSpan[i % span];
            ticks += in.ticks;
            const uint64_t sourceUs = kOriginUs + ticks * 1000000 / 90000;
            const uint64_t outputUs = sourceUs + kBaseLatencyUs + in.lateUs + 100;
            PacedFrame frame(nullptr, int(i), uint32_t(ticks + 0xFFF00000u), true, outputUs);
            const uint64_t submitUs = outputUs - 600;
            frame.setDeliveryTimeline(submitUs - 300, submitUs - 100, submitUs);
            frame.setDecoderOutputComplete(false);
            const uint64_t completedUs = std::max(outputUs, decoderFreeUs) + in.decodeUs;
            decoderFreeUs = completedUs;
            // The worker collects a frame once the previous present returned,
            // and then waits for its decode if that is still running.
            const uint64_t dequeueUs = std::max(outputUs, lastSubmittedUs);
            const uint64_t now = std::max(dequeueUs, completedUs);
            frame.noteDecodeSyncWaitUs(now - dequeueUs);
            if (timestamped) frame.noteDecodeCompletedUs(completedUs);
            frame.noteGpuReadyUs(now);
            const auto d = controller.schedule(frame, now);
            if (d.trajectory.resyncRecommended) controller.noteTrajectoryDeferred();
            const uint64_t prepareStartUs = std::max(now, d.renderStartUs);
            const uint64_t preparedUs = prepareStartUs + in.prepareUs;
            controller.notePreparationDuration(in.prepareUs, 0, preparedUs);
            const uint64_t targetReachedUs = std::max(preparedUs, d.targetUs);
            const uint64_t renderedUs = std::max(preparedUs + in.renderUs, targetReachedUs);
            controller.noteDeferredGpuReady(renderedUs - targetReachedUs, true, renderedUs,
                renderedUs - prepareStartUs, renderedUs != targetReachedUs);
            controller.noteSchedulerDelays(0, 0, true);
            controller.noteSubmission(true, false, renderedUs, renderedUs);
            lastSubmittedUs = renderedUs;
            const uint64_t previous = previousSpan[i % span];
            previousSpan[i % span] = d.playoutDelayUs;
            if (i < total / 2) continue;
            const auto drift = d.playoutDelayUs > previous ? d.playoutDelayUs - previous : previous - d.playoutDelayUs;
            result.repeatedSpanDriftUs = std::max(result.repeatedSpanDriftUs, drift);
            ++result.frames;
            result.bufferLow = std::min(result.bufferLow, d.playoutDelayUs);
            result.bufferHigh = std::max(result.bufferHigh, d.playoutDelayUs);
            result.late += renderedUs > d.targetUs + tolerance;
            // The mapping as one-second means: it is a slewed clock offset.
            secondSum += int64_t(d.sourceTimeUs) - int64_t(sourceUs);
            if (++secondCount == 138) {
                result.mappingLow = std::min(result.mappingLow, secondSum / 138);
                result.mappingHigh = std::max(result.mappingHigh, secondSum / 138);
                secondSum = 0; secondCount = 0;
            }
        }
        return result;
    };
    for (const auto& preset : kPresets) {
        const auto& smoothing = kSmoothing[1];
        const std::string name = label("slow-gpu-decode", preset, smoothing);
        const auto session = vrrProductionSession(preset.mode, 144, 144, smoothing.smooth);
        const Run timestamped = run(session, true), collected = run(session, false);
        report("%-52s buffer %5" PRIu64 "-%5" PRIu64 " mapping moved %4" PRId64 " late %4zu of %zu | collection time only: buffer %5" PRIu64 "-%5" PRIu64 " mapping moved %4" PRId64 " late %4zu\n",
            name.c_str(), timestamped.bufferLow, timestamped.bufferHigh, timestamped.mappingHigh - timestamped.mappingLow,
            timestamped.late, timestamped.frames, collected.bufferLow, collected.bufferHigh,
            collected.mappingHigh - collected.mappingLow, collected.late);
        check(timestamped.repeatedSpanDriftUs <= uint64_t(session.timingOptions.toleranceUs), name,
            "the same point in repeating GPU workloads retains its buffer within tolerance",
            "largest same-frame change between consecutive settled spans %" PRIu64 " us",
            timestamped.repeatedSpanDriftUs);
        // The span's own fastest decode moves 0.7 ms between its light and
        // heavy passages, and the mapping follows it (vrr18: 0.6 ms).
        check(timestamped.mappingHigh - timestamped.mappingLow <= 1000, name,
            "the source clock does not move with the buffer on a steady slow-decode workload",
            "one-second mean mapping moved %" PRId64 " us over the last half",
            timestamped.mappingHigh - timestamped.mappingLow);
    }
}

} // namespace

int main(int argc, char** argv)
{
    struct Scenario { const char* name; void (*run)(); };
    const Scenario scenarios[] = {
        {"start-release", testStartingDelayReleasesOnCleanInput},
        {"within-allowance", testMissesWithinAllowanceBuyNoDelay},
        {"unabsorbable-loss", testUnabsorbableLossBuysNoDelay},
        {"interval-pressure", testIntervalPressureAndRelease},
        {"sparse-delivery", testSparseDeliveryPressure},
        {"recurring-hitch-forecast", testRecurringHitchForecast},
        {"forecast-goal-recovery", testForecastGoalRecovery},
        {"forecast-evidence", testForecastRequiresRecurringDelivery},
        {"forecast-mixed-wakes", testForecastChargesInducedCompression},
        {"forecast-erratic-source", testForecastRequiresBoundedSourceWander},
        {"release-with-spacing-floor", testReleaseBehindSpacingFloor},
        {"source-cadence-buffer", testSourceCadenceBufferGrowth},
        {"client-timing-attribution", testClientTimingAttribution},
        {"rate-round-trip", testRateRoundTrip},
        {"ordinary-variation", testOrdinaryVariation},
        {"display-ceiling", testDisplayCeilingShedsStandingPhase},
        {"versus-fixed-refresh", testVersusFixedRefreshLateFrames},
        {"versus-fixed-refresh-stall", testVersusFixedRefreshStalls},
        {"versus-fixed-refresh-captures", testVersusFixedRefreshCaptures},
        {"rate-drop", testRateDropReleasesMappingExcess},
        {"clock-drift", testClockDriftNeitherRatchetsNorErodes},
        {"loss-and-downstream", testLossAndDownstreamWorkBuyNoInputDelay},
        {"catch-up-carry", testCatchUpCarryDrainsBetweenLateFrames},
        {"invalid-expiry", testExpiryThroughInvalidEvidence},
        {"smoothing-budget", testSmoothingStaysInsideItsBudget},
        {"slow-gpu-decode", testSlowGpuDecodeHoldsASteadyBuffer},
    };
    std::vector<const char*> selection;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--strict") == 0) g_Strict = true;
        else selection.push_back(argv[i]);
    }
    const auto begin = std::chrono::steady_clock::now();
    // Every scenario owns its controllers, so they run concurrently. Output
    // stays in scenario order.
    std::vector<std::pair<const char*, std::future<Log>>> running;
    for (bool canLatch : {true, false}) for (const auto& scenario : scenarios) {
        bool selected = selection.empty();
        for (const char* name : selection) selected |= std::strcmp(name, scenario.name) == 0;
        if (!selected) continue;
        running.emplace_back(scenario.name, std::async(std::launch::async, [run = scenario.run, name = scenario.name, canLatch] {
            Log log;
            t_Log = &log;
            t_CanLatch = canLatch;
            const auto started = std::chrono::steady_clock::now();
            run();
            report("-- %s: %.2f s\n", scenarioName(name).c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            t_Log = nullptr;
            return log;
        }));
    }
    for (auto& [name, result] : running) {
        for (const auto& [failure, line] : result.get()) {
            std::fflush(failure ? stdout : stderr);
            std::fputs(line.c_str(), failure ? stderr : stdout);
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    for (auto& known : g_Known) {
        if (known.failed) {
            std::printf("OPEN KNOWN REGRESSION (%zu failing checks under %s): %s\n",
                known.failed, known.scenarioPrefix, known.evidence);
        }
        else if (known.passed) {
            ++g_Failures;
            std::fprintf(stderr, "FIXED KNOWN REGRESSION under %s: every check passes now; delete its g_Known entry "
                "so the contract fails hard from here on.\n", known.scenarioPrefix);
        }
    }
    std::fflush(stdout);
    if (g_Failures) {
        std::fprintf(stderr, "VRR buffer regressions: %d of %d contracts FAILED (%.1f s)\n",
            g_Failures.load(), g_Checks.load(), elapsed);
        return 1;
    }
    std::printf("VRR buffer regressions: all %d contracts passed (%.1f s)\n", g_Checks.load(), elapsed);
    return 0;
}

