// Fast, headless development gate for the production adaptive buffer.
#include "vrrstreamscenarios.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <stdexcept>

namespace {
using namespace VrrSim;
using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void require(bool condition, const QString& message)
{
    if (!condition) throw std::runtime_error(message.toStdString());
}

// MSVC's clock() reports wall time. The hosted check needs the CPU time
// charged to this process, so unrelated runner work cannot fail the gate.
std::optional<double> processCpuMs()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return std::nullopt;
    const auto ticks = [](const FILETIME& time) {
        return (uint64_t(time.dwHighDateTime) << 32) | uint64_t(time.dwLowDateTime);
    };
    return double(ticks(kernel) + ticks(user)) / 10000.0;
#else
    const auto ticks = std::clock();
    if (ticks == std::clock_t(-1)) return std::nullopt;
    return double(ticks) * 1000.0 / CLOCKS_PER_SEC;
#endif
}

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString() + " (" + path + ")");
    QJsonParseError error{};
    auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(),
            path + " must contain a JSON object (" + error.errorString() + ")");
    return doc.object();
}

void writeObject(const QString& path, const QJsonObject& object)
{
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly), file.errorString() + " (" + path + ")");
    const auto bytes = QJsonDocument(object).toJson();
    require(file.write(bytes) == bytes.size() && file.commit(), "Could not save " + path);
}

QJsonValue number(uint64_t value)
{
    return QJsonValue(static_cast<qint64>(value));
}

QJsonObject gapMetricsJson(const GapMetrics& metrics)
{
    return {
        {"worst_display_gap_us", number(metrics.worstGapUs)},
        {"p99_presented_jerk_us", number(metrics.jerkP99Us)},
        {"worst_presented_jerk_us", number(metrics.worstJerkUs)},
        {"presented_jerk_over_2ms_count", number(metrics.jerkOver2Ms)},
    };
}

QJsonObject metricsJson(const RunMetrics& m)
{
    return {
        {"target_pct", m.targetPercent},
        {"final_score_pct", m.finalScoreAvailable ? QJsonValue(m.finalScorePercent) : QJsonValue()},
        {"time_at_target_pct", m.atTargetUs + m.belowTargetUs ? QJsonValue(m.timeAtTargetPercent()) : QJsonValue()},
        {"coverage_pct", m.coveragePercent()},
        {"session_us", number(m.elapsedUs)}, {"calibration_us", number(m.calibrationUs)},
        {"unavailable_us", number(m.unavailableUs)}, {"at_target_us", number(m.atTargetUs)},
        {"below_target_us", number(m.belowTargetUs)}, {"longest_below_target_us", number(m.longestBelowTargetUs)},
        {"mean_buffer_us", m.meanBufferUs}, {"p95_buffer_us", number(m.bufferP95Us)},
        {"mean_extra_delay_us", m.meanExtraDelayUs}, {"extra_delay_cycles", number(m.extraDelayCycles)},
        {"sustained_extra_delay_cycles", number(m.sustainedExtraDelayCycles)},
        {"p99_buffer_us", number(m.bufferP99Us)}, {"peak_buffer_us", number(m.peakBufferUs)},
        {"final_buffer_us", number(m.finalBufferUs)}, {"final_minimum_buffer_us", number(m.finalMinBufferUs)},
        {"steady_mean_buffer_us", m.steadyMeanBufferUs}, {"steady_buffer_range_us", number(m.steadyBufferRangeUs)},
        {"steady_coverage_pct", m.steadyCoveragePercent()},
        {"steady_at_target_pct", m.steadyScoredUs ? QJsonValue(m.steadyTimeAtTargetPercent()) : QJsonValue()},
        {"steady_above_target_pct", m.steadyScoredUs ? QJsonValue(m.steadyTimeAboveTargetPercent()) : QJsonValue()},
        {"steady_mean_ready_to_display_us", m.steadyMeanReadyToDisplayUs},
        {"sessions_observed", number(m.sessionsObserved)}, {"buffer_observed_us", number(m.bufferObservedUs)},
        {"at_minimum_buffer_us", number(m.minimumBufferTimeUs)}, {"buffer_growth_us_per_min", m.bufferGrowthUsPerMinute},
        {"buffer_reversals_per_min", m.bufferReversalsPerMinute}, {"total_buffer_movement_us", number(m.totalBufferMovementUs)},
        {"recovery_us", m.recovered ? number(m.recoveryUs) : QJsonValue()},
        {"p95_hitch_us", number(m.hitchP95Us)}, {"p99_hitch_us", number(m.hitchP99Us)},
        {"max_hitch_us", number(m.worstHitchUs)}, {"p95_presented_jerk_us", number(m.displayJerkP95Us)},
        {"p99_presented_jerk_us", number(m.displayJerkP99Us)}, {"presented_jerk_over_2ms_pct", m.displayJerkOver2MsPercent},
        {"client_eligible_us", number(m.clientEligibleUs)}, {"client_weighted_loss_us", m.clientLossUs},
        {"p95_source_to_display_us", number(m.endToEndP95Us)}, {"p95_queue_residence_us", number(m.residenceP95Us)},
        {"mean_queue_residence_us", m.meanQueueResidenceUs}, {"mean_ready_to_display_us", m.meanReadyToDisplayUs},
        {"mean_source_to_display_us", m.meanSourceToDisplayUs},
        {"outcome_digest", QString::number(m.fingerprint, 16)}
    };
}

