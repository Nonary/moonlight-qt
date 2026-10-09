#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

// Observation-only, bounded history. Frame delivery never allocates or draws.
// The owner supplies synchronization; snapshots/drawing happen off the pacer.
namespace Overlay {
struct TimingGraphInput {
    uint64_t submissionUs = 0, targetUs = 0, sourcePeriodUs = 0;
    uint64_t bufferUs = 0, requestedBufferUs = 0, toleranceUs = 0;
    uint64_t submissionId = 0, displayId = 0, displayUs = 0;
    // Local observations against the original cadence slot, before a late
    // frame moves its final target. Zero timestamps mean unavailable.
    uint64_t sourceTimeUs = 0, networkReadyUs = 0, decoderOutputUs = 0, decoderReadyUs = 0;
    int64_t cadenceRetimingUs = 0;
    bool sourceTimingValid = false;
    uint32_t backend = 0;
    bool submitted = false, idValid = false, displayValid = false, discontinuity = false;
};
struct TimingGraphPoint {
    uint64_t submissionUs = 0, targetUs = 0, targetIntervalUs = 0, submissionIntervalUs = 0;
    uint64_t sourcePeriodUs = 0, bufferUs = 0, requestedBufferUs = 0, toleranceUs = 0;
    uint64_t submissionId = 0, displayUs = 0, generation = 0;
    uint64_t sourceTimeUs = 0, networkReadyUs = 0, decoderOutputUs = 0, decoderReadyUs = 0;
    int64_t cadenceRetimingUs = 0;
    bool sourceTimingValid = false;
    uint32_t backend = 0;
    bool idValid = false, breakBefore = false;
};
using TimingGraphPoints = std::vector<TimingGraphPoint>;
struct BufferHitchStats {
    static constexpr uint64_t WindowUs = 120000000;
    uint64_t toleranceUs = 0;
    uint64_t absorbed = 0, missed = 0, unknown = 0;
    uint64_t observedFrames = 0, measuredFrames = 0, worstExcessUs = 0;
};
struct TimingGraphSnapshot {
    TimingGraphPoints points;
    BufferHitchStats hitches;
};

// Three aligned lanes of raw frame intervals: planned (target) cadence, client
// submissions and OS-reported display events. The lanes share one millisecond
// axis centred on the planned interval, so a disturbance shows up in the lane
// where it enters the pipeline instead of in lines drawn over each other. A
// fourth panel compares incoming stage offsets with the actual buffer.
struct TimingGraphLayout {
    static constexpr int Lanes = 3, Frames = 240;
    static constexpr int BufferLane = Lanes;
    static constexpr int Width = 800, Left = 76, RightMargin = 14;
    static constexpr int LaneTop = 32, LaneSpacing = 78, PlotOffset = 24, PlotHeight = 46;
    static constexpr int Height = LaneTop + (Lanes + 1) * LaneSpacing + 98;
    static constexpr double RadiusUs = 2000.0;
    // Deviations this small are not noticeable at typical VRR rates; they are
    // drawn on the reference line so only noticeable disturbances stand out.
    static constexpr double FlatUs = 1000.0;
    static constexpr int titleTop(int lane) { return LaneTop + lane * LaneSpacing; }
    static constexpr int plotTop(int lane) { return titleTop(lane) + PlotOffset; }
    static constexpr int plotBottom(int lane) { return plotTop(lane) + PlotHeight; }
};

enum class TimingGraphLane { Target, Submit, Display };

// Signed stage offsets expose early delivery as well as lateness. The buffer
// threshold is the delay actually used by this frame, never a requested cap or
// a final target that was clamped after discovering late work. Decoder output
// is shown separately when no GPU-completion observation is available.
struct BufferGraphSample {
    double networkUs = 0, decoderOutputUs = 0, decoderReadyUs = 0, bufferUs = 0;
    bool valid = false, networkValid = false, outputValid = false, readyValid = false;
    bool absorbed() const { return readyValid && decoderReadyUs <= bufferUs; }
};

inline BufferGraphSample bufferGraphSample(const TimingGraphPoint& p)
{
    BufferGraphSample s;
    if (!p.sourceTimingValid || !p.sourceTimeUs) return s;
    const auto relative = [&](uint64_t at) {
        const double raw = at >= p.sourceTimeUs ? double(at - p.sourceTimeUs) : -double(p.sourceTimeUs - at);
        return raw - double(p.cadenceRetimingUs);
    };
    s.valid = true;
    s.bufferUs = double(p.bufferUs);
    s.networkValid = p.networkReadyUs != 0;
    s.outputValid = p.decoderOutputUs != 0 && (!s.networkValid || p.decoderOutputUs >= p.networkReadyUs);
    s.readyValid = p.decoderReadyUs != 0 && s.outputValid && p.decoderReadyUs >= p.decoderOutputUs;
    if (s.networkValid) s.networkUs = relative(p.networkReadyUs);
    if (s.outputValid) s.decoderOutputUs = relative(p.decoderOutputUs);
    if (s.readyValid) s.decoderReadyUs = relative(p.decoderReadyUs);
    return s;
}

// Interval ending at points[i] in one lane. Display intervals need OS
// feedback for both frames of the same epoch, so a missing presentation is
// never bridged into one long interval.
inline bool timingGraphInterval(const TimingGraphPoints& points, size_t i, TimingGraphLane lane, uint64_t& intervalUs)
{
    if (i >= points.size()) return false;
    const auto& p = points[i];
    switch (lane) {
    case TimingGraphLane::Target:
        intervalUs = p.targetIntervalUs;
        return intervalUs != 0;
    case TimingGraphLane::Submit:
        intervalUs = p.submissionIntervalUs;
        return intervalUs != 0;
    case TimingGraphLane::Display:
        if (i == 0) return false;
        {
            const auto& previous = points[i - 1];
            if (p.breakBefore || p.generation != previous.generation || p.backend != previous.backend ||
                !previous.displayUs || p.displayUs <= previous.displayUs) return false;
            intervalUs = p.displayUs - previous.displayUs;
        }
        return true;
    }
    return false;
}

// Independent of the 240-frame plot and its visibility. A hitch is one input
// frame ready beyond its profile tolerance after its cadence slot. Coverage uses that frame's
// applied buffer. Unknown GPU completion can identify a late lower bound but
// never establish successful absorption. Counters expire in 100 ms buckets.
class BufferHitchHistory {
public:
    static constexpr uint64_t BucketUs = 100000;
    static constexpr size_t Buckets = BufferHitchStats::WindowUs / BucketUs;
    void record(const TimingGraphPoint& p) {
        const uint64_t at = p.submissionUs;
        if (!at || at == m_LastAtUs) return;
        // Different tolerance means a different hitch definition; do not mix
        // counts from profiles or retain samples from a restarted local clock.
        if (at < m_LastAtUs || p.toleranceUs != m_ToleranceUs) m_Buckets->fill({});
        m_ToleranceUs = p.toleranceUs;
        m_LastAtUs = at;
        const uint64_t tick = at / BucketUs;
        auto& bucket = (*m_Buckets)[tick % Buckets];
        if (bucket.tick != tick) bucket = Bucket{tick};
        auto& stats = bucket.stats;
        ++stats.observedFrames;
        const auto s = bufferGraphSample(p);
        if (!s.valid || !p.toleranceUs) return;
        if (!s.readyValid) {
            if ((s.networkValid && s.networkUs > p.toleranceUs) ||
                (s.outputValid && s.decoderOutputUs > p.toleranceUs)) ++stats.unknown;
            return;
        }
        ++stats.measuredFrames;
        if (s.decoderReadyUs <= p.toleranceUs) return;
        if (s.absorbed()) ++stats.absorbed;
        else {
            ++stats.missed;
            stats.worstExcessUs = std::max(stats.worstExcessUs,
                uint64_t(std::ceil(s.decoderReadyUs - s.bufferUs)));
        }
    }
    BufferHitchStats snapshot(uint64_t atUs = 0) const {
        const uint64_t tick = (atUs ? atUs : m_LastAtUs) / BucketUs;
        BufferHitchStats result;
        result.toleranceUs = m_ToleranceUs;
        for (const auto& b : *m_Buckets) {
            if (!b.stats.observedFrames || b.tick > tick || tick - b.tick >= Buckets) continue;
            result.absorbed += b.stats.absorbed; result.missed += b.stats.missed;
            result.unknown += b.stats.unknown;
            result.observedFrames += b.stats.observedFrames;
            result.measuredFrames += b.stats.measuredFrames;
            result.worstExcessUs = std::max(result.worstExcessUs, b.stats.worstExcessUs);
        }
        return result;
    }
private:
    struct Bucket { uint64_t tick = 0; BufferHitchStats stats; };
    std::unique_ptr<std::array<Bucket, Buckets>> m_Buckets =
        std::make_unique<std::array<Bucket, Buckets>>();
    uint64_t m_LastAtUs = 0, m_ToleranceUs = 0;
};

class TimingGraphHistory {
public:
    static constexpr size_t SnapshotPoints = TimingGraphLayout::Frames + 1;
    static constexpr size_t Capacity = 256;

