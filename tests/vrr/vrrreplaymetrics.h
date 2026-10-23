#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

// Shared by replay and deterministic tests: test the actual scoring path.
namespace VrrReplayMetrics {

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
    static constexpr size_t kPageSize = 256;
    static constexpr size_t kPageCount =
        (kBucketCount + kPageSize - 1) / kPageSize;
    using HistogramPage = std::array<uint32_t, kPageSize>;

    uint64_t count = 0;
    uint64_t minimum = 0;
    uint64_t maximum = 0;
    long double mean = 0;
    long double squaredDifferenceTotal = 0;
    // Most distributions occupy only a few of the 168k possible buckets.
    // Keep exact bucket counts without allocating the whole range per metric.
    std::vector<std::unique_ptr<HistogramPage>> histogram;
    std::vector<uint64_t> pageTotals;

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
            histogram.resize(kPageCount);
            pageTotals.resize(kPageCount);
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
        const size_t bucket = bucketFor(value);
        const size_t pageIndex = bucket / kPageSize;
        auto& page = histogram[pageIndex];
        if (!page) page = std::make_unique<HistogramPage>();
        ++(*page)[bucket % kPageSize];
        ++pageTotals[pageIndex];
    }

    void addElapsed(uint64_t endUs, uint64_t startUs)
    {
        if (startUs != 0 && endUs >= startUs) {
            add(endUs - startUs);
        }
    }

    uint64_t percentileRatio(uint64_t numerator, uint64_t denominator) const
    {
        if (count == 0 || denominator == 0) {
            return 0;
        }
        const uint64_t boundedNumerator = std::min(denominator, numerator);
        const uint64_t rank = std::max<uint64_t>(
            1,
            (count / denominator) * boundedNumerator +
                ((count % denominator) * boundedNumerator +
                 denominator - 1) /
                    denominator);
        uint64_t cumulative = 0;
        for (size_t pageIndex = 0; pageIndex < pageTotals.size(); ++pageIndex) {
            if (cumulative + pageTotals[pageIndex] < rank) {
                cumulative += pageTotals[pageIndex];
                continue;
            }
            const HistogramPage& page = *histogram[pageIndex];
            for (size_t offset = 0; offset < kPageSize; ++offset) {
                const size_t bucket = pageIndex * kPageSize + offset;
                if (bucket >= kBucketCount) break;
                cumulative += page[offset];
                if (cumulative >= rank) {
                    return bucketUpperBound(bucket);
                }
            }
        }
        return maximum;
    }

    uint64_t percentile(unsigned int percent) const
    {
        return percentileRatio(percent, 100);
    }
};

// Sender-spacing cadence. Compares each consecutive pair of presented
// submissions against the sender's own RTP spacing, which is the cadence the
// game produced. The legacy sender metric excludes pairs beyond its source/arrival interval
// limit. This is a metric exclusion, not evidence of a host stall. A
// hitch is a pair presented more than two milliseconds wider than the sender
// spacing; hitches are attributed to a frame that arrived later than the
// playout delay, a render-lead jump, the display-spacing floor, or other.
struct SenderCadenceTracker {
    static constexpr uint64_t kStallIntervalUs = 25000;
    static constexpr uint64_t kHitchUs = 2000;
    static constexpr uint64_t kRenderLeadJumpUs = 500;
    static constexpr uint64_t kFloorSlackUs = 300;

    bool havePrior = false;
    bool haveResidual = false;
    uint64_t priorSenderUs = 0;
    uint64_t priorDecodeUs = 0;
    uint64_t priorSubmissionUs = 0;
    int64_t priorResidualUs = 0;
    uint64_t pairs = 0;
    uint64_t hitches = 0;
    uint64_t spacingErrorsOverHitch = 0;
    uint64_t clientSpacingPairs = 0;
    uint64_t clientSpacingErrorsOver3ms = 0;
    uint64_t sourceStallPairs = 0;
    Distribution clientSpacingErrorUs;
    uint64_t hitchLateArrivals = 0;
    uint64_t hitchRenderLeadJumps = 0;
    uint64_t hitchDisplayFloor = 0;
    uint64_t hitchOther = 0;
    Distribution absoluteSpacingErrorUs;
    Distribution absoluteJerkUs;
    // Evenness of what was shown, independent of the sender: the change in
    // presented interval from one pair to the next. This is the quantity the
    // viewer perceives as stutter; matching a jittery sender scores well on
    // the fields above and badly here.
    bool havePresentedInterval = false;
    uint64_t priorPresentedIntervalUs = 0;
    uint64_t presentedJerkPairs = 0;
    uint64_t presentedJerkOverHitch = 0;
    Distribution presentedJerkUs;

