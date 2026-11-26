#include "vrrstreamscenarios.h"
#include "../../app/streaming/video/ffmpeg-renderers/dxgiwaitable.h"

#include "../../app/settings/vrrtimingoptions.h"
#include "vrrreplaymetrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace VrrSim {
namespace {

constexpr uint64_t Second = 1000000;
constexpr uint64_t DrainStepUs = 250;
constexpr double MinuteUs = 60.0 * Second;

Phase phase(unsigned seconds, int hz = 120)
{
    Phase result{};
    result.durationUs = uint64_t(seconds) * Second;
    result.hostHz = hz;
    return result;
}

Workload workload(const char* name, std::initializer_list<Phase> phases,
                  uint64_t recoverAfterSeconds = 0, bool extended = false)
{
    Workload result;
    result.scenario.name = name;
    result.scenario.phases = phases;
    result.recoveryStartUs = recoverAfterSeconds * Second;
    result.extended = extended;
    return result;
}

double percent(uint64_t numerator, uint64_t denominator)
{
    return denominator ? 100.0 * double(numerator) / double(denominator) : 0.0;
}

uint64_t difference(uint64_t a, uint64_t b)
{
    return a > b ? a - b : b - a;
}

struct DelayCycle {
    bool sawLow = false, sawHigh = false;

    bool observe(uint64_t us) {
        if (us <= 500) {
            const bool complete = sawHigh;
            sawLow = true;
            sawHigh = false;
            return complete;
        }
        if (sawLow && us >= 2000) sawHigh = true;
        return false;
    }
};

class Accumulator {
public:
    Accumulator(const Workload& workload, const Options& options,
                std::optional<uint64_t> qualifyingReserveUs = std::nullopt)
        : m_RecoveryStartUs(workload.recoveryStartUs), m_QualifyingReserveUs(qualifyingReserveUs)
    {
        VrrTimingOptions requested;
        requested.targetHundredths = options.targetHundredths;
        const auto resolved = requested.resolved(options.latencyMode);
        m_Result.targetPercent = resolved.targetHundredths / 100.0;
        m_ToleranceUs = uint64_t(resolved.toleranceUs);
        for (const auto& phase : workload.scenario.phases)
            m_Result.elapsedUs += phase.durationUs;
        m_Result.fingerprint = 14695981039346656037ULL;
    }

    void present(const Observation& frame)
    {
        // Use the virtual producer lifetime as the denominator, even when the
        // last few frames are submitted after the producer stops.
        const uint64_t endUs = std::min(frame.submittedUs, m_Result.elapsedUs);
        const uint64_t startUs = std::min(m_Previous.submittedUs, m_Result.elapsedUs);
        const uint64_t dt = endUs >= startUs ? endUs - startUs : 0;
        const bool sameSession = m_HavePrevious && frame.session == m_Previous.session;
        const bool ordered = !m_HavePrevious ||
            (frame.submittedUs >= m_Previous.submittedUs && frame.shownUs >= m_Previous.shownUs);
        if (!ordered || frame.readyUs < frame.sourceUs || frame.dequeuedUs < frame.outputUs ||
            frame.outputUs < frame.sourceUs || frame.submittedUs < std::max(frame.readyUs, frame.dequeuedUs) ||
            frame.shownUs < frame.submittedUs) ++m_Result.timestampViolations;
        if (sameSession && (frame.frame <= m_Previous.frame || frame.sourceUs < m_Previous.sourceUs))
            ++m_Result.frameOrderViolations;
        if (!sameSession) {
            ++m_Result.sessionsObserved;
            if (!m_HavePrevious) m_Result.firstBufferUs = frame.bufferUs;
            else m_Result.maxReconnectStartBufferUs = std::max(m_Result.maxReconnectStartBufferUs, frame.bufferUs);
            m_PreviousDirection = 0;
            m_HaveShownInterval = false;
        }
        if (!sameSession || frame.frame != m_Previous.frame + 1) {
            m_ExtraCycle = {};
            m_SustainedExtraCycle = {};
            m_ExtraCount = m_ExtraIndex = 0;
        }
        m_Result.extraDelayCycles += m_ExtraCycle.observe(frame.extraDelayUs);
        m_ExtraWindow[m_ExtraIndex] = frame.extraDelayUs;
        m_ExtraIndex = (m_ExtraIndex + 1) % m_ExtraWindow.size();
        m_ExtraCount = std::min(m_ExtraCount + 1, m_ExtraWindow.size());
        if (m_ExtraCount == m_ExtraWindow.size()) {
            uint64_t totalUs = 0;
            for (const auto delayUs : m_ExtraWindow) totalUs += delayUs;
            m_Result.sustainedExtraDelayCycles += m_SustainedExtraCycle.observe(totalUs / m_ExtraCount);
        }

        // The score visible between two feedback events is the score at the
        // earlier event. A dropped or ineligible pair cannot lend it coverage.
        const bool eligible = sameSession && frame.eligibleInterval && ordered;
        bool atTarget = false;
        if (!m_HavePrevious || (sameSession && !m_Previous.calibrated))
            m_Result.calibrationUs += dt;
        else if (!eligible || !m_Previous.scoreAvailable || !frame.scoreAvailable)
            m_Result.unavailableUs += dt;
        else if (m_Previous.scorePercent >= m_Result.targetPercent) {
            atTarget = true;
            m_Result.atTargetUs += dt;
        }
        else {
            m_Result.belowTargetUs += dt;
            m_BelowRunUs += dt;
            m_Result.longestBelowTargetUs = std::max(m_Result.longestBelowTargetUs, m_BelowRunUs);
        }
        if (!eligible || !m_Previous.calibrated || !m_Previous.scoreAvailable ||
            !frame.scoreAvailable || atTarget) m_BelowRunUs = 0;

        if (m_RecoveryStartUs && startUs < m_RecoveryStartUs && endUs >= m_RecoveryStartUs)
            m_Result.bufferAtRecoveryStartUs = m_Previous.bufferUs;
        if (m_RecoveryStartUs && !m_Result.recovered) {
            if (!atTarget) m_RecoveryRunUs = 0;
            else if (endUs > m_RecoveryStartUs) {
                m_RecoveryRunUs += endUs - std::max(startUs, m_RecoveryStartUs);
                if (m_RecoveryRunUs >= Second) {
                    m_Result.recoveryUs = endUs - m_RecoveryRunUs - m_RecoveryStartUs;
                    m_Result.recovered = true;
                }
            }
        }
        const uint64_t steadyStartUs = m_Result.elapsedUs - m_Result.elapsedUs / 4;
        const uint64_t steadyStart = std::max(startUs, steadyStartUs);
        const bool scored = eligible && m_Previous.calibrated && m_Previous.scoreAvailable && frame.scoreAvailable;
        if (!scored) m_NoDrainUs = 0;
        if (m_QualifyingReserveUs && endUs > steadyStart) {
            const uint64_t excessUs = m_Previous.bufferUs - std::min(m_Previous.bufferUs, *m_QualifyingReserveUs);
            if (scored && m_Previous.scorePercent > m_Result.targetPercent && excessUs > DrainStepUs) {
                if (!m_NoDrainUs) m_DrainStartBufferUs = m_Previous.bufferUs;
                m_NoDrainUs += endUs - steadyStart;
                m_Result.longestSteadySurplusWithoutDrainUs = std::max(m_Result.longestSteadySurplusWithoutDrainUs,
                                                                      m_NoDrainUs);
                // Small steps count cumulatively. A rise followed by a fall
                // to the same reserve does not count as draining the queue.
                if (m_DrainStartBufferUs - std::min(m_DrainStartBufferUs, frame.bufferUs) >= DrainStepUs)
                    m_NoDrainUs = 0;
            }
            else m_NoDrainUs = 0;
        }
        if (sameSession && ordered) {
            m_Result.bufferObservedUs += dt;
            m_BufferIntegral += double(m_Previous.bufferUs) * double(dt);
            m_ExtraIntegral += double(m_Previous.extraDelayUs) * double(dt);
            if (endUs > steadyStart) {
                const uint64_t spanUs = endUs - steadyStart;
                m_SteadyDurationUs += spanUs;
                m_SteadyIntegral += double(m_Previous.bufferUs) * double(spanUs);
                m_SteadyMin = std::min(m_SteadyMin, m_Previous.bufferUs);
                m_SteadyMax = std::max(m_SteadyMax, m_Previous.bufferUs);
                if (scored) {
                    m_Result.steadyScoredUs += spanUs;
                    if (atTarget) m_Result.steadyAtTargetUs += spanUs;
                    if (m_Previous.scorePercent > m_Result.targetPercent) m_Result.steadyAboveTargetUs += spanUs;
                }
            }
            if (m_Previous.bufferUs == m_Previous.minimumBufferUs) m_Result.minimumBufferTimeUs += dt;
            const int direction = (frame.bufferUs > m_Previous.bufferUs) -
                                  (frame.bufferUs < m_Previous.bufferUs);
            if (direction) {
                if (m_PreviousDirection && direction != m_PreviousDirection) ++m_Result.bufferReversals;
                const uint64_t move = difference(frame.bufferUs, m_Previous.bufferUs);
                m_Result.totalBufferMovementUs += move;
                m_NetGrowthUs += direction * double(move);
                m_PreviousDirection = direction;
            }
            if (frame.sourceUs >= m_Previous.sourceUs) {
                const uint64_t display = frame.shownUs - m_Previous.shownUs;
                const uint64_t source = frame.sourceUs - m_Previous.sourceUs;
                m_Hitch.add(display - std::min(display, source));
                if (m_HaveShownInterval) {
                    const uint64_t jerk = difference(display, m_ShownIntervalUs);
                    m_DisplayJerk.add(jerk);
                    m_JerkOver2Ms += jerk > 2000;
                }
                m_ShownIntervalUs = display;
                m_HaveShownInterval = true;
            }
        }
        if (eligible && frame.clientEligibleInterval && frame.intendedIntervalUs) {
            // Same severity-weighted cost as the controller. Charge only the
            // observed part of an interval crossing the measurement boundary.
            const uint64_t excess = frame.intervalErrorUs - std::min(frame.intervalErrorUs, m_ToleranceUs);
            m_Result.clientEligibleUs += dt;
            m_Result.clientLossUs += dt * std::min(1.0, double(excess) / frame.intendedIntervalUs);
        }
        m_Buffer.add(frame.bufferUs);
        m_Result.finalBufferUs = frame.bufferUs;
        m_Result.finalMinBufferUs = frame.minimumBufferUs;
        m_Result.finalScoreAvailable = frame.scoreAvailable;
        m_Result.finalScorePercent = frame.scorePercent;
        if (frame.shownUs >= frame.sourceUs) m_EndToEnd.add(frame.shownUs - frame.sourceUs);
        if (frame.dequeuedUs >= frame.outputUs) m_Residence.add(frame.dequeuedUs - frame.outputUs);
        if (frame.shownUs >= frame.readyUs) {
            m_ReadyToDisplayTotalUs += frame.shownUs - frame.readyUs;
            ++m_ReadyToDisplaySamples;
            if (frame.submittedUs >= steadyStartUs && frame.submittedUs < m_Result.elapsedUs) {
                m_SteadyReadyToDisplayUs += frame.shownUs - frame.readyUs;
                ++m_SteadyReadySamples;
            }
        }

        auto mix = [&](uint64_t value) { m_Result.fingerprint = (m_Result.fingerprint ^ value) * 1099511628211ULL; };
        mix(frame.frame);
        mix(frame.session);
        mix(frame.sourceUs);
        mix(frame.readyUs);
        mix(frame.outputUs);
        mix(frame.dequeuedUs);
        mix(frame.submittedUs);
        mix(frame.shownUs);
        mix(frame.bufferUs);
        mix(frame.intervalErrorUs);
        mix(uint64_t(std::llround(1000000 * frame.scorePercent)));
        mix((frame.calibrated ? 1U : 0U) | (frame.eligibleInterval ? 2U : 0U) |
            (frame.scoreAvailable ? 4U : 0U) | (frame.clientEligibleInterval ? 8U : 0U));
        m_Previous = frame;
        m_HavePrevious = true;
    }

