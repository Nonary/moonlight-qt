#pragma once

#include <QtGlobal>
#include <stdint.h>

// Sub-millisecond sleep primitives for the pacing threads (the cadence
// thread's frame-target wait and VrrPresenter's present-target hold).
//
// The contract on every platform: block until the requested instant in the
// LiGetMicroseconds() timebase, waking at or shortly after it (scheduler
// latency, typically well under a millisecond). Callers that must LAND on an
// exact instant keep their own margin and spin the final stretch against
// LiGetMicroseconds() - see waitForVrrCadenceTargetUs() - so the primitive's
// wake latency never decides pacing accuracy, only how much of the wait
// burns CPU.
class HighResSleep
{
public:
    // Block until LiGetMicroseconds() >= targetUs. Returns immediately if
    // the target is already in the past.
    static void sleepUntilUs(uint64_t targetUs);

    // Relative convenience wrapper over sleepUntilUs().
    static void sleepUs(uint64_t durationUs);
};
