#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include "vrrreplayconfig.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace {

struct InputFrame {
    uint64_t sequence = 0;
    int number = 0;
    uint32_t rtp = 0;
    bool rtpValid = false;
    uint64_t decodeUs = 0;
    uint64_t arrivalUs = 0;
};

struct Distribution {
    std::vector<uint64_t> values;

    uint64_t percentile(unsigned int perMille) const
    {
        if (values.empty()) return 0;
        std::vector<uint64_t> ordered = values;
        std::sort(ordered.begin(), ordered.end());
        const size_t rank = std::max<size_t>(
            1, (ordered.size() * std::min(1000U, perMille) + 999) / 1000);
        return ordered[rank - 1];
    }

    QJsonObject json() const
    {
        QJsonObject result;
        result["count"] = static_cast<qint64>(values.size());
        result["p50"] = static_cast<qint64>(percentile(500));
        result["p95"] = static_cast<qint64>(percentile(950));
        result["p99"] = static_cast<qint64>(percentile(990));
        result["max"] = static_cast<qint64>(
            values.empty() ? 0 : *std::max_element(values.begin(), values.end()));
        return result;
    }
};

struct Capture {
    VrrSessionConfig session;
    std::vector<InputFrame> frames;
    std::vector<uint64_t> preparationUs;
    std::vector<uint64_t> presentCallUs;
    uint64_t decisionCallUs = 0;
};

uint64_t value(const QList<QByteArray>& fields,
               const QMap<QByteArray, int>& columns,
               const QByteArray& name)
{
    const int column = columns.value(name, -1);
    return column >= 0 && column < fields.size() ?
        fields[column].toULongLong() : 0;
}

bool loadCapture(const QString& path, Capture& capture, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    const QList<QByteArray> header = file.readLine().trimmed().split(',');
    QMap<QByteArray, int> columns;
    for (int i = 0; i < header.size(); ++i) columns.insert(header[i], i);
    const QList<QByteArray> required {
        "arrival_sequence", "frame", "rtp_timestamp", "rtp_valid",
        "decode_complete_us", "pacer_arrival_us", "display_refresh_hz",
        "stream_rate_hz", "additional_queued_frame", "prepare_us",
        "present_call_us", "controller_call_us"
    };
    for (const QByteArray& name : required) {
        if (!columns.contains(name)) {
            error = "missing CSV column: " + QString::fromLatin1(name);
            return false;
        }
    }

    Distribution decisionCalls;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QList<QByteArray> fields = line.split(',');
        if (fields.size() != header.size()) {
            error = "malformed CSV row";
            return false;
        }
        InputFrame frame;
        frame.sequence = value(fields, columns, "arrival_sequence");
        frame.number = fields[columns["frame"]].toInt();
        frame.rtp = static_cast<uint32_t>(value(fields, columns, "rtp_timestamp"));
        frame.rtpValid = value(fields, columns, "rtp_valid") != 0;
        frame.decodeUs = value(fields, columns, "decode_complete_us");
        frame.arrivalUs = value(fields, columns, "pacer_arrival_us");
        capture.frames.push_back(frame);

        if (capture.session.displayRefreshHz == 0) {
            capture.session.displayRefreshHz = static_cast<int>(
                value(fields, columns, "display_refresh_hz"));
            capture.session.streamRateHz = static_cast<int>(
                value(fields, columns, "stream_rate_hz"));
            capture.session.allowAdditionalQueuedFrame =
                value(fields, columns, "additional_queued_frame") != 0;
        }
        const uint64_t preparation = value(fields, columns, "prepare_us");
        if (preparation != 0) capture.preparationUs.push_back(preparation);
        const uint64_t presentCall = value(fields, columns, "present_call_us");
        if (presentCall != 0) capture.presentCallUs.push_back(presentCall);
        const uint64_t decisionCall = value(fields, columns, "controller_call_us");
        if (decisionCall != 0) decisionCalls.values.push_back(decisionCall);
    }
    std::sort(capture.frames.begin(), capture.frames.end(),
              [](const InputFrame& left, const InputFrame& right) {
                  return left.sequence < right.sequence;
              });
    capture.decisionCallUs = decisionCalls.percentile(500);
    if (capture.frames.empty() || capture.session.displayRefreshHz <= 0 ||
            capture.session.streamRateHz <= 0) {
        error = "capture has no frames or invalid session rates";
        return false;
    }
    if (capture.preparationUs.empty()) capture.preparationUs.push_back(0);
    if (capture.presentCallUs.empty()) capture.presentCallUs.push_back(0);
    return true;
}

uint64_t addSaturated(uint64_t left, uint64_t right)
{
    return left > std::numeric_limits<uint64_t>::max() - right ?
        std::numeric_limits<uint64_t>::max() : left + right;
}

