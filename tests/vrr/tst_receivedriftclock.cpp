#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/receivedriftclock.h"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
using Vrr13::ReceiveDriftClock;

static int64_t run(int64_t ppm, bool phaseStep, bool packetStep, bool cadenceNoise) {
    ReceiveDriftClock c;
    int64_t drift = 0;
    for (uint64_t tick = 1; tick <= 18000; ++tick) {
        const uint64_t at = 1000000 + tick * 10000;
        const bool lowerRate = tick >= 6000;
        uint64_t period = lowerRate ? 10000 : 8621;
        if (cadenceNoise) period += (tick % 2 ? 30 : 0);
        const int64_t offset = 1000000 + int64_t(tick) * ppm / 100 +
            (packetStep && lowerRate ? 4000 : 0);
        // Decoder latency/producer mapping deliberately changes by4ms.
        // It must not be an input to the receive-clock observer.
        const int64_t decoderPhase = phaseStep && tick >= 2000 && !lowerRate ? 4000 : 0;
        (void)decoderPhase;
        drift += c.advance(at, true, offset, period, 1, 12);
        assert(std::abs(c.fitPpb()) <= ReceiveDriftClock::MaximumPpb);
        if (tick == 4000 || tick == 6500) {
            const int64_t trueDrift = int64_t(tick) * ppm / 100;
            // Startup fits need15s. Steady subsequent differences, rather
            // than startup absolute phase, are the clock invariant.
            (void)trueDrift;
        }
    }
    assert(std::abs(c.fitPpb() - ppm * 1000) <= 100);
    return drift;
}
int main() {
    assert(run(0, true, false, false) == 0);
    assert(run(0, true, true, false) == 0);
    const int64_t positive = run(50, false, false, false);
    const int64_t negative = run(-50, false, false, false);
    assert(std::abs(run(50, true, false, false) - positive) <= 2);
    assert(std::abs(run(-50, true, false, false) - negative) <= 2);
    assert(std::abs(run(50, true, true, false) - positive) <= 2);
    assert(std::abs(run(-50, true, true, false) - negative) <= 2);
    assert(std::abs(run(50, true, false, true) - positive) <= 2);
    ReceiveDriftClock c;
    for (uint64_t i=1;i<=3000;++i) c.advance(1000000+i*10000,true,1000000+i/2,8621,1,12);
    assert(c.fitPpb()>49000 && c.fitPpb()<51000);
    const int64_t prior=c.fitPpb();
    assert(c.advance(31000000,true,9000000,8621,1,12)==0);
    assert(c.fitPpb()==prior); // duplicate time did not admit a false point
    c.advance(31010000,true,9000000,8621,2,12);
    assert(c.fitPpb()==prior); // epoch phase jump does not become clock rate
    for(uint64_t i=1;i<=1500;++i) c.advance(31010000+i*100000,false,0,8621,2,12);
    assert(c.fitPpb()==0); // no unbounded extrapolation from stale evidence
    std::cout << "receive drift clock invariants passed\n";
}