    RunMetrics finish(const Counts& counts)
    {
        m_Result.counts = counts;
        // Without an ending observation, any tail is unknown. Keep the full
        // producer lifetime partitioned even if the worker never presented.
        m_Result.unavailableUs += m_Result.elapsedUs -
            std::min(m_Result.elapsedUs, m_Previous.submittedUs);
        m_Result.bufferSamples = m_Buffer.count;
        m_Result.bufferP95Us = m_Buffer.percentile(95);
        m_Result.bufferP99Us = m_Buffer.percentile(99);
        m_Result.peakBufferUs = m_Buffer.maximum;
        if (m_Result.bufferObservedUs) {
            m_Result.meanBufferUs = m_BufferIntegral / m_Result.bufferObservedUs;
            m_Result.meanExtraDelayUs = m_ExtraIntegral / m_Result.bufferObservedUs;
            m_Result.bufferGrowthUsPerMinute = MinuteUs * m_NetGrowthUs / m_Result.bufferObservedUs;
            m_Result.bufferReversalsPerMinute = MinuteUs * m_Result.bufferReversals / m_Result.bufferObservedUs;
        }
        if (m_SteadyDurationUs) {
            m_Result.steadyMeanBufferUs = m_SteadyIntegral / m_SteadyDurationUs;
            m_Result.steadyBufferRangeUs = m_SteadyMax - m_SteadyMin;
        }
        m_Result.hitchP95Us = m_Hitch.percentile(95);
        m_Result.hitchP99Us = m_Hitch.percentile(99);
        m_Result.worstHitchUs = m_Hitch.maximum;
        m_Result.displayJerkP95Us = m_DisplayJerk.percentile(95);
        m_Result.displayJerkP99Us = m_DisplayJerk.percentile(99);
        m_Result.displayJerkOver2MsPercent = percent(m_JerkOver2Ms, m_DisplayJerk.count);
        m_Result.residenceP95Us = m_Residence.percentile(95);
        m_Result.endToEndP95Us = m_EndToEnd.percentile(95);
        m_Result.meanQueueResidenceUs = double(m_Residence.mean);
        m_Result.meanSourceToDisplayUs = double(m_EndToEnd.mean);
        if (m_ReadyToDisplaySamples)
            m_Result.meanReadyToDisplayUs = double(m_ReadyToDisplayTotalUs) / m_ReadyToDisplaySamples;
        if (m_SteadyReadySamples)
            m_Result.steadyMeanReadyToDisplayUs = double(m_SteadyReadyToDisplayUs) / m_SteadyReadySamples;
        return m_Result;
    }

private:
    RunMetrics m_Result{};
    Observation m_Previous{};
    bool m_HavePrevious = false, m_HaveShownInterval = false;
    DelayCycle m_ExtraCycle, m_SustainedExtraCycle;
    std::array<uint64_t, 4> m_ExtraWindow{};
    size_t m_ExtraCount = 0, m_ExtraIndex = 0;
    int m_PreviousDirection = 0;
    uint64_t m_RecoveryStartUs, m_ToleranceUs = 0, m_BelowRunUs = 0, m_RecoveryRunUs = 0;
    uint64_t m_ShownIntervalUs = 0, m_JerkOver2Ms = 0;
    double m_NetGrowthUs = 0, m_BufferIntegral = 0, m_ExtraIntegral = 0;
    uint64_t m_SteadyDurationUs = 0, m_SteadyMin = UINT64_MAX, m_SteadyMax = 0;
    double m_SteadyIntegral = 0;
    uint64_t m_ReadyToDisplayTotalUs = 0, m_ReadyToDisplaySamples = 0;
    uint64_t m_SteadyReadyToDisplayUs = 0, m_SteadyReadySamples = 0;
    std::optional<uint64_t> m_QualifyingReserveUs;
    uint64_t m_NoDrainUs = 0, m_DrainStartBufferUs = 0;
    VrrReplayMetrics::Distribution m_Buffer, m_EndToEnd, m_Residence, m_Hitch, m_DisplayJerk;
};

uint64_t dropped(const Counts& counts)
{
    return counts.queueDrops + counts.replacements + counts.reconnectDrops + counts.decodeDrops + counts.presenterDrops;
}

} // namespace

double RunMetrics::timeAtTargetPercent() const
{
    return percent(atTargetUs, atTargetUs + belowTargetUs);
}

double RunMetrics::coveragePercent() const
{
    return percent(atTargetUs + belowTargetUs, elapsedUs - calibrationUs);
}

double RunMetrics::steadyCoveragePercent() const { return percent(steadyScoredUs, elapsedUs / 4); }
double RunMetrics::steadyTimeAtTargetPercent() const { return percent(steadyAtTargetUs, steadyScoredUs); }
double RunMetrics::steadyTimeAboveTargetPercent() const { return percent(steadyAboveTargetUs, steadyScoredUs); }

