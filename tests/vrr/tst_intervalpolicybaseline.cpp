#include "vrrproductionsession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

// Scan the original physical rings on every observation. This independent
// reference also sees retained future buckets again after a backwards clock.
static bool rollingScoresMatchScan()
{
    using Buffer = Vrr13::IntervalBuffer;
    enum Field { Evaluated, Failed, Loss, ClientEvaluated, ClientLoss, BufferEvaluated,
                 BufferFailed, BufferLoss, FieldCount };
    using Scores = std::array<double, FieldCount>;
    struct ScoreEntry { uint64_t tick = 0; Scores value{}; };
    struct ShortSample { uint64_t tick, error, service, intended; bool client; };
    std::vector<ScoreEntry> reference(Buffer::MaximumScoreBuckets);
    std::deque<ShortSample> recent;
    Buffer buffer;
    Buffer::Sample previous;
    uint64_t source = 297000000, first = 0, samples = 0, warmup = 0;
    bool calibrated = false;
    for (uint64_t i = 1; i <= 6600; ++i) {
        if (i == 1900) source -= 17000000;
        if (i == 3300) source += 401000000;
        if (i == 3411) source -= 400000000;
        if (i == 5000) {
            buffer.reset();
            std::fill(reference.begin(), reference.end(), ScoreEntry{});
            previous = {};
            calibrated = false;
        }
        source += i % 5 ? 33333 : 2501;
        const uint64_t lag = i % 11 == 0 ? 4100 : i % 7 * 213;
        Buffer::Sample s{i, source, source + lag, source, source + lag, 1500, true, true};
        s.serialService = i % 37 < 23 ? 45000 : 400;
        s.inputOnTime = i % 13 < 9;
        if (i % 711 == 0) s.valid = false;
        if (i % 881 == 0) s.submitted = 0;
        constexpr uint64_t windows[] = {300000000, 120000001, 200000, 0, 900000000};
        const uint64_t window = windows[(i / 193) % 5];
        const bool weighted = i % 79 < 64, averaged = i % 107 < 71, shared = i % 131 < 41;
        buffer.observe(s, 0, 8000, 1000000, 250, weighted, 995000, 500, window,
                       250000, 2, averaged, 0, shared);
        const auto observed = buffer.stats();
        const bool adjacent = previous.valid && s.valid && s.submitted > previous.submitted &&
            previous.submitted && s.submitted - previous.submitted < 1000000 &&
            s.intended > previous.intended && s.intended - previous.intended < 1000000;
        const auto tick = s.submitted / Buffer::ScoreBucketUs;
        double average = 0, clientAverage = 0;
        bool clientAverageValid = false, averageValid = false, overloaded = false;
        if (adjacent) {
            const uint64_t actual = s.submitted - previous.submitted, intended = s.intended - previous.intended;
            const uint64_t error = std::max(actual, intended) - std::min(actual, intended);
            const bool client = previous.inputOnTime && s.inputOnTime;
            const uint64_t shortTick = s.submitted / 10000;
            recent.push_back({shortTick, error, s.serialService, intended, client});
            while (shortTick - recent.front().tick >= 100) recent.pop_front();
            uint64_t total = 0, clientError = 0, clients = 0, service = 0, intendedTotal = 0;
            for (const auto& item : recent) {
                total += item.error;
                if (item.client) { clientError += item.error; ++clients; }
                service += item.service;
                intendedTotal += item.intended;
            }
            average = double(total) / recent.size();
            clientAverageValid = clients > 0;
            clientAverage = clients ? double(clientError) / clients : 0;
            if (!samples) { first = s.submitted; warmup = calibrated ? 1000000 : 250000; }
            ++samples;
            averageValid = recent.size() >= 2 && samples >= 2 && s.submitted - first >= warmup;
            calibrated |= averageValid;
            overloaded = averageValid && service > intendedTotal;

            auto& bucket = reference[tick % reference.size()];
            if (bucket.tick != tick) bucket = {tick, {}};
            auto& score = bucket.value;
            const double loss = actual * std::clamp((double(error) - 500) / intended, 0.0, 1.0);
            score[Evaluated] += actual;
            if (error > 500) score[Failed] += actual;
            if (weighted) score[Loss] += loss;
            if (client) { score[ClientEvaluated] += actual; score[ClientLoss] += loss; }
            if (!shared && (!averaged || averageValid)) {
                const double excess = std::max(0.0, (averaged ? average : double(error)) - 500);
                score[BufferEvaluated] += actual;
                if (excess > 0) score[BufferFailed] += actual;
                if (weighted) score[BufferLoss] += actual * std::min(1.0, excess / intended);
            }
        } else {
            recent.clear();
            samples = 0;
        }
        previous = s;
        if (observed.averageErrorUs != average || observed.clientAverageErrorUs != clientAverage ||
            observed.clientAverageValid != clientAverageValid || observed.averageValid != averageValid ||
            (adjacent && observed.serviceOverloaded != overloaded)) {
            std::fprintf(stderr, "one-second history differs from scan at frame %llu\n",
                         static_cast<unsigned long long>(i));
            return false;
        }

        const auto width = std::clamp<uint64_t>((window + Buffer::ScoreBucketUs - 1) / Buffer::ScoreBucketUs,
                                                 1, reference.size());
        Scores expected{};
        for (const auto& b : reference) {
            if (b.tick <= tick && tick - b.tick < width)
                for (size_t j = 0; j < expected.size(); ++j) expected[j] += b.value[j];
        }
        const Scores actual{double(observed.evaluatedUs), double(observed.failedUs), observed.weightedLossUs,
            double(observed.clientEvaluatedUs), observed.clientWeightedLossUs, double(observed.bufferEvaluatedUs),
            double(observed.bufferFailedUs), observed.bufferWeightedLossUs};
        for (size_t j = 0; j < expected.size(); ++j) {
            const bool fractional = j == Loss || j == ClientLoss || j == BufferLoss;
            const double tolerance = fractional ? 1e-12 * expected[j] : 0;
            if (std::abs(actual[j] - expected[j]) > tolerance) {
                std::fprintf(stderr, "score field %zu at frame %llu: expected %.17g, got %.17g\n",
                             j, static_cast<unsigned long long>(i), expected[j], actual[j]);
                return false;
            }
        }
    }
    return true;
}

