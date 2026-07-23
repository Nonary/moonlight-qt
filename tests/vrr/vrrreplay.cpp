#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include "vrrreplayconfig.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QProcess>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

namespace {

constexpr char kTraceMagic[] = "MLVRR1\n";

class TraceReader {
public:
    explicit TraceReader(const QString& path) : m_File(path) {}

    bool open(QString& error)
    {
        if (!m_File.open(QIODevice::ReadOnly)) {
            error = m_File.errorString();
            return false;
        }
        const QByteArray magic = m_File.read(sizeof(kTraceMagic) - 1);
        m_Compressed = magic == kTraceMagic;
        if (!m_Compressed && !m_File.seek(0)) {
            error = m_File.errorString();
            return false;
        }
        return true;
    }

    bool readLine(QByteArray& line, QString& error)
    {
        if (!m_Compressed) {
            line = m_File.readLine();
            return !line.isEmpty();
        }

        while (m_ChunkOffset >= m_Chunk.size()) {
            char lengthBytes[4];
            const qint64 lengthRead = m_File.read(lengthBytes, 4);
            if (lengthRead == 0) {
                return false;
            }
            if (lengthRead != 4) {
                error = "partial compressed chunk length";
                return false;
            }
            const auto* bytes = reinterpret_cast<const unsigned char*>(
                lengthBytes);
            const uint32_t compressedBytes =
                static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) |
                (static_cast<uint32_t>(bytes[3]) << 24);
            const QByteArray encoded = m_File.read(compressedBytes);
            if (encoded.size() != static_cast<int>(compressedBytes)) {
                error = "partial compressed chunk payload";
                return false;
            }
            m_Chunk = qUncompress(
                reinterpret_cast<const uchar*>(encoded.constData()),
                encoded.size());
            m_ChunkOffset = 0;
            if (m_Chunk.isEmpty()) {
                error = "invalid compressed chunk";
                return false;
            }
        }

        const int newline = m_Chunk.indexOf('\n', m_ChunkOffset);
        if (newline < 0) {
            error = "compressed chunk ends in a partial CSV row";
            return false;
        }
        line = m_Chunk.mid(m_ChunkOffset, newline - m_ChunkOffset + 1);
        m_ChunkOffset = newline + 1;
        return true;
    }

private:
    QFile m_File;
    bool m_Compressed = false;
    QByteArray m_Chunk;
    int m_ChunkOffset = 0;
};

struct Columns {
    int traceSchema = -1;
    int arrivalSequence = -1;
    int frame = -1;
    int rtpTimestamp = -1;
    int rtpValid = -1;
    int decodeCompleteUs = -1;
    int pacerArrivalUs = -1;
    int queueDepthBefore = -1;
    int queueDepthAfter = -1;
    int queueAccepted = -1;
    int dequeueUs = -1;
    int queueDiscontinuity = -1;
    int decisionValid = -1;
    int decisionUs = -1;
    int displayRefreshHz = -1;
    int streamRateHz = -1;
    int canLatch = -1;
    int sourceTimeUs = -1;
    int sourcePeriodUs = -1;
    int readinessBudgetUs = -1;
    int timingBudgetUs = -1;
    int renderLeadUs = -1;
    int renderWakeLeadUs = -1;
    int targetWakeLeadUs = -1;
    int guardUs = -1;
    int headroomUs = -1;
    int renderStartUs = -1;
    int preparationStartUs = -1;
    int preparationEndUs = -1;
    int preparationUs = -1;
    int renderWaitOvershootUs = -1;
    int targetSchedulerDelayUs = -1;
    int targetSchedulerDelayValid = -1;
    int recordedTargetUs = -1;
    int submissionBoundaryUs = -1;
    int presentCallUs = -1;
    int submitErrorUs = -1;
    int submissionSpacingUs = -1;
    int spacingMarginUs = -1;
    int spacingGuardFeedbackUs = -1;
    int presented = -1;
    int cancelled = -1;
    int disposition = -1;
    int dropped = -1;
    int tearClassification = -1;
    int tearRisk = -1;
    int submissionIdValid = -1;
    int submissionId = -1;
    int latchValid = -1;
    int latchSubmissionId = -1;
    int latchTimeUs = -1;
    int latchPresentRefreshSequence = -1;
    int latchSyncRefreshSequence = -1;
    int latchedPresent = -1;
    int phaseDiscontinuity = -1;
    int rebased = -1;
    int deepTrace = -1;
    int nativePresentTimingValid = -1;
    int nativePresentCallUs = -1;
    int presentCountBeforeValid = -1;
    int frameStatsBeforeValid = -1;
    int gpuReadyTimingValid = -1;
    int gpuReadyWaitUs = -1;
    int controllerCallUs = -1;
    int staleAgeUs = -1;
    int renderWaitEntryUs = -1;
    int renderWaitFinalUs = -1;
    int targetWaitEntryUs = -1;
    int targetWaitFinalUs = -1;
    int correctionWaitStartUs = -1;
    int correctionWaitEndUs = -1;
    int appliedReadinessReserveUs = -1;
    QMap<QString, int> capturedParameterColumns;

    bool resolve(const QList<QByteArray>& header, QString& error)
    {
        const auto find = [&header](const char* name) {
            return header.indexOf(name);
        };
        traceSchema = find("trace_schema");
        arrivalSequence = find("arrival_sequence");
        frame = find("frame");
        rtpTimestamp = find("rtp_timestamp");
        rtpValid = find("rtp_valid");
        decodeCompleteUs = find("decode_complete_us");
        pacerArrivalUs = find("pacer_arrival_us");
        queueDepthBefore = find("arrival_queue_depth_before");
        queueDepthAfter = find("arrival_queue_depth_after");
        queueAccepted = find("queue_accepted");
        dequeueUs = find("dequeue_us");
        queueDiscontinuity = find("queue_discontinuity");
        decisionValid = find("decision_valid");
        decisionUs = find("decision_us");
        displayRefreshHz = find("display_refresh_hz");
        streamRateHz = find("stream_rate_hz");
        canLatch = find("can_latch_present");
        sourceTimeUs = find("source_time_us");
        sourcePeriodUs = find("source_period_us");
        readinessBudgetUs = find("readiness_budget_us");
        timingBudgetUs = find("timing_budget_us");
        renderLeadUs = find("render_lead_us");
        renderWakeLeadUs = find("render_wake_lead_us");
        targetWakeLeadUs = find("target_wake_lead_us");
        guardUs = find("guard_us");
        headroomUs = find("headroom_us");
        renderStartUs = find("render_start_us");
        preparationStartUs = find("prepare_start_us");
        preparationEndUs = find("prepare_end_us");
        preparationUs = find("prepare_us");
        renderWaitOvershootUs = find("render_wait_overshoot_us");
        targetSchedulerDelayUs = find("target_scheduler_delay_us");
        targetSchedulerDelayValid = find("target_scheduler_delay_valid");
        recordedTargetUs = find("target_us");
        submissionBoundaryUs = find("submission_boundary_us");
        presentCallUs = find("present_call_us");
        submitErrorUs = find("submit_error_us");
        submissionSpacingUs = find("submission_spacing_us");
        spacingMarginUs = find("spacing_margin_us");
        spacingGuardFeedbackUs = find("spacing_guard_feedback_us");
        presented = find("presented");
        cancelled = find("cancelled");
        disposition = find("disposition");
        dropped = find("dropped");
        tearClassification = find("tear_classification");
        tearRisk = find("tear_risk");
        submissionIdValid = find("submission_id_valid");
        submissionId = find("submission_id");
        latchValid = find("latch_valid");
        latchSubmissionId = find("latch_submission_id");
        latchTimeUs = find("latch_time_us");
        latchPresentRefreshSequence = find("latch_present_refresh_seq");
        latchSyncRefreshSequence = find("latch_sync_refresh_seq");
        latchedPresent = find("latched_present");
        phaseDiscontinuity = find("phase_discontinuity");
        rebased = find("rebased");
        deepTrace = find("deep_trace");
        nativePresentTimingValid = find("native_present_timing_valid");
        nativePresentCallUs = find("native_present_call_us");
        presentCountBeforeValid = find("present_count_before_valid");
        frameStatsBeforeValid = find("frame_stats_before_valid");
        gpuReadyTimingValid = find("gpu_ready_timing_valid");
        gpuReadyWaitUs = find("gpu_ready_wait_us");
        controllerCallUs = find("controller_call_us");
        staleAgeUs = find("stale_age_us");
        renderWaitEntryUs = find("render_wait_entry_us");
        renderWaitFinalUs = find("render_wait_final_us");
        targetWaitEntryUs = find("target_wait_entry_us");
        targetWaitFinalUs = find("target_wait_final_us");
        correctionWaitStartUs = find("correction_wait_start_us");
        correctionWaitEndUs = find("correction_wait_end_us");
        appliedReadinessReserveUs = find("applied_readiness_reserve_us");
        for (const QString& path : vrrReplayParameterNames()) {
            if (!path.startsWith("controller.")) continue;
            const QString key = path.mid(QString("controller.").size());
            const int column = header.indexOf(
                ("param_" + key).toLatin1());
            if (column >= 0) capturedParameterColumns.insert(path, column);
        }

        const int required[] = {
            traceSchema, arrivalSequence, frame, rtpTimestamp, rtpValid,
            decodeCompleteUs, pacerArrivalUs, queueDepthBefore,
            queueAccepted, dequeueUs, decisionValid,
            decisionUs, displayRefreshHz, streamRateHz, canLatch,
            sourceTimeUs, sourcePeriodUs, readinessBudgetUs, timingBudgetUs,
            renderLeadUs, renderWakeLeadUs, targetWakeLeadUs, guardUs,
            headroomUs, renderStartUs, preparationStartUs,
            preparationEndUs, preparationUs, renderWaitOvershootUs,
            targetSchedulerDelayUs, targetSchedulerDelayValid,
            recordedTargetUs, submissionBoundaryUs, presentCallUs,
            submitErrorUs, submissionSpacingUs, presented, cancelled,
            disposition, dropped, tearClassification, tearRisk,
            latchValid, latchSubmissionId,
            latchPresentRefreshSequence, nativePresentCallUs, gpuReadyWaitUs,
        };
        if (std::any_of(std::begin(required), std::end(required),
                        [](int column) { return column < 0; })) {
            error = "trace schema is missing exact-simulation fields (schema 3, 4, or 5 required)";
            return false;
        }
        m_Maximum = header.size() - 1;
        return true;
    }

    int maximum() const { return m_Maximum; }

private:
    int m_Maximum = -1;
};

struct Distribution {
    static constexpr uint64_t kExactLimitUs = 100000;
    static constexpr uint64_t kMediumLimitUs = 1000000;
    static constexpr uint64_t kLongLimitUs = 60000000;
    static constexpr uint64_t kMediumBucketUs = 100;
    static constexpr uint64_t kLongBucketUs = 1000;
    static constexpr size_t kExactBuckets = kExactLimitUs + 1;
    static constexpr size_t kMediumBuckets =
        (kMediumLimitUs - kExactLimitUs + kMediumBucketUs - 1) /
            kMediumBucketUs;
    static constexpr size_t kLongBuckets =
        (kLongLimitUs - kMediumLimitUs + kLongBucketUs - 1) /
            kLongBucketUs;
    static constexpr size_t kOverflowBucket =
        kExactBuckets + kMediumBuckets + kLongBuckets;
    static constexpr size_t kBucketCount = kOverflowBucket + 1;

    uint64_t count = 0;
    uint64_t minimum = 0;
    uint64_t maximum = 0;
    long double mean = 0;
    long double squaredDifferenceTotal = 0;
    std::vector<uint32_t> histogram;

    static size_t bucketFor(uint64_t value)
    {
        if (value <= kExactLimitUs) {
            return static_cast<size_t>(value);
        }
        if (value <= kMediumLimitUs) {
            return kExactBuckets + static_cast<size_t>(
                (value - kExactLimitUs - 1) / kMediumBucketUs);
        }
        if (value <= kLongLimitUs) {
            return kExactBuckets + kMediumBuckets + static_cast<size_t>(
                (value - kMediumLimitUs - 1) / kLongBucketUs);
        }
        return kOverflowBucket;
    }