std::vector<Workload> streamWorkloads()
{
    auto minor = phase(90);
    minor.jitterUs = 200;
    auto jitter = phase(30);
    jitter.jitterUs = 3000;
    auto alternating = phase(30);
    alternating.alternatingFirstUs = 11000;
    alternating.alternatingSecondUs = 7000;
    auto pauses = phase(30);
    pauses.hostPauseUs = 50000;
    pauses.disturbancePeriodUs = 2 * Second;
    auto freeze = phase(2);
    freeze.hostPauseUs = 100000;
    auto drift = phase(60);
    drift.clockDriftPpm = 250;
    auto overload = phase(6);
    overload.decodeUs = 12500; // single decoder cannot sustain 120 FPS
    auto encoderWarmup = phase(45);
    encoderWarmup.encodeUs = 2000;
    auto encoderBacklog = phase(30);
    encoderBacklog.encodeUs = encoderWarmup.encodeUs;
    encoderBacklog.encoderStallUs = 40000;
    encoderBacklog.disturbancePeriodUs = Second;
    // Let the late intervals age out of Balanced's 120-second score history.
    auto encoderRecovery = phase(150);
    encoderRecovery.encodeUs = encoderWarmup.encodeUs;
    auto encoder = workload("encoder-backlog", {encoderWarmup, encoderBacklog, encoderRecovery}, 75);
    encoder.scenario.dxgiWaitable = true;
    auto periodic = phase(30);
    periodic.networkSpikeUs = 1000;
    periodic.disturbancePeriodUs = 125000;
    auto sparseDelivery = phase(120, 118);
    sparseDelivery.networkSpikeUs = 3000;
    sparseDelivery.disturbancePeriodUs = Second;
    auto surplus = workload("surplus-drain", {sparseDelivery});
    surplus.scenario.displayHz = 120;
    auto jump = phase(20);
    jump.rtpJumpUs = -3000000;
    // Long connections also exercise the fallback median of settled delay.
    auto reconnect = phase(110);
    reconnect.reconnect = true;
    auto ceiling = workload("display-ceiling", {phase(30, 144)});
    ceiling.scenario.streamHz = ceiling.scenario.displayHz = 144;

    auto startup = phase(12);
    startup.jitterUs = 4000;
    startup.networkSpikeUs = 1800;
    startup.disturbancePeriodUs = 11 * 8333;
    auto settled = phase(78);
    settled.jitterUs = 3200;
    auto cluster = settled;
    cluster.durationUs = 3 * Second;
    cluster.networkSpikeUs = 2500;
    cluster.disturbancePeriodUs = 9 * 8333;
    auto quieter = settled;
    quieter.durationUs = 14750000;
    auto persistent = cluster;
    persistent.durationUs = 40 * Second;
    auto forecast = workload("ceiling-forecast", {startup, settled, cluster, quieter, persistent});
    forecast.scenario.displayHz = 120;
    forecast.scenario.dxgiWaitable = true;

    // Consecutive host RTP gaps from the reported variable-rate stream.
    // Replay only the source cadence; delivery and display run independently.
    auto varying = phase(18, 68);
    varying.jitterUs = 1000;
    varying.sourceIntervalsUs = {
        13800, 14933, 15322, 14189, 15045, 21200, 13666, 19580, 15972, 18693,
        16700, 13089, 16989, 13178, 16944, 14700, 16067, 14222, 15956, 15578,
        14744, 14322, 16023, 14777, 15834, 15055, 16578, 14822, 16434, 15077,
        16523, 16466, 15434, 16022, 16844, 14000, 16567, 14389, 17322, 14300,
        15256, 14377, 16111, 13634, 16078, 13722, 16066, 14912, 14355, 14922,
        15334, 14255, 15422, 14312, 15800, 12866, 15445, 14333, 16100, 12278,
        16100, 13111, 15067, 13511, 15911, 13200, 14066, 13378, 14278, 14400,
        15078, 15189, 14089, 14311, 14911, 12878, 14955, 14856, 12644, 14111,
    };
    auto judder = workload("variable-source-judder", {varying});
    judder.scenario.displayHz = 120;

    // Consecutive RTP gaps into frames 5360-5439 of the 70 FPS gameplay capture.
    // Transport jitter uses the simulator's clock independently of the source.
    auto game = phase(18, 70);
    game.jitterUs = 1800;
    game.sourceIntervalsUs = {
        14556, 13033, 15767, 13522, 11533, 15611, 13444, 14600, 15278, 14378,
        13900, 13389, 14689, 14878, 12911, 12044, 14233, 13411, 11800, 13422,
        12400, 13422, 15589, 14233, 13489, 12611, 13122, 12778, 14411, 13867,
        13922, 13311, 14644, 11944, 15522, 11267, 14978, 17100, 11233, 19389,
        18322, 14978, 13656, 14989, 16444, 12356, 13256, 16400, 12678, 12756,
        14000, 13089, 14489, 11744, 13367, 12767, 17389, 13822, 24000, 11722,
        13178, 14811, 13967, 10733, 16211, 13144, 12056, 13044, 14756, 14667,
        12933, 12889, 14667, 11989, 16911, 11244, 13856, 14900, 12633, 12944,
    };
    auto unevenGame = workload("uneven-game-cadence", {game});
    unevenGame.scenario.displayHz = 120;

    // Frame 14208 onward in the 120 -> 60 FPS capture. The source moves
    // independently of the injected transport jitter in the five-second burst.
    auto uneven = phase(5, 60);
    uneven.sourceIntervalsUs = {
        17134, 17166, 17178, 17045, 17122, 16500, 17367, 17333, 17355, 17612,
        17388, 16945, 17155, 22356, 12567, 17155, 16911, 17878, 17067, 17433,
        17000, 17645, 16977, 17856, 17033, 17156, 17466, 16912, 17133, 18333,
        16367, 17100, 16822, 18133, 20256, 12844, 20578, 13345, 20566, 12811,
    };
    uneven.jitterUs = 2500;
    uneven.networkSpikeUs = 3000;
    uneven.disturbancePeriodUs = 200000;
    auto rateDrop = workload("rate-drop-hitches", {phase(40), phase(30, 60), uneven, phase(30, 60)});
    rateDrop.scenario.displayHz = 120;

    // Exercise ten mixed virtual minutes on every fast run.
    auto mixedSpike = phase(30);
    mixedSpike.networkSpikeUs = 8000;
    auto mixedFreeze = phase(30);
    mixedFreeze.hostPauseUs = 50000;
    auto mixedJitter = phase(60);
    mixedJitter.jitterUs = 3000;
    return {
        workload("stable", {phase(90)}),
        workload("minor-jitter", {minor}),
        workload("sustained-jitter", {phase(75), jitter}),
        workload("alternating-host", {alternating}),
        workload("periodic-host-pauses", {phase(3), pauses}),
        workload("isolated-freeze", {phase(5), freeze, phase(30)}, 7),
        workload("rate-round-trip", {phase(10), phase(12, 60), phase(30)}, 22),
        workload("source-clock-drift", {drift}),
        workload("decoder-overload", {phase(4), overload, phase(145)}, 10),
        encoder,
        workload("jitter-clears", {phase(75), jitter, phase(150)}, 105),
        workload("periodic-disturbance", {periodic}),
        surplus,
        workload("clock-jump", {phase(8), jump}, 8),
        workload("long-stable", {phase(1800)}, 0, true),
        workload("reconnect", {phase(110), reconnect, reconnect}),
        ceiling,
        forecast,
        judder,
        unevenGame,
        rateDrop,
        workload("mixed", {phase(30), mixedFreeze, mixedSpike, phase(30, 60),
            phase(30), alternating, phase(60), mixedJitter, phase(300)}, 300)
    };
}

RunMetrics measure(const Workload& workload, const Options& options, std::optional<uint64_t> qualifyingReserveUs)
{
    Accumulator accumulator(workload, options, qualifyingReserveUs);
    const auto present = [](void* context, const Observation& frame) {
        static_cast<Accumulator*>(context)->present(frame);
    };
    const auto counts = simulate(workload.scenario, options, present, &accumulator);
    return accumulator.finish(counts);
}

OracleResult fixedBufferOracle(const Workload& workload, const Options& options,
                               const std::vector<uint64_t>& candidates, double requiredTimeAtTargetPercent,
                               OracleWindow window)
{
    if (!std::isfinite(requiredTimeAtTargetPercent) || requiredTimeAtTargetPercent < 0 ||
        requiredTimeAtTargetPercent > 100) throw std::invalid_argument("Invalid oracle time-at-target percentage");
    OracleResult result;
    for (auto candidate : candidates) {
        if (candidate > uint64_t(std::numeric_limits<int64_t>::max()))
            throw std::invalid_argument("Oracle buffer exceeds supported range");
        Options fixed = options;
        fixed.fixedBufferUs = int64_t(candidate);
        OraclePoint point;
        point.bufferUs = candidate;
        RunMetrics metrics;
        try {
            metrics = measure(workload, fixed);
        } catch (const FixedBufferUnavailable&) {
            point.feasible = false;
            result.points.push_back(point);
            continue;
        }
        point.dropped = dropped(metrics.counts);
        const bool steady = window == OracleWindow::FinalQuarter;
        point.timeAtTargetPercent = steady ? metrics.steadyTimeAtTargetPercent() : metrics.timeAtTargetPercent();
        point.coveragePercent = steady ? metrics.steadyCoveragePercent() : metrics.coveragePercent();
        point.steadyMeanReadyToDisplayUs = metrics.steadyMeanReadyToDisplayUs;
        if (metrics.finalScoreAvailable) point.finalScorePercent = metrics.finalScorePercent;
        point.qualifies = metrics.atTargetUs + metrics.belowTargetUs > 0 &&
            metrics.calibrationUs < metrics.elapsedUs && point.coveragePercent >= (steady ? 99 : 90) &&
            point.dropped == 0 && point.timeAtTargetPercent >= requiredTimeAtTargetPercent &&
            metrics.timestampViolations == 0 && metrics.frameOrderViolations == 0 &&
            metrics.counts.produced == metrics.counts.presented;
        if (point.qualifies && (!result.found || candidate < result.minimumUs)) {
            result.minimumUs = candidate;
            result.found = true;
        }
        result.points.push_back(point);
    }
    return result;
}