QJsonObject oracleJson(const OracleResult& oracle)
{
    QJsonArray points;
    for (const auto& p : oracle.points) {
        QJsonObject point{{"buffer_us", number(p.bufferUs)}, {"feasible", p.feasible}, {"qualifies", p.qualifies}};
        if (p.feasible) {
            point.insert("time_at_target_pct", p.timeAtTargetPercent);
            point.insert("coverage_pct", p.coveragePercent);
            point.insert("dropped", number(p.dropped));
            point.insert("steady_mean_ready_to_display_us", p.steadyMeanReadyToDisplayUs);
            point.insert("final_score_pct", p.finalScorePercent ? QJsonValue(*p.finalScorePercent) : QJsonValue());
        }
        points.append(point);
    }
    return {{"minimum_tested_us", oracle.found ? number(oracle.minimumUs) : QJsonValue()}, {"points", points}};
}

QJsonObject forecastJson(const ForecastComparison& comparison)
{
    const auto window = [](const ForecastWindow& w) {
        return QJsonObject{{"frames", number(w.frames)}, {"scored_us", number(w.sampledUs)},
                           {"at_goal_us", number(w.atGoalUs)}, {"minimum_score_pct", w.minimumScore},
                           {"mean_buffer_us", w.meanBufferUs}, {"mean_ready_to_submit_us", w.meanReadyToSubmitUs},
                           {"mean_ready_to_display_us", w.meanReadyToDisplayUs}, {"display_jerk_p99_us", number(w.displayJerkP99Us)}};
    };
    return {{"decision_frame", number(comparison.frame)}, {"score_at_decision_pct", comparison.scoreAtDecision},
            {"trial_maximum_us", number(comparison.capUs)},
            {"brief_adaptive", window(comparison.brief)}, {"brief_capped", window(comparison.cappedBrief)},
            {"sustained_adaptive", window(comparison.sustained)}, {"sustained_capped", window(comparison.cappedSustained)}};
}

// Compare only matching virtual experiments. Every phase and option is included
// in the input object, while the controller implementation is free to change.
QJsonObject compareMetrics(const QJsonObject& before, const QJsonObject& after)
{
    QJsonObject changes;
    for (auto field = after.begin(); field != after.end(); ++field) {
        const auto previous = before.value(field.key());
        if (previous.isUndefined() && (field.key() == "mean_extra_delay_us" ||
                field.key() == "extra_delay_cycles" || field.key() == "sustained_extra_delay_cycles"))
            continue; // Older reports did not record planned smoothing and recovery delay.
        require(!previous.isUndefined(), "Baseline is missing metric " + field.key());
        if (previous.isDouble() && field.value().isDouble())
            changes.insert(field.key(), field.value().toDouble() - previous.toDouble());
    }
    return changes;
}

struct PhaseTime { const char* key; uint64_t Phase::*member; bool omitZero = false; };
constexpr PhaseTime phaseTimes[] = {
    {"duration_us", &Phase::durationUs}, {"network_delay_us", &Phase::networkDelayUs},
    {"jitter_us", &Phase::jitterUs}, {"decode_us", &Phase::decodeUs}, {"prepare_us", &Phase::prepareUs},
    {"alternating_first_us", &Phase::alternatingFirstUs}, {"alternating_second_us", &Phase::alternatingSecondUs},
    {"disturbance_period_us", &Phase::disturbancePeriodUs}, {"network_spike_us", &Phase::networkSpikeUs},
    {"host_pause_us", &Phase::hostPauseUs},
    {"encode_us", &Phase::encodeUs, true}, {"encoder_stall_us", &Phase::encoderStallUs, true}
};