// This stream sits exactly on a 99% goal in real arithmetic. Each pair
// loses 101.51 us in binary floating point. A different summation order can
// put it on the other side of the growth/release branch.
static bool thresholdMatchesOriginalOrder()
{
    using Buffer = Vrr13::IntervalBuffer;
    Buffer buffer;
    struct Entry { uint64_t tick = 0; double loss = 0; };
    std::vector<Entry> ring(Buffer::MaximumScoreBuckets);
    for (uint64_t i = 1; i <= 1200; ++i) {
        const uint64_t submitted = 297000000 + i * 10151;
        Buffer::Sample s{i, 297000000 + i * 10000, submitted, submitted - 100, submitted,
                         1000, true, true};
        buffer.observe(s, 0, 9000, 100000, 250, true, 990000, 51, 5100000, 0, 2,
                       false, 0, true);
        const auto tick = submitted / Buffer::ScoreBucketUs;
        auto& bucket = ring[tick % ring.size()];
        if (bucket.tick != tick) bucket = {tick, 0};
        if (i > 1) bucket.loss += 10151 * (100.0 / 10000);
        double expected = 0;
        for (const auto& entry : ring)
            if (entry.tick <= tick && tick - entry.tick < 51) expected += entry.loss;
        const auto stats = buffer.stats();
        if (stats.averageValid && stats.weightedLossUs != expected) {
            std::fprintf(stderr, "target-boundary summation changed at frame %llu\n",
                         static_cast<unsigned long long>(i));
            return false;
        }
    }
    return true;
}

