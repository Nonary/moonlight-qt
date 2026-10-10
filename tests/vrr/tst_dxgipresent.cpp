#include "../../app/streaming/video/ffmpeg-renderers/dxgipresent.h"
#include "../../app/streaming/video/ffmpeg-renderers/d3d11fencewait.h"
#include "../../app/streaming/video/ffmpeg-renderers/d3d11presentpolicy.h"

#include <cstdio>
#include "../../app/streaming/video/ffmpeg-renderers/dxgiwaitable.h"

namespace {
struct FakeSwapChain
{
    unsigned int interval = 99;
    unsigned int flags = 99;
    unsigned int calls = 0;
    long result = 0;

    long Present(unsigned int newInterval, unsigned int newFlags)
    {
        interval = newInterval;
        flags = newFlags;
        ++calls;
        return result;
    }
};
}

int main()
{
    constexpr unsigned int allowTearing = 0x200;
    FakeSwapChain swapChain;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    {
        using namespace DxgiWaitable;
        for (int bits = 0; bits < 16; ++bits)
            check(requested(bits & 1, bits & 2, bits & 4, bits & 8) == (bits == 3),
                  "default waitable pacing requires VRR, V-sync, real playback, and no composition override");
        const struct { const char* value; bool composition; } overrides[] = {
            {nullptr, false}, {"", false}, {"0", false}, {"1", true},
            {"2", false}, {"true", false}, {"01", false}
        };
        for (const auto& override : overrides) {
            const bool composition = compositionRequested(true, override.value);
            check(composition == override.composition,
                  "only MOONLIGHT_VRR_COMPOSITION=1 may request composition");
            check(requested(true, true, false, composition) == !composition,
                  "unset or non-enabling composition overrides must retain default waitable DXGI");
            check(!compositionRequested(false, override.value),
                  "the composition override must not activate composition outside VRR");
        }
        Admission admission;
        uint64_t now = 0;
        unsigned calls = 0;
        auto clock = [&] { return now; };
        auto signal = [&](unsigned ms) { ++calls; now += ms * 1000; return Wake::Signalled; };
        auto timeout = [&](unsigned ms) { ++calls; now += ms * 1000; return Wake::Timeout; };
        check(admission.acquire(admission.epoch(), clock, signal) == Status::Admitted && calls == 1,
              "even the first frame must wait for admission");
        admission.interrupt(false);
        check(admission.acquire(admission.epoch(), clock, signal) == Status::Admitted && calls == 1,
              "cancellation/resize before Present retains the consumed permit");
        admission.presented();
        check(admission.acquire(admission.epoch(), clock, timeout) == Status::Timeout && now == 51000,
              "queue saturation is bounded and must not admit rendering");
        auto oldEpoch = admission.epoch();
        check(admission.acquire(oldEpoch, clock, [&](unsigned) {
                  admission.interrupt(false); return Wake::Signalled;
              }) == Status::Interrupted, "a signal racing resize must not admit stale rendering");
        check(admission.acquire(admission.epoch(), clock, signal) == Status::Admitted && calls == 51,
              "a signal consumed during interruption is retained for the next frame");
        admission.presented();
        check(admission.acquire(admission.epoch(), clock, [](unsigned) { return Wake::Failed; }) == Status::Failed,
              "native wait failure must not admit rendering");
        admission.interrupt(true);
        check(admission.acquire(admission.epoch(), clock, signal) == Status::Interrupted && calls == 51,
              "shutdown remains cancelled even when the waiter starts after interruption");
        Admission stalled;
        unsigned stalledCalls = 0;
        check(stalled.acquire(0, [] { return uint64_t{1}; }, [&](unsigned) {
                  ++stalledCalls; return Wake::Timeout;
              }) == Status::Timeout && stalledCalls == 50, "a stalled clock still has a finite wait bound");
    }
    {
        D3D11PresentPolicy::PreparedCompletion prepared;
        check(!prepared.ready(7), "an unprepared frame must have no completion proof");
        prepared.begin(7);
        check(!prepared.complete(7, false) && !prepared.ready(7),
              "a failed fence wait must never admit presentation or surface reuse");
        check(!prepared.complete(6, true) && !prepared.ready(7),
              "another frame's completion must never release the active frame");
        check(prepared.complete(7, true) && prepared.ready(7),
              "the exact successful marker must admit a nonblocking Present");
        prepared.clear();
        check(!prepared.ready(7) && !prepared.complete(7, true),
              "a completion returning after cancellation must not revive the frame");
        prepared.begin(8);
        check(!prepared.complete(7, true) && !prepared.ready(8),
              "a completion returning after replacement must not admit its successor");
        check(prepared.complete(8, true) && prepared.ready(8),
              "the replacement needs its own completed rendering marker");
    }
    {
        using D3D11PresentPolicy::FlipObservation;
        const FlipObservation freshBlank{true, true, false, true, true, 1000, 1050};
        const auto protect = [&](FlipObservation observation, uint64_t nowUs,
                                 uint64_t windowUs, bool expectedLatch,
                                 const char* message) {
            unsigned queryCalls = 0, clockCalls = 0;
            const bool latched = D3D11PresentPolicy::latch(false, false, windowUs,
                [&] { ++queryCalls; return observation; },
                [&] { ++clockCalls; return nowUs; });
            check(latched == expectedLatch, message);
            check(queryCalls == unsigned(windowUs != 0),
                  "enabled protection must make one observation without polling");
            if (!windowUs) check(clockCalls == 0,
                "explicitly disabled protection must not query the clock");
            const auto parameters = DxgiPresentParameters::adaptive(latched, allowTearing);
            const auto previousCalls = swapChain.calls;
            parameters.present(swapChain);
            check(swapChain.calls == previousCalls + 1 &&
                      swapChain.interval == unsigned(expectedLatch) &&
                      swapChain.flags == (expectedLatch ? 0U : allowTearing),
                  "uncertain scanout must submit (1,0), and only an admitted blank may submit (0,ALLOW_TEARING)");
            check(swapChain.interval == parameters.syncInterval &&
                      swapChain.flags == parameters.flags,
                  "flip protection telemetry must match the actual native parameters");
        };
        protect(freshBlank, 1100, 8333, false,
                "a fresh blank with a known retired predecessor may retain adaptive presentation");
        auto observation = freshBlank;
        observation.predecessorKnown = false;
        protect(observation, 1100, 8333, true,
                "startup or a missing predecessor ID must not permit an unsafe tearing flip");
        observation = freshBlank;
        observation.pending = true;
        protect(observation, 1100, 8333, true,
                "a pending predecessor must remain protected even when the raster reports blank");
        observation.inVerticalBlank = false;
        protect(observation, 1100, 8333, true,
                "pending work during active scanout must remain protected");
        observation.pending = false;
        protect(observation, 1100, 8333, true,
                "an accounted-for predecessor can still be actively scanning out");
        observation = freshBlank;
        observation.statisticsValid = false;
        protect(observation, 1100, 8333, true,
                "invalid or disjoint statistics must not become proof of a safe blank");
        observation = freshBlank;
        observation.inVerticalBlank = false;
        protect(observation, 1100, 8333, true,
                "active scanout must stay protected before a subsequent query failure");
        observation.rasterValid = false;
        observation.inVerticalBlank = true;
        protect(observation, 1100, 8333, true,
                "a raster failure after active scanout must not reuse a blank flag");
        // The preceding active-scanout observation and these later failures
        // must never turn into a session-wide permission to tear.
        for (int i = 0; i < 8; ++i) {
            protect(observation, 1100, 8333, true,
                    "repeated raster failures must never disable flip protection");
        }
        observation.statisticsValid = false;
        protect(observation, 1100, 8333, true,
                "missing both timing sources must select synchronized fallback");
        observation = freshBlank;
        observation.rasterQueryStartUs = 0;
        protect(observation, 1100, 8333, true,
                "a blank without an observation timestamp must remain protected");
        observation = freshBlank;
        observation.rasterQueryEndUs = 999;
        protect(observation, 1100, 8333, true,
                "a reversed query clock must not authorize tearing");
        protect(freshBlank, 1049, 8333, true,
                "a future raster observation must not authorize tearing");
        protect(freshBlank, 1250, 8333, false,
                "the 250 us total-age limit must include its exact boundary");
        protect(freshBlank, 1251, 8333, true,
                "an observation older than the 250 us cap must remain protected");
        observation = freshBlank;
        observation.rasterQueryEndUs = 1251;
        protect(observation, 1251, 8333, true,
                "a slow raster query must not produce usable phase evidence");
        observation.rasterQueryEndUs = 1160;
        protect(observation, 1260, 8333, true,
                "query duration and preemption after the query must share one total-age budget");
        protect(freshBlank, 2000, 8333, true,
                "a stale blank after preemption must not authorize tearing");
        protect(freshBlank, 1100, 1600, false,
                "the period-relative freshness limit must include its exact boundary");
        protect(freshBlank, 1101, 1600, true,
                "a short display period must tighten freshness below the 250 us cap");
        protect(freshBlank, 1050, 15, true,
                "a nonzero protection window with no precision budget must fail closed");
        protect({}, 1100, 0, false,
                "zero must remain an explicit opt-out of native flip protection");

        for (const bool plannedLatched : {false, true}) {
            for (const bool synchronizeAll : {false, true}) {
                if (!plannedLatched && !synchronizeAll) continue;
                for (const uint64_t windowUs : {uint64_t(0), uint64_t(8333)}) {
                    unsigned queryCalls = 0, clockCalls = 0;
                    const bool latched = D3D11PresentPolicy::latch(
                        plannedLatched, synchronizeAll, windowUs,
                        [&] { ++queryCalls; return FlipObservation{}; },
                        [&] { ++clockCalls; return uint64_t(1100); });
                    check(latched && queryCalls == 0 && clockCalls == 0,
                          "planned and all-sync protection must bypass observation and clock queries even when the guard is disabled");
                    DxgiPresentParameters::adaptive(latched, allowTearing).present(swapChain);
                    check(swapChain.interval == 1 && swapChain.flags == 0,
                          "planned and all-sync branches must clear ALLOW_TEARING");
                }
            }
        }
    }
    for (const auto readyAt : {0ULL, 3000ULL, 50000ULL, 100000ULL}) {
        uint64_t now = 0;
        const auto result = D3D11FenceWait::wait(7, [&] { return now; },
            [&] { return now >= readyAt ? 7ULL : 6ULL; },
            [&](unsigned timeoutMs) { now += timeoutMs * 1000; return true; });
        check(result.status == (readyAt <= 50000 ? D3D11FenceWait::Status::Complete :
            D3D11FenceWait::Status::Timeout),
            "missing event notifications must not hide completed GPU work or admit incomplete work");
        check(now == std::min<uint64_t>(readyAt, 50000),
            "completion polling must recover promptly and retain the 50 ms total timeout");
        check(result.elapsedUs == now && result.waitCalls == now / 1000 &&
              result.stopReason == (readyAt <= 50000 ? D3D11FenceWait::StopReason::Completed :
                  D3D11FenceWait::StopReason::Deadline),
              "wait diagnostics must distinguish elapsed deadline from successful completion");
    }
    {
        uint64_t now = 0;
        unsigned calls = 0;
        const auto result = D3D11FenceWait::wait(7, [&] { return now; },
            [&] { return now >= 3000 ? 7ULL : 6ULL; },
            [&](unsigned timeoutMs) { if (calls++) now += timeoutMs * 1000; return true; });
        check(result.status == D3D11FenceWait::Status::Complete && now == 3000 && calls == 4,
              "a stale signalled event must not release a still-incomplete frame");
    }
    check(D3D11FenceWait::wait(7, [] { return 0ULL; }, [] { return 6ULL; },
          [](unsigned) { return false; }).status == D3D11FenceWait::Status::WaitFailed,
          "native wait failures must propagate");
    check(D3D11FenceWait::wait(7, [] { return 0ULL; },
          [] { return std::numeric_limits<uint64_t>::max(); },
          [](unsigned) { return true; }).status == D3D11FenceWait::Status::DeviceRemoved,
          "the device-removed fence sentinel must never count as GPU completion");
    check(D3D11FenceWait::wait(7, [] { return 0ULL; }, [] { return 6ULL; },
          [](unsigned) { return true; }).status == D3D11FenceWait::Status::Timeout,
          "stale events and a stalled clock must not loop forever");
    {
        const auto result = D3D11FenceWait::wait(7, [] { return 0ULL; }, [] { return 6ULL; },
            [](unsigned) { return true; });
        check(result.stopReason == D3D11FenceWait::StopReason::IterationLimit &&
              result.elapsedUs == 0 && result.waitCalls == 100,
              "an iteration guard must not be diagnosed as a 50 ms GPU stall");
    }
    {
        uint64_t now = 1000;
        const auto result = D3D11FenceWait::wait(7, [&] { return now; }, [] { return 6ULL; },
            [&](unsigned) { now = 999; return true; });
        check(result.status == D3D11FenceWait::Status::Timeout &&
              result.stopReason == D3D11FenceWait::StopReason::ClockReversed &&
              result.elapsedUs == 0 && result.waitCalls == 1,
              "a reversed clock must report its cause without unsigned elapsed-time underflow");
    }
    {
        uint64_t now = 0;
        const auto result = D3D11FenceWait::wait(7, [&] { return now; }, [] { return 6ULL; },
            [&](unsigned) { now += 100; return false; });
        check(result.status == D3D11FenceWait::Status::WaitFailed &&
              result.stopReason == D3D11FenceWait::StopReason::NativeWaitFailed &&
              result.elapsedUs == 100 && result.waitCalls == 1,
              "native wait failure diagnostics must count the failing call and its elapsed time");
    }
    {
        // An already completed preparation marker needs no CPU wait. Its
        // completion proof is retained through the later cadence hold.
        uint64_t now = 8000;
        const auto result = D3D11FenceWait::wait(7, [&] { return now; },
            [] { return 7ULL; },
            [&](unsigned timeoutMs) { now += timeoutMs * 1000; return true; });
        check(result.status == D3D11FenceWait::Status::Complete &&
              result.elapsedUs == 0 && result.waitCalls == 0 && now == 8000,
              "an already complete preparation marker must add no CPU wait");
    }
    {
        // Preparation waits only for its exact remaining GPU work, before
        // the worker begins its cadence hold. Present does not repeat it.
        uint64_t now = 8000;
        const auto result = D3D11FenceWait::wait(7, [&] { return now; },
            [&] { return now >= 10000 ? 7ULL : 6ULL; },
            [&](unsigned timeoutMs) { now += timeoutMs * 1000; return true; });
        check(result.status == D3D11FenceWait::Status::Complete &&
              result.elapsedUs == 2000 && result.waitCalls == 2 && now == 10000,
              "preparation must charge only the remaining exact-marker wait");
    }
    const auto submit = [&](DxgiPresentParameters parameters,
                            unsigned int interval, unsigned int flags) {
        const auto previousCalls = swapChain.calls;
        check(parameters.present(swapChain) == swapChain.result,
              "native Present result must propagate unchanged");
        check(swapChain.calls == previousCalls + 1,
              "one frame must issue exactly one native Present");
        check(swapChain.interval == interval && swapChain.flags == flags,
              "native Present received the wrong interval or flags");
        check(swapChain.interval == parameters.syncInterval &&
                  swapChain.flags == parameters.flags,
              "native arguments must match the parameters reported in telemetry");
    };

    // Switch both ways: the latched interval must not be silently replaced by
    // zero, and tearing must never carry over into a synchronized submission.
    submit(DxgiPresentParameters::adaptive(true, allowTearing), 1, 0);
    submit(DxgiPresentParameters::adaptive(false, allowTearing), 0, allowTearing);
    submit(DxgiPresentParameters::adaptive(true, allowTearing), 1, 0);

    // Preserve the legacy software-paced caller's explicit interval-zero path.
    submit({0, 0}, 0, 0);
    submit({0, allowTearing}, 0, allowTearing);

    // Both errors and non-display success statuses belong to the caller.
    swapChain.result = -1;
    submit(DxgiPresentParameters::adaptive(true, allowTearing), 1, 0);
    swapChain.result = 1;
    submit(DxgiPresentParameters::adaptive(false, allowTearing), 0, allowTearing);
    return failures == 0 ? 0 : 1;
}
