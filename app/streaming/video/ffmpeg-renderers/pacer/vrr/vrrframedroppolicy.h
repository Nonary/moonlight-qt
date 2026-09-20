#pragma once

#include "vrrtimingcontroller.h"
#include <algorithm>
#include <limits>

// Call only when a newer successor is available. Never discard the sole image
// after a host stall. Shared by the worker and the all-arrival queue simulation.
namespace VrrFrameDropPolicy {
// Decode service has already been paid for when the worker checks the ready
// image. A queued successor is not evidence that its GPU decode has finished.
// Counting this wait as replaceable backlog can reject every ready image when
// all decodes take longer than the age limit. Keep genuine queue residence and
// scheduler delay, without changing the source clock or latency telemetry.
inline uint64_t ageExcludingDecodeWaitUs(uint64_t nowUs, uint64_t originUs,
                                       uint64_t decodeSyncWaitUs)
{
    const uint64_t elapsedUs = nowUs > originUs ? nowUs - originUs : 0;
    return elapsedUs - std::min(elapsedUs, decodeSyncWaitUs);
}

// Queue-only rejection, before calling a backend that may block for GPU
// decode. Use both the fitted cadence and the next source interval so a
// slower source transition cannot be mistaken for stale queued work.
inline bool beforeDecodeWait(const PacedFrame& frame, const PacedFrame& successor,
                             uint64_t arrivalUs, uint64_t nowUs,
                             uint64_t sourcePeriodUs, bool metronome)
{
    if (!sourcePeriodUs || !arrivalUs || nowUs <= arrivalUs ||
            !frame.timestampValid() || !successor.timestampValid() ||
            frame.frameNumber() < 0 ||
            int64_t(successor.frameNumber()) != int64_t(frame.frameNumber()) + 1) {
        return false;
    }
    const uint32_t ticks = successor.rtpTimestamp() - frame.rtpTimestamp();
    if (ticks == 0 || ticks > 90000) return false;
    const uint64_t nextIntervalUs = (uint64_t(ticks) * 1000000 + 89999) / 90000;
    const uint64_t periodUs = std::max(sourcePeriodUs, nextIntervalUs);
    const uint64_t periods = metronome ? 4 : 2;
    return periodUs <= std::numeric_limits<uint64_t>::max() / periods &&
        nowUs - arrivalUs > periodUs * periods;
}

inline uint64_t maximumAgeUs(const VrrTimingDecision& decision, bool metronome, bool latencyFix)
{
    // One source interval is normal occupancy for a single worker that waits
    // on the preceding frame's target. Using it as the stale threshold makes
    // a refresh-rate stream alternate present/drop as soon as its successor
    // arrives. Lower-latency modes still measure age from admission, but they
    // must tolerate two intervals before replacing valid decoded work.
    (void) latencyFix;
    const uint64_t periods = metronome ? 4 : 2;
    return decision.sourcePeriodUs > std::numeric_limits<uint64_t>::max() / periods ?
        std::numeric_limits<uint64_t>::max() : decision.sourcePeriodUs * periods;
}

inline bool beforeRender(const VrrTimingDecision& decision, uint64_t displayPeriodUs,
                         uint64_t ageUs, bool metronome, bool latencyFix)
{
    const bool oversupply = displayPeriodUs != 0 && decision.sourcePeriodUs <= displayPeriodUs;
    const bool floorBacklog = (oversupply || latencyFix) &&
        decision.presentationFloorPushUs > decision.sourcePeriodUs / 2;
    return decision.sourcePeriodUs != 0 &&
        (ageUs > maximumAgeUs(decision, metronome, latencyFix) ||
         (metronome && decision.missedTicks != 0) || floorBacklog);
}

inline bool afterRenderWait(const VrrTimingDecision& decision, uint64_t ageOriginUs,
                            uint64_t nowUs, bool metronome, bool latencyFix,
                            uint64_t decodeSyncWaitUs = 0)
{
    const uint64_t originUs = latencyFix ? ageOriginUs : decision.targetUs;
    const uint64_t limitUs = maximumAgeUs(decision, metronome, latencyFix);
    // A target-relative horizon already starts after decode synchronization.
    // Only admission-relative age includes the service we must exclude here.
    const uint64_t ageUs = ageExcludingDecodeWaitUs(
        nowUs, originUs, latencyFix ? decodeSyncWaitUs : 0);
    return limitUs != 0 && ageUs > limitUs;
}
}