// These fingerprints record current interval diagnostics before removing
// the remaining retired interval policy branches. They are a regression guard, not an optical measurement.
// Re-recorded 2026-11-09 for the absorbable-loss solver and release hold. The
// first late frames of each rate layer are budgeted against a quarter of the
// goal window, and every release waits out the hold, so 51 of 54 changed.
// Re-recorded 2026-11-13: the solver's aim may cost at most 500 us beyond what
// the goal itself needs, so 33 of 54 changed.
static uint64_t run(int preset, int smoothing, int presentation, bool custom)
{
    VrrSessionConfig session;
    session.latencyMode = preset;
    session.streamRateHz = 120;
    session.displayRefreshHz = 120;
    session.smoothFrameTiming = smoothing != 0;
    session.absorbMicrostutter = smoothing == 2;
    if (custom) session.timingOptions = {1500, 9975, 30, 750};
    session = vrrHistoricalGoalConfig(session);
    const bool latch = presentation != 0;
    const bool scheduled = presentation == 2;
    auto policy = vrrTimingParametersForSession(session, scheduled, scheduled);
    // Compare against the captured pre-merge policy. Separate worker and
    // startup tests exercise the new ceiling and preparation safeguards.
    policy.playoutCeilingPhaseReplacement = 0;
    policy.playoutOffsetSendSpanNormalization = 0;
    policy.preparationInitialSampleExcluded = 1;
    VrrTimingController controller(session, latch, policy);
    uint64_t digest = 1469598103934665603ULL;
    const auto append = [&](uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            digest ^= uint8_t(value >> (byte * 8));
            digest *= 1099511628211ULL;
        }
    };
    uint64_t sourceUs = 1000000;
    uint32_t rtp = 0;
    uint64_t previousSubmit = 0;
    for (unsigned i = 1; i <= 3600; ++i) {
        // Exercise steady cadence, a rate transition, gaps, jitter, and recovery.
        const uint64_t period = i >= 1200 && i < 1800 ? 16667 : 8333;
        sourceUs += period;
        rtp += period == 16667 ? 1500 : 750;
        const uint64_t delay = i % 113 == 0 ? 4700 : i % 17 == 0 ? 900 : 150;
        const uint64_t ready = sourceUs + delay;
        PacedFrame frame(nullptr, int(i), rtp, true, ready);
        frame.setDeliveryTimeline(sourceUs + 20, sourceUs + 60, sourceUs + 100);
        if (i == 2300) frame.setLostPackets(1);
        const auto decision = controller.schedule(frame, ready);
        const uint64_t prepared = ready + (i % 79 == 0 ? 1800 : 350);
        controller.notePreparationDuration(prepared - ready, 0, prepared, 0);
        const uint64_t submitted = std::max({decision.targetUs, prepared, previousSubmit + 1});
        controller.noteSchedulerDelays(0, i % 149 == 0 ? 1200 : 0, true);
        controller.noteSubmission(true, false, submitted);
        previousSubmit = submitted;
        append(decision.targetUs); append(decision.renderStartUs);
        append(decision.playoutDelayUs); append(decision.sourcePeriodUs);
        append(uint64_t(decision.cadenceSmoothingUs)); append(decision.latchedPresentation);
        append(decision.rebased); append(decision.phaseDiscontinuity);
        append(submitted); append(controller.earliestSubmissionUs());
        const auto interval = controller.intervalStats();
        append(interval.evaluatedUs); append(interval.failedUs);
        append(interval.averageValid); append(interval.serviceOverloaded);
        append(interval.initialCalibrationComplete); append(interval.calibrationCoverageUs);
        append(interval.calibrationSamples); append(interval.update.atUs);
        append(interval.update.frame); append(interval.update.attributedFrame);
        append(interval.update.intervalErrorUs); append(interval.update.latenessUs);
        append(uint64_t(interval.update.action));
        const auto stats = controller.intervalStats().inputPercentile;
        append(stats.targetUs); append(stats.currentUs); append(stats.samples);
        append(stats.eligible); append(stats.ineligible); append(stats.absorbed);
        append(stats.missed); append(stats.recentEligible); append(stats.recentMissed);
        append(stats.validPairCoverageUs); append(stats.tailUncertain);
        append(stats.evidenceValid); append(stats.growthBlocked);
        append(stats.recentReleaseSamples); append(stats.recentReleaseMisses);
        // Preserve the original fingerprint positions for the retired duplicate and constant.
        append(stats.recentReleaseMisses); append(1000000);
        append(stats.effectiveFallRateUsPerSecond);
        append(stats.effectiveFallRateUsPerSecond * 1000);
        append(stats.expiredUnknown); append(stats.clipped);
    }
    return digest;
}