    uint64_t bucketUpperBound(size_t bucket) const
    {
        if (bucket < kExactBuckets) {
            return static_cast<uint64_t>(bucket);
        }
        if (bucket < kExactBuckets + kMediumBuckets) {
            const uint64_t offset = static_cast<uint64_t>(
                bucket - kExactBuckets + 1);
            return std::min(kMediumLimitUs,
                            kExactLimitUs + offset * kMediumBucketUs);
        }
        if (bucket < kOverflowBucket) {
            const uint64_t offset = static_cast<uint64_t>(
                bucket - kExactBuckets - kMediumBuckets + 1);
            return std::min(kLongLimitUs,
                            kMediumLimitUs + offset * kLongBucketUs);
        }
        return maximum;
    }

    void add(uint64_t value)
    {
        if (count == 0) {
            minimum = value;
            maximum = value;
            histogram.resize(kBucketCount);
        }
        else {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        ++count;
        const long double delta = static_cast<long double>(value) - mean;
        mean += delta / static_cast<long double>(count);
        const long double adjustedDelta =
            static_cast<long double>(value) - mean;
        squaredDifferenceTotal += delta * adjustedDelta;
        ++histogram[bucketFor(value)];
    }

    void addElapsed(uint64_t endUs, uint64_t startUs)
    {
        if (startUs != 0 && endUs >= startUs) {
            add(endUs - startUs);
        }
    }

    uint64_t percentile(unsigned int percent) const
    {
        if (count == 0) {
            return 0;
        }
        const uint64_t boundedPercent = std::min(100U, percent);
        const uint64_t rank = std::max<uint64_t>(
            1, (count * boundedPercent + 99) / 100);
        uint64_t cumulative = 0;
        for (size_t bucket = 0; bucket < histogram.size(); ++bucket) {
            cumulative += histogram[bucket];
            if (cumulative >= rank) {
                return bucketUpperBound(bucket);
            }
        }
        return maximum;
    }
};

// Rate-band and paired-delta diagnostics are supplemental. Keep their memory
// fixed even for multi-hour or unusually high-rate traces while retaining
// exact count/mean/stddev/min/max and a deterministic quantile sample.
constexpr size_t kMaximumDiagnosticSamples = 32768;
constexpr size_t kMaximumPendingSubmissionBands = 4096;

struct BoundedDistribution {
    uint64_t count = 0;
    uint64_t minimum = 0;
    uint64_t maximum = 0;
    long double mean = 0;
    long double squaredDifferenceTotal = 0;
    std::vector<uint64_t> samples;

    static uint64_t sampleHash(uint64_t value)
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    void add(uint64_t value)
    {
        if (count == 0) {
            minimum = value;
            maximum = value;
            samples.reserve(kMaximumDiagnosticSamples);
        }
        else {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        ++count;
        const long double delta = static_cast<long double>(value) - mean;
        mean += delta / static_cast<long double>(count);
        const long double adjustedDelta =
            static_cast<long double>(value) - mean;
        squaredDifferenceTotal += delta * adjustedDelta;

        if (samples.size() < kMaximumDiagnosticSamples) {
            samples.push_back(value);
            return;
        }
        const uint64_t selected = sampleHash(count) % count;
        if (selected < kMaximumDiagnosticSamples) {
            samples[static_cast<size_t>(selected)] = value;
        }
    }

    void addElapsed(uint64_t endUs, uint64_t startUs)
    {
        if (startUs != 0 && endUs >= startUs) {
            add(endUs - startUs);
        }
    }
};

struct SignedAccumulator {
    uint64_t count = 0;
    int64_t minimum = 0;
    int64_t maximum = 0;
    long double mean = 0;
    long double squaredDifferenceTotal = 0;
    uint64_t negative = 0;
    uint64_t zero = 0;
    uint64_t positive = 0;

    void add(int64_t value)
    {
        if (count == 0) {
            minimum = value;
            maximum = value;
        }
        else {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        ++count;
        const long double delta = static_cast<long double>(value) - mean;
        mean += delta / static_cast<long double>(count);
        const long double adjustedDelta =
            static_cast<long double>(value) - mean;
        squaredDifferenceTotal += delta * adjustedDelta;
        negative += value < 0 ? 1 : 0;
        zero += value == 0 ? 1 : 0;
        positive += value > 0 ? 1 : 0;
    }
};

enum RateBandIndex : size_t {
    UnknownRate,
    Below40Fps,
    Fps40To49,
    Fps50To59,
    Fps60To69,
    Fps70To79,
    Fps80To89,
    Fps90To100,
    Fps101To109,
    Fps110To116,
    Above116Fps,
    Fps40To116,
    Fps60To100,
    RateBandCount,
};

constexpr std::array<const char*, RateBandCount> kRateBandNames {
    "unknown", "below_40", "40_49", "50_59", "60_69", "70_79",
    "80_89", "90_100", "101_109", "110_116", "above_116",
    "40_116", "60_100",
};

struct CadenceBandMetrics {
    uint64_t presentedFrames = 0;
    uint64_t cadenceTransitions = 0;
    uint64_t modelledIntervalViolations = 0;
    uint64_t scanoutAnomalies = 0;
    uint64_t repeatedRefreshes = 0;
    BoundedDistribution decodeToSubmission;
    BoundedDistribution absoluteCadenceResidual;
    BoundedDistribution absoluteJerk;
    SignedAccumulator signedCadenceResidual;
};

constexpr uint64_t kJerkAnomalyThresholdUs = 4000;

struct AnomalyWindowMetrics {
    uint64_t anomalies = 0;
    uint64_t consecutive = 0;
    uint64_t longestConsecutive = 0;
    uint64_t worstOneSecond = 0;
    uint64_t worstTenSeconds = 0;
    uint64_t worstSixtySeconds = 0;
    std::deque<uint64_t> oneSecond;
    std::deque<uint64_t> tenSeconds;
    std::deque<uint64_t> sixtySeconds;

    static void appendWindow(std::deque<uint64_t>& values,
                             uint64_t nowUs, uint64_t durationUs,
                             uint64_t& maximum)
    {
        while (!values.empty() && nowUs >= values.front() &&
                nowUs - values.front() >= durationUs) {
            values.pop_front();
        }
        values.push_back(nowUs);
        maximum = std::max(maximum,
                           static_cast<uint64_t>(values.size()));
    }

    void observe(uint64_t timestampUs, bool anomalous)
    {
        if (!anomalous) {
            consecutive = 0;
            return;
        }
        ++anomalies;
        ++consecutive;
        longestConsecutive = std::max(longestConsecutive, consecutive);
        appendWindow(oneSecond, timestampUs, 1000000ULL, worstOneSecond);
        appendWindow(tenSeconds, timestampUs, 10000000ULL, worstTenSeconds);
        appendWindow(sixtySeconds, timestampUs, 60000000ULL,
                     worstSixtySeconds);
    }
};

struct Metrics {
    uint64_t delivered = 0;
    uint64_t scheduled = 0;
    uint64_t traceSchema = 0;
    uint64_t firstArrivalSequence = 0;
    uint64_t lastArrivalSequence = 0;
    uint64_t firstArrivalUs = 0;
    uint64_t lastArrivalUs = 0;
    uint64_t originalDrops = 0;
    uint64_t originalTearRisks = 0;
    uint64_t simulatedTearRisks = 0;
    uint64_t simulatedLatchedFrames = 0;
    uint64_t scanoutAnomalies = 0;
    uint64_t repeatedRefreshes = 0;
    uint64_t exactReferenceTargets = 0;
    uint64_t exactSimulatedSubmissions = 0;
    uint64_t exactTearClassifications = 0;
    uint64_t invalidExecutionResiduals = 0;
    uint64_t workerArrivals = 0;
    uint64_t workerAccepted = 0;
    uint64_t workerCapacityEvictions = 0;
    uint64_t deepTraceRows = 0;
    uint64_t submissionIdValidRows = 0;
    uint64_t latchValidRows = 0;
    uint64_t uniqueLatchSamples = 0;
    uint64_t staleLatchSamples = 0;
    uint64_t submissionSequenceResets = 0;
    uint64_t latchSequenceResets = 0;
    uint64_t nativePresentTimingValidRows = 0;
    uint64_t presentCountBeforeValidRows = 0;
    uint64_t frameStatsBeforeValidRows = 0;
    uint64_t gpuReadyTimingValidRows = 0;
    QMap<QByteArray, uint64_t> dispositions;
    QMap<QByteArray, uint64_t> tearClassifications;
    QMap<QByteArray, uint64_t> simulatedTearClassifications;
    Distribution observedDecodeToArrival;
    Distribution observedArrivalToDequeue;
    Distribution observedDequeueToDecision;
    Distribution observedDecodeToSubmission;
    Distribution observedArrivalToSubmission;
    Distribution observedDecisionToSubmission;
    Distribution observedProjectedSourceToSubmission;
    Distribution observedSubmissionSpacing;
    Distribution observedAbsoluteSubmitError;
    Distribution observedPreparation;
    Distribution observedPresentCall;
    Distribution observedNativePresentCall;
    Distribution observedGpuReadyWait;
    Distribution observedControllerCall;
    Distribution observedStaleAge;
    Distribution observedRenderWait;
    Distribution observedTargetWait;
    Distribution observedCorrectionWait;
    Distribution simulatedDecodeToSubmission;
    Distribution simulatedArrivalToSubmission;
    Distribution simulatedDecisionToSubmission;
    Distribution simulatedProjectedSourceToSubmission;
    Distribution simulatedSubmissionSpacing;
    Distribution simulatedAbsoluteSubmitError;
    Distribution referenceTargetDrift;
    Distribution referenceSourceTimeDrift;
    Distribution referenceSourcePeriodDrift;
    Distribution referenceReadinessBudgetDrift;
    Distribution referenceTimingBudgetDrift;
    Distribution referenceRenderLeadDrift;
    Distribution referenceRenderWakeLeadDrift;
    Distribution referenceTargetWakeLeadDrift;
    Distribution referenceGuardDrift;
    Distribution referenceHeadroomDrift;
    Distribution referenceRenderStartDrift;
    Distribution simulatedTargetDrift;
    Distribution simulatedSubmissionDrift;
    Distribution simulatedCadenceError;
    BoundedDistribution pairedAbsoluteSubmissionDelta;
    SignedAccumulator pairedSubmissionDelta;
    std::array<CadenceBandMetrics, RateBandCount> observedRateBands;
    std::array<CadenceBandMetrics, RateBandCount> simulatedRateBands;
    AnomalyWindowMetrics observedJerkAnomalies;
    AnomalyWindowMetrics simulatedJerkAnomalies;
};

uint64_t unsignedField(const QList<QByteArray>& fields, int column)
{
    return fields[column].toULongLong();
}

int64_t signedField(const QList<QByteArray>& fields, int column)
{
    return fields[column].toLongLong();
}

uint64_t optionalUnsignedField(const QList<QByteArray>& fields, int column)
{
    return column >= 0 && column < fields.size() ?
        fields[column].toULongLong() : 0;
}

int64_t optionalSignedField(const QList<QByteArray>& fields, int column)
{
    return column >= 0 && column < fields.size() ?
        fields[column].toLongLong() : 0;
}

uint64_t absoluteValue(int64_t value)
{
    if (value >= 0) {
        return static_cast<uint64_t>(value);
    }
    return static_cast<uint64_t>(-(value + 1)) + 1;
}

int64_t signedDifference(uint64_t left, uint64_t right)
{
    if (left >= right) {
        const uint64_t difference = left - right;
        return difference > static_cast<uint64_t>(
            std::numeric_limits<int64_t>::max()) ?
                std::numeric_limits<int64_t>::max() :
                static_cast<int64_t>(difference);
    }
    const uint64_t difference = right - left;
    return difference > static_cast<uint64_t>(
        std::numeric_limits<int64_t>::max()) ?
            std::numeric_limits<int64_t>::min() :
            -static_cast<int64_t>(difference);
}

struct CadenceSample {
    bool valid = false;
    bool jerkValid = false;
    uint64_t sourceElapsedUs = 0;
    uint64_t submissionElapsedUs = 0;
    int64_t residualUs = 0;
    int64_t jerkUs = 0;
    int64_t phaseUs = 0;
};

struct CadenceTracker {
    bool havePresentation = false;
    bool haveResidual = false;
    uint64_t priorSourceUs = 0;
    uint64_t priorSubmissionUs = 0;
    int64_t priorResidualUs = 0;