std::vector<std::string> checkSurplusDrain(const RunMetrics& adaptive, const OracleResult& reference)
{
    std::vector<std::string> failures;
    if (adaptive.steadyCoveragePercent() < 99 || adaptive.steadyTimeAtTargetPercent() < 99 ||
        dropped(adaptive.counts) != 0) {
        failures.emplace_back("draining reserve must preserve the target for 99% of scored steady time, "
                              "with 99% steady coverage and zero drops");
    }
    if (!reference.found) {
        failures.emplace_back("no fixed reserve met the target, coverage and drop bounds on the same stream");
        return failures;
    }
    // Allow one 250 us reserve step for adaptation. Compare held latency as
    // well as requested protection: a pacer can lower its reported reserve
    // yet keep the decoded frame waiting behind a presentation floor.
    const auto& point = *std::find_if(reference.points.begin(), reference.points.end(),
        [&](const OraclePoint& p) { return p.qualifies && p.bufferUs == reference.minimumUs; });
    // Twenty seconds allows the longest 10 s profile hold plus a 250 us
    // net decrease at its 50 us/s release rate.
    if (adaptive.longestSteadySurplusWithoutDrainUs > 20 * Second ||
        adaptive.steadyMeanBufferUs > point.bufferUs + DrainStepUs ||
        adaptive.steadyMeanReadyToDisplayUs > point.steadyMeanReadyToDisplayUs + DrainStepUs) {
        std::ostringstream message;
        message << std::fixed;
        message.precision(2);
        message << "target " << adaptive.targetPercent << "%, above goal for "
                << adaptive.steadyTimeAboveTargetPercent() << "% of scored steady time, longest surplus without a 250 us drain "
                << double(adaptive.longestSteadySurplusWithoutDrainUs) / Second << " s (limit 20 s); mean reserve "
                << std::lround(adaptive.steadyMeanBufferUs) << " us vs qualifying " << point.bufferUs
                << " us, decode-ready to display " << std::lround(adaptive.steadyMeanReadyToDisplayUs)
                << " us vs " << std::lround(point.steadyMeanReadyToDisplayUs) << " us (allowance 250 us)";
        failures.push_back(message.str());
    }
    return failures;
}