    void observe(uint64_t senderUs, uint64_t decodeUs, uint64_t submissionUs,
                 bool lateArrival, bool renderLeadJump,
                 uint64_t displayPeriodUs)
    {
        // Local timestamps use zero for unavailable evidence. Do not let a
        // missing endpoint become the predecessor of a later clean interval.
        // The sender clock may legitimately begin at RTP timestamp zero.
        if (decodeUs == 0 || submissionUs == 0) {
            havePrior = haveResidual = havePresentedInterval = false;
            return;
        }
        if (havePrior && senderUs > priorSenderUs &&
                decodeUs >= priorDecodeUs &&
                submissionUs >= priorSubmissionUs) {
            const uint64_t senderIntervalUs = senderUs - priorSenderUs;
            const uint64_t arrivalIntervalUs = decodeUs - priorDecodeUs;
            if (senderIntervalUs <= kStallIntervalUs) {
                // The user's 3 ms client-error goal must include network and
                // decode stalls. Only a gap on the source clock is excluded;
                // a long local arrival gap cannot excuse a client hitch.
                const uint64_t actual = submissionUs - priorSubmissionUs;
                const uint64_t error = actual > senderIntervalUs ?
                    actual - senderIntervalUs : senderIntervalUs - actual;
                ++clientSpacingPairs;
                clientSpacingErrorsOver3ms += error > 3000;
                clientSpacingErrorUs.add(error);
            }
            else {
                ++sourceStallPairs;
            }
            if (senderIntervalUs <= kStallIntervalUs &&
                    arrivalIntervalUs <= kStallIntervalUs) {
                const uint64_t submissionIntervalUs =
                    submissionUs - priorSubmissionUs;
                if (havePresentedInterval) {
                    const uint64_t jerkUs =
                        submissionIntervalUs > priorPresentedIntervalUs ?
                            submissionIntervalUs - priorPresentedIntervalUs :
                            priorPresentedIntervalUs - submissionIntervalUs;
                    presentedJerkUs.add(jerkUs);
                    ++presentedJerkPairs;
                    if (jerkUs > kHitchUs) {
                        ++presentedJerkOverHitch;
                    }
                }
                havePresentedInterval = true;
                priorPresentedIntervalUs = submissionIntervalUs;
                const int64_t residualUs =
                    static_cast<int64_t>(submissionIntervalUs) -
                    static_cast<int64_t>(senderIntervalUs);
                ++pairs;
                absoluteSpacingErrorUs.add(static_cast<uint64_t>(
                    residualUs < 0 ? -residualUs : residualUs));
                spacingErrorsOverHitch += residualUs > static_cast<int64_t>(kHitchUs) ||
                    residualUs < -static_cast<int64_t>(kHitchUs);
                if (haveResidual) {
                    const int64_t jerkUs = residualUs - priorResidualUs;
                    absoluteJerkUs.add(static_cast<uint64_t>(
                        jerkUs < 0 ? -jerkUs : jerkUs));
                }
                haveResidual = true;
                priorResidualUs = residualUs;
                if (residualUs > static_cast<int64_t>(kHitchUs)) {
                    ++hitches;
                    if (lateArrival) {
                        ++hitchLateArrivals;
                    }
                    else if (renderLeadJump) {
                        ++hitchRenderLeadJumps;
                    }
                    else if (senderIntervalUs < displayPeriodUs &&
                             submissionIntervalUs <=
                                 displayPeriodUs + kFloorSlackUs) {
                        ++hitchDisplayFloor;
                    }
                    else {
                        ++hitchOther;
                    }
                }
            }
            else {
                haveResidual = false;
                havePresentedInterval = false;
            }
        }
        else {
            haveResidual = false;
            havePresentedInterval = false;
        }
        havePrior = true;
        priorSenderUs = senderUs;
        priorDecodeUs = decodeUs;
        priorSubmissionUs = submissionUs;
    }
};

} // namespace VrrReplayMetrics
