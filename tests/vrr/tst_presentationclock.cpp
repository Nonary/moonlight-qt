#include "../../app/streaming/video/ffmpeg-renderers/presentationclock.h"

#include <cstdio>
#include <limits>

int main()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    const auto sample = PresentationClockSample::translate(90000, 100000, 20000, 20004);
    expect(sample.timeUs == 19002 && sample.uncertaintyUs == 3,
           "interrupt time is translated into the midpoint of the worker clock bracket");
    const auto laterEpoch = PresentationClockSample::translate(1090000, 1100000, 120000, 120004);
    expect(laterEpoch.timeUs == 119002, "a fresh correlation tolerates a changed clock epoch");
    expect(!PresentationClockSample::translate(100001, 100000, 20000, 20004).timeUs,
           "a future display timestamp is not accepted");
    expect(!PresentationClockSample::translate(0, 100000, 20000, 20004).timeUs,
           "a missing display timestamp is not accepted");
    expect(!PresentationClockSample::translate(1, 1000002, 200000, 200004).timeUs,
           "events older than the feedback matching window are rejected");
    expect(!PresentationClockSample::translate(90000, 100000, 20004, 20000).timeUs,
           "backwards worker clock brackets are rejected");
    expect(!PresentationClockSample::translate(90000, 100000, 20000, 20501).timeUs,
           "scheduler stalls cannot create a precise timestamp");
    expect(!PresentationClockSample::translate(90000, 100000, 10, 14).timeUs,
           "samples before the worker clock epoch cannot underflow");
    const auto max = std::numeric_limits<uint64_t>::max();
    const auto large = PresentationClockSample::translate(max - 10000, max, max - 4, max);
    expect(large.timeUs == max - 1002, "large counter values do not overflow");
    return failures ? 1 : 0;
}