    CadenceSample observe(uint64_t sourceUs, uint64_t submissionUs,
                          bool discontinuity)
    {
        CadenceSample sample;
        sample.phaseUs = signedDifference(submissionUs, sourceUs);
        if (!discontinuity && havePresentation && sourceUs >= priorSourceUs &&
                submissionUs >= priorSubmissionUs) {
            sample.sourceElapsedUs = sourceUs - priorSourceUs;
            sample.submissionElapsedUs = submissionUs - priorSubmissionUs;
            if (sample.sourceElapsedUs != 0) {
                sample.valid = true;
                sample.residualUs = signedDifference(
                    sample.submissionElapsedUs, sample.sourceElapsedUs);
                if (haveResidual) {
                    sample.jerkValid = true;
                    sample.jerkUs = sample.residualUs - priorResidualUs;
                }
            }
        }

        havePresentation = true;
        priorSourceUs = sourceUs;
        priorSubmissionUs = submissionUs;
        if (sample.valid) {
            haveResidual = true;
            priorResidualUs = sample.residualUs;
        }
        else {
            haveResidual = false;
        }
        return sample;
    }
};

int roundedRateForPeriod(uint64_t periodUs)
{
    if (periodUs == 0) {
        return 0;
    }
    return static_cast<int>((1000000ULL + periodUs / 2) / periodUs);
}

RateBandIndex primaryRateBand(int rateHz)
{
    if (rateHz <= 0) return UnknownRate;
    if (rateHz < 40) return Below40Fps;
    if (rateHz < 50) return Fps40To49;
    if (rateHz < 60) return Fps50To59;
    if (rateHz < 70) return Fps60To69;
    if (rateHz < 80) return Fps70To79;
    if (rateHz < 90) return Fps80To89;
    if (rateHz <= 100) return Fps90To100;
    if (rateHz < 110) return Fps101To109;
    if (rateHz <= 116) return Fps110To116;
    return Above116Fps;
}

template<typename Function>
void forRateBands(int rateHz, Function function)
{
    function(primaryRateBand(rateHz));
    if (rateHz >= 40 && rateHz <= 116) {
        function(Fps40To116);
    }
    if (rateHz >= 60 && rateHz <= 100) {
        function(Fps60To100);
    }
}

void addCadenceFrame(std::array<CadenceBandMetrics, RateBandCount>& bands,
                     int rateHz, uint64_t decodeCompleteUs,
                     uint64_t submissionUs, const CadenceSample& sample,
                     bool intervalViolation)
{
    forRateBands(rateHz, [&](RateBandIndex index) {
        CadenceBandMetrics& band = bands[index];
        ++band.presentedFrames;
        band.decodeToSubmission.addElapsed(submissionUs, decodeCompleteUs);
        band.modelledIntervalViolations += intervalViolation ? 1 : 0;
        if (sample.valid) {
            ++band.cadenceTransitions;
            band.absoluteCadenceResidual.add(absoluteValue(
                sample.residualUs));
            band.signedCadenceResidual.add(sample.residualUs);
        }
        if (sample.jerkValid) {
            band.absoluteJerk.add(absoluteValue(sample.jerkUs));
        }
    });
}

void addScanoutOutcome(
    std::array<CadenceBandMetrics, RateBandCount>& bands, int rateHz,
    uint64_t anomalies, uint64_t repeatedRefreshes)
{
    forRateBands(rateHz, [&](RateBandIndex index) {
        bands[index].scanoutAnomalies += anomalies;
        bands[index].repeatedRefreshes += repeatedRefreshes;
    });
}

uint64_t saturatingAdd(uint64_t left, uint64_t right)
{
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

QJsonObject distributionObject(const Distribution& distribution)
{
    QJsonObject object;
    object["count"] = static_cast<qint64>(distribution.count);
    if (distribution.count == 0) {
        object["min"] = 0;
        object["mean"] = 0;
        object["stddev"] = 0;
        object["p50"] = 0;
        object["p90"] = 0;
        object["p95"] = 0;
        object["p99"] = 0;
        object["max"] = 0;
        return object;
    }
    object["min"] = static_cast<qint64>(distribution.minimum);
    object["mean"] = static_cast<double>(distribution.mean);
    object["stddev"] = static_cast<double>(std::sqrt(
        distribution.squaredDifferenceTotal /
        static_cast<long double>(distribution.count)));
    object["p50"] = static_cast<qint64>(distribution.percentile(50));
    object["p90"] = static_cast<qint64>(distribution.percentile(90));
    object["p95"] = static_cast<qint64>(distribution.percentile(95));
    object["p99"] = static_cast<qint64>(distribution.percentile(99));
    object["max"] = static_cast<qint64>(distribution.maximum);
    return object;
}

QJsonObject boundedDistributionObject(const BoundedDistribution& distribution)
{
    QJsonObject object;
    object["count"] = static_cast<qint64>(distribution.count);
    object["sample_count"] = static_cast<qint64>(
        distribution.samples.size());
    object["sample_capacity"] = static_cast<qint64>(
        kMaximumDiagnosticSamples);
    object["quantiles_approximate"] =
        distribution.count > distribution.samples.size();
    if (distribution.count == 0) {
        object["min"] = 0;
        object["mean"] = 0;
        object["stddev"] = 0;
        object["p50"] = 0;
        object["p90"] = 0;
        object["p95"] = 0;
        object["p99"] = 0;
        object["max"] = 0;
        return object;
    }

    std::vector<uint64_t> sortedValues = distribution.samples;
    std::sort(sortedValues.begin(), sortedValues.end());
    const auto sampledPercentile = [&sortedValues](unsigned int percent) {
        const size_t rank = std::max<size_t>(
            1, (sortedValues.size() * std::min(100U, percent) + 99) / 100);
        return sortedValues[rank - 1];
    };
    object["min"] = static_cast<qint64>(distribution.minimum);
    object["mean"] = static_cast<double>(distribution.mean);
    object["stddev"] = static_cast<double>(std::sqrt(
        distribution.squaredDifferenceTotal /
        static_cast<long double>(distribution.count)));
    object["p50"] = static_cast<qint64>(sampledPercentile(50));
    object["p90"] = static_cast<qint64>(sampledPercentile(90));
    object["p95"] = static_cast<qint64>(sampledPercentile(95));
    object["p99"] = static_cast<qint64>(sampledPercentile(99));
    object["max"] = static_cast<qint64>(distribution.maximum);
    return object;
}

QJsonObject signedAccumulatorObject(const SignedAccumulator& accumulator)
{
    QJsonObject object;
    object["count"] = static_cast<qint64>(accumulator.count);
    object["min"] = static_cast<qint64>(
        accumulator.count == 0 ? 0 : accumulator.minimum);
    object["mean"] = accumulator.count == 0 ? 0.0 :
        static_cast<double>(accumulator.mean);
    object["stddev"] = accumulator.count == 0 ? 0.0 :
        static_cast<double>(std::sqrt(
            accumulator.squaredDifferenceTotal /
            static_cast<long double>(accumulator.count)));
    object["max"] = static_cast<qint64>(
        accumulator.count == 0 ? 0 : accumulator.maximum);
    object["negative"] = static_cast<qint64>(accumulator.negative);
    object["zero"] = static_cast<qint64>(accumulator.zero);
    object["positive"] = static_cast<qint64>(accumulator.positive);
    return object;
}

QJsonObject cadenceBandObject(const CadenceBandMetrics& band)
{
    QJsonObject object;
    object["presented_frames"] = static_cast<qint64>(band.presentedFrames);
    object["cadence_transitions"] = static_cast<qint64>(
        band.cadenceTransitions);
    object["decode_to_submission_us"] = boundedDistributionObject(
        band.decodeToSubmission);
    object["absolute_cadence_residual_us"] = boundedDistributionObject(
        band.absoluteCadenceResidual);
    object["signed_cadence_residual_us"] = signedAccumulatorObject(
        band.signedCadenceResidual);
    object["absolute_jerk_us"] = boundedDistributionObject(
        band.absoluteJerk);
    object["modelled_interval_violations"] = static_cast<qint64>(
        band.modelledIntervalViolations);
    object["scanout_anomalies"] = static_cast<qint64>(
        band.scanoutAnomalies);
    object["repeated_refreshes"] = static_cast<qint64>(
        band.repeatedRefreshes);
    object["scanout_anomalies_per_10000_presented"] =
        band.presentedFrames == 0 ? 0.0 :
            static_cast<double>(band.scanoutAnomalies) * 10000.0 /
                static_cast<double>(band.presentedFrames);
    return object;
}

QJsonObject cadenceBandsObject(
    const std::array<CadenceBandMetrics, RateBandCount>& bands)
{
    QJsonObject object;
    for (size_t i = 0; i < bands.size(); ++i) {
        object[kRateBandNames[i]] = cadenceBandObject(bands[i]);
    }
    return object;
}

QJsonObject anomalyWindowObject(const AnomalyWindowMetrics& metrics)
{
    QJsonObject object;
    object["threshold_us"] = static_cast<qint64>(kJerkAnomalyThresholdUs);
    object["events"] = static_cast<qint64>(metrics.anomalies);
    object["longest_consecutive_run"] = static_cast<qint64>(
        metrics.longestConsecutive);
    object["worst_1s_count"] = static_cast<qint64>(metrics.worstOneSecond);
    object["worst_10s_count"] = static_cast<qint64>(metrics.worstTenSeconds);
    object["worst_60s_count"] = static_cast<qint64>(
        metrics.worstSixtySeconds);
    return object;
}

QJsonObject validityObject(uint64_t valid, uint64_t eligible)
{
    QJsonObject object;
    object["valid"] = static_cast<qint64>(valid);
    object["eligible"] = static_cast<qint64>(eligible);
    object["coverage_percent"] = eligible == 0 ? 0.0 :
        static_cast<double>(valid) * 100.0 / static_cast<double>(eligible);
    return object;
}

QJsonObject countObject(const QMap<QByteArray, uint64_t>& counts)
{
    QJsonObject object;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        object[QString::fromUtf8(it.key())] = static_cast<qint64>(it.value());
    }
    return object;
}

uint64_t periodForRate(int rateHz)
{
    if (rateHz <= 0) {
        return 16667;
    }
    return (1000000ULL + static_cast<uint64_t>(rateHz) / 2) /
        static_cast<uint64_t>(rateHz);
}

QByteArray simulatedTearClassification(bool presented, bool latched,
                                       bool canLatch,
                                       bool hadPriorSubmission,
                                       uint64_t submissionUs,
                                       uint64_t priorSubmissionUs,
                                       uint64_t displayPeriodUs)
{
    if (!presented) {
        return "not_presented";
    }
    if (latched && canLatch) {
        return "confirmed_safe_latched";
    }
    if (!hadPriorSubmission) {
        return "first_submission_unknown";
    }
    const uint64_t spacingUs = submissionUs >= priorSubmissionUs ?
        submissionUs - priorSubmissionUs : 0;
    return spacingUs < displayPeriodUs ?
        QByteArray("adaptive_interval_violation") :
        QByteArray("adaptive_interval_safe");
}

QJsonObject summaryObject(const Metrics& metrics, qint64 elapsedMs,
                          const QString& tracePath, qint64 traceBytes,
                          int capturedDisplayHz, int capturedStreamFps,
                          int simulatedDisplayHz, int simulatedStreamFps,
                          bool simulatedCanLatch,
                          const VrrReplayScenario& scenario)
{
    const uint64_t captureDurationUs =
        metrics.lastArrivalUs >= metrics.firstArrivalUs ?
            metrics.lastArrivalUs - metrics.firstArrivalUs : 0;
    const bool arrivalSequenceComplete = metrics.delivered != 0 &&
        metrics.firstArrivalSequence == 1 &&
        metrics.lastArrivalSequence == metrics.delivered;
    const uint64_t expectedArrivalRows = metrics.lastArrivalSequence >=
            metrics.firstArrivalSequence && metrics.firstArrivalSequence != 0 ?
        metrics.lastArrivalSequence - metrics.firstArrivalSequence + 1 : 0;
    const uint64_t missingTraceRows = expectedArrivalRows > metrics.delivered ?
        expectedArrivalRows - metrics.delivered : 0;

    QJsonObject capture;
    capture["trace"] = QFileInfo(tracePath).fileName();
    capture["trace_bytes"] = traceBytes;
    capture["schema"] = static_cast<qint64>(metrics.traceSchema);
    capture["duration_us"] = static_cast<qint64>(captureDurationUs);
    capture["duration_seconds"] = static_cast<double>(captureDurationUs) /
        1000000.0;
    capture["delivered_frames"] = static_cast<qint64>(metrics.delivered);
    capture["scheduled_frames"] = static_cast<qint64>(metrics.scheduled);
    capture["arrival_sequence_first"] = static_cast<qint64>(
        metrics.firstArrivalSequence);
    capture["arrival_sequence_last"] = static_cast<qint64>(
        metrics.lastArrivalSequence);
    capture["arrival_sequence_complete"] = arrivalSequenceComplete;
    capture["missing_trace_rows"] = static_cast<qint64>(missingTraceRows);
    capture["display_hz"] = capturedDisplayHz;
    capture["stream_fps"] = capturedStreamFps;

    const uint64_t presentedFrames =
        metrics.observedDecodeToSubmission.count;
    QJsonObject telemetryCoverage;
    telemetryCoverage["deep_trace_rows"] = validityObject(
        metrics.deepTraceRows, metrics.scheduled);
    telemetryCoverage["submission_id"] = validityObject(
        metrics.submissionIdValidRows, presentedFrames);
    telemetryCoverage["latch_sample"] = validityObject(
        metrics.latchValidRows, presentedFrames);
    telemetryCoverage["native_present_timing"] = validityObject(
        metrics.nativePresentTimingValidRows, presentedFrames);
    telemetryCoverage["present_count_before"] = validityObject(
        metrics.presentCountBeforeValidRows, presentedFrames);
    telemetryCoverage["frame_stats_before"] = validityObject(
        metrics.frameStatsBeforeValidRows, presentedFrames);
    telemetryCoverage["gpu_ready_timing"] = validityObject(
        metrics.gpuReadyTimingValidRows, presentedFrames);
    telemetryCoverage["unique_latch_samples"] = static_cast<qint64>(
        metrics.uniqueLatchSamples);
    telemetryCoverage["stale_latch_samples"] = static_cast<qint64>(
        metrics.staleLatchSamples);
    telemetryCoverage["submission_sequence_resets"] = static_cast<qint64>(
        metrics.submissionSequenceResets);
    telemetryCoverage["latch_sequence_resets"] = static_cast<qint64>(
        metrics.latchSequenceResets);
    capture["telemetry_coverage"] = telemetryCoverage;

    QJsonObject observedLatency;
    observedLatency["decode_to_pacer_arrival"] = distributionObject(
        metrics.observedDecodeToArrival);
    observedLatency["pacer_arrival_to_dequeue"] = distributionObject(
        metrics.observedArrivalToDequeue);
    observedLatency["dequeue_to_decision"] = distributionObject(
        metrics.observedDequeueToDecision);
    observedLatency["decode_to_submission"] = distributionObject(
        metrics.observedDecodeToSubmission);
    observedLatency["pacer_arrival_to_submission"] = distributionObject(
        metrics.observedArrivalToSubmission);
    observedLatency["decision_to_submission"] = distributionObject(
        metrics.observedDecisionToSubmission);
    observedLatency["projected_source_to_submission"] = distributionObject(
        metrics.observedProjectedSourceToSubmission);
    observedLatency["submission_spacing"] = distributionObject(
        metrics.observedSubmissionSpacing);

    QJsonObject observedCosts;
    observedCosts["preparation"] = distributionObject(
        metrics.observedPreparation);
    observedCosts["present_call"] = distributionObject(
        metrics.observedPresentCall);
    observedCosts["native_present_call"] = distributionObject(
        metrics.observedNativePresentCall);
    observedCosts["gpu_ready_wait"] = distributionObject(
        metrics.observedGpuReadyWait);
    observedCosts["controller_call"] = distributionObject(
        metrics.observedControllerCall);
    observedCosts["stale_age_at_check"] = distributionObject(
        metrics.observedStaleAge);
    observedCosts["render_wait"] = distributionObject(
        metrics.observedRenderWait);
    observedCosts["target_wait"] = distributionObject(
        metrics.observedTargetWait);
    observedCosts["spacing_correction_wait"] = distributionObject(
        metrics.observedCorrectionWait);

    QJsonObject observedTears;
    observedTears["classifications"] = countObject(
        metrics.tearClassifications);
    observedTears["modelled_interval_violations"] = static_cast<qint64>(
        metrics.originalTearRisks);
    observedTears["scanout_anomalies"] = static_cast<qint64>(
        metrics.scanoutAnomalies);
    observedTears["repeated_refreshes"] = static_cast<qint64>(
        metrics.repeatedRefreshes);

    QJsonObject observed;
    observed["dispositions"] = countObject(metrics.dispositions);
    observed["drops"] = static_cast<qint64>(metrics.originalDrops);
    observed["latency_us"] = observedLatency;
    observed["execution_cost_us"] = observedCosts;
    observed["absolute_submit_error_us"] = distributionObject(
        metrics.observedAbsoluteSubmitError);
    observed["cadence_by_rounded_source_rate_fps"] = cadenceBandsObject(
        metrics.observedRateBands);
    observed["jerk_anomaly_windows"] = anomalyWindowObject(
        metrics.observedJerkAnomalies);
    observed["tear_and_scanout"] = observedTears;

    QJsonObject simulatedLatency;
    simulatedLatency["decode_to_submission"] = distributionObject(
        metrics.simulatedDecodeToSubmission);
    simulatedLatency["pacer_arrival_to_submission"] = distributionObject(
        metrics.simulatedArrivalToSubmission);
    simulatedLatency["decision_to_submission"] = distributionObject(
        metrics.simulatedDecisionToSubmission);
    simulatedLatency["projected_source_to_submission"] = distributionObject(
        metrics.simulatedProjectedSourceToSubmission);
    simulatedLatency["submission_spacing"] = distributionObject(
        metrics.simulatedSubmissionSpacing);

    QJsonObject simulatedTears;
    simulatedTears["classifications"] = countObject(
        metrics.simulatedTearClassifications);
    simulatedTears["modelled_interval_violations"] = static_cast<qint64>(
        metrics.simulatedTearRisks);
    simulatedTears["latched_frames"] = static_cast<qint64>(
        metrics.simulatedLatchedFrames);
    simulatedTears["scanout_prediction"] = "not inferred; recorded DXGI evidence is retained under observed";

    QJsonObject simulation;
    simulation["model"] = "recorded-world-controller-v2";
    simulation["display_hz"] = simulatedDisplayHz;
    simulation["stream_fps"] = simulatedStreamFps;
    simulation["can_latch_present"] = simulatedCanLatch;
    simulation["scenario"] = scenario.name;
    simulation["mode"] = scenario.mode;
    QJsonObject resolvedParameters;
    resolvedParameters["controller"] = vrrTimingParametersToJson(
        scenario.controller);
    resolvedParameters["worker"] = vrrWorkerParametersToJson(scenario.worker);
    simulation["resolved_parameters"] = resolvedParameters;
    simulation["parameter_fingerprint"] = QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument(resolvedParameters).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256).toHex());
    simulation["fixed_recorded_admission_and_lifecycle"] =
        scenario.mode == "fixed";
    if (scenario.mode == "worker") {
        QJsonObject worker;
        worker["model"] = "recorded-arrival-capacity-audit-v1";
        worker["arrivals"] = static_cast<qint64>(metrics.workerArrivals);
        worker["accepted"] = static_cast<qint64>(metrics.workerAccepted);
        worker["capacity_evictions"] = static_cast<qint64>(
            metrics.workerCapacityEvictions);
        worker["scanout_prediction"] = false;
        simulation["worker"] = worker;
    }
    simulation["drops"] = static_cast<qint64>(scenario.mode == "worker" ?
        metrics.workerCapacityEvictions : metrics.originalDrops);
    simulation["latency_us"] = simulatedLatency;
    simulation["absolute_submit_error_us"] = distributionObject(
        metrics.simulatedAbsoluteSubmitError);
    simulation["target_drift_us"] = distributionObject(
        metrics.simulatedTargetDrift);
    simulation["submission_drift_us"] = distributionObject(
        metrics.simulatedSubmissionDrift);
    simulation["cadence_error_us"] = distributionObject(
        metrics.simulatedCadenceError);
    simulation["gap_aware_cadence_by_rounded_source_rate_fps"] =
        cadenceBandsObject(metrics.simulatedRateBands);
    simulation["jerk_anomaly_windows"] = anomalyWindowObject(
        metrics.simulatedJerkAnomalies);
    QJsonObject pairedSubmissionDelta;
    pairedSubmissionDelta["signed_us"] = signedAccumulatorObject(
        metrics.pairedSubmissionDelta);
    pairedSubmissionDelta["absolute_us"] = boundedDistributionObject(
        metrics.pairedAbsoluteSubmissionDelta);
    pairedSubmissionDelta["scope"] =
        "candidate minus recorded submission for the same decoded frame";
    simulation["paired_submission_delta"] = pairedSubmissionDelta;
    simulation["tear"] = simulatedTears;