std::vector<std::string> checkWorkload(const Workload& workload, const Options& options,
                                       const RunMetrics& m, bool checkDefaults)
{
    std::vector<std::string> failures;
    auto require = [&](bool ok, const char* message) {
        if (!ok) failures.emplace_back(message);
    };
    const auto drops = dropped(m.counts);
    require(m.counts.presented > 0 && m.bufferSamples == m.counts.presented, "missing presented-frame observations");
    require(m.counts.produced == m.counts.presented + drops, "produced frames were not accounted for exactly once");
    require(!m.timestampViolations && !m.frameOrderViolations, "frame identity or modeled event ordering regressed");
    require(m.calibrationUs + m.unavailableUs + m.atTargetUs + m.belowTargetUs == m.elapsedUs,
            "score coverage does not partition elapsed virtual time");
    require(m.longestBelowTargetUs <= m.belowTargetUs && m.minimumBufferTimeUs <= m.bufferObservedUs,
            "inconsistent time-weighted metrics");
    require(m.clientLossUs >= 0 && m.clientLossUs <= m.clientEligibleUs,
            "client-attributed loss exceeds eligible time");
    if (options.fixedBufferUs >= 0) {
        const auto fixed = uint64_t(options.fixedBufferUs);
        require(m.peakBufferUs == fixed && m.totalBufferMovementUs == 0 && m.finalBufferUs == fixed,
                "fixed reserve changed during simulation");
    }
    if (!checkDefaults) return failures;
    const auto& name = workload.scenario.name;
    if (name == "rate-drop-hitches") {
        require(m.bufferP95Us <= 2000 && m.timeAtTargetPercent() >= 99 && m.coveragePercent() >= 98,
                "the 120 to 60 FPS transition and irregular source intervals bought excess reserve or lost pacing coverage");

        // Give the same settled stream 2 ms of available reserve. Vary only
        // the size of the late delivery. A short successor is not another hitch.
        struct Outcomes {
            uint64_t peak = 0, hitches = 0, previousSubmitUs = 0, previousFrame = 0;
        };
        const auto trial = [&](uint64_t spikeUs) {
            auto scenario = workload.scenario;
            scenario.phases.resize(3);
            auto& late = scenario.phases.back();
            late = phase(18, 60);
            late.networkSpikeUs = spikeUs;
            late.disturbancePeriodUs = 125000;
            const BufferCapTrial cap{1, 2000};
            Outcomes outcome;
            const auto observe = [](void* context, const Observation& f) {
                auto& o = *static_cast<Outcomes*>(context);
                if (f.sourceUs >= 70 * Second) {
                    o.peak = std::max(o.peak, std::max(f.requestedBufferUs, f.bufferUs));
                    if (f.eligibleInterval && f.frame == o.previousFrame + 1 &&
                        f.submittedUs > o.previousSubmitUs + f.intendedIntervalUs + 500)
                        ++o.hitches;
                }
                o.previousFrame = f.frame;
                o.previousSubmitUs = f.submittedUs;
            };
            auto strict = options;
            strict.targetHundredths = 9999;
            simulate(scenario, strict, observe, &outcome, &cap);
            return outcome;
        };
        const auto six = trial(6000), ten = trial(10000), fixable = trial(1800);
        require(six.hitches > 100 && six.hitches == ten.hitches && !six.peak && !ten.peak,
                "two over-cap delivery sizes must count the same late frames and buy zero reserve");
        if (!fixable.peak || fixable.peak > 2000 || fixable.hitches >= six.hitches) {
            std::ostringstream msg;
            msg << "recoverable delivery reached " << fixable.peak << " us, with " << fixable.hitches
                << " late frames compared with " << six.hitches << " beyond the allowance";
            failures.push_back(msg.str());
        }
        return failures;
    }
    if (name == "uneven-game-cadence") {
        require(m.displayJerkOver2MsPercent <= 9.5 && m.displayJerkP99Us <= 6200 && !drops,
                "irregular host gaps caused excessive changes in presented intervals");
        require(m.sustainedExtraDelayCycles == 0 && m.meanBufferUs <= 3100 &&
                m.meanReadyToDisplayUs <= 3100 && m.timeAtTargetPercent() >= 99,
                "smoothing oscillated or exceeded its buffer, latency or pacing bounds");
    }
    if (name == "variable-source-judder") {
        const auto patternSize = workload.scenario.phases.front().sourceIntervalsUs.size();
        if (options.latencyMode == 2 && options.smooth && options.canLatch &&
                options.fixedBufferUs < 0 && (!options.targetHundredths || options.targetHundredths == 9900) && patternSize) {
            const auto repetitions = (m.counts.produced + patternSize - 1) / patternSize;
            require(m.sustainedExtraDelayCycles == 0 && m.extraDelayCycles <= repetitions,
                    "smoothing repeatedly built and drained delay across the variable source intervals");
            require(m.displayJerkOver2MsPercent <= 5.5 && !drops &&
                    m.coveragePercent() >= 99 && m.timeAtTargetPercent() >= 99,
                    "variable cadence exceeded 5.5% presented interval changes over 2 ms or lost scoring coverage");
            require(m.meanBufferUs <= 1250 && m.meanBufferUs + m.meanExtraDelayUs <= 2500 &&
                    m.meanReadyToDisplayUs <= 2200,
                    "smoothing hid the oscillation by retaining input buffer, extra delay or display wait");
        }
        return failures;
    }
    if (name == "mixed") {
        require(m.coveragePercent() >= 90 && m.recovered && m.finalScoreAvailable &&
                m.finalScorePercent >= m.targetPercent && m.steadyMeanBufferUs <= m.finalMinBufferUs + 250 &&
                m.steadyBufferRangeUs <= 250,
                "mixed workload failed to sustain coverage and settle near the floor during the clean ending");
        if (drops && workload.scenario.streamHz == workload.scenario.displayHz && options.canLatch) {
            // A late frame at the refresh ceiling can leave every successor a
            // refresh behind. Allow one replacement only with a measured
            // display-latency saving over presenting the same frame.
            auto referenceOptions = options;
            referenceOptions.deferReplacements = true;
            const auto reference = measure(workload, referenceOptions);
            require(drops == 1 && m.counts.replacements == 1 && !dropped(reference.counts) &&
                    m.timeAtTargetPercent() >= 99 && m.coveragePercent() >= 99 &&
                    m.meanReadyToDisplayUs + 250 < reference.meanReadyToDisplayUs,
                    "a ceiling replacement must save at least 250 us of mean display wait against a drop-free run");
        }
        else require(!drops, "mixed workload dropped a frame with display headroom");
        return failures;
    }
    // Production-quality bounds for the catalog's adaptive Balanced run.
    if (options.fixedBufferUs >= 0 || options.latencyMode != 1 || options.targetHundredths ||
        !options.smooth || !options.canLatch) return failures;
    require(m.atTargetUs + m.belowTargetUs > 0, "no qualified timing observations");
    if (name != "decoder-overload" && name != "encoder-backlog" && name != "reconnect" && name != "display-ceiling")
        require(drops == 0, "frames dropped on a workload that fits both decoder and display capacity");
    if (name == "stable" || name == "long-stable" || name == "minor-jitter" || name == "source-clock-drift") {
        require(m.timeAtTargetPercent() >= 98 && m.coveragePercent() >= 98,
                "clean source failed to sustain its timing target");
        require(m.finalBufferUs <= m.finalMinBufferUs + 250, "clean input retained more than 250 us of excess reserve");
        require(m.displayJerkP99Us <= 500, "clean source added over 500 us of modeled p99 presentation jerk");
        if (name == "long-stable")
            require(m.bufferP95Us <= m.finalMinBufferUs + 250, "long stable run accumulated excess reserve");
    }
    if (name == "stable") {
        // Same producer, presentation model and production target, tested at
        // the profile floor and a larger allowed reserve.
        const auto oracle = fixedBufferOracle(workload, options, {m.finalMinBufferUs + 1000, m.finalMinBufferUs});
        require(oracle.found && m.steadyMeanBufferUs <= double(oracle.minimumUs) + 250 &&
                m.steadyBufferRangeUs <= 250,
                "settled adaptive reserve exceeded the stable workload's fixed-buffer reference by 250 us or oscillated");

        Options relaxed = options;
        relaxed.targetHundredths = 9500;
        const auto clean = measure(workload, relaxed);
        auto zero = relaxed;
        zero.fixedBufferUs = 0;
        const auto immediate = measure(workload, zero);
        require(clean.meanReadyToDisplayUs <= immediate.meanReadyToDisplayUs + 500 &&
                clean.meanSourceToDisplayUs <= m.meanSourceToDisplayUs + 50 &&
                clean.timeAtTargetPercent() >= 99 && clean.coveragePercent() >= 99 &&
                clean.displayJerkP99Us <= 50 && !clean.bufferReversals && !dropped(clean.counts),
                "95% on clean input must relieve presentation latency and preserve clean cadence");

        // At 240 FPS some decoded frames wait behind the current presentation.
        // Time from decode readiness to display also counts time held by the
        // pacer after dequeue. Check both sides of that ownership boundary.
        Workload queued = workload;
        queued.scenario.streamHz = queued.scenario.phases.front().hostHz = 240;
        queued.scenario.displayHz = 280;
        const auto strictQueue = measure(queued, options);
        const auto relaxedQueue = measure(queued, relaxed);
        const auto zeroQueue = measure(queued, zero);
        require(relaxedQueue.meanQueueResidenceUs <= zeroQueue.meanQueueResidenceUs + 50 &&
                relaxedQueue.meanReadyToDisplayUs <= strictQueue.meanReadyToDisplayUs + 50 &&
                relaxedQueue.meanReadyToDisplayUs <= zeroQueue.meanReadyToDisplayUs + 250 &&
                relaxedQueue.timeAtTargetPercent() >= 99 && relaxedQueue.coveragePercent() >= 99 &&
                relaxedQueue.displayJerkP99Us <= 50 && !dropped(relaxedQueue.counts),
                "95% must drain the decoded queue on a stream with measured standing wait");
    }
    if (name == "surplus-drain") {
        // The stream requests 120 FPS. The 118 FPS source leaves enough
        // display headroom for releasing reserve to advance actual flips.
        auto heavier = workload;
        auto& sparse = heavier.scenario.phases.front();
        sparse.durationUs = 240 * Second;
        sparse.networkSpikeUs = 5000;
        sparse.disturbancePeriodUs = Second / 2;
        Options low = options;
        low.latencyMode = 2;
        low.targetHundredths = 9900;
        const auto reference = fixedBufferOracle(heavier, low, {2000, 2250, 2500, 3000, 3500},
                                                 99, OracleWindow::FinalQuarter);
        const auto sparseRun = measure(heavier, low, reference.minimumUs);
        const auto drainFailures = checkSurplusDrain(sparseRun, reference);
        failures.insert(failures.end(), drainFailures.begin(), drainFailures.end());
        require(sparseRun.steadyBufferRangeUs <= DrainStepUs,
                "5 ms delivery spikes must settle without repeated reserve purchases");

        for (const auto& [target, reserve] : {std::pair<int, uint64_t>{9500, 0}, {9830, 500}}) {
            auto submillisecond = low;
            submillisecond.targetHundredths = target;
            const auto affordable = fixedBufferOracle(heavier, submillisecond, {0, 250, 500, 750, 1000},
                                                      99, OracleWindow::FinalQuarter);
            const auto adaptive = measure(heavier, submillisecond, affordable.minimumUs);
            const auto issues = checkSurplusDrain(adaptive, affordable);
            failures.insert(failures.end(), issues.begin(), issues.end());
            require(affordable.found && affordable.minimumUs == reserve && adaptive.finalBufferUs == reserve &&
                    adaptive.steadyBufferRangeUs == 0 && adaptive.finalScorePercent >= adaptive.targetPercent,
                    "the adaptive reserve must settle at the affordable sub-millisecond goal");
        }

        // The previously affordable reserve must lose approval when the same
        // delay occurs four times as often, even with the clean score history.
        auto frequent = sparse;
        frequent.durationUs = 90 * Second;
        frequent.disturbancePeriodUs = Second / 8;
        heavier.scenario.phases.push_back(frequent);
        const auto frequentRun = measure(heavier, low);
        require(frequentRun.finalBufferUs > sparseRun.finalBufferUs &&
                frequentRun.steadyTimeAtTargetPercent() >= 99 && frequentRun.steadyCoveragePercent() >= 99 &&
                !dropped(frequentRun.counts),
                "more frequent delivery spikes must requalify protection and sustain the selected 99% target");

        // Use only the frequent phase so that the trailing score reflects
        // this delivery pattern. A fixed trial one step lower must miss each
        // goal, and the adaptive run must settle near the passing trial.
        frequent.durationUs = 240 * Second;
        heavier.scenario.phases = {frequent};
        for (const auto& [target, reserve] : {std::pair<int, uint64_t>{9500, 1500},
                                             {9700, 2750}, {9950, 4250}}) {
            low.targetHundredths = target;
            const auto goalReference = fixedBufferOracle(heavier, low, {reserve - DrainStepUs, reserve},
                                                         99, OracleWindow::FinalQuarter);
            require(goalReference.found && goalReference.minimumUs == reserve,
                    "adjacent fixed trials must bracket the selected pacing target");
            const auto run = measure(heavier, low, goalReference.minimumUs);
            const auto goalFailures = checkSurplusDrain(run, goalReference);
            failures.insert(failures.end(), goalFailures.begin(), goalFailures.end());
            require(run.steadyBufferRangeUs <= DrainStepUs && run.finalScoreAvailable &&
                    run.finalScorePercent >= run.targetPercent,
                    "the selected pacing goal must hold after the reserve settles");
        }
    }
    if (name == "sustained-jitter") {
        require(m.timeAtTargetPercent() >= 95 && m.coveragePercent() >= 95,
                "sustained jitter fell short of the timing target for more than 5% of qualified time or lost coverage");
        require(m.finalBufferUs > m.finalMinBufferUs + 250 && m.finalBufferUs <= m.finalMinBufferUs + 4000,
                "sustained 3 ms delivery jitter failed to earn bounded protection");

        // A long clean prefix masks the onset of jitter in the trailing score.
        // The fixed trials establish the local cost of meeting the stricter
        // goal on this workload, including the production queue and drop rules.
        Options tight = options;
        tight.targetHundredths = 9990;
        const auto adaptive = measure(workload, tight);
        const auto oracle = fixedBufferOracle(workload, tight, {1500, 2000, 2500, 3000, 3500});
        require(adaptive.timeAtTargetPercent() >= 95 && adaptive.coveragePercent() >= 98 &&
                adaptive.longestBelowTargetUs <= 3 * Second && !dropped(adaptive.counts),
                "persistent delivery jitter left the 99.90% target for too long or shed frames");
        require(oracle.found && adaptive.meanBufferUs <= double(oracle.minimumUs) + 500 &&
                adaptive.bufferReversalsPerMinute <= 3,
                "early jitter protection exceeded the fixed reference by 500 us or repeatedly reversed");

        Workload transient = workload;
        Phase burst{};
        burst.durationUs = 450000;
        burst.jitterUs = 5000;
        transient.scenario.seed = 13002;
        transient.scenario.phases = {phase(70), burst, phase(35)};
        const auto cleared = measure(transient, tight);
        if (cleared.steadyMeanBufferUs > cleared.finalMinBufferUs + 250 ||
                cleared.steadyBufferRangeUs > 250 || dropped(cleared.counts)) {
            std::ostringstream message;
            message << "brief delivery burst retained " << std::lround(cleared.steadyMeanBufferUs)
                    << " us mean reserve, range " << cleared.steadyBufferRangeUs
                    << " us, drops " << dropped(cleared.counts);
            failures.push_back(message.str());
        }
    }
    if (name == "jitter-clears") {
        require(m.bufferAtRecoveryStartUs > m.finalMinBufferUs + 250, "disturbance did not establish a reserve");
        require(m.recovered && m.finalBufferUs <= m.finalMinBufferUs + 250,
                "clean phase failed to restore the timing target and release the reserve");
        Options relaxed = options;
        relaxed.targetHundredths = 9500;
        const auto recovered = measure(workload, relaxed);
        auto zero = relaxed;
        zero.fixedBufferUs = 0;
        const auto immediate = measure(workload, zero);
        require(recovered.finalBufferUs <= recovered.finalMinBufferUs + 250 &&
                recovered.steadyMeanReadyToDisplayUs <= immediate.steadyMeanReadyToDisplayUs + 250 &&
                recovered.meanSourceToDisplayUs <= m.meanSourceToDisplayUs + 50 &&
                recovered.timeAtTargetPercent() >= 98 && recovered.coveragePercent() >= 98 &&
                recovered.bufferReversalsPerMinute <= 3 && !dropped(recovered.counts),
                "95% must meet goal and reclaim presentation latency as delivery clears");

        // Keep jitter running so the clean phases cannot mask its pacing cost.
        auto sustained = workload;
        auto jitter = workload.scenario.phases[1];
        jitter.durationUs = 120 * Second;
        sustained.scenario.phases = {jitter};
        sustained.recoveryStartUs = 0;
        const auto reference = fixedBufferOracle(sustained, relaxed, {0, 250, 500, 750, 1000},
                                                 99, OracleWindow::FinalQuarter);
        const auto duringJitter = measure(sustained, relaxed, reference.minimumUs);
        const auto jitterFailures = checkSurplusDrain(duringJitter, reference);
        failures.insert(failures.end(), jitterFailures.begin(), jitterFailures.end());
    }
    if (name == "rate-round-trip" || name == "isolated-freeze" || name == "clock-jump")
        require(m.recovered && m.recoveryUs < 10 * Second, "timing target did not recover after the transition");
    if (name == "decoder-overload") {
        require(drops > 0, "serial decode overload never exhausted its bounded input queue");
        require(m.clientLossUs > 0 && m.recovered && m.finalBufferUs <= m.finalMinBufferUs + 250,
                "decode overload failed to record client loss and recover by the end of the clean history");
    }
    if (name == "encoder-backlog") {
        require(m.counts.maxCaptureToSendUs == 42000 && m.counts.compressedHostSendIntervals > 0,
                "steady capture did not produce encoded bursts after the injected encoder delay");
        require(m.counts.decodeDrops == 0 && m.recovered && m.finalBufferUs <= m.finalMinBufferUs + 250,
                "encoder bursts overflowed the compressed queue or failed to recover after the encoder cleared");
        require(m.counts.maxPendingPresents == DxgiWaitable::MaximumFrameLatency &&
                m.counts.presenterAdmissionWaitUs > 0 && m.counts.presenterDrops == 0,
                "encoder test did not exercise waitable DXGI backpressure at the modeled queue limit");
    }
    if (name == "reconnect") {
        require(m.sessionsObserved == 3 && m.calibrationUs > 0 && m.finalScoreAvailable,
                "three connections failed to return valid pacing feedback");
        require(m.maxReconnectStartBufferUs + 1000 <= m.firstBufferUs &&
                m.finalBufferUs <= m.finalMinBufferUs + 250,
                "reconnect failed to use the settled delay from the prior session");

        auto quick = workload;
        quick.scenario.streamHz = quick.scenario.displayHz = 120;
        quick.scenario.dxgiWaitable = true;
        for (auto& span : quick.scenario.phases) {
            span.hostHz = 120;
            span.durationUs = 17 * Second;
        }
        Options low = options;
        low.latencyMode = 2;
        low.targetHundredths = 9900;
        const auto shortRun = measure(quick, low);
        require(shortRun.sessionsObserved == 3 && shortRun.firstBufferUs > 1000 &&
                shortRun.maxReconnectStartBufferUs == 0 && shortRun.timeAtTargetPercent() >= 99,
                "short clean 120/120 sessions must reopen at their measured zero reserve");

        quick.scenario.phases[0].durationUs = 2 * Second;
        quick.scenario.phases.resize(2);
        const auto unfinished = measure(quick, low);
        require(unfinished.sessionsObserved == 2 && unfinished.maxReconnectStartBufferUs == unfinished.firstBufferUs,
                "a reconnect before reserve qualification must retain cold-start behavior");

        for (auto& span : quick.scenario.phases) {
            span.hostHz = 118;
            span.durationUs = 75 * Second;
            span.networkSpikeUs = 5000;
            span.disturbancePeriodUs = 125000;
        }
        const auto noisy = measure(quick, low);
        require(noisy.sessionsObserved == 2 && noisy.maxReconnectStartBufferUs > noisy.firstBufferUs &&
                noisy.maxReconnectStartBufferUs == noisy.finalBufferUs && noisy.finalScorePercent >= 99,
                "a short connection must also retain a qualified nonzero reserve for recurring spikes");
    }
    if (name == "alternating-host" || name == "periodic-host-pauses")
        require(m.bufferGrowthUsPerMinute <= 250, "host-only variation ratcheted up the buffer");
    if (name == "alternating-host")
        require(m.displayJerkP99Us <= 1000, "Reduce judder left over 1 ms p99 jerk on the alternating source");
    if (name == "periodic-host-pauses" || name == "isolated-freeze")
        require(m.worstHitchUs <= 500 && m.clientLossUs < 500,
                "source pauses introduced client timing loss or stretched modeled presentation beyond source cadence");
    if (name == "periodic-disturbance")
        require(m.bufferReversalsPerMinute <= 30, "1 ms periodic disturbance oscillated the applied reserve");
    if (name == "display-ceiling") {
        require(m.counts.presented >= 0.98 * m.counts.produced, "display ceiling shed over 2% of the frames");
        require(m.displayJerkP99Us <= 1000, "stable display ceiling added over 1 ms p99 jerk");
        Options relaxed = options;
        relaxed.targetHundredths = 9500;
        const auto ceiling = measure(workload, relaxed);
        require(dropped(ceiling.counts) <= drops && ceiling.displayJerkP99Us <= 250 &&
                ceiling.timeAtTargetPercent() >= 99 && ceiling.coveragePercent() >= 99,
                "relaxing the goal at the physical display ceiling introduced skips or cadence errors");

        // Require a real advance at the modeled flip, even though the waitable
        // queue can keep displaying late after the applied reserve reaches zero.
        Options low = options;
        low.latencyMode = 2;
        low.targetHundredths = 9900;
        auto matched = workload;
        matched.scenario.streamHz = matched.scenario.displayHz = 120;
        matched.scenario.dxgiWaitable = true;
        auto& delivery = matched.scenario.phases.front();
        delivery = phase(60, 120);
        for (uint64_t spikeUs : {0u, 1000u}) {
            delivery.networkSpikeUs = spikeUs;
            delivery.disturbancePeriodUs = spikeUs ? 125000 : 0;
            const auto run = measure(matched, low);
            require(run.meanReadyToDisplayUs < (spikeUs ? 4500 : 4000) && run.meanBufferUs < 500 &&
                    !dropped(run.counts) && run.displayJerkP99Us <= 250 && run.bufferReversalsPerMinute <= 10 &&
                    run.coveragePercent() >= 99 && run.timeAtTargetPercent() >= 99 && run.finalScorePercent >= 99,
                    "120 FPS at 120 Hz must lower display wait while sustaining the 99% goal without skips or oscillation");
        }
        // A full-rate source can alternate intervals while its fitted frame
        // allowance moves by only a few microseconds. After a delivery burst,
        // eight slower source intervals give the flip queue room to catch up.
        auto spikes = phase(25);
        spikes.networkSpikeUs = 5000;
        spikes.disturbancePeriodUs = 11 * 8333;
        auto recovered = phase(16);
        recovered.alternatingFirstUs = 8333 - 650;
        recovered.alternatingSecondUs = 8334 + 650;
        recovered.jitterUs = 750;
        auto dip = phase(1, 100);
        dip.durationUs = 80000;
        auto relocked = recovered;
        relocked.durationUs = 30 * Second;
        matched.scenario.phases = {spikes, recovered, dip, relocked};
        const auto drained = measure(matched, low);
        require(drained.peakBufferUs >= 3500 && drained.steadyMeanBufferUs <= 250 &&
                drained.steadyMeanReadyToDisplayUs < 1500 && drained.steadyTimeAtTargetPercent() >= 99 &&
                drained.steadyCoveragePercent() >= 99 && drained.displayJerkP99Us <= 250 &&
                drained.bufferReversalsPerMinute <= 10 && !dropped(drained.counts),
                "full-refresh recovery must reclaim multi-millisecond reserve and flip latency when delivery improves");

        auto early = matched;
        recovered.durationUs = 5 * Second;
        early.recoveryStartUs = spikes.durationUs;
        early.scenario.phases = {spikes, recovered};
        const auto releasing = measure(early, low);
        require(releasing.bufferAtRecoveryStartUs >= 3500 && releasing.finalBufferUs <= 1000 &&
                releasing.finalScorePercent >= 99 && releasing.bufferReversals <= 3 && !dropped(releasing.counts),
                "a settled high reserve must drain within five seconds of improved full-refresh delivery");
        matched.scenario.phases = {spikes, recovered, dip, relocked};
        const auto earlyRelock = measure(matched, low);
        require(earlyRelock.steadyMeanReadyToDisplayUs < 1500 && earlyRelock.steadyTimeAtTargetPercent() >= 99 &&
                earlyRelock.displayJerkP99Us <= 250 && !dropped(earlyRelock.counts),
                "early release must let a brief source-rate dip repay standing display latency");
        auto reconnect = recovered;
        reconnect.durationUs = Second;
        reconnect.reconnect = true;
        early.scenario.phases.push_back(reconnect);
        const auto provisional = measure(early, low);
        require(provisional.sessionsObserved == 2 && provisional.maxReconnectStartBufferUs == provisional.firstBufferUs,
                "a provisional ceiling release must earn full confirmation before it seeds a reconnect");

        auto alternating = matched;
        auto burst = spikes;
        burst.durationUs = recovered.durationUs = 8 * Second;
        alternating.scenario.phases.clear();
        for (int cycle = 0; cycle < 10; ++cycle) {
            alternating.scenario.phases.push_back(burst);
            alternating.scenario.phases.push_back(recovered);
        }
        const auto recurring = measure(alternating, low);
        require(recurring.bufferReversalsPerMinute <= 8 && recurring.totalBufferMovementUs <= 55000 &&
                recurring.finalScorePercent >= 99 && recurring.displayJerkP99Us <= 250 && !dropped(recurring.counts),
                "repeating eight-second bursts must not cause excess reserve oscillation or dropped frames");

        recovered.durationUs = 60 * Second;
        recovered.jitterUs = 4000;
        matched.scenario.phases = {recovered};
        const auto sustained = measure(matched, low);
        const auto reference = fixedBufferOracle(matched, low, {2500, 2750, 3000}, 99, OracleWindow::FinalQuarter);
        require(reference.found && sustained.steadyMeanBufferUs <= reference.minimumUs + DrainStepUs &&
                sustained.steadyTimeAtTargetPercent() >= 99 && sustained.steadyCoveragePercent() >= 99 &&
                sustained.bufferReversalsPerMinute <= 10 && !dropped(sustained.counts),
                "sustained full-refresh jitter must stay within one trial step of a fixed reserve that meets goal");
    }
    return failures;
}

