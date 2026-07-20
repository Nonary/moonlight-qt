#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <QMutex>

// Pacer work can happen on the decoder, render, V-sync, and VRR worker
// threads. Keep its cumulative measurements separate from VIDEO_STATS, which
// is owned by the decoder thread and is periodically windowed/reset there.
// A snapshot is copied while holding this mutex, so its counters and latest
// state always describe one coherent publication.
struct PacerTelemetrySnapshot {
    uint64_t sequence = 0;
    uint64_t sampleTimeUs = 0;

    uint64_t renderedFrames = 0;
    uint64_t pacerDroppedFrames = 0;
    uint64_t totalPacerTimeUs = 0;
    uint64_t totalRenderTimeUs = 0;

    bool vrrActive = false;
    uint64_t vrrPacingDroppedFrames = 0;
    uint64_t vrrEligibleFrames = 0;
    uint64_t vrrPrepareLateFrames = 0;
    uint64_t vrrTargetWaitEntryLateFrames = 0;
    uint64_t vrrPresentFailedFrames = 0;
    uint64_t vrrPresentCancelledFrames = 0;
    uint64_t vrrSpacingCorrections = 0;

    uint64_t vrrPrepareLatenessP50Us = 0;
    uint64_t vrrPrepareLatenessP95Us = 0;
    uint64_t vrrPrepareLatenessP99Us = 0;
    int64_t vrrSubmitErrorP50Us = 0;
    int64_t vrrSubmitErrorP95Us = 0;
    int64_t vrrSubmitErrorP99Us = 0;
    int64_t vrrSubmitErrorMaxUs = 0;

    // These are a decision-time sample, not an aggregate or a proxy for the
    // target used by a different frame.
    uint64_t vrrStateSequence = 0;
    uint64_t vrrStateSampleTimeUs = 0;
    int64_t vrrReadinessBudgetUs = 0;
    uint64_t vrrTimingBudgetUs = 0;
    uint64_t vrrRenderLeadUs = 0;
    uint64_t vrrRenderWakeLeadUs = 0;
    uint64_t vrrTargetWakeLeadUs = 0;
    uint64_t vrrGuardUs = 0;
    uint64_t vrrSourcePeriodUs = 0;
};

struct VrrTelemetrySample {
    uint64_t publicationTimeUs = 0;
    uint64_t decisionTimeUs = 0;
    uint64_t pacerTimeUs = 0;
    uint64_t renderTimeUs = 0;
    uint64_t preparationLatenessUs = 0;
    int64_t submitErrorUs = 0;

    bool prepareLate = false;
    bool targetWaitEntryLate = false;
    bool spacingCorrected = false;
    bool presented = false;
    bool cancelled = false;

    int64_t readinessBudgetUs = 0;
    uint64_t timingBudgetUs = 0;
    uint64_t renderLeadUs = 0;
    uint64_t renderWakeLeadUs = 0;
    uint64_t targetWakeLeadUs = 0;
    uint64_t guardUs = 0;
    uint64_t sourcePeriodUs = 0;
};

class PacerTelemetry {
public:
    PacerTelemetrySnapshot snapshot() const
    {
        QMutexLocker lock(&m_Lock);
        PacerTelemetrySnapshot snapshot = m_Snapshot;
        populatePrepareLatenessPercentilesLocked(snapshot);
        populateSubmitErrorPercentilesLocked(snapshot);
        return snapshot;
    }

    void beginVrrSession(uint64_t sampleTimeUs)
    {
        QMutexLocker lock(&m_Lock);
        m_Snapshot.vrrActive = true;
        touchLocked(sampleTimeUs);
    }

    void recordLegacyDrop(uint64_t sampleTimeUs)
    {
        QMutexLocker lock(&m_Lock);
        ++m_Snapshot.pacerDroppedFrames;
        touchLocked(sampleTimeUs);
    }

    void recordLegacyFrame(uint64_t pacerTimeUs,
                           uint64_t renderTimeUs,
                           uint64_t sampleTimeUs)
    {
        QMutexLocker lock(&m_Lock);
        m_Snapshot.totalPacerTimeUs += pacerTimeUs;
        m_Snapshot.totalRenderTimeUs += renderTimeUs;
        ++m_Snapshot.renderedFrames;
        touchLocked(sampleTimeUs);
    }

    void recordVrrDrop(uint64_t sampleTimeUs)
    {
        QMutexLocker lock(&m_Lock);
        ++m_Snapshot.pacerDroppedFrames;
        ++m_Snapshot.vrrPacingDroppedFrames;
        touchLocked(sampleTimeUs);
    }

    void recordVrrOutcome(bool presented,
                          bool cancelled,
                          uint64_t sampleTimeUs)
    {
        QMutexLocker lock(&m_Lock);
        recordVrrOutcomeLocked(presented, cancelled);
        touchLocked(sampleTimeUs);
    }