    QJsonObject fidelity;
    fidelity["reference_target_drift_us"] = distributionObject(
        metrics.referenceTargetDrift);
    fidelity["reference_source_time_drift_us"] = distributionObject(
        metrics.referenceSourceTimeDrift);
    fidelity["reference_source_period_drift_us"] = distributionObject(
        metrics.referenceSourcePeriodDrift);
    fidelity["reference_readiness_budget_drift_us"] = distributionObject(
        metrics.referenceReadinessBudgetDrift);
    fidelity["reference_timing_budget_drift_us"] = distributionObject(
        metrics.referenceTimingBudgetDrift);
    fidelity["reference_render_lead_drift_us"] = distributionObject(
        metrics.referenceRenderLeadDrift);
    fidelity["reference_render_wake_lead_drift_us"] = distributionObject(
        metrics.referenceRenderWakeLeadDrift);
    fidelity["reference_target_wake_lead_drift_us"] = distributionObject(
        metrics.referenceTargetWakeLeadDrift);
    fidelity["reference_guard_drift_us"] = distributionObject(
        metrics.referenceGuardDrift);
    fidelity["reference_headroom_drift_us"] = distributionObject(
        metrics.referenceHeadroomDrift);
    fidelity["reference_render_start_drift_us"] = distributionObject(
        metrics.referenceRenderStartDrift);
    fidelity["exact_reference_targets"] = static_cast<qint64>(
        metrics.exactReferenceTargets);
    fidelity["exact_simulated_submissions"] = static_cast<qint64>(
        metrics.exactSimulatedSubmissions);
    fidelity["exact_tear_classifications"] = static_cast<qint64>(
        metrics.exactTearClassifications);
    fidelity["invalid_execution_residuals"] = static_cast<qint64>(
        metrics.invalidExecutionResiduals);
    fidelity["baseline_exact"] = metrics.scheduled != 0 &&
        metrics.exactReferenceTargets == metrics.scheduled &&
        metrics.exactSimulatedSubmissions ==
            metrics.observedDecodeToSubmission.count &&
        metrics.exactTearClassifications == metrics.delivered &&
        metrics.invalidExecutionResiduals == 0;

    QJsonObject summary;
    summary["capture"] = capture;
    summary["observed"] = observed;
    summary["simulation"] = simulation;
    summary["fidelity"] = fidelity;
    summary["runtime_ms"] = elapsedMs;
    summary["latency_scope"] =
        "client decode completion through native submission; host capture/encode/network timestamps are not present in this trace schema";
    summary["tear_semantics"] =
        "interval violation is deterministic client risk; DXGI refresh evidence is observed, not a literal optical tear sensor";