int main(int argc, char** argv)
{
    if (!rollingScoresMatchScan() || !thresholdMatchesOriginalOrder()) return 1;
    const bool generate = argc == 2 && std::strcmp(argv[1], "--generate") == 0;
    // Recorded before removing the historical policies.
    static const uint64_t expected[] = {
        0x353cf3189e8e78cbULL,
        0x38ad3d2607bce0c7ULL,
        0x771acb7de5812749ULL,
        0xf10f4898a30023d6ULL,
        0x38e20c17f9dd01a6ULL,
        0x711b64511c8c3524ULL,
        0x4f380aef2b3d39d3ULL,
        0x2c42e0700d2d0ccaULL,
        0x3b12cd5365d48b98ULL,
        0xb2c8cbc801657566ULL,
        0x64f4554417fabe43ULL,
        0x6aa4b01a6b2222f6ULL,
        0x896382a9475d3675ULL,
        0xde8d2d5469c608a3ULL,
        0xef97ff2244776212ULL,
        0xfac3fd000a9106d7ULL,
        0x98b63d07f348fd80ULL,
        0xda333fd560088283ULL,
        0x2f172d670fd3e1feULL,
        0x38ad3d2607bce0c7ULL,
        0x894be49ecf66268cULL,
        0xf10f4898a30023d6ULL,
        0xf8080a32f3371e1eULL,
        0x711b64511c8c3524ULL,
        0x96e07e24fcd577ddULL,
        0x2c42e0700d2d0ccaULL,
        0xd176790104dafcb6ULL,
        0xb2c8cbc801657566ULL,
        0xb3eb51aaf1f0459eULL,
        0x6aa4b01a6b2222f6ULL,
        0xd978a64aa8e7e840ULL,
        0xde8d2d5469c608a3ULL,
        0x67538130866abcffULL,
        0xfac3fd000a9106d7ULL,
        0xaa1a37946ea21213ULL,
        0xda333fd560088283ULL,
        0xe27f35fa4624790dULL,
        0x38ad3d2607bce0c7ULL,
        0xde503935f1ccd284ULL,
        0xf10f4898a30023d6ULL,
        0x57b59253cf163d88ULL,
        0x711b64511c8c3524ULL,
        0x4e366ca9236fe489ULL,
        0x2c42e0700d2d0ccaULL,
        0xd35db8a846459123ULL,
        0xb2c8cbc801657566ULL,
        0x4b093f44d74ea17bULL,
        0x6aa4b01a6b2222f6ULL,
        0x80cc7d3a52260368ULL,
        0xde8d2d5469c608a3ULL,
        0x3650d9294fe5df4cULL,
        0xfac3fd000a9106d7ULL,
        0x8e3f8988333255dbULL,
        0xda333fd560088283ULL,
    };
    size_t index = 0;
    int failures = 0;
    for (int preset = 0; preset < 3; ++preset)
        for (int smoothing = 0; smoothing < 3; ++smoothing)
            for (int presentation = 0; presentation < 3; ++presentation)
                for (bool custom : {false, true}) {
                    const auto result = run(preset, smoothing, presentation, custom);
                    if (generate) std::printf("0x%016llxULL,\n", static_cast<unsigned long long>(result));
                    else if (index >= sizeof(expected) / sizeof(expected[0]) || result != expected[index]) {
                        std::fprintf(stderr, "policy fingerprint mismatch: preset=%d smoothing=%d presentation=%d custom=%d got=%016llx\n",
                                     preset, smoothing, presentation, int(custom), static_cast<unsigned long long>(result));
                        ++failures;
                    }
                    ++index;
                }
    return failures ? 1 : 0;
}
