#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace VrrSim {

// A phase supplies its own workload for this span of virtual time. A periodic
// disturbance starts at phase entry. A zero period applies it once.
struct Phase {
    uint64_t durationUs = 0;
    int hostHz = 120;
    uint64_t jitterUs = 0, networkDelayUs = 1000, decodeUs = 1000, prepareUs = 200;
    uint64_t encodeUs = 0, encoderStallUs = 0;
    uint64_t alternatingFirstUs = 0, alternatingSecondUs = 0;
    uint64_t disturbancePeriodUs = 0, networkSpikeUs = 0, hostPauseUs = 0;
    int clockDriftPpm = 0;
    int64_t rtpJumpUs = 0;
    bool reconnect = false;
};

struct Scenario {
    std::string name;
    int streamHz = 120, displayHz = 144;
    uint32_t seed = 42;
    std::vector<Phase> phases;
    bool dxgiWaitable = false;
};

struct Options {
    int latencyMode = 1, targetHundredths = 0;
    bool smooth = true, canLatch = true;
    int64_t fixedBufferUs = -1;
    bool deferReplacements = false; // Counterfactual run that presents the current frame.
};

class FixedBufferUnavailable : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// The immediate presenter freezes the score at submission and delivers this
// snapshot on the modeled flip. A reconnect may discard a pending flip.
// All times use the simulation epoch. readyUs is the independently timestamped
// decode fence and outputUs is the producer's decoded queue admission.
struct Observation {
    uint64_t frame = 0, session = 0, sourceUs = 0, readyUs = 0, outputUs = 0, dequeuedUs = 0;
    uint64_t submittedUs = 0, shownUs = 0, bufferUs = 0, minimumBufferUs = 0;
    uint64_t preparationStartedUs = 0;
    uint64_t intendedIntervalUs = 0, intervalErrorUs = 0;
    double scorePercent = 0, clientScorePercent = 0;
    bool calibrated = false, scoreAvailable = false, clientScoreAvailable = false;
    bool eligibleInterval = false, clientEligibleInterval = false;
};

struct Counts {
    uint64_t produced = 0, presented = 0, queueDrops = 0, replacements = 0;
    uint64_t reconnectDrops = 0, decodeDrops = 0;
    uint64_t maxCaptureToSendUs = 0, compressedHostSendIntervals = 0;
    uint64_t presenterDrops = 0, maxPendingPresents = 0, presenterAdmissionWaitUs = 0;
};

using PresentObserver = void (*)(void*, const Observation&);
Counts simulate(const Scenario& scenario, const Options& options,
                PresentObserver onPresent, void* context);

struct GapMetrics {
    uint64_t worstGapUs = 0, jerkP99Us = 0, worstJerkUs = 0, jerkOver2Ms = 0;
};

// Offline reference. Retains the baseline scanout times and uses the measured
// next flip to check how many replacement frames could fill an unused slot.
struct GapRecoveryReference {
    GapMetrics baseline, filled;
    uint64_t baselineDrops = 0, filledDrops = 0, insertedFrames = 0;
    uint64_t baselineFramesPreserved = 0, oldestInsertedFrameAgeUs = 0;
};

GapRecoveryReference measureGapRecoveryReference(const Scenario& scenario, const Options& options);

}