uint64_t rtpDeltaUs(uint32_t older, uint32_t newer)
{
    const uint32_t ticks = newer - older;
    return (static_cast<uint64_t>(ticks) * 1000000ULL + 45000ULL) / 90000ULL;
}

QJsonObject simulate(const Capture& capture, VrrReplayScenario scenario,
                     size_t queueCapacity)
{
    if (!scenario.controllerCustomized) {
        scenario.controller = vrrTimingParametersForSession(capture.session);
    }
    VrrTimingController controller(capture.session, false, scenario.controller);
    std::deque<size_t> queue;
    size_t nextArrival = 0;
    size_t serviceOrdinal = 0;
    uint64_t nowUs = capture.frames.front().arrivalUs;
    uint64_t capacityDrops = 0;
    uint64_t staleDrops = 0;
    uint64_t presented = 0;
    uint64_t maximumQueueDepth = 0;
    Distribution queueDepth;
    Distribution decodeLatency;
    Distribution senderSpacingError;
    Distribution presentationIntervals;
    Distribution presentationJerk;
    bool havePresented = false;
    uint64_t priorSubmissionUs = 0;
    uint32_t priorPresentedRtp = 0;
    bool priorPresentedRtpValid = false;
    int64_t priorSpacingErrorUs = 0;
    bool havePriorSpacingError = false;

    auto admitThrough = [&](uint64_t boundaryUs) {
        while (nextArrival < capture.frames.size() &&
                capture.frames[nextArrival].arrivalUs <= boundaryUs) {
            if (queue.size() >= queueCapacity) {
                queue.pop_front();
                ++capacityDrops;
            }
            queue.push_back(nextArrival++);
            maximumQueueDepth = std::max<uint64_t>(maximumQueueDepth,
                                                    queue.size());
            queueDepth.values.push_back(queue.size());
        }
    };

    while (nextArrival < capture.frames.size() || !queue.empty()) {
        if (queue.empty()) {
            nowUs = std::max(nowUs, capture.frames[nextArrival].arrivalUs);
        }
        admitThrough(nowUs);
        if (queue.empty()) continue;

        const InputFrame& input = capture.frames[queue.front()];
        queue.pop_front();
        const uint64_t decisionUs = addSaturated(nowUs,
                                                  capture.decisionCallUs);
        PacedFrame frame(nullptr, input.number, input.rtp, input.rtpValid,
                         input.decodeUs);
        const VrrTimingDecision decision = controller.schedule(frame, decisionUs);
        const bool metronome = scenario.controller.playoutMetronomeEnabled != 0;
        const uint64_t agePeriods = metronome ? 4 : 2;
        const uint64_t maximumAgeUs = decision.sourcePeriodUs >
                std::numeric_limits<uint64_t>::max() / agePeriods ?
            std::numeric_limits<uint64_t>::max() :
            decision.sourcePeriodUs * agePeriods;
        const uint64_t ageUs = decisionUs >= input.decodeUs ?
            decisionUs - input.decodeUs : 0;
        if (!queue.empty() && decision.sourcePeriodUs != 0 &&
                (ageUs > maximumAgeUs ||
                 (metronome && decision.missedTicks != 0))) {
            ++staleDrops;
            controller.noteSubmission(false, false, 0);
            nowUs = decisionUs;
            continue;
        }

        uint64_t renderStartUs = std::max(decisionUs, decision.renderStartUs);
        admitThrough(renderStartUs);
        if (!queue.empty() && maximumAgeUs != 0 &&
                renderStartUs > addSaturated(decision.targetUs, maximumAgeUs)) {
            ++staleDrops;
            controller.noteSubmission(false, false, 0);
            nowUs = renderStartUs;
            continue;
        }

        const uint64_t preparationUs = capture.preparationUs[
            serviceOrdinal % capture.preparationUs.size()];
        const uint64_t presentCallUs = capture.presentCallUs[
            serviceOrdinal % capture.presentCallUs.size()];
        ++serviceOrdinal;
        const uint64_t preparationEndUs = addSaturated(renderStartUs,
                                                        preparationUs);
        admitThrough(preparationEndUs);
        controller.notePreparationDuration(preparationUs);
        controller.noteSpacingDeficit(0);
        uint64_t submissionUs = std::max(preparationEndUs, decision.targetUs);
        submissionUs = std::max(submissionUs, controller.earliestSubmissionUs());
        admitThrough(submissionUs);
        controller.noteSubmission(true, false, submissionUs);
        ++presented;
        decodeLatency.values.push_back(submissionUs >= input.decodeUs ?
            submissionUs - input.decodeUs : 0);

        if (havePresented) {
            const uint64_t presentationInterval = submissionUs - priorSubmissionUs;
            presentationIntervals.values.push_back(presentationInterval);
            if (input.rtpValid && priorPresentedRtpValid) {
                const uint64_t sourceInterval = rtpDeltaUs(priorPresentedRtp,
                                                           input.rtp);
                const int64_t spacingError =
                    static_cast<int64_t>(presentationInterval) -
                    static_cast<int64_t>(sourceInterval);
                senderSpacingError.values.push_back(static_cast<uint64_t>(
                    std::llabs(spacingError)));
                if (havePriorSpacingError) {
                    presentationJerk.values.push_back(static_cast<uint64_t>(
                        std::llabs(spacingError - priorSpacingErrorUs)));
                }
                priorSpacingErrorUs = spacingError;
                havePriorSpacingError = true;
            }
        }
        havePresented = true;
        priorSubmissionUs = submissionUs;
        priorPresentedRtp = input.rtp;
        priorPresentedRtpValid = input.rtpValid;
        nowUs = addSaturated(submissionUs, presentCallUs);
        admitThrough(nowUs);
    }

    QJsonObject result;
    result["scenario"] = scenario.name;
    result["queue_capacity"] = static_cast<qint64>(queueCapacity);
    result["arrivals"] = static_cast<qint64>(capture.frames.size());
    result["presented"] = static_cast<qint64>(presented);
    result["capacity_drops"] = static_cast<qint64>(capacityDrops);
    result["stale_drops"] = static_cast<qint64>(staleDrops);
    result["total_drops"] = static_cast<qint64>(capacityDrops + staleDrops);
    result["drop_percent"] = capture.frames.empty() ? 0.0 :
        100.0 * static_cast<double>(capacityDrops + staleDrops) /
            static_cast<double>(capture.frames.size());
    result["maximum_queue_depth"] = static_cast<qint64>(maximumQueueDepth);
    result["queue_depth"] = queueDepth.json();
    result["decode_to_submission_us"] = decodeLatency.json();
    result["sender_spacing_error_us"] = senderSpacingError.json();
    result["presentation_interval_us"] = presentationIntervals.json();
    result["presentation_jerk_us"] = presentationJerk.json();
    result["resolved_controller"] = vrrTimingParametersToJson(scenario.controller);
    return result;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName("vrrqueuesim");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Replay every captured arrival through the real VRR controller and bounded worker queue");
    parser.addHelpOption();
    parser.addPositionalArgument("csv", "Expanded VRR trace CSV");
    QCommandLineOption configOption(
        "config", "Replay scenario configuration", "json");
    QCommandLineOption capacityOption(
        "queue-capacity", "Override queue capacity", "frames");
    QCommandLineOption outputOption(
        "output", "Write JSON output", "json");
    parser.addOption(configOption);
    parser.addOption(capacityOption);
    parser.addOption(outputOption);
    parser.process(application);
    if (parser.positionalArguments().size() != 1) parser.showHelp(1);

    Capture capture;
    QString error;
    if (!loadCapture(parser.positionalArguments().front(), capture, error)) {
        qCritical("Unable to load capture: %s", qPrintable(error));
        return 1;
    }

    VrrReplayConfiguration configuration;
    if (parser.isSet(configOption)) {
        QFile configFile(parser.value(configOption));
        if (!configFile.open(QIODevice::ReadOnly) ||
                !loadVrrReplayConfiguration(configFile.readAll(),
                                            configuration, error)) {
            qCritical("Unable to load configuration: %s", qPrintable(error));
            return 1;
        }
    }
    else {
        configuration.scenarios.append(VrrReplayScenario {});
    }

    bool capacityOk = false;
    const qulonglong capacityOverride = parser.value(capacityOption).
        toULongLong(&capacityOk);
    if (parser.isSet(capacityOption) && (!capacityOk || capacityOverride == 0)) {
        qCritical("queue capacity must be a positive integer");
        return 1;
    }

    QJsonArray scenarios;
    for (const VrrReplayScenario& scenario : configuration.scenarios) {
        scenarios.append(simulate(
            capture, scenario,
            parser.isSet(capacityOption) ?
                static_cast<size_t>(capacityOverride) :
                scenario.worker.queueCapacity));
    }
    QJsonObject root;
    root["model"] = "all-arrival-event-driven-worker-v1";
    root["trace"] = parser.positionalArguments().front();
    root["display_hz"] = capture.session.displayRefreshHz;
    root["stream_fps"] = capture.session.streamRateHz;
    root["decision_call_us"] = static_cast<qint64>(capture.decisionCallUs);
    root["preparation_samples"] = static_cast<qint64>(
        capture.preparationUs.size());
    root["present_call_samples"] = static_cast<qint64>(
        capture.presentCallUs.size());
    root["scenarios"] = scenarios;
    const QByteArray output = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (parser.isSet(outputOption)) {
        QFile file(parser.value(outputOption));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                file.write(output) != output.size()) {
            qCritical("Unable to write output: %s", qPrintable(file.errorString()));
            return 1;
        }
    }
    else {
        std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
    }
    return 0;
}