    void recordVrrFrame(const VrrTelemetrySample& sample)
    {
        QMutexLocker lock(&m_Lock);

        ++m_Snapshot.vrrEligibleFrames;
        m_Snapshot.totalPacerTimeUs += sample.pacerTimeUs;
        m_Snapshot.totalRenderTimeUs += sample.renderTimeUs;
        if (sample.prepareLate) {
            ++m_Snapshot.vrrPrepareLateFrames;
            addPrepareLatenessLocked(sample.preparationLatenessUs);
        }
        if (sample.targetWaitEntryLate) {
            ++m_Snapshot.vrrTargetWaitEntryLateFrames;
        }
        // Submission error is meaningful only for output that was actually
        // presented. Cancelled submissions are reported separately.
        if (sample.presented && !sample.cancelled) {
            addSubmitErrorLocked(sample.submitErrorUs);
        }
        if (sample.spacingCorrected) {
            ++m_Snapshot.vrrSpacingCorrections;
        }
        recordVrrOutcomeLocked(sample.presented, sample.cancelled);
        if (sample.presented && !sample.cancelled) {
            ++m_Snapshot.renderedFrames;
        }

        touchLocked(sample.publicationTimeUs);
        m_Snapshot.vrrStateSequence = m_Snapshot.sequence;
        m_Snapshot.vrrStateSampleTimeUs = sample.decisionTimeUs;
        m_Snapshot.vrrReadinessBudgetUs = sample.readinessBudgetUs;
        m_Snapshot.vrrTimingBudgetUs = sample.timingBudgetUs;
        m_Snapshot.vrrRenderLeadUs = sample.renderLeadUs;
        m_Snapshot.vrrRenderWakeLeadUs = sample.renderWakeLeadUs;
        m_Snapshot.vrrTargetWakeLeadUs = sample.targetWakeLeadUs;
        m_Snapshot.vrrGuardUs = sample.guardUs;
        m_Snapshot.vrrSourcePeriodUs = sample.sourcePeriodUs;
    }

private:
    static constexpr size_t kPrepareLatenessSampleCount = 128;
    static constexpr size_t kSubmitErrorSampleCount = 128;

    static size_t percentileIndex(size_t count, size_t percentile)
    {
        // Use the nearest-rank definition. count is nonzero at each call.
        return ((count * percentile + 99) / 100) - 1;
    }

    void touchLocked(uint64_t sampleTimeUs)
    {
        ++m_Snapshot.sequence;
        m_Snapshot.sampleTimeUs = sampleTimeUs;
    }

    void recordVrrOutcomeLocked(bool presented, bool cancelled)
    {
        if (cancelled) {
            ++m_Snapshot.vrrPresentCancelledFrames;
        }
        else if (!presented) {
            ++m_Snapshot.vrrPresentFailedFrames;
        }
    }

    void addPrepareLatenessLocked(uint64_t latenessUs)
    {
        m_PrepareLatenessSamples[m_NextPrepareLatenessSample] = latenessUs;
        m_NextPrepareLatenessSample =
            (m_NextPrepareLatenessSample + 1) % kPrepareLatenessSampleCount;
        m_PrepareLatenessSampleSize = std::min(
            m_PrepareLatenessSampleSize + 1, kPrepareLatenessSampleCount);
    }

    void populatePrepareLatenessPercentilesLocked(
        PacerTelemetrySnapshot& snapshot) const
    {
        if (m_PrepareLatenessSampleSize == 0) {
            return;
        }

        std::array<uint64_t, kPrepareLatenessSampleCount> sortedSamples =
            m_PrepareLatenessSamples;
        std::sort(sortedSamples.begin(),
                  sortedSamples.begin() + m_PrepareLatenessSampleSize);
        snapshot.vrrPrepareLatenessP50Us = sortedSamples[
            percentileIndex(m_PrepareLatenessSampleSize, 50)];
        snapshot.vrrPrepareLatenessP95Us = sortedSamples[
            percentileIndex(m_PrepareLatenessSampleSize, 95)];
        snapshot.vrrPrepareLatenessP99Us = sortedSamples[
            percentileIndex(m_PrepareLatenessSampleSize, 99)];
    }

    void addSubmitErrorLocked(int64_t errorUs)
    {
        m_SubmitErrorSamples[m_NextSubmitErrorSample] = errorUs;
        m_NextSubmitErrorSample =
            (m_NextSubmitErrorSample + 1) % kSubmitErrorSampleCount;
        m_SubmitErrorSampleSize = std::min(
            m_SubmitErrorSampleSize + 1, kSubmitErrorSampleCount);
    }

    void populateSubmitErrorPercentilesLocked(
        PacerTelemetrySnapshot& snapshot) const
    {
        if (m_SubmitErrorSampleSize == 0) {
            return;
        }

        std::array<int64_t, kSubmitErrorSampleCount> sortedSamples =
            m_SubmitErrorSamples;
        std::sort(sortedSamples.begin(),
                  sortedSamples.begin() + m_SubmitErrorSampleSize);
        snapshot.vrrSubmitErrorP50Us = sortedSamples[
            percentileIndex(m_SubmitErrorSampleSize, 50)];
        snapshot.vrrSubmitErrorP95Us = sortedSamples[
            percentileIndex(m_SubmitErrorSampleSize, 95)];
        snapshot.vrrSubmitErrorP99Us = sortedSamples[
            percentileIndex(m_SubmitErrorSampleSize, 99)];
        snapshot.vrrSubmitErrorMaxUs = sortedSamples[
            m_SubmitErrorSampleSize - 1];
    }

    mutable QMutex m_Lock;
    PacerTelemetrySnapshot m_Snapshot;
    std::array<uint64_t, kPrepareLatenessSampleCount> m_PrepareLatenessSamples {};
    size_t m_NextPrepareLatenessSample = 0;
    size_t m_PrepareLatenessSampleSize = 0;
    std::array<int64_t, kSubmitErrorSampleCount> m_SubmitErrorSamples {};
    size_t m_NextSubmitErrorSample = 0;
    size_t m_SubmitErrorSampleSize = 0;
};
