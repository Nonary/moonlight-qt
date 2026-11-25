#pragma once

#include "vrrstreamsimulator.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace VrrSim {

struct Workload {
    Scenario scenario;
    // Virtual time of the end of the disturbance, or zero for no recovery test.
    uint64_t recoveryStartUs = 0;
    bool extended = false;
};

struct RunMetrics {
    Counts counts{};
    uint64_t elapsedUs = 0;
    uint64_t calibrationUs = 0, unavailableUs = 0, atTargetUs = 0, belowTargetUs = 0;
    uint64_t longestBelowTargetUs = 0;
    double targetPercent = 0, finalScorePercent = 0;
    bool finalScoreAvailable = false;

    uint64_t bufferSamples = 0, bufferObservedUs = 0, minimumBufferTimeUs = 0;
    uint64_t bufferP95Us = 0, bufferP99Us = 0, peakBufferUs = 0;
    uint64_t finalBufferUs = 0, finalMinBufferUs = 0, bufferAtRecoveryStartUs = 0;
    uint64_t firstBufferUs = 0, maxReconnectStartBufferUs = 0;
    uint64_t totalBufferMovementUs = 0, bufferReversals = 0;
    double meanBufferUs = 0, bufferGrowthUsPerMinute = 0, bufferReversalsPerMinute = 0;
    // Elapsed-time mean and full reserve range over the final quarter.
    double steadyMeanBufferUs = 0;
    uint64_t steadyBufferRangeUs = 0;
    uint64_t steadyScoredUs = 0, steadyAtTargetUs = 0, steadyAboveTargetUs = 0;
    double steadyMeanReadyToDisplayUs = 0;
    uint64_t longestSteadySurplusWithoutDrainUs = 0; // Requires a qualifying fixed-reserve reference.

    // Hitch magnitudes are positive excess of modeled display interval over the
    // matching source interval. Percentiles include zero for on-time pairs.
    uint64_t hitchP95Us = 0, hitchP99Us = 0, worstHitchUs = 0;
    uint64_t displayJerkP95Us = 0, displayJerkP99Us = 0;
    double displayJerkOver2MsPercent = 0;
    uint64_t residenceP95Us = 0, endToEndP95Us = 0;
    double meanQueueResidenceUs = 0, meanReadyToDisplayUs = 0, meanSourceToDisplayUs = 0;
    uint64_t clientEligibleUs = 0;
    double clientLossUs = 0;

    // First uninterrupted second at/above target after recoveryStartUs.
    bool recovered = false;
    uint64_t recoveryUs = 0;
    uint64_t timestampViolations = 0, frameOrderViolations = 0;
    uint64_t sessionsObserved = 0;
    uint64_t fingerprint = 0; // Deterministic digest of modeled frame outcomes.

    double timeAtTargetPercent() const;
    double coveragePercent() const; // Fraction of post-calibration time scored.
    double steadyCoveragePercent() const;
    double steadyTimeAtTargetPercent() const;
    double steadyTimeAboveTargetPercent() const;
};

std::vector<Workload> streamWorkloads();
RunMetrics measure(const Workload& workload, const Options& options,
                   std::optional<uint64_t> qualifyingReserveUs = std::nullopt);
std::vector<std::string> checkWorkload(const Workload& workload, const Options& options,
                                       const RunMetrics& metrics, bool checkDefaults = true);
std::vector<std::string> checkMetricAccounting();
std::vector<std::string> checkGapRecoveryReference();

struct OraclePoint {
    uint64_t bufferUs = 0, dropped = 0;
    double timeAtTargetPercent = 0, coveragePercent = 0;
    double steadyMeanReadyToDisplayUs = 0;
    std::optional<double> finalScorePercent;
    bool feasible = true, qualifies = false;
};

struct OracleResult {
    bool found = false;
    uint64_t minimumUs = 0;
    std::vector<OraclePoint> points;
};

enum class OracleWindow { WholeSession, FinalQuarter };
OracleResult fixedBufferOracle(const Workload& workload, const Options& options,
                              const std::vector<uint64_t>& candidates,
                              double requiredTimeAtTargetPercent = 95.0,
                              OracleWindow window = OracleWindow::WholeSession);
std::vector<std::string> checkSurplusDrain(const RunMetrics& adaptive, const OracleResult& reference);

} // namespace VrrSim