    void record(const TimingGraphInput& in)
    {
        if (in.discontinuity) { ++m_Generation; m_Break = true; }
        if (in.submitted && in.submissionUs) {
            const TimingGraphPoint* prev = m_Count ? &(*m_Points)[(m_Next + Capacity - 1) % Capacity] : nullptr;
            if (prev && (in.submissionUs <= prev->submissionUs || in.backend != prev->backend ||
                         (in.idValid && prev->idValid && in.submissionId <= prev->submissionId))) {
                ++m_Generation; m_Break = true;
            }
            TimingGraphPoint p;
            p.submissionUs = in.submissionUs; p.targetUs = in.targetUs;
            p.sourcePeriodUs = in.sourcePeriodUs; p.bufferUs = in.bufferUs;
            p.requestedBufferUs = in.requestedBufferUs;
            p.toleranceUs = in.toleranceUs;
            p.sourceTimeUs = in.sourceTimeUs; p.cadenceRetimingUs = in.cadenceRetimingUs;
            p.sourceTimingValid = in.sourceTimingValid;
            p.networkReadyUs = in.networkReadyUs; p.decoderOutputUs = in.decoderOutputUs;
            p.decoderReadyUs = in.decoderReadyUs;
            p.submissionId = in.submissionId; p.idValid = in.idValid;
            p.backend = in.backend; p.generation = m_Generation; p.breakBefore = m_Break;
            m_Hitches.record(p);
            if (prev && !m_Break) {
                p.submissionIntervalUs = in.submissionUs - prev->submissionUs;
                if (prev->targetUs && in.targetUs > prev->targetUs)
                    p.targetIntervalUs = in.targetUs - prev->targetUs;
            }
            (*m_Points)[m_Next] = p; m_Next = (m_Next + 1) % Capacity;
            if (m_Count < Capacity) ++m_Count;
            m_Break = false;
        }
        // Feedback commonly names an earlier frame. Never attach it to the
        // frame whose Present happened to return the observation. Refresh
        // references are excluded by the caller; only DisplayEvent is valid.
        if (in.displayValid && in.displayUs && in.backend) {
            for (size_t age = 0; age < m_Count; ++age) {
                auto& p = (*m_Points)[(m_Next + Capacity - 1 - age) % Capacity];
                if (p.generation != m_Generation) break;
                if (p.idValid && p.backend == in.backend && p.submissionId == in.displayId) {
                    if (!p.displayUs && in.displayUs >= p.submissionUs) p.displayUs = in.displayUs;
                    break;
                }
            }
        }
    }

    // The newest drawn frames plus the predecessor of the oldest, which the
    // display lane needs for its first interval.
    void copyTo(TimingGraphSnapshot& snapshot, uint64_t atUs = 0) const
    {
        auto& out = snapshot.points;
        out.clear();
        const size_t count = std::min(m_Count, SnapshotPoints);
        const auto begin = (m_Next + Capacity - count) % Capacity;
        for (size_t i = 0; i < count; ++i) out.push_back((*m_Points)[(begin + i) % Capacity]);
        snapshot.hitches = m_Hitches.snapshot(atUs);
    }
private:
    // One allocation at pacer construction; avoid a large inline object on
    // Windows test/main-thread stacks. No allocations while recording.
    std::unique_ptr<std::array<TimingGraphPoint, Capacity>> m_Points =
        std::make_unique<std::array<TimingGraphPoint, Capacity>>();
    size_t m_Count = 0, m_Next = 0;
    uint64_t m_Generation = 0;
    bool m_Break = true;
    BufferHitchHistory m_Hitches;
};
}