QJsonObject phaseJson(const Phase& p, bool includeAllKeys = false)
{
    QJsonObject phase{{"host_hz", p.hostHz}, {"clock_drift_ppm", p.clockDriftPpm},
                      {"rtp_jump_us", QJsonValue(qint64(p.rtpJumpUs))}, {"reconnect", p.reconnect}};
    for (auto field : phaseTimes) {
        if (includeAllKeys || !field.omitZero || p.*field.member)
            phase.insert(field.key, number(p.*field.member));
    }
    if (includeAllKeys || !p.sourceIntervalsUs.empty()) {
        QJsonArray gaps;
        for (const auto gap : p.sourceIntervalsUs) gaps.append(number(gap));
        phase.insert("source_intervals_us", gaps);
    }
    return phase;
}

QJsonObject workloadJson(const Workload& w)
{
    QJsonArray phases;
    for (const auto& p : w.scenario.phases) phases.append(phaseJson(p));
    QJsonObject workload{{"schema", 1}, {"name", QString::fromStdString(w.scenario.name)}, {"seed", number(w.scenario.seed)},
            {"stream_hz", w.scenario.streamHz}, {"display_hz", w.scenario.displayHz},
            {"recovery_start_us", number(w.recoveryStartUs)}, {"phases", phases}};
    if (w.scenario.dxgiWaitable) workload.insert("dxgi_waitable", true);
    return workload;
}

qint64 integer(const QJsonObject& obj, const char* key, qint64 fallback, qint64 lo, qint64 hi)
{
    if (!obj.contains(key)) return fallback;
    const auto value = obj.value(key);
    const double n = value.toDouble();
    require(value.isDouble() && n >= double(lo) && n <= double(hi) && std::trunc(n) == n,
            QString("%1 must be an integer from %2 to %3").arg(key).arg(lo).arg(hi));
    return qint64(n);
}

void checkKeys(const QJsonObject& obj, const QJsonObject& allowed)
{
    for (auto i = obj.begin(); i != obj.end(); ++i) require(allowed.contains(i.key()), "Unknown field " + i.key());
}

Workload readWorkload(const QString& path)
{
    const auto doc = readObject(path);
    Workload w;
    auto& s = w.scenario;
    auto allowed = workloadJson(w);
    allowed.insert("dxgi_waitable", false);
    checkKeys(doc, allowed);
    require(integer(doc, "schema", 0, 1, 1) == 1, "Expected workload schema 1");
    s.name = doc.value("name").toString().trimmed().toStdString();
    require(!s.name.empty(), "A workload needs a name");
    s.seed = uint32_t(integer(doc, "seed", s.seed, 0, UINT32_MAX));
    s.streamHz = int(integer(doc, "stream_hz", s.streamHz, 1, 1000));
    s.displayHz = int(integer(doc, "display_hz", s.displayHz, 1, 1000));
    require(!doc.contains("dxgi_waitable") || doc.value("dxgi_waitable").isBool(), "dxgi_waitable must be boolean");
    s.dxgiWaitable = doc.value("dxgi_waitable").toBool();
    const auto phases = doc.value("phases").toArray();
    require(!phases.isEmpty(), "A workload needs at least one phase");
    constexpr qint64 dayUs = 86400000000LL;
    uint64_t totalUs = 0;
    const auto phaseKeys = phaseJson(Phase{}, true);
    for (const auto entry : phases) {
        require(entry.isObject(), "Each phase must be an object");
        const auto obj = entry.toObject();
        checkKeys(obj, phaseKeys);
        Phase p;
        p.hostHz = int(integer(obj, "host_hz", s.streamHz, 1, 1000));
        p.clockDriftPpm = int(integer(obj, "clock_drift_ppm", 0, -999999, 999999));
        p.rtpJumpUs = integer(obj, "rtp_jump_us", 0, -dayUs, dayUs);
        require(!obj.contains("reconnect") || obj.value("reconnect").isBool(), "reconnect must be boolean");
        p.reconnect = obj.value("reconnect").toBool();
        for (auto field : phaseTimes)
            p.*field.member = uint64_t(integer(obj, field.key, qint64(p.*field.member), 0, dayUs));
        require(p.durationUs > 0, "Each phase needs a positive duration_us");
        require((p.alternatingFirstUs == 0) == (p.alternatingSecondUs == 0), "Set both alternating intervals");
        if (obj.contains("source_intervals_us")) {
            require(obj.value("source_intervals_us").isArray() && !p.alternatingFirstUs,
                    "source_intervals_us needs an array and cannot be combined with alternating intervals");
            for (const auto gap : obj.value("source_intervals_us").toArray()) {
                const qint64 us = gap.toInteger(-1);
                require(us > 0 && us <= dayUs, "Source intervals must be positive integer microseconds within one day");
                p.sourceIntervalsUs.push_back(uint64_t(us));
            }
        }
        totalUs += p.durationUs;
        require(totalUs <= dayUs, "Maximum virtual workload duration is one day");
        s.phases.push_back(p);
    }
    w.recoveryStartUs = uint64_t(integer(doc, "recovery_start_us", 0, 0, qint64(totalUs - 1)));
    return w;
}

