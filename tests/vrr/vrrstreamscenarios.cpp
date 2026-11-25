#include "vrrstreamscenarios.h"
#include "../../app/streaming/video/ffmpeg-renderers/dxgiwaitable.h"

#include "../../app/settings/vrrtimingoptions.h"
#include "vrrreplaymetrics.h"

#include <algorithm>
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
    int m_PreviousDirection = 0;
    uint64_t m_RecoveryStartUs, m_ToleranceUs = 0, m_BelowRunUs = 0, m_RecoveryRunUs = 0;
    uint64_t m_ShownIntervalUs = 0, m_JerkOver2Ms = 0;
    double m_NetGrowthUs = 0, m_BufferIntegral = 0;
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
    // The worker waits 30 s before collecting its first one-second settled
    // delay samples, and persists a median only after 60 samples.
    auto reconnect = phase(110);
    reconnect.reconnect = true;
    auto ceiling = workload("display-ceiling", {phase(30, 144)});
    ceiling.scenario.streamHz = ceiling.scenario.displayHz = 144;

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
        require(clean.meanReadyToDisplayUs + 500 < m.meanReadyToDisplayUs &&
                clean.meanSourceToDisplayUs + 500 < m.meanSourceToDisplayUs &&
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
        require(relaxedQueue.meanQueueResidenceUs + 15 < strictQueue.meanQueueResidenceUs &&
                relaxedQueue.meanReadyToDisplayUs + 200 < strictQueue.meanReadyToDisplayUs &&
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
        require(cleared.steadyMeanBufferUs <= cleared.finalMinBufferUs + 250 &&
                cleared.steadyBufferRangeUs <= 250 && !dropped(cleared.counts),
                "a brief delivery burst bought standing reserve after input became clean");
    }
    if (name == "jitter-clears") {
        require(m.bufferAtRecoveryStartUs > m.finalMinBufferUs + 250, "disturbance did not establish a reserve");
        require(m.recovered && m.finalBufferUs <= m.finalMinBufferUs + 250,
                "clean phase failed to restore the timing target and release the reserve");
        Options relaxed = options;
        relaxed.targetHundredths = 9500;
        const auto recovered = measure(workload, relaxed);
        require(recovered.finalBufferUs <= recovered.finalMinBufferUs + 250 &&
                recovered.meanSourceToDisplayUs + 250 < m.meanSourceToDisplayUs &&
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
    }
    return failures;
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