namespace {
ForecastWindow forecastWindow(const std::vector<Observation>& frames, uint64_t startUs, uint64_t endUs,
                              double goal)
{
    ForecastWindow window;
    VrrReplayMetrics::Distribution jerk;
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        if (f.sourceUs < startUs || f.sourceUs >= endUs) continue;
        // Both branches use the same source identities and source-time window.
        // Submission occupancy and flip spacing are allowed to diverge.
        const auto nextUs = i + 1 < frames.size() ? frames[i + 1].sourceUs : endUs;
        const auto span = std::min(endUs, nextUs) - f.sourceUs;
        if (f.scoreAvailable && f.eligibleInterval) {
            window.sampledUs += span;
            if (f.scorePercent >= goal) window.atGoalUs += span;
            window.minimumScore = std::min(window.minimumScore, f.scorePercent);
        }
        ++window.frames;
        window.meanBufferUs += f.bufferUs;
        window.meanReadyToSubmitUs += f.submittedUs - f.readyUs;
        window.meanReadyToDisplayUs += f.shownUs - f.readyUs;
        if (i >= 2 && frames[i - 2].sourceUs >= startUs)
            jerk.add(difference(f.shownUs - frames[i - 1].shownUs, frames[i - 1].shownUs - frames[i - 2].shownUs));
    }
    if (window.frames) {
        window.meanBufferUs /= window.frames;
        window.meanReadyToSubmitUs /= window.frames;
        window.meanReadyToDisplayUs /= window.frames;
    }
    window.displayJerkP99Us = jerk.percentile(99);
    return window;
}
}