qint64 cliInteger(const QCommandLineParser& cli, const char* key, qint64 lo, qint64 hi)
{
    bool ok = false;
    const auto value = cli.value(key).toLongLong(&ok);
    require(ok && value >= lo && value <= hi, QString("--%1 expects an integer from %2 to %3").arg(key).arg(lo).arg(hi));
    return value;
}

QJsonObject inputs(const Workload& w, const Options& o)
{
    return {{"workload", workloadJson(w)}, {"latency_mode", o.latencyMode}, {"target_hundredths", o.targetHundredths},
            {"smooth", o.smooth}, {"can_latch", o.canLatch}, {"fixed_buffer_us", QJsonValue(qint64(o.fixedBufferUs))}};
}

constexpr const char* helpText = "Run the fast synthetic buffer gate, or select a workload to experiment.";

void configure(QCommandLineParser& cli)
{
    cli.setApplicationDescription(helpText);
    cli.addHelpOption();
    cli.addPositionalArgument("workload", "Run one or more named workloads (see --list).", "[workload ...]");
    cli.addOptions({
        {"list", "List built-in workloads."},
        {"stress", "Exercise extended workloads and a deterministic rate, preset and seed matrix."},
        {"load", "Read an editable phased workload (schema 1).", "file"},
        {"export", "Write the selected workload as JSON and exit.", "file"},
        {"fps", "Scale phase host rates to this negotiated stream rate (1-1000).", "Hz"},
        {"display", "Set the display refresh rate (1-1000).", "Hz"},
        {"seed", "Set the deterministic workload seed.", "uint32"},
        {"preset", "Use balanced, smooth or low-latency.", "name"},
        {"target", "Set the production timing target (90.00-99.99%).", "percent"},
        {"no-latch", "Model a presenter without synchronized latching."},
        {"no-smoothing", "Run with Reduce judder off."},
        {"fixed", "Hold the applied buffer at a fixed size.", "microseconds"},
        {"oracle", "Compare fixed buffers on the identical workload (comma-separated sizes in us).", "0,500,1000,..."},
        {"gap-recovery-reference", "Calculate the offline spare-slot reference at the display ceiling."},
        {"output", "Save the inputs, metrics, assertions and optional comparison as JSON.", "file"},
        {"compare", "Compare metrics against a report from the same experiment.", "baseline.json"},
        {"budget-ms", "Fail if total wall time exceeds this many milliseconds.", "ms"},
        {"cpu-budget-ms", "Fail if total process CPU time exceeds this many milliseconds.", "ms"}
    });
}

struct Experiment {
    Workload workload;
    Options options;
    bool catalogChecks;
};

void setStreamRate(Scenario& s, int hz)
{
    for (auto& p : s.phases) p.hostHz = std::max(1, int(std::lround(double(p.hostHz) * hz / s.streamHz)));
    s.streamHz = hz;
}