    // Stable top-level fields retain compatibility with existing launcher
    // summaries and comparison files.
    summary["display_hz"] = simulatedDisplayHz;
    summary["stream_fps"] = simulatedStreamFps;
    summary["delivered_frames"] = static_cast<qint64>(metrics.delivered);
    summary["replayed_frames"] = static_cast<qint64>(metrics.scheduled);
    summary["original_drops"] = static_cast<qint64>(metrics.originalDrops);
    summary["original_tear_risks"] = static_cast<qint64>(
        metrics.originalTearRisks);
    summary["original_scanout_anomalies"] = static_cast<qint64>(
        metrics.scanoutAnomalies);
    summary["original_repeated_refreshes"] = static_cast<qint64>(
        metrics.repeatedRefreshes);
    summary["replay_tear_risks"] = static_cast<qint64>(
        metrics.simulatedTearRisks);
    summary["replay_latched_frames"] = static_cast<qint64>(
        metrics.simulatedLatchedFrames);
    summary["original_abs_submit_error_p95_us"] = observed.value(
        "absolute_submit_error_us").toObject().value("p95");
    summary["replay_abs_submit_error_p95_us"] = simulation.value(
        "absolute_submit_error_us").toObject().value("p95");
    summary["replay_target_drift_p95_us"] = simulation.value(
        "target_drift_us").toObject().value("p95");
    summary["replay_target_drift_p99_us"] = simulation.value(
        "target_drift_us").toObject().value("p99");
    summary["replay_cadence_error_p95_us"] = simulation.value(
        "cadence_error_us").toObject().value("p95");
    summary["original_decode_to_submission_p95_us"] = observedLatency.value(
        "decode_to_submission").toObject().value("p95");
    summary["replay_decode_to_submission_p95_us"] = simulatedLatency.value(
        "decode_to_submission").toObject().value("p95");
    summary["replay_arrival_to_submission_p95_us"] = simulatedLatency.value(
        "pacer_arrival_to_submission").toObject().value("p95");
    summary["original_decode_to_submission_stddev_us"] =
        observedLatency.value("decode_to_submission").toObject().value(
            "stddev");
    summary["replay_decode_to_submission_stddev_us"] =
        simulatedLatency.value("decode_to_submission").toObject().value(
            "stddev");
    const QJsonObject cadenceBands = simulation.value(
        "gap_aware_cadence_by_rounded_source_rate_fps").toObject();
    const QJsonObject fullRangeBand = cadenceBands.value("40_116").toObject();
    summary["replay_40_116_cadence_residual_p95_us"] = fullRangeBand.value(
        "absolute_cadence_residual_us").toObject().value("p95");
    summary["replay_40_116_cadence_residual_p99_us"] = fullRangeBand.value(
        "absolute_cadence_residual_us").toObject().value("p99");
    summary["replay_40_116_jerk_p95_us"] = fullRangeBand.value(
        "absolute_jerk_us").toObject().value("p95");
    summary["replay_40_116_jerk_p99_us"] = fullRangeBand.value(
        "absolute_jerk_us").toObject().value("p99");
    const QJsonObject focusBand = cadenceBands.value("60_100").toObject();
    summary["replay_60_100_cadence_residual_p95_us"] = focusBand.value(
        "absolute_cadence_residual_us").toObject().value("p95");
    summary["replay_60_100_cadence_residual_p99_us"] = focusBand.value(
        "absolute_cadence_residual_us").toObject().value("p99");
    summary["replay_60_100_jerk_p95_us"] = focusBand.value(
        "absolute_jerk_us").toObject().value("p95");
    summary["replay_60_100_jerk_p99_us"] = focusBand.value(
        "absolute_jerk_us").toObject().value("p99");
    summary["paired_submission_delta_stddev_us"] = pairedSubmissionDelta.value(
        "signed_us").toObject().value("stddev");
    summary["paired_abs_submission_delta_p99_us"] = pairedSubmissionDelta.value(
        "absolute_us").toObject().value("p99");
    summary["model"] = "recorded-world-controller-v2";
    return summary;
}

struct TimelineDetails {
    int recordedSourceRateHz = 0;
    int simulatedSourceRateHz = 0;
    uint64_t recordedSourcePeriodUs = 0;
    uint64_t simulatedSourcePeriodUs = 0;
    CadenceSample recordedCadence;
    CadenceSample simulatedCadence;
    int64_t recordedSpacingMarginUs = 0;
    int64_t simulatedSpacingMarginUs = 0;
    uint64_t guardUs = 0;
    uint64_t headroomUs = 0;
    int64_t readinessBudgetUs = 0;
    uint64_t readinessReserveUs = 0;
    uint64_t queueDepthBefore = 0;
    uint64_t queueDepthAfter = 0;
    bool queueDiscontinuity = false;
    bool recordedLatched = false;
    bool simulatedLatched = false;
    bool latchValid = false;
    uint64_t latchSubmissionId = 0;
    uint64_t latchTimeUs = 0;
    uint64_t latchPresentRefreshSequence = 0;
    uint64_t latchSyncRefreshSequence = 0;
};

bool writeTimelineHeader(QFile& file)
{
    static const QByteArray header =
        "arrival_sequence,frame,disposition,decode_complete_us,pacer_arrival_us,"
        "recorded_target_us,simulated_target_us,target_delta_us,"
        "recorded_submission_us,simulated_submission_us,submission_delta_us,"
        "recorded_decode_to_submission_us,simulated_decode_to_submission_us,"
        "recorded_tear_classification,simulated_tear_classification,"
        "recorded_source_rate_hz_rounded,simulated_source_rate_hz_rounded,"
        "recorded_source_period_us,simulated_source_period_us,"
        "recorded_cadence_valid,simulated_cadence_valid,"
        "recorded_source_elapsed_us,simulated_source_elapsed_us,"
        "recorded_submission_elapsed_us,simulated_submission_elapsed_us,"
        "recorded_cadence_residual_us,simulated_cadence_residual_us,"
        "recorded_jerk_valid,simulated_jerk_valid,recorded_jerk_us,simulated_jerk_us,"
        "recorded_phase_us,simulated_phase_us,"
        "recorded_spacing_margin_us,simulated_spacing_margin_us,"
        "guard_us,headroom_us,readiness_budget_us,readiness_reserve_us,"
        "queue_depth_before,queue_depth_after,queue_discontinuity,"
        "recorded_latched,simulated_latched,latch_valid,latch_submission_id,"
        "latch_time_us,latch_present_refresh_seq,latch_sync_refresh_seq\n";
    return file.write(header) == header.size();
}

bool writeTimelineRow(QFile& file, uint64_t arrivalSequence, int frame,
                      const QByteArray& disposition,
                      uint64_t decodeCompleteUs, uint64_t pacerArrivalUs,
                      uint64_t recordedTargetUs, uint64_t simulatedTargetUs,
                      uint64_t recordedSubmissionUs,
                      uint64_t simulatedSubmissionUs,
                      const QByteArray& recordedTear,
                      const QByteArray& simulatedTear,
                      const TimelineDetails& details)
{
    QByteArray line;
    line.reserve(768);
    const auto append = [&line](const QByteArray& value) {
        if (!line.isEmpty()) {
            line.append(',');
        }
        line.append(value);
    };
    append(QByteArray::number(arrivalSequence));
    append(QByteArray::number(frame));
    append(disposition);
    append(QByteArray::number(decodeCompleteUs));
    append(QByteArray::number(pacerArrivalUs));
    append(QByteArray::number(recordedTargetUs));
    append(QByteArray::number(simulatedTargetUs));
    append(QByteArray::number(signedDifference(simulatedTargetUs,
                                               recordedTargetUs)));
    append(QByteArray::number(recordedSubmissionUs));
    append(QByteArray::number(simulatedSubmissionUs));
    append(QByteArray::number(signedDifference(simulatedSubmissionUs,
                                               recordedSubmissionUs)));
    append(QByteArray::number(recordedSubmissionUs >= decodeCompleteUs ?
        recordedSubmissionUs - decodeCompleteUs : 0));
    append(QByteArray::number(simulatedSubmissionUs >= decodeCompleteUs ?
        simulatedSubmissionUs - decodeCompleteUs : 0));
    append(recordedTear);
    append(simulatedTear);
    append(QByteArray::number(details.recordedSourceRateHz));
    append(QByteArray::number(details.simulatedSourceRateHz));
    append(QByteArray::number(details.recordedSourcePeriodUs));
    append(QByteArray::number(details.simulatedSourcePeriodUs));
    append(QByteArray::number(details.recordedCadence.valid ? 1 : 0));
    append(QByteArray::number(details.simulatedCadence.valid ? 1 : 0));
    append(QByteArray::number(details.recordedCadence.sourceElapsedUs));
    append(QByteArray::number(details.simulatedCadence.sourceElapsedUs));
    append(QByteArray::number(details.recordedCadence.submissionElapsedUs));
    append(QByteArray::number(details.simulatedCadence.submissionElapsedUs));
    append(QByteArray::number(details.recordedCadence.residualUs));
    append(QByteArray::number(details.simulatedCadence.residualUs));
    append(QByteArray::number(details.recordedCadence.jerkValid ? 1 : 0));
    append(QByteArray::number(details.simulatedCadence.jerkValid ? 1 : 0));
    append(QByteArray::number(details.recordedCadence.jerkUs));
    append(QByteArray::number(details.simulatedCadence.jerkUs));
    append(QByteArray::number(details.recordedCadence.phaseUs));
    append(QByteArray::number(details.simulatedCadence.phaseUs));
    append(QByteArray::number(details.recordedSpacingMarginUs));
    append(QByteArray::number(details.simulatedSpacingMarginUs));
    append(QByteArray::number(details.guardUs));
    append(QByteArray::number(details.headroomUs));
    append(QByteArray::number(details.readinessBudgetUs));
    append(QByteArray::number(details.readinessReserveUs));
    append(QByteArray::number(details.queueDepthBefore));
    append(QByteArray::number(details.queueDepthAfter));
    append(QByteArray::number(details.queueDiscontinuity ? 1 : 0));
    append(QByteArray::number(details.recordedLatched ? 1 : 0));
    append(QByteArray::number(details.simulatedLatched ? 1 : 0));
    append(QByteArray::number(details.latchValid ? 1 : 0));
    append(QByteArray::number(details.latchSubmissionId));
    append(QByteArray::number(details.latchTimeUs));
    append(QByteArray::number(details.latchPresentRefreshSequence));
    append(QByteArray::number(details.latchSyncRefreshSequence));
    line.append('\n');
    return file.write(line) == line.size();
}

QJsonValue jsonPathValue(const QJsonObject& root, const QString& path)
{
    QJsonValue value(root);
    for (const QString& component : path.split('.', Qt::SkipEmptyParts)) {
        if (!value.isObject()) return {};
        value = value.toObject().value(component);
    }
    return value;
}

bool evaluateAssertions(const VrrReplayScenario& scenario,
                        QJsonObject& summary)
{
    QJsonArray results;
    bool passed = true;
    for (const VrrReplayAssertion& assertion : scenario.assertions) {
        const QJsonValue metricValue = jsonPathValue(summary, assertion.metric);
        const double actual = metricValue.toDouble(
            std::numeric_limits<double>::quiet_NaN());
        bool result = std::isfinite(actual);
        if (result && assertion.operation == "<") result = actual < assertion.value;
        else if (result && assertion.operation == "<=") result = actual <= assertion.value;
        else if (result && assertion.operation == "==") result = actual == assertion.value;
        else if (result && assertion.operation == ">=") result = actual >= assertion.value;
        else if (result && assertion.operation == ">") result = actual > assertion.value;
        QJsonObject item;
        item["metric"] = assertion.metric;
        item["operator"] = assertion.operation;
        item["expected"] = assertion.value;
        item["actual"] = std::isfinite(actual) ? QJsonValue(actual) : QJsonValue();
        item["passed"] = result;
        results.append(item);
        passed = passed && result;
    }
    QJsonObject assertionSummary;
    assertionSummary["passed"] = passed;
    assertionSummary["results"] = results;
    summary["assertions"] = assertionSummary;
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName("vrrreplay");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Replay an exact captured VRR workload and simulate the current timing controller");
    parser.addHelpOption();
    parser.addPositionalArgument("trace", "Input .vrrtrace or expanded CSV");
    QCommandLineOption displayOption(
        "display-hz", "Override display refresh for the candidate simulation", "hz");
    QCommandLineOption streamOption(
        "stream-fps", "Override negotiated stream rate for the candidate simulation", "fps");
    QCommandLineOption latchOption(
        "no-latch", "Disable per-present latched mode in the candidate simulation");
    QCommandLineOption compareOption(
        "compare", "Add deltas against a prior replay JSON summary", "json");
    QCommandLineOption outputOption(
        "output", "Write UTF-8 JSON to a file instead of stdout", "json");
    QCommandLineOption timelineOption(
        "timeline", "Write the recorded and simulated per-frame timeline as CSV", "csv");
    QCommandLineOption exactOption(
        "require-exact-baseline",
        "Fail unless an unmodified configuration exactly reproduces targets, submissions, and tear classes");
    QCommandLineOption configOption(
        "config", "Load versioned replay scenarios and parameters", "json");
    QCommandLineOption scenarioOption(
        "scenario", "Run only the named scenario (repeatable)", "name");
    QCommandLineOption setOption(
        "set", "Override a resolved parameter as section.name=value", "override");
    QCommandLineOption modeOption(
        "mode", "Override scenario mode: fixed or worker", "mode");
    QCommandLineOption listParametersOption(
        "list-parameters", "List every replay parameter and exit");
    QCommandLineOption dumpDefaultsOption(
        "dump-default-config", "Print a complete default JSON configuration and exit");
    parser.addOption(displayOption);
    parser.addOption(streamOption);
    parser.addOption(latchOption);
    parser.addOption(compareOption);
    parser.addOption(outputOption);
    parser.addOption(timelineOption);
    parser.addOption(exactOption);
    parser.addOption(configOption);
    parser.addOption(scenarioOption);
    parser.addOption(setOption);
    parser.addOption(modeOption);
    parser.addOption(listParametersOption);
    parser.addOption(dumpDefaultsOption);
    parser.process(application);

    if (parser.isSet(listParametersOption)) {
        for (const QString& name : vrrReplayParameterNames()) {
            std::printf("%s\n", qPrintable(name));
        }
        return 0;
    }
    if (parser.isSet(dumpDefaultsOption)) {
        const QByteArray defaults = QJsonDocument(
            vrrDefaultReplayConfigurationJson()).toJson(QJsonDocument::Indented);
        std::fwrite(defaults.constData(), 1,
                    static_cast<size_t>(defaults.size()), stdout);
        return 0;
    }

    if (parser.positionalArguments().size() != 1) {
        parser.showHelp(2);
    }
    if (parser.isSet(exactOption) &&
            (parser.isSet(displayOption) || parser.isSet(streamOption) ||
             parser.isSet(latchOption) || parser.isSet(setOption) ||
             parser.isSet(modeOption) || parser.isSet(configOption))) {
        std::fprintf(stderr,
                     "--require-exact-baseline cannot be used with candidate overrides\n");
        return 2;
    }

    const QString tracePath = parser.positionalArguments().front();
    QString error;

    VrrReplayConfiguration replayConfiguration;
    if (parser.isSet(configOption)) {
        QFile configFile(parser.value(configOption));
        if (!configFile.open(QIODevice::ReadOnly) ||
                !loadVrrReplayConfiguration(configFile.readAll(),
                                            replayConfiguration, error)) {
            std::fprintf(stderr, "Unable to load replay config: %s\n",
                         qPrintable(error.isEmpty() ?
                             configFile.errorString() : error));
            return 2;
        }
    }
    else {
        replayConfiguration.scenarios.append(VrrReplayScenario {});
    }

    const QStringList requestedScenarios = parser.values(scenarioOption);
    if (!requestedScenarios.isEmpty()) {
        QList<VrrReplayScenario> filtered;
        for (const QString& requested : requestedScenarios) {
            const auto found = std::find_if(
                replayConfiguration.scenarios.cbegin(),
                replayConfiguration.scenarios.cend(),
                [&requested](const VrrReplayScenario& item) {
                    return item.name == requested;
                });
            if (found == replayConfiguration.scenarios.cend()) {
                std::fprintf(stderr, "Unknown replay scenario: %s\n",
                             qPrintable(requested));
                return 2;
            }
            filtered.append(*found);
        }
        replayConfiguration.scenarios = filtered;
    }

    if (replayConfiguration.scenarios.size() > 1) {
        if (parser.isSet(timelineOption)) {
            std::fprintf(stderr,
                         "--timeline requires selecting a single scenario\n");
            return 2;
        }
        QJsonArray scenarioResults;
        bool allPassed = true;
        for (const VrrReplayScenario& scenario :
             replayConfiguration.scenarios) {
            QStringList arguments { tracePath, "--config",
                                    parser.value(configOption), "--scenario",
                                    scenario.name };
            for (const QString& overrideValue : parser.values(setOption))
                arguments << "--set" << overrideValue;
            if (parser.isSet(modeOption)) arguments << "--mode" << parser.value(modeOption);
            if (parser.isSet(displayOption)) arguments << "--display-hz" << parser.value(displayOption);
            if (parser.isSet(streamOption)) arguments << "--stream-fps" << parser.value(streamOption);
            if (parser.isSet(latchOption)) arguments << "--no-latch";
            if (parser.isSet(compareOption)) arguments << "--compare" << parser.value(compareOption);
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(), arguments);
            child.waitForFinished(-1);
            QJsonParseError childError;
            const QJsonDocument childDocument = QJsonDocument::fromJson(
                child.readAllStandardOutput(), &childError);
            if (!childDocument.isObject()) {
                std::fprintf(stderr, "Scenario %s failed: %s%s\n",
                             qPrintable(scenario.name),
                             qPrintable(childError.errorString()),
                             child.readAllStandardError().constData());
                return child.exitCode() == 0 ? 1 : child.exitCode();
            }
            QJsonObject result = childDocument.object();
            result["scenario"] = scenario.name;
            result["exit_code"] = child.exitCode();
            allPassed = allPassed && child.exitCode() == 0;
            scenarioResults.append(result);
        }
        QJsonObject batch;
        batch["config_schema"] = 1;
        batch["trace"] = QFileInfo(tracePath).fileName();
        batch["scenarios"] = scenarioResults;
        batch["passed"] = allPassed;
        const QByteArray output = QJsonDocument(batch).toJson(
            QJsonDocument::Indented);
        if (parser.isSet(outputOption)) {
            QFile outputFile(parser.value(outputOption));
            if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                    outputFile.write(output) != output.size()) return 1;
        }
        else std::fwrite(output.constData(), 1,
                         static_cast<size_t>(output.size()), stdout);
        return allPassed ? 0 : 4;
    }

    VrrReplayScenario scenario = replayConfiguration.scenarios.front();
    if (parser.isSet(modeOption)) scenario.mode = parser.value(modeOption);
    if (scenario.mode != "fixed" && scenario.mode != "worker") {
        std::fprintf(stderr, "--mode must be fixed or worker\n"); return 2;
    }
    for (const QString& overrideValue : parser.values(setOption)) {
        if (!applyVrrReplayOverride(overrideValue, scenario, error)) {
            std::fprintf(stderr, "Invalid replay override: %s\n",
                         qPrintable(error)); return 2;
        }
    }
    TraceReader reader(tracePath);
    if (!reader.open(error)) {
        std::fprintf(stderr, "Unable to open trace: %s\n",
                     qPrintable(error));
        return 1;
    }

    QByteArray headerLine;
    if (!reader.readLine(headerLine, error)) {
        std::fprintf(stderr, "Unable to read trace header: %s\n",
                     qPrintable(error));
        return 1;
    }
    Columns columns;
    if (!columns.resolve(headerLine.trimmed().split(','), error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }

    QFile timelineFile;
    if (parser.isSet(timelineOption)) {
        timelineFile.setFileName(parser.value(timelineOption));
        if (!timelineFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                !writeTimelineHeader(timelineFile)) {
            std::fprintf(stderr, "Unable to create timeline: %s\n",
                         qPrintable(timelineFile.errorString()));
            return 1;
        }
    }

    Metrics metrics;
    VrrSessionConfig capturedConfig;
    VrrSessionConfig simulatedConfig;
    VrrTimingParameters capturedParameters;
    std::unique_ptr<VrrTimingController> referenceController;
    std::unique_ptr<VrrTimingController> simulatedController;
    bool capturedCanLatch = false;
    bool simulatedCanLatch = false;
    bool haveSimulatedSubmission = false;
    uint64_t priorSimulatedSubmissionUs = 0;
    bool haveLatch = false;
    uint64_t priorLatchSubmission = 0;
    uint64_t priorPresentRefresh = 0;
    CadenceTracker observedCadenceTracker;
    CadenceTracker simulatedCadenceTracker;
    struct SubmissionBand {
        uint64_t id = 0;
        int rateHz = 0;
    };
    std::deque<SubmissionBand> pendingSubmissionBands;

    QElapsedTimer timer;
    timer.start();
    QByteArray line;
    while (reader.readLine(line, error)) {
        const QList<QByteArray> fields = line.trimmed().split(',');
        if (fields.size() <= columns.maximum()) {
            std::fprintf(stderr, "Malformed trace row %llu\n",
                         static_cast<unsigned long long>(metrics.delivered + 2));
            return 1;
        }
        const uint64_t traceSchema = unsignedField(fields,
                                                    columns.traceSchema);
        if (traceSchema != 3 && traceSchema != 4 && traceSchema != 5) {
            std::fprintf(stderr, "Unsupported trace schema on row %llu\n",
                         static_cast<unsigned long long>(metrics.delivered + 2));
            return 1;
        }
        if (traceSchema >= 4 && columns.spacingGuardFeedbackUs < 0) {
            std::fprintf(stderr,
                         "Schema 4 trace is missing spacing_guard_feedback_us\n");
            return 1;
        }
        if (scenario.mode == "worker" && traceSchema < 5) {
            std::fprintf(stderr,
                         "Worker simulation requires a schema 5 trace\n");
            return 2;
        }
        if (metrics.traceSchema == 0) {
            metrics.traceSchema = traceSchema;
        }
        else if (metrics.traceSchema != traceSchema) {
            std::fprintf(stderr, "Trace schema changes within the capture\n");
            return 1;
        }

        ++metrics.delivered;
        const uint64_t arrivalSequence = unsignedField(
            fields, columns.arrivalSequence);
        const uint64_t pacerArrivalUs = unsignedField(
            fields, columns.pacerArrivalUs);
        if (metrics.firstArrivalSequence == 0 ||
                arrivalSequence < metrics.firstArrivalSequence) {
            metrics.firstArrivalSequence = arrivalSequence;
        }
        metrics.lastArrivalSequence = std::max(metrics.lastArrivalSequence,
                                                arrivalSequence);
        if (metrics.firstArrivalUs == 0 || pacerArrivalUs < metrics.firstArrivalUs) {
            metrics.firstArrivalUs = pacerArrivalUs;
        }
        metrics.lastArrivalUs = std::max(metrics.lastArrivalUs, pacerArrivalUs);

        const QByteArray disposition = fields[columns.disposition];
        const QByteArray recordedTear = fields[columns.tearClassification];
        metrics.deepTraceRows += optionalUnsignedField(
            fields, columns.deepTrace) != 0 ? 1 : 0;
        const bool submissionIdValid = optionalUnsignedField(
            fields, columns.submissionIdValid) != 0;
        if (submissionIdValid) {
            ++metrics.submissionIdValidRows;
            const SubmissionBand submissionBand {
                optionalUnsignedField(fields, columns.submissionId),
                roundedRateForPeriod(unsignedField(
                    fields, columns.sourcePeriodUs)),
            };
            if (haveLatch && submissionBand.id < priorLatchSubmission) {
                ++metrics.submissionSequenceResets;
                haveLatch = false;
                pendingSubmissionBands.clear();
            }
            if (!pendingSubmissionBands.empty() &&
                    submissionBand.id < pendingSubmissionBands.back().id) {
                ++metrics.submissionSequenceResets;
                pendingSubmissionBands.clear();
            }
            if (!pendingSubmissionBands.empty() &&
                    submissionBand.id == pendingSubmissionBands.back().id) {
                pendingSubmissionBands.back() = submissionBand;
            }
            else {
                pendingSubmissionBands.push_back(submissionBand);
            }
            while (pendingSubmissionBands.size() >
                    kMaximumPendingSubmissionBands) {
                pendingSubmissionBands.pop_front();
            }
        }
        if (scenario.mode == "worker") {
            ++metrics.workerArrivals;
            const bool accepted = unsignedField(
                fields, columns.queueAccepted) != 0;
            const uint64_t queueDepthBefore = unsignedField(
                fields, columns.queueDepthBefore);
            if (accepted) {
                ++metrics.workerAccepted;
                if (queueDepthBefore >= scenario.worker.queueCapacity) {
                    ++metrics.workerCapacityEvictions;
                }
            }
        }
        ++metrics.dispositions[disposition];
        ++metrics.tearClassifications[recordedTear];
        if (unsignedField(fields, columns.dropped) != 0) {
            ++metrics.originalDrops;
        }
        if (unsignedField(fields, columns.tearRisk) != 0) {
            ++metrics.originalTearRisks;
        }

        const uint64_t decodeCompleteUs = unsignedField(
            fields, columns.decodeCompleteUs);
        const uint64_t dequeueUs = unsignedField(fields, columns.dequeueUs);
        const uint64_t decisionUs = unsignedField(fields, columns.decisionUs);
        metrics.observedDecodeToArrival.addElapsed(pacerArrivalUs,
                                                   decodeCompleteUs);
        metrics.observedArrivalToDequeue.addElapsed(dequeueUs,
                                                    pacerArrivalUs);
        metrics.observedDequeueToDecision.addElapsed(decisionUs, dequeueUs);
        if (capturedConfig.displayRefreshHz == 0) {
            capturedConfig.displayRefreshHz = static_cast<int>(signedField(
                fields, columns.displayRefreshHz));
            capturedConfig.streamRateHz = static_cast<int>(signedField(
                fields, columns.streamRateHz));
            capturedCanLatch = unsignedField(fields, columns.canLatch) != 0;
            if (traceSchema >= 5) {
                VrrReplayScenario capturedScenario;
                int expectedParameters = 0;
                for (const QString& path : vrrReplayParameterNames()) {
                    if (!path.startsWith("controller.")) continue;
                    ++expectedParameters;
                    const auto column = columns.capturedParameterColumns.find(
                        path);
                    if (column == columns.capturedParameterColumns.end() ||
                            !applyVrrReplayOverride(
                                path + "=" + QString::number(unsignedField(
                                    fields, column.value())),
                                capturedScenario, error)) {
                        std::fprintf(stderr,
                                     "Schema 5 trace has invalid captured parameters: %s\n",
                                     qPrintable(error));
                        return 1;
                    }
                }
                if (columns.capturedParameterColumns.size() !=
                        expectedParameters) {
                    std::fprintf(stderr,
                                 "Schema 5 trace is missing captured controller parameters\n");
                    return 1;
                }
                capturedParameters = capturedScenario.controller;
            }
            simulatedConfig = capturedConfig;
            if (parser.isSet(displayOption)) {
                simulatedConfig.displayRefreshHz =
                    parser.value(displayOption).toInt();
            }
            if (parser.isSet(streamOption)) {
                simulatedConfig.streamRateHz =
                    parser.value(streamOption).toInt();
            }
            simulatedCanLatch = capturedCanLatch && !parser.isSet(latchOption);
            referenceController = std::make_unique<VrrTimingController>(
                capturedConfig, capturedCanLatch, capturedParameters);
            simulatedController = std::make_unique<VrrTimingController>(
                simulatedConfig, simulatedCanLatch, scenario.controller);
        }

        if (unsignedField(fields, columns.latchValid) != 0) {
            ++metrics.latchValidRows;
            const uint64_t latchSubmission = unsignedField(
                fields, columns.latchSubmissionId);
            const uint64_t presentRefresh = unsignedField(
                fields, columns.latchPresentRefreshSequence);
            if (haveLatch && (latchSubmission < priorLatchSubmission ||
                    presentRefresh < priorPresentRefresh)) {
                ++metrics.latchSequenceResets;
                haveLatch = false;
            }
            int latchRateHz = 0;
            for (const SubmissionBand& submission : pendingSubmissionBands) {
                if (submission.id == latchSubmission) {
                    latchRateHz = submission.rateHz;
                    break;
                }
            }
            if (haveLatch && latchSubmission > priorLatchSubmission &&
                    presentRefresh >= priorPresentRefresh) {
                const uint64_t presentDelta =
                    latchSubmission - priorLatchSubmission;
                const uint64_t refreshDelta =
                    presentRefresh - priorPresentRefresh;
                const uint64_t anomalies = presentDelta > refreshDelta ?
                    presentDelta - refreshDelta : 0;
                const uint64_t repeated = refreshDelta > presentDelta ?
                    refreshDelta - presentDelta : 0;
                if (presentDelta > refreshDelta) {
                    metrics.scanoutAnomalies += anomalies;
                }
                if (refreshDelta > presentDelta) {
                    metrics.repeatedRefreshes += repeated;
                }
                addScanoutOutcome(metrics.observedRateBands,
                                  latchRateHz, anomalies, repeated);
            }
            if (!haveLatch || latchSubmission > priorLatchSubmission) {
                ++metrics.uniqueLatchSamples;
                haveLatch = true;
                priorLatchSubmission = latchSubmission;
                priorPresentRefresh = presentRefresh;
            }
            else {
                ++metrics.staleLatchSamples;
            }
            while (!pendingSubmissionBands.empty() &&
                    pendingSubmissionBands.front().id < latchSubmission) {
                pendingSubmissionBands.pop_front();
            }
        }

        TimelineDetails timelineDetails;
        timelineDetails.recordedSourcePeriodUs = unsignedField(
            fields, columns.sourcePeriodUs);
        timelineDetails.recordedSourceRateHz = roundedRateForPeriod(
            timelineDetails.recordedSourcePeriodUs);
        timelineDetails.recordedSpacingMarginUs = optionalSignedField(
            fields, columns.spacingMarginUs);
        timelineDetails.guardUs = unsignedField(fields, columns.guardUs);
        timelineDetails.headroomUs = unsignedField(fields,
                                                    columns.headroomUs);
        timelineDetails.readinessBudgetUs = signedField(
            fields, columns.readinessBudgetUs);
        timelineDetails.readinessReserveUs = optionalUnsignedField(
            fields, columns.appliedReadinessReserveUs);
        timelineDetails.queueDepthBefore = unsignedField(
            fields, columns.queueDepthBefore);
        timelineDetails.queueDepthAfter = optionalUnsignedField(
            fields, columns.queueDepthAfter);
        timelineDetails.queueDiscontinuity = optionalUnsignedField(
            fields, columns.queueDiscontinuity) != 0;
        timelineDetails.recordedLatched = optionalUnsignedField(
            fields, columns.latchedPresent) != 0;
        timelineDetails.latchValid = unsignedField(
            fields, columns.latchValid) != 0;
        timelineDetails.latchSubmissionId = unsignedField(
            fields, columns.latchSubmissionId);
        timelineDetails.latchTimeUs = optionalUnsignedField(
            fields, columns.latchTimeUs);
        timelineDetails.latchPresentRefreshSequence = unsignedField(
            fields, columns.latchPresentRefreshSequence);
        timelineDetails.latchSyncRefreshSequence = optionalUnsignedField(
            fields, columns.latchSyncRefreshSequence);

        if (unsignedField(fields, columns.decisionValid) == 0) {
            ++metrics.simulatedTearClassifications["not_presented"];
            metrics.exactTearClassifications +=
                recordedTear == "not_presented" ? 1 : 0;
            if (timelineFile.isOpen() &&
                    !writeTimelineRow(timelineFile, arrivalSequence,
                        static_cast<int>(signedField(fields, columns.frame)),
                        disposition, decodeCompleteUs, pacerArrivalUs,
                        0, 0, 0, 0, recordedTear, "not_presented",
                        timelineDetails)) {
                std::fprintf(stderr, "Unable to write timeline: %s\n",
                             qPrintable(timelineFile.errorString()));
                return 1;
            }
            continue;
        }

        ++metrics.scheduled;
        metrics.observedPreparation.add(unsignedField(fields,
                                                       columns.preparationUs));
        metrics.observedPresentCall.add(unsignedField(fields,
                                                       columns.presentCallUs));
        if (optionalUnsignedField(fields,
                                  columns.nativePresentTimingValid) != 0) {
            ++metrics.nativePresentTimingValidRows;
            metrics.observedNativePresentCall.add(unsignedField(
                fields, columns.nativePresentCallUs));
        }
        metrics.presentCountBeforeValidRows += optionalUnsignedField(
            fields, columns.presentCountBeforeValid) != 0 ? 1 : 0;
        metrics.frameStatsBeforeValidRows += optionalUnsignedField(
            fields, columns.frameStatsBeforeValid) != 0 ? 1 : 0;
        if (optionalUnsignedField(fields,
                                  columns.gpuReadyTimingValid) != 0) {
            ++metrics.gpuReadyTimingValidRows;
            metrics.observedGpuReadyWait.add(unsignedField(
                fields, columns.gpuReadyWaitUs));
        }
        if (columns.controllerCallUs >= 0)
            metrics.observedControllerCall.add(unsignedField(
                fields, columns.controllerCallUs));
        if (columns.staleAgeUs >= 0)
            metrics.observedStaleAge.add(unsignedField(
                fields, columns.staleAgeUs));
        if (columns.renderWaitEntryUs >= 0 && columns.renderWaitFinalUs >= 0)
            metrics.observedRenderWait.addElapsed(
                unsignedField(fields, columns.renderWaitFinalUs),
                unsignedField(fields, columns.renderWaitEntryUs));
        if (columns.targetWaitEntryUs >= 0 && columns.targetWaitFinalUs >= 0)
            metrics.observedTargetWait.addElapsed(
                unsignedField(fields, columns.targetWaitFinalUs),
                unsignedField(fields, columns.targetWaitEntryUs));
        if (columns.correctionWaitStartUs >= 0 &&
                columns.correctionWaitEndUs >= 0)
            metrics.observedCorrectionWait.addElapsed(
                unsignedField(fields, columns.correctionWaitEndUs),
                unsignedField(fields, columns.correctionWaitStartUs));
        PacedFrame frame(nullptr,
                         static_cast<int>(signedField(fields, columns.frame)),
                         static_cast<uint32_t>(unsignedField(
                             fields, columns.rtpTimestamp)),
                         unsignedField(fields, columns.rtpValid) != 0,
                         decodeCompleteUs);
        const VrrTimingDecision referenceDecision =
            referenceController->schedule(frame, decisionUs);
        const VrrTimingDecision simulatedDecision =
            simulatedController->schedule(frame, decisionUs);
        timelineDetails.simulatedSourcePeriodUs =
            simulatedDecision.sourcePeriodUs;
        timelineDetails.simulatedSourceRateHz = roundedRateForPeriod(
            simulatedDecision.sourcePeriodUs);
        timelineDetails.simulatedLatched =
            simulatedDecision.latchedPresentation;
        const uint64_t recordedTargetUs = unsignedField(
            fields, columns.recordedTargetUs);
        const uint64_t referenceTargetDrift = absoluteValue(
            signedDifference(referenceDecision.targetUs, recordedTargetUs));
        metrics.referenceTargetDrift.add(referenceTargetDrift);
        metrics.exactReferenceTargets += referenceTargetDrift == 0 ? 1 : 0;
        metrics.referenceSourceTimeDrift.add(absoluteValue(signedDifference(
            referenceDecision.sourceTimeUs,
            unsignedField(fields, columns.sourceTimeUs))));
        metrics.referenceSourcePeriodDrift.add(absoluteValue(signedDifference(
            referenceDecision.sourcePeriodUs,
            unsignedField(fields, columns.sourcePeriodUs))));
        metrics.referenceReadinessBudgetDrift.add(absoluteValue(
            referenceDecision.readinessBudgetUs -
            signedField(fields, columns.readinessBudgetUs)));
        metrics.referenceTimingBudgetDrift.add(absoluteValue(signedDifference(
            referenceDecision.timingBudgetUs,
            unsignedField(fields, columns.timingBudgetUs))));
        metrics.referenceRenderLeadDrift.add(absoluteValue(signedDifference(
            referenceDecision.renderLeadUs,
            unsignedField(fields, columns.renderLeadUs))));
        metrics.referenceRenderWakeLeadDrift.add(absoluteValue(
            signedDifference(referenceDecision.renderWakeLeadUs,
                unsignedField(fields, columns.renderWakeLeadUs))));
        metrics.referenceTargetWakeLeadDrift.add(absoluteValue(
            signedDifference(referenceDecision.targetWakeLeadUs,
                unsignedField(fields, columns.targetWakeLeadUs))));
        metrics.referenceGuardDrift.add(absoluteValue(signedDifference(
            referenceDecision.guardUs,
            unsignedField(fields, columns.guardUs))));
        metrics.referenceHeadroomDrift.add(absoluteValue(signedDifference(
            referenceDecision.headroomUs,
            unsignedField(fields, columns.headroomUs))));
        metrics.referenceRenderStartDrift.add(absoluteValue(signedDifference(
            referenceDecision.renderStartUs,
            unsignedField(fields, columns.renderStartUs))));
        metrics.simulatedTargetDrift.add(absoluteValue(signedDifference(
            simulatedDecision.targetUs, recordedTargetUs)));

        const uint64_t preparationUs = unsignedField(fields,
                                                      columns.preparationUs);
        referenceController->notePreparationDuration(preparationUs);
        simulatedController->notePreparationDuration(preparationUs);
        referenceController->noteSchedulerDelays(
            unsignedField(fields, columns.renderWaitOvershootUs),
            unsignedField(fields, columns.targetSchedulerDelayUs),
            unsignedField(fields, columns.targetSchedulerDelayValid) != 0);
        simulatedController->noteSchedulerDelays(
            unsignedField(fields, columns.renderWaitOvershootUs),
            unsignedField(fields, columns.targetSchedulerDelayUs),
            unsignedField(fields, columns.targetSchedulerDelayValid) != 0);
        if (disposition == "presented" || disposition == "output_dropped") {
            // The worker first records a clean boundary and then, only if its
            // second clock check finds a deficit, applies that correction.
            // Schema 3 stores the combined wait correction, not whether the
            // second check also fired the guard-learning callback. Treating
            // every first-check wait as feedback incorrectly inflates guard.
            referenceController->noteSpacingDeficit(0);
            simulatedController->noteSpacingDeficit(0);
            const uint64_t guardFeedbackUs =
                columns.spacingGuardFeedbackUs >= 0 ?
                    unsignedField(fields, columns.spacingGuardFeedbackUs) : 0;
            if (guardFeedbackUs != 0) {
                referenceController->noteSpacingDeficit(guardFeedbackUs);
                simulatedController->noteSpacingDeficit(guardFeedbackUs);
            }
        }

        const uint64_t recordedPreparationStartUs = unsignedField(
            fields, columns.preparationStartUs);
        const uint64_t recordedPreparationEndUs = unsignedField(
            fields, columns.preparationEndUs);
        const uint64_t recordedRenderStartUs = unsignedField(
            fields, columns.renderStartUs);
        const uint64_t recordedSubmissionUs = unsignedField(
            fields, columns.submissionBoundaryUs);
        const bool presented = unsignedField(fields, columns.presented) != 0;
        const bool cancelled = unsignedField(fields, columns.cancelled) != 0;

        uint64_t simulatedSubmissionUs = 0;
        if (presented) {
            const uint64_t recordedRenderFloorUs = std::max(
                decisionUs, recordedRenderStartUs);
            const uint64_t prepareStartResidualUs =
                recordedPreparationStartUs >= recordedRenderFloorUs ?
                    recordedPreparationStartUs - recordedRenderFloorUs : 0;
            const uint64_t simulatedPreparationStartUs = saturatingAdd(
                std::max(decisionUs, simulatedDecision.renderStartUs),
                prepareStartResidualUs);
            const uint64_t simulatedPreparationEndUs = saturatingAdd(
                simulatedPreparationStartUs, preparationUs);
            const uint64_t recordedSubmissionFloorUs = std::max(
                recordedPreparationEndUs, recordedTargetUs);
            uint64_t submissionResidualUs = 0;
            if (recordedSubmissionUs >= recordedSubmissionFloorUs) {
                submissionResidualUs =
                    recordedSubmissionUs - recordedSubmissionFloorUs;
            }
            else {
                ++metrics.invalidExecutionResiduals;
            }
            const uint64_t simulatedSubmissionFloorUs = std::max({
                simulatedPreparationEndUs,
                simulatedDecision.targetUs,
                simulatedController->earliestSubmissionUs(),
            });
            simulatedSubmissionUs = saturatingAdd(
                simulatedSubmissionFloorUs, submissionResidualUs);
        }

        const bool hadPriorSimulatedSubmission = haveSimulatedSubmission;
        const QByteArray simulatedTear = simulatedTearClassification(
            presented, simulatedDecision.latchedPresentation,
            simulatedCanLatch, hadPriorSimulatedSubmission,
            simulatedSubmissionUs, priorSimulatedSubmissionUs,
            periodForRate(simulatedConfig.displayRefreshHz));
        ++metrics.simulatedTearClassifications[simulatedTear];
        metrics.exactTearClassifications +=
            simulatedTear == recordedTear ? 1 : 0;

        if (presented) {
            const bool recordedDiscontinuity = optionalUnsignedField(
                fields, columns.rebased) != 0 || optionalUnsignedField(
                    fields, columns.phaseDiscontinuity) != 0;
            timelineDetails.recordedCadence = observedCadenceTracker.observe(
                unsignedField(fields, columns.sourceTimeUs),
                recordedSubmissionUs, recordedDiscontinuity);
            timelineDetails.simulatedCadence = simulatedCadenceTracker.observe(
                simulatedDecision.sourceTimeUs, simulatedSubmissionUs,
                simulatedDecision.rebased ||
                    simulatedDecision.phaseDiscontinuity);
            if (hadPriorSimulatedSubmission) {
                const uint64_t simulatedSpacingUs =
                    simulatedSubmissionUs >= priorSimulatedSubmissionUs ?
                        simulatedSubmissionUs - priorSimulatedSubmissionUs : 0;
                timelineDetails.simulatedSpacingMarginUs = signedDifference(
                    simulatedSpacingUs,
                    periodForRate(simulatedConfig.displayRefreshHz));
            }

            const int64_t pairedDelta = signedDifference(
                simulatedSubmissionUs, recordedSubmissionUs);
            metrics.pairedSubmissionDelta.add(pairedDelta);
            metrics.pairedAbsoluteSubmissionDelta.add(absoluteValue(
                pairedDelta));
            addCadenceFrame(metrics.observedRateBands,
                timelineDetails.recordedSourceRateHz, decodeCompleteUs,
                recordedSubmissionUs, timelineDetails.recordedCadence,
                recordedTear == "adaptive_interval_violation");
            addCadenceFrame(metrics.simulatedRateBands,
                timelineDetails.simulatedSourceRateHz, decodeCompleteUs,
                simulatedSubmissionUs, timelineDetails.simulatedCadence,
                simulatedTear == "adaptive_interval_violation");
            metrics.observedJerkAnomalies.observe(recordedSubmissionUs,
                timelineDetails.recordedCadence.jerkValid &&
                    absoluteValue(timelineDetails.recordedCadence.jerkUs) >=
                        kJerkAnomalyThresholdUs);
            metrics.simulatedJerkAnomalies.observe(simulatedSubmissionUs,
                timelineDetails.simulatedCadence.jerkValid &&
                    absoluteValue(timelineDetails.simulatedCadence.jerkUs) >=
                        kJerkAnomalyThresholdUs);

            metrics.exactSimulatedSubmissions +=
                simulatedSubmissionUs == recordedSubmissionUs ? 1 : 0;
            metrics.simulatedSubmissionDrift.add(absoluteValue(
                signedDifference(simulatedSubmissionUs,
                                 recordedSubmissionUs)));
            metrics.observedAbsoluteSubmitError.add(absoluteValue(
                signedField(fields, columns.submitErrorUs)));
            metrics.simulatedAbsoluteSubmitError.add(absoluteValue(
                signedDifference(simulatedSubmissionUs,
                                 simulatedDecision.targetUs)));
            metrics.observedDecodeToSubmission.addElapsed(
                recordedSubmissionUs, decodeCompleteUs);
            metrics.observedArrivalToSubmission.addElapsed(
                recordedSubmissionUs, pacerArrivalUs);
            metrics.observedDecisionToSubmission.addElapsed(
                recordedSubmissionUs, decisionUs);
            metrics.observedProjectedSourceToSubmission.addElapsed(
                recordedSubmissionUs,
                unsignedField(fields, columns.sourceTimeUs));
            metrics.observedSubmissionSpacing.add(unsignedField(
                fields, columns.submissionSpacingUs));
            metrics.simulatedDecodeToSubmission.addElapsed(
                simulatedSubmissionUs, decodeCompleteUs);
            metrics.simulatedArrivalToSubmission.addElapsed(
                simulatedSubmissionUs, pacerArrivalUs);
            metrics.simulatedDecisionToSubmission.addElapsed(
                simulatedSubmissionUs, decisionUs);
            metrics.simulatedProjectedSourceToSubmission.addElapsed(
                simulatedSubmissionUs, simulatedDecision.sourceTimeUs);

            if (simulatedDecision.latchedPresentation) {
                ++metrics.simulatedLatchedFrames;
            }
            if (hadPriorSimulatedSubmission) {
                const uint64_t spacingUs =
                    simulatedSubmissionUs >= priorSimulatedSubmissionUs ?
                        simulatedSubmissionUs - priorSimulatedSubmissionUs : 0;
                metrics.simulatedSubmissionSpacing.add(spacingUs);
                metrics.simulatedCadenceError.add(absoluteValue(
                    signedDifference(spacingUs,
                                     simulatedDecision.sourcePeriodUs)));
                if (simulatedTear == "adaptive_interval_violation") {
                    ++metrics.simulatedTearRisks;
                }
            }
            haveSimulatedSubmission = true;
            priorSimulatedSubmissionUs = simulatedSubmissionUs;
        }

        referenceController->noteSubmission(presented, cancelled,
                                             recordedSubmissionUs);
        simulatedController->noteSubmission(presented, cancelled,
                                             simulatedSubmissionUs);

        if (timelineFile.isOpen() &&
                !writeTimelineRow(timelineFile, arrivalSequence,
                    static_cast<int>(signedField(fields, columns.frame)),
                    disposition, decodeCompleteUs, pacerArrivalUs,
                    recordedTargetUs, simulatedDecision.targetUs,
                    recordedSubmissionUs, simulatedSubmissionUs,
                    recordedTear, simulatedTear, timelineDetails)) {
            std::fprintf(stderr, "Unable to write timeline: %s\n",
                         qPrintable(timelineFile.errorString()));
            return 1;
        }
    }
    if (!error.isEmpty()) {
        std::fprintf(stderr, "Trace read failed: %s\n", qPrintable(error));
        return 1;
    }
    if (timelineFile.isOpen()) {
        timelineFile.close();
        if (timelineFile.error() != QFileDevice::NoError) {
            std::fprintf(stderr, "Unable to finish timeline: %s\n",
                         qPrintable(timelineFile.errorString()));
            return 1;
        }
    }

    QJsonObject summary = summaryObject(
        metrics, timer.elapsed(), tracePath, QFileInfo(tracePath).size(),
        capturedConfig.displayRefreshHz, capturedConfig.streamRateHz,
        simulatedConfig.displayRefreshHz, simulatedConfig.streamRateHz,
        simulatedCanLatch, scenario);
    if (parser.isSet(compareOption)) {
        QFile baselineFile(parser.value(compareOption));
        if (!baselineFile.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "Unable to read comparison summary: %s\n",
                         qPrintable(baselineFile.errorString()));
            return 1;
        }
        QJsonParseError parseError;
        const QJsonDocument baselineDocument = QJsonDocument::fromJson(
            baselineFile.readAll(), &parseError);
        if (!baselineDocument.isObject()) {
            std::fprintf(stderr, "Invalid comparison summary: %s\n",
                         qPrintable(parseError.errorString()));
            return 1;
        }
        const QJsonObject baseline = baselineDocument.object();
        QJsonObject deltas;
        const char* lowerIsBetter[] = {
            "replay_tear_risks",
            "replay_abs_submit_error_p95_us",
            "replay_cadence_error_p95_us",
            "replay_decode_to_submission_p95_us",
            "replay_arrival_to_submission_p95_us",
            "replay_decode_to_submission_stddev_us",
            "replay_40_116_cadence_residual_p95_us",
            "replay_40_116_cadence_residual_p99_us",
            "replay_40_116_jerk_p95_us",
            "replay_40_116_jerk_p99_us",
            "replay_60_100_cadence_residual_p95_us",
            "replay_60_100_cadence_residual_p99_us",
            "replay_60_100_jerk_p95_us",
            "replay_60_100_jerk_p99_us",
            "paired_abs_submission_delta_p99_us",
        };
        int improved = 0;
        int regressed = 0;
        int compared = 0;
        for (const char* metric : lowerIsBetter) {
            if (!summary.contains(metric) || !baseline.contains(metric)) {
                continue;
            }
            const qint64 delta = static_cast<qint64>(
                summary.value(metric).toDouble()) - static_cast<qint64>(
                baseline.value(metric).toDouble());
            deltas[QString::fromLatin1(metric) + "_delta"] = delta;
            improved += delta < 0 ? 1 : 0;
            regressed += delta > 0 ? 1 : 0;
            ++compared;
        }
        deltas["compared_metrics"] = compared;
        deltas["improved_metrics"] = improved;
        deltas["regressed_metrics"] = regressed;
        deltas["verdict"] = regressed == 0 && improved != 0 ? "better" :
            improved == 0 && regressed != 0 ? "worse" :
            improved == 0 && regressed == 0 ? "unchanged" : "mixed";
        summary["comparison"] = deltas;
    }

    const bool assertionsPassed = evaluateAssertions(scenario, summary);
    const bool baselineExact = summary.value("fidelity").toObject().value(
        "baseline_exact").toBool();
    const QByteArray output = QJsonDocument(summary).toJson(
        QJsonDocument::Indented);
    if (parser.isSet(outputOption)) {
        QFile outputFile(parser.value(outputOption));
        if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                outputFile.write(output) != output.size()) {
            std::fprintf(stderr, "Unable to write replay summary: %s\n",
                         qPrintable(outputFile.errorString()));
            return 1;
        }
    }
    else {
        std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()),
                    stdout);
    }

    if (parser.isSet(exactOption) && !baselineExact) {
        std::fprintf(stderr,
                     "Exact baseline validation failed; inspect the fidelity object\n");
        return 3;
    }
    return assertionsPassed ? 0 : 4;
}
