#include "../../app/streaming/video/incomingframetiming.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;

void check(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

void expectScore(const IncomingFrameTiming& timing, double expected, const char* description)
{
    const auto score = timing.score();
    check(score && std::abs(*score - expected) < 0.000001, description);
}

struct Stream {
    IncomingFrameTiming timing;
    uint32_t frame;
    uint32_t timestamp;

    explicit Stream(VrrTimingOptions options = VrrTimingOptions::preset(1),
                    uint32_t firstFrame = 1, uint32_t firstStamp = 0)
        : timing(options), frame(firstFrame), timestamp(firstStamp)
    {
        timing.observe(frame, timestamp);
    }

    void next(uint32_t ticks, uint32_t frames = 1)
    {
        frame += frames;
        timestamp += ticks;
        timing.observe(frame, timestamp);
    }

    void run(int count, uint32_t ticks)
    {
        for (int i = 0; i < count; ++i) next(ticks);
    }
};
}

int main()
{
    Stream rates;
    rates.run(2, 750);
    check(!rates.timing.score(), "wait for independent host cadence evidence");
    rates.run(20, 750);
    rates.run(20, 1500);
    rates.run(20, 3000);
    rates.run(20, 4590);
    rates.run(20, 750);
    expectScore(rates.timing, 100, "sustained rate changes and steady 51 ms output carry no loss");

    Stream jitter, boundary, tight(VrrTimingOptions::preset(0));
    for (int i = 0; i < 200; ++i) {
        jitter.next(i % 2 ? 801 : 999); // 8.9 and 11.1 ms around 10 ms
        boundary.next(i % 2 ? 855 : 945);
        tight.next(i % 2 ? 855 : 945);
    }
    expectScore(jitter.timing, 94, "alternating host intervals expose error on both frames");
    expectScore(boundary.timing, 100, "the 500 us tolerance includes its boundary");
    expectScore(tight.timing, 97, "the same timestamps honor a tighter 200 us tolerance");

    Stream weighted;
    weighted.run(10, 900);
    weighted.next(1080);
    expectScore(weighted.timing, 100 * (1 - 1800.0 / 112000), "a final outlier is visible immediately");
    weighted.run(10, 900);
    expectScore(weighted.timing, 100 * (1 - 1800.0 / 212000), "loss is weighted by host interval duration");

    auto options = VrrTimingOptions::preset(1);
    options.historySeconds = 10;
    Stream expiry(options);
    expiry.run(10, 900);
    expiry.next(1080);
    expiry.run(990, 900);
    check(expiry.timing.score() && *expiry.timing.score() < 100, "loss remains within ten seconds of host time");
    expiry.run(10, 900);
    expectScore(expiry.timing, 100, "host time expires the configured history");
    expiry.next(900000);
    check(!expiry.timing.score(), "a long interruption expires old evidence");

    Stream wrap(options, UINT32_MAX - 2, UINT32_MAX - 1799);
    wrap.run(8, 900);
    expectScore(wrap.timing, 100, "frame and RTP wrap through zero");

    Stream gap;
    gap.run(8, 900);
    gap.next(4500, 3);
    gap.run(8, 1500);
    gap.timing.observe(gap.frame - 1, gap.timestamp - 1);
    gap.next(0);
    gap.run(8, 750);
    expectScore(gap.timing, 100, "missing, reordered and repeated samples do not make cadence evidence");
    gap.next(UINT32_MAX - 10000);
    check(!gap.timing.score(), "an RTP clock restart starts a new history");
    gap.run(8, 3000);
    expectScore(gap.timing, 100, "cadence reacquires after a clock restart");

    Stream absent;
    absent.run(20, 0);
    check(!absent.timing.score(), "absent RTP reports unavailable host timing");
    return failures == 0 ? 0 : 1;
}