std::vector<Experiment> selectExperiments(const QCommandLineParser& cli, const std::vector<Workload>& catalog)
{
    const auto names = cli.positionalArguments();
    std::vector<Workload> selected;
    if (cli.isSet("load")) {
        require(names.isEmpty(), "Use --load or named workloads for a run");
        selected.push_back(readWorkload(cli.value("load")));
    } else if (names.isEmpty()) {
        for (const auto& w : catalog) if (!w.extended || cli.isSet("stress")) selected.push_back(w);
    } else {
        for (const auto& name : names) {
            auto it = std::find_if(catalog.begin(), catalog.end(), [&](const Workload& w) { return w.scenario.name == name.toStdString(); });
            require(it != catalog.end(), "Unknown workload " + name + " (see --list)");
            require(names.count(name) == 1, "Repeated workload " + name);
            selected.push_back(*it);
        }
    }
    Options options;
    if (cli.isSet("preset")) {
        const auto preset = cli.value("preset");
        require(preset == "balanced" || preset == "smooth" || preset == "low-latency", "Unknown preset " + preset);
        options.latencyMode = preset == "smooth" ? 0 : preset == "low-latency" ? 2 : 1;
    }
    if (cli.isSet("target")) {
        bool ok = false;
        const double pct = cli.value("target").toDouble(&ok);
        require(ok && pct >= 90 && pct <= 99.99 && std::abs(std::round(pct * 100) - pct * 100) < 0.000001,
                "--target expects a percentage from 90.00 to 99.99, with up to two decimal places");
        options.targetHundredths = int(std::round(pct * 100));
    }
    options.canLatch = !cli.isSet("no-latch");
    options.smooth = !cli.isSet("no-smoothing");
    if (cli.isSet("fixed")) options.fixedBufferUs = cliInteger(cli, "fixed", 0, 1000000);
    bool defaults = true;
    for (const auto* option : {"fps", "display", "seed", "preset", "target", "fixed", "no-latch", "no-smoothing", "load"})
        if (cli.isSet(option)) defaults = false;
    std::vector<Experiment> experiments;
    for (auto& w : selected) {
        if (cli.isSet("fps")) setStreamRate(w.scenario, int(cliInteger(cli, "fps", 1, 1000)));
        if (cli.isSet("display")) w.scenario.displayHz = int(cliInteger(cli, "display", 1, 1000));
        if (cli.isSet("seed")) w.scenario.seed = uint32_t(cliInteger(cli, "seed", 0, UINT32_MAX));
        // Preserve the catalog assertions when a user spells out a default
        // or loads an exported, unmodified workload.
        Options runOptions = options;
        if (w.scenario.name == "rate-drop-hitches") runOptions.smooth = false;
        const bool lowLatencyWorkload = w.scenario.name == "ceiling-forecast" ||
            w.scenario.name == "variable-source-judder";
        if (lowLatencyWorkload && !cli.isSet("preset")) runOptions.latencyMode = 2;
        const auto input = inputs(w, runOptions);
        const bool catalogChecks = defaults || std::any_of(catalog.begin(), catalog.end(), [&](const Workload& entry) {
            if (entry.scenario.name != w.scenario.name) return false;
            if (w.scenario.name == "rate-drop-hitches") {
                Options recorded;
                recorded.smooth = false;
                return inputs(entry, recorded) == input;
            }
            if (lowLatencyWorkload)
                return workloadJson(entry) == workloadJson(w) && runOptions.latencyMode == 2 &&
                       (!runOptions.targetHundredths || runOptions.targetHundredths == 9900) &&
                       runOptions.fixedBufferUs < 0 && runOptions.canLatch && runOptions.smooth;
            if (w.scenario.name == "surplus-drain" && options.fixedBufferUs < 0 && options.canLatch && options.smooth)
                return workloadJson(entry) == input.value("workload").toObject();
            return inputs(entry, {}) == input;
        });
        experiments.push_back({w, runOptions, catalogChecks});
        if (w.scenario.name == "surplus-drain" && catalogChecks && options.fixedBufferUs < 0 &&
            !cli.isSet("preset") && !cli.isSet("target") && !cli.isSet("export")) {
            // Balanced already covers 99.50%. Include the reported Low Latency
            // profile, a looser goal, and a goal that requires more than the floor.
            for (int target : {9500, 9900, 9990}) {
                Options o = options;
                o.latencyMode = 2;
                o.targetHundredths = target;
                experiments.push_back({w, o, true});
            }
        }
    }
    if (cli.isSet("stress")) {
        require(defaults && names.empty(), "--stress selects its own catalog, rates, presets and seeds");
        const auto mixed = std::find_if(catalog.begin(), catalog.end(), [](const Workload& w) { return w.scenario.name == "mixed"; });
        require(mixed != catalog.end(), "Missing mixed stress workload");
        int index = 0;
        for (int hz : {30, 60, 90, 120, 144, 240}) {
            auto w = *mixed;
            setStreamRate(w.scenario, hz);
            w.scenario.displayHz = std::max(144, hz);
            w.scenario.seed = 31337u + uint32_t(index);
            Options o;
            o.latencyMode = index % 3;
            o.canLatch = index % 2 == 0;
            o.smooth = index % 3 != 0;
            // A software-spaced presenter at its exact ceiling has known
            // replacement/coverage limitations in the historical contracts.
            experiments.push_back({w, o, o.canLatch || hz < w.scenario.displayHz});
            ++index;
        }
    }
    require(!experiments.empty(), "No workloads selected");
    return experiments;
}