ForecastComparison compareCeilingForecast(const Scenario& scenario, const Options& options)
{
    ForecastComparison result;
    const auto policy = VrrTimingOptions{}.resolved(options.latencyMode);
    const double goal = (options.targetHundredths ? options.targetHundredths : policy.targetHundredths) / 100.0;
    const auto& phases = scenario.phases;
    const auto burstStart = phases.at(0).durationUs + phases.at(1).durationUs;
    const auto burstEnd = burstStart + phases.at(2).durationUs;
    const auto sustainedStart = burstEnd + phases.at(3).durationUs;
    const auto end = sustainedStart + phases.at(4).durationUs;
    std::vector<Observation> adaptive, capped;
    const auto collect = [](void* out, const Observation& f) { static_cast<std::vector<Observation>*>(out)->push_back(f); };
    const auto counts = simulate(scenario, options, collect, &adaptive);
    const auto first = std::find_if(adaptive.begin(), adaptive.end(), [&](const auto& f) { return f.sourceUs >= burstStart; });
    if (first == adaptive.end() || !first->scoreAvailable || first->scorePercent < goal || first->bufferUs > result.capUs) {
        result.failures.emplace_back("short burst must begin at goal with a reserve within the trial cap");
        return result;
    }
    const auto purchase = std::find_if(first, adaptive.end(), [&](const auto& f) {
        return f.sourceUs < burstEnd && f.scoreAvailable && f.scorePercent >= goal &&
               f.bufferUs <= result.capUs && f.requestedBufferUs > result.capUs;
    });
    // A future controller can avoid the purchase entirely. It must still meet
    // goal in the burst and respond when the same pressure becomes sustained.
    const auto& decision = purchase == adaptive.end() ? *first : *purchase;
    result.frame = decision.frame;
    result.scoreAtDecision = decision.scorePercent;
    const BufferCapTrial trial{decision.frame, result.capUs};
    const auto alternate = simulate(scenario, options, collect, &capped, &trial);
    if (dropped(counts) || dropped(alternate) || adaptive.size() != capped.size()) {
        result.failures.emplace_back("both branches must present every source frame");
        return result;
    }
    for (size_t i = 0; i < adaptive.size(); ++i) {
        const auto& a = adaptive[i];
        const auto& b = capped[i];
        if (a.frame != b.frame || a.sourceUs != b.sourceUs || a.readyUs != b.readyUs ||
            (a.frame <= trial.frame && (a.submittedUs != b.submittedUs || a.shownUs != b.shownUs || a.bufferUs != b.bufferUs)) ||
            (a.frame < trial.frame && (a.requestedBufferUs != b.requestedBufferUs || a.scorePercent != b.scorePercent)) ||
            (a.frame > trial.frame && b.bufferUs > trial.maximumUs)) {
            result.failures.emplace_back("producer, pre-purchase history, or adaptive trial limit diverged");
            return result;
        }
    }
    result.brief = forecastWindow(adaptive, decision.sourceUs, sustainedStart, goal);
    result.cappedBrief = forecastWindow(capped, decision.sourceUs, sustainedStart, goal);
    result.sustained = forecastWindow(adaptive, sustainedStart, end, goal);
    result.cappedSustained = forecastWindow(capped, sustainedStart, end, goal);
    const auto& live = result.brief;
    const auto& lower = result.cappedBrief;
    if (live.sampledUs != sustainedStart - decision.sourceUs || lower.sampledUs != live.sampledUs ||
        result.sustained.sampledUs != end - sustainedStart || result.cappedSustained.sampledUs != result.sustained.sampledUs)
        result.failures.emplace_back("the brief and sustained comparisons require complete pacing-score coverage");
    if (live.atGoalUs != live.sampledUs || result.sustained.atGoalUs != result.sustained.sampledUs ||
        result.cappedSustained.atGoalUs == result.cappedSustained.sampledUs)
        result.failures.emplace_back("adaptive pacing must meet goal and the smaller reserve must expose sustained pressure");

    const double excess = live.meanBufferUs - lower.meanBufferUs;
    if (lower.atGoalUs == lower.sampledUs && excess > policy.toleranceUs &&
        lower.displayJerkP99Us <= live.displayJerkP99Us + policy.toleranceUs &&
        lower.meanReadyToDisplayUs <= live.meanReadyToDisplayUs + policy.toleranceUs) {
        result.failures.emplace_back("above-goal purchase spent excess reserve while the smaller branch met goal");
    }
    return result;
}

std::vector<std::string> checkGapRecoveryReference()
{
    std::vector<std::string> failures;
    for (int hz : {120, 144}) {
        auto warmup = phase(45, hz);
        warmup.encodeUs = 2000;
        auto disturbed = warmup;
        disturbed.durationUs = 12 * Second;
        disturbed.encoderStallUs = 40000;
        disturbed.disturbancePeriodUs = Second;
        auto recovery = warmup;
        recovery.durationUs = 3 * Second;
        for (uint64_t jitter : {250, 750, 1500}) {
            warmup.jitterUs = disturbed.jitterUs = recovery.jitterUs = jitter;
            const Scenario scenario{"jittered-encoder-recovery", hz, hz, 42, {warmup, disturbed, recovery}, true};
            const auto result = measureGapRecoveryReference(scenario, {});
            if (!result.baselineFramesPreserved || !result.insertedFrames ||
                    result.filledDrops + result.insertedFrames != result.baselineDrops ||
                    result.filled.worstGapUs >= result.baseline.worstGapUs ||
                    result.filled.jerkP99Us > result.baseline.jerkP99Us ||
                    result.filled.jerkOver2Ms > result.baseline.jerkOver2Ms) {
                failures.push_back("ceiling gap reference failed at " + std::to_string(hz) +
                                   " Hz with " + std::to_string(jitter) + " us of jitter");
            }
        }
    }
    return failures;
}

std::vector<std::string> checkMetricAccounting()
{
    std::vector<std::string> failures;
    {
        Accumulator cycles(workload("extra-delay-counter", {phase(1)}), {});
        Observation frame{};
        const auto presentExtra = [&](uint64_t extraUs) {
            ++frame.frame;
            frame.sourceUs = frame.submittedUs = frame.shownUs = frame.frame * 10000;
            frame.extraDelayUs = extraUs;
            cycles.present(frame);
        };
        for (const uint64_t level : {500, 2000, 500, 2000}) {
            for (int i = 0; i < 4; ++i) presentExtra(level);
        }
        ++frame.frame; // The return to low delay belongs to a new sequence.
        for (int i = 0; i < 4; ++i) presentExtra(500);
        presentExtra(2000);
        presentExtra(500);
        const auto result = cycles.finish({});
        if (result.extraDelayCycles != 2 || result.sustainedExtraDelayCycles != 1)
            failures.emplace_back("extra-delay counters lost a completed cycle or bridged a frame gap");
    }
    // Irregular observation periods distinguish elapsed-time accounting from
    // counting frames. One dropped pair breaks the qualified score run.
    auto fixture = workload("metric-fixture", {phase(3)});
    fixture.recoveryStartUs = 1200000;
    Accumulator accumulator(fixture, {});
    Observation f{};
    auto add = [&](uint64_t ms, uint64_t buffer, double score, bool calibrated, bool available, bool eligible) {
        ++f.frame;
        f.session = 1;
        f.submittedUs = ms * 1000;
        f.sourceUs = f.submittedUs - 100;
        f.outputUs = f.submittedUs - 60;
        f.dequeuedUs = f.outputUs + 10;
        f.readyUs = f.submittedUs - 40;
        f.shownUs = f.submittedUs + f.frame;
        f.bufferUs = buffer;
        f.minimumBufferUs = 1000;
        f.scorePercent = score;
        f.calibrated = calibrated;
        f.scoreAvailable = available;
        f.eligibleInterval = eligible;
        accumulator.present(f);
    };
    add(100, 0, 0, false, false, false);
    add(300, 1000, 100, true, true, true);
    f.clientEligibleInterval = true;
    f.intervalErrorUs = 1500;
    f.intendedIntervalUs = 200000;
    add(500, 3000, 90, true, true, true);
    f.clientEligibleInterval = false;
    add(900, 1000, 99.5, true, true, true);
    ++f.frame;
    add(1100, 1000, 100, true, false, false);
    add(1200, 1000, 100, true, true, true);
    add(1700, 1000, 100, true, true, true);
    add(2350, 1000, 99.5, true, true, true);
    add(2900, 1000, 100, true, true, true);
    const auto r = accumulator.finish({});
    if (r.calibrationUs != 300000 || r.unavailableUs != 400000 || r.atTargetUs != 1900000 ||
        r.belowTargetUs != 400000 || r.longestBelowTargetUs != 400000 || !r.recovered || r.recoveryUs != 0 ||
        r.clientEligibleUs != 200000 || std::abs(r.clientLossUs - 1000) > 0.001)
        failures.emplace_back("hand-calculated virtual-time score accounting failed");
    if (r.totalBufferMovementUs != 5000 || r.bufferReversals != 1 || r.minimumBufferTimeUs != 2200000 ||
        std::abs(r.meanBufferUs - 3400000000.0 / 2800000.0) > 0.001)
        failures.emplace_back("hand-calculated time-weighted reserve accounting failed");
    const double meanDisplayWait = 50.0 / 9; // Frame numbers 1..10, excluding the dropped fifth frame.
    if (r.meanQueueResidenceUs != 10 || std::abs(r.meanReadyToDisplayUs - 40 - meanDisplayWait) > 0.001 ||
        std::abs(r.meanSourceToDisplayUs - 100 - meanDisplayWait) > 0.001)
        failures.emplace_back("per-presented-frame latency accounting failed");
    if (r.steadyScoredUs != 650000 || r.steadyAtTargetUs != 650000 || r.steadyAboveTargetUs != 100000 ||
        r.steadyMeanReadyToDisplayUs != 49.5 || std::abs(r.steadyCoveragePercent() - 100.0 * 650 / 750) > 0.001)
        failures.emplace_back("final-quarter coverage, surplus duration or decode-ready latency accounting failed");

    Accumulator drain(workload("drain-accounting", {phase(120)}), {}, 1000);
    Observation sample{};
    sample.calibrated = sample.scoreAvailable = true;
    auto drainSample = [&](unsigned second, uint64_t reserve, double score, bool eligible = true) {
        ++sample.frame;
        sample.submittedUs = sample.shownUs = second * Second;
        sample.bufferUs = reserve;
        sample.scorePercent = score;
        sample.eligibleInterval = eligible;
        drain.present(sample);
    };
    drainSample(89, 2500, 100);
    drainSample(92, 2400, 100);
    drainSample(96, 2250, 100); // 250 us net drain in six measured seconds.
    drainSample(99, 2400, 100);
    drainSample(102, 2250, 100); // Returning to the prior reserve is zero net drain.
    drainSample(105, 2250, 99.5);
    drainSample(109, 2250, 100);
    drainSample(113, 2250, 100, false);
    drainSample(120, 2250, 100);
    if (drain.finish({}).longestSteadySurplusWithoutDrainUs != 9 * Second)
        failures.emplace_back("surplus drain clock failed to account for net steps, equality or missing coverage");

    auto stable = workload("model-fixture", {phase(5)});
    Options fixed;
    fixed.fixedBufferUs = 1000;
    const auto small = measure(stable, fixed);
    fixed.fixedBufferUs = 14000;
    const auto large = measure(stable, fixed);
    if (large.counts.produced != small.counts.produced || large.endToEndP95Us <= small.endToEndP95Us ||
        (large.residenceP95Us <= small.residenceP95Us && dropped(large.counts) == dropped(small.counts)))
        failures.emplace_back("fixed reserve did not feed back into queue residence or drops on identical input");
    const auto oracle = fixedBufferOracle(stable, {}, {1000000, 3000, 1000});
    if (!oracle.found || oracle.minimumUs != 1000 || oracle.points.size() != 3 ||
        oracle.points[0].feasible || !oracle.points[1].qualifies || !oracle.points[2].qualifies)
        failures.emplace_back("fixed reserve sweep did not select the lowest qualifying size");
    const auto drainOracle = fixedBufferOracle(stable, {}, {1000}, 99, OracleWindow::FinalQuarter);
    if (!checkSurplusDrain(small, drainOracle).empty())
        failures.emplace_back("a stream at the qualifying reserve and latency must pass the surplus-drain gate");
    stable.scenario.phases.front().decodeUs = 25000;
    const auto infeasible = fixedBufferOracle(stable, {}, {1000});
    if (infeasible.found || !infeasible.points.front().dropped)
        failures.emplace_back("fixed reserve sweep qualified a workload with dropped frames");

    // One 27 ms encode at 100 FPS queues two following captures. With
    // 2 ms service afterward, send times start at 27, 29, 31, 33 and 42 ms.
    auto encoding = phase(1, 100);
    encoding.encodeUs = 2000;
    encoding.encoderStallUs = 25000;
    const auto burst = workload("encoder-fixture", {encoding});
    bool steadyCapture = true;
    const auto encoded = simulate(burst.scenario, {}, [](void* context, const Observation& frame) {
        if (frame.sourceUs != (frame.frame - 1) * 10000)
            *static_cast<bool*>(context) = false;
    }, &steadyCapture);
    if (!steadyCapture || encoded.produced != 100 || !encoded.presented ||
            encoded.maxCaptureToSendUs != 27000 || encoded.compressedHostSendIntervals != 4)
        failures.emplace_back("encoder backlog changed capture cadence or did not release in serial order");
    return failures;
}

} // namespace VrrSim