void checkOutputPath(const QString& path, const QCommandLineParser& cli)
{
    for (const auto* option : {"compare", "load"})
        require(!cli.isSet(option) || QFileInfo(path) != QFileInfo(cli.value(option)),
                "Choose an output path different from the input file");
}

QJsonObject findPrevious(const QJsonArray& baseline, const QJsonObject& input)
{
    QJsonObject match;
    for (const auto entry : baseline) {
        const auto row = entry.toObject();
        if (row.value("input").toObject() == input) {
            require(match.isEmpty(), "Baseline contains duplicate entries for these inputs");
            match = row;
        }
    }
    require(!match.isEmpty() && match.value("metrics").isObject(),
            "No matching baseline for " + input.value("workload").toObject().value("name").toString()
            + " with these phases and options");
    return match;
}

int run(const QCommandLineParser& cli)
{
    const auto catalog = streamWorkloads();
    if (cli.isSet("list")) {
        for (const auto& w : catalog) std::printf("%-22s %s\n", w.scenario.name.c_str(), w.extended ? "extended" : "fast");
        return 0;
    }
    const auto experiments = selectExperiments(cli, catalog);
    if (cli.isSet("export")) {
        require(experiments.size() == 1, "Select one workload to export");
        checkOutputPath(cli.value("export"), cli);
        writeObject(cli.value("export"), workloadJson(experiments.front().workload));
        return 0;
    }
    std::vector<uint64_t> candidates;
    if (cli.isSet("oracle")) {
        require(!cli.isSet("fixed"), "The oracle compares an adaptive run with a grid of fixed buffers");
        for (const auto& val : cli.value("oracle").split(',')) {
            bool ok = false;
            const auto us = val.toLongLong(&ok);
            require(ok && us >= 0 && us <= 1000000, "--oracle expects comma-separated buffers in microseconds");
            candidates.push_back(uint64_t(us));
        }
    }
    QJsonArray baseline;
    if (cli.isSet("compare")) {
        auto doc = readObject(cli.value("compare"));
        require(doc.value("report_schema").toInt() == 1, "Expected buffer simulator report schema 1");
        baseline = doc.value("runs").toArray();
        require(!baseline.isEmpty(), "Baseline has no runs");
    }
    if (cli.isSet("output")) checkOutputPath(cli.value("output"), cli);
    const auto budgetMs = cli.isSet("budget-ms") ? cliInteger(cli, "budget-ms", 1, INT32_MAX) : 0;

    const auto cpuBudgetMs = cli.isSet("cpu-budget-ms") ? cliInteger(cli, "cpu-budget-ms", 1, INT32_MAX) : 0;
    const auto started = Clock::now();
    const auto cpuStarted = processCpuMs();
    auto selfFailures = checkMetricAccounting();
    if (cli.isSet("stress")) {
        const auto gapFailures = checkGapRecoveryReference();
        selfFailures.insert(selfFailures.end(), gapFailures.begin(), gapFailures.end());
    }
    // A short seeded stream guards determinism even when the selected workload
    // does not have jitter. Identical inputs must produce identical frame results.
    Workload repeat;
    repeat.scenario.name = "repeatability";
    Phase jitter;
    jitter.durationUs = 3000000;
    jitter.jitterUs = 3000;
    repeat.scenario.phases.push_back(jitter);
    auto first = measure(repeat, {});
    if (first.fingerprint != measure(repeat, {}).fingerprint || first.counts.presented < 2)
        selfFailures.push_back("Seeded stream outcomes must repeat");
    QJsonArray suiteFailures;
    for (const auto& failure : selfFailures) {
        std::fprintf(stderr, "FAIL simulator: %s\n", failure.c_str());
        suiteFailures.append(QString::fromStdString(failure));
    }
    bool passed = selfFailures.empty();
    QJsonArray rows;
    std::printf("workload                 Hz   at goal %%  coverage %%  mean buf us   p99 buf us    drops    run ms  checks\n");
    for (const auto& experiment : experiments) {
        const auto& w = experiment.workload;
        const auto& o = experiment.options;
        const auto input = inputs(w, o);
        const auto previous = baseline.isEmpty() ? QJsonObject{} : findPrevious(baseline, input);
        std::optional<OracleResult> drainReference;
        std::optional<uint64_t> qualifyingReserveUs;
        if (experiment.catalogChecks && w.scenario.name == "surplus-drain" && o.fixedBufferUs < 0) {
            drainReference = fixedBufferOracle(w, o, {0, 250, 500, 750, 1000, 1500, 2000, 2250, 2500, 3000},
                                              99, OracleWindow::FinalQuarter);
            if (drainReference->found) qualifyingReserveUs = drainReference->minimumUs;
        }
        const auto start = Clock::now();
        const auto m = measure(w, o, qualifyingReserveUs);
        const double ms = elapsedMs(start);
        auto failures = checkWorkload(w, o, m, experiment.catalogChecks);
        std::optional<ForecastComparison> forecast;
        if (experiment.catalogChecks && w.scenario.name == "ceiling-forecast") {
            forecast = compareCeilingForecast(w.scenario, o);
            failures.insert(failures.end(), forecast->failures.begin(), forecast->failures.end());
        }
        if (drainReference) {
            const auto drainFailures = checkSurplusDrain(m, *drainReference);
            failures.insert(failures.end(), drainFailures.begin(), drainFailures.end());
        }
        auto metrics = metricsJson(m);
        if (qualifyingReserveUs)
            metrics.insert("longest_steady_surplus_without_drain_us", number(m.longestSteadySurplusWithoutDrainUs));
        const auto& c = m.counts;
        const auto drops = c.queueDrops + c.replacements + c.reconnectDrops + c.decodeDrops + c.presenterDrops;
        metrics.insert("produced", number(c.produced));
        metrics.insert("presented", number(c.presented));
        metrics.insert("dropped", number(drops));
        metrics.insert("decoded_queue_drops", number(c.queueDrops));
        metrics.insert("replacement_drops", number(c.replacements));
        metrics.insert("reconnect_drops", number(c.reconnectDrops));
        metrics.insert("decode_drops", number(c.decodeDrops));
        if (w.scenario.dxgiWaitable && o.canLatch) {
            metrics.insert("presenter_drops", number(c.presenterDrops));
            metrics.insert("max_pending_presents", number(c.maxPendingPresents));
            metrics.insert("presenter_admission_wait_us", number(c.presenterAdmissionWaitUs));
        }
        if (c.maxCaptureToSendUs) {
            metrics.insert("max_capture_to_send_us", number(c.maxCaptureToSendUs));
            metrics.insert("compressed_host_send_intervals", number(c.compressedHostSendIntervals));
        }
        metrics.insert("runtime_ms", ms);
        metrics.insert("first_buffer_us", number(m.firstBufferUs));
        if (m.sessionsObserved > 1)
            metrics.insert("max_reconnect_start_buffer_us", number(m.maxReconnectStartBufferUs));
        QJsonArray errors;
        for (const auto& f : failures) {
            passed = false;
            errors.append(QString::fromStdString(f));
            std::fprintf(stderr, "FAIL [%s] %s\n", w.scenario.name.c_str(), f.c_str());
        }
        QJsonObject row{{"input", input}, {"metrics", metrics}, {"catalog_checks", experiment.catalogChecks}, {"failures", errors}};
        if (drainReference) row.insert("steady_drain_reference", oracleJson(*drainReference));
        if (forecast) row.insert("forecast_comparison", forecastJson(*forecast));
        const auto atGoal = metrics.value("time_at_target_pct");
        const auto goalText = atGoal.isDouble() ? QString::number(atGoal.toDouble(), 'f', 2) : QStringLiteral("n/a");
        std::printf("%-22s %4d %11s %11.2f %12.0f %12llu %8llu %9.2f  %s\n", w.scenario.name.c_str(),
                    w.scenario.streamHz, qPrintable(goalText), m.coveragePercent(), m.meanBufferUs,
                    static_cast<unsigned long long>(m.bufferP99Us), static_cast<unsigned long long>(drops), ms,
                    experiment.catalogChecks ? "policy + model" : "model");
        if (cli.isSet("gap-recovery-reference")) {
            const auto gap = measureGapRecoveryReference(w.scenario, o);
            row.insert("gap_recovery_reference", QJsonObject{
                {"baseline", gapMetricsJson(gap.baseline)}, {"filled", gapMetricsJson(gap.filled)},
                {"baseline_drops", number(gap.baselineDrops)}, {"filled_drops", number(gap.filledDrops)},
                {"inserted_frames", number(gap.insertedFrames)},
                {"baseline_frames_preserved", number(gap.baselineFramesPreserved)},
                {"oldest_inserted_frame_age_us", number(gap.oldestInsertedFrameAgeUs)},
            });
            std::printf("  spare slots filled %llu, worst display gap %.3f -> %.3f ms\n",
                        static_cast<unsigned long long>(gap.insertedFrames),
                        gap.baseline.worstGapUs / 1000.0, gap.filled.worstGapUs / 1000.0);
        }
        if (!candidates.empty()) {
            const auto oracle = fixedBufferOracle(w, o, candidates);
            row.insert("oracle", oracleJson(oracle));
            std::puts("  fixed us    at goal %   coverage %    drops   qualifies");
            for (const auto& p : oracle.points) {
                std::printf("  %8llu", static_cast<unsigned long long>(p.bufferUs));
                if (p.feasible)
                    std::printf("   %10.2f   %10.2f %8llu   %s\n", p.timeAtTargetPercent, p.coveragePercent,
                                static_cast<unsigned long long>(p.dropped), p.qualifies ? "yes" : "no");
                else std::puts("   exceeds queue budget");
            }
            if (oracle.found)
                std::printf("  minimum qualifying fixed buffer %llu us, adaptive mean minus fixed %+.0f us\n",
                            static_cast<unsigned long long>(oracle.minimumUs), m.meanBufferUs - double(oracle.minimumUs));
            else std::puts("  no tested fixed buffer qualified");
        }
        if (!previous.isEmpty()) {
            const auto changes = compareMetrics(previous.value("metrics").toObject(), metrics);
            row.insert("delta", changes);
            const auto deltaGoal = changes.value("time_at_target_pct");
            const auto goalDeltaText = deltaGoal.isDouble() ? QString::number(deltaGoal.toDouble(), 'f', 2) : QStringLiteral("n/a");
            std::printf("  change: at goal %s pp, mean buffer %+.0f us, p99 buffer %+.0f us, drops %+.0f, runtime %+.2f ms\n",
                        qPrintable(goalDeltaText), changes.value("mean_buffer_us").toDouble(), changes.value("p99_buffer_us").toDouble(),
                        changes.value("dropped").toDouble(), changes.value("runtime_ms").toDouble());
        }
        rows.append(row);
    }
    const double elapsed = elapsedMs(started);
    if (budgetMs && elapsed > budgetMs) {
        const auto error = QString("Runtime %1 ms exceeds budget %2 ms").arg(elapsed, 0, 'f', 1).arg(budgetMs);
        std::fprintf(stderr, "FAIL %s\n", qPrintable(error));
        suiteFailures.append(error);
        passed = false;
    }
    const auto cpuFinished = processCpuMs();
    const auto cpuElapsed = cpuStarted && cpuFinished && *cpuFinished >= *cpuStarted ?
        std::optional<double>(*cpuFinished - *cpuStarted) : std::nullopt;
    if (cpuBudgetMs && (!cpuElapsed || *cpuElapsed > cpuBudgetMs)) {
        const auto error = cpuElapsed ?
            QString("Process CPU time %1 ms exceeds budget %2 ms").arg(*cpuElapsed, 0, 'f', 1).arg(cpuBudgetMs) :
            QString("Process CPU time is unavailable");
        std::fprintf(stderr, "FAIL %s\n", qPrintable(error));
        suiteFailures.append(error);
        passed = false;
    }
    if (cli.isSet("output")) {
        QJsonObject report{{"report_schema", 1}, {"runs", rows}, {"elapsed_ms", elapsed},
                           {"suite_failures", suiteFailures}, {"passed", passed}};
        if (cpuElapsed) report.insert("cpu_ms", *cpuElapsed);
        writeObject(cli.value("output"), report);
    }
    std::printf("%s   %zu virtual streams in %.1f ms", passed ? "PASS" : "FAIL", experiments.size(), elapsed);
    if (cpuElapsed) std::printf(" (%.1f ms CPU)", *cpuElapsed);
    std::puts("");
    return passed ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser cli;
    configure(cli);
    cli.process(app);
    try {
        return run(cli);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vrrbuffersim: %s\n", error.what());
        return 2;
    }
}
