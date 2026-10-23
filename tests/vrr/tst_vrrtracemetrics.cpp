#include "assertions.h"
#include "vrrreplaymetrics.h"
#include "../../app/streaming/video/timinggraph.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/cadencedetection.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
using VrrReplayMetrics::Distribution;
using VrrReplayMetrics::SenderCadenceTracker;

void testTailStatisticsAndMissingDurations()
{
    Distribution d;
    check(d.count == 0 && d.percentile(99) == 0, "an empty distribution has no measured samples");
    d.addElapsed(200, 0); d.addElapsed(99, 100);
    check(d.count == 0, "missing or reversed timing endpoints cannot become zero-duration successes");
    d.addElapsed(100, 100);
    check(d.count == 1 && d.minimum == 0, "a measured zero duration remains a real observation");
    Distribution tail;
    for (uint64_t i = 0; i < 10000; ++i) tail.add(i);
    check(tail.count == 10000 && std::abs(double(tail.mean) - 4999.5) < 0.00001,
          "tail summaries retain every sample and the actual mean");
    check(tail.percentile(99) == 9899 && tail.percentileRatio(995,1000) == 9949 &&
          tail.percentileRatio(999,1000) == 9989 && tail.percentileRatio(9995,10000) == 9994,
          "p99, p99.5, p99.9 and p99.95 use independently known sample ranks");
    for (const auto sample : {100000ULL,100001ULL,999999ULL,1000001ULL,60000001ULL}) {
        Distribution single; single.add(sample);
        check(single.maximum == sample && single.percentile(99) >= sample,
              "coarse and overflow histogram buckets cannot understate tail latency");
    }
}

void testHistogramBoundaryRanksCannotInflateLatency()
{
    // Explicit expected upper endpoints constrain both directions. A loose
    // percentile >= sample check would allow every tail to become 60 seconds.
    const std::pair<uint64_t, uint64_t> boundaries[] = {
        {255, 255}, {256, 256}, {99999, 99999}, {100000, 100000},
        {100001, 100100}, {100100, 100100}, {100101, 100200},
        {999999, 1000000}, {1000000, 1000000}, {1000001, 1001000},
        {1001000, 1001000}, {1001001, 1002000}, {60000000, 60000000},
        {60000001, 60000001}
    };
    for (const auto& boundary : boundaries) {
        Distribution single;
        single.add(boundary.first);
        check(single.percentileRatio(1, 2) == boundary.second &&
              single.percentileRatio(9995, 10000) == boundary.second,
              "histogram boundary quantiles use the known bucket endpoint without arbitrary latency inflation");
    }

    // Deliberately insert out of order across sparse histogram pages. Each
    // independently known rank must retain its own bucket's upper endpoint.
    Distribution sparse;
    for (uint64_t sample : {60000001ULL, 100001ULL, 256ULL, 1000001ULL, 255ULL})
        sparse.add(sample);
    check(sparse.count == 5 && sparse.percentile(20) == 255 &&
          sparse.percentile(40) == 256 && sparse.percentile(60) == 100100 &&
          sparse.percentile(80) == 1001000 && sparse.percentile(100) == 60000001,
          "sparse pages and mixed histogram scales preserve independently known population ranks");

    Distribution spread;
    for (uint64_t sample : {0ULL, 1000ULL, 2000ULL, 3000ULL}) spread.add(sample);
    check(spread.mean == 1500 && spread.squaredDifferenceTotal == 5000000,
          "latency spread retains the independently computed mean and squared deviations");
}

void testSenderFidelityAndPresentedEvennessAreDifferent()
{
    SenderCadenceTracker followsSender, smoothOutput, delayedSmoothOutput;
    uint64_t source = 1000000;
    for (uint64_t i = 0; i < 120; ++i) {
        if (i) source += i % 2 ? 7000 : 13000;
        followsSender.observe(source, source + 1000, source + 10000, false, false, 5000);
        smoothOutput.observe(source, source + 1000, 1010000 + i * 10000, false, false, 5000);
        delayedSmoothOutput.observe(source, source + 1000, 1030000 + i * 10000, false, false, 5000);
    }
    check(followsSender.absoluteSpacingErrorUs.maximum == 0 &&
          followsSender.presentedJerkUs.percentile(99) == 6000 && followsSender.presentedJerkOverHitch == 118,
          "perfect sender fidelity must not label a visibly uneven sequence smooth");
    check(smoothOutput.absoluteSpacingErrorUs.maximum == 3000 && smoothOutput.presentedJerkUs.maximum == 0,
          "regular output must score even when it intentionally departs from jittery source stamps");
    check(delayedSmoothOutput.presentedJerkUs.maximum == smoothOutput.presentedJerkUs.maximum &&
          delayedSmoothOutput.absoluteSpacingErrorUs.maximum == smoothOutput.absoluteSpacingErrorUs.maximum,
          "constant added latency cannot masquerade as a cadence improvement");
}

void testHitchThresholdsAndAttribution()
{
    SenderCadenceTracker t;
    uint64_t source = 1000000, actual = 1010000;
    t.observe(source, source + 1000, actual, false, false, 5000);
    for (uint64_t interval : {10000ULL,12000ULL,10000ULL,12001ULL}) {
        source += 10000; actual += interval;
        t.observe(source, source + 1000, actual, false, false, 5000);
    }
    check(t.presentedJerkPairs == 3 && t.presentedJerkOverHitch == 1 && t.hitches == 1,
          "the 2 ms hitch boundary is strict and the first interval cannot invent a jerk pair");
    SenderCadenceTracker client;
    client.observe(1000000,1001000,1010000,false,false,5000);
    client.observe(1010000,1011000,1023000,false,false,5000);
    client.observe(1020000,1021000,1036001,false,false,5000);
    check(client.clientSpacingPairs == 2 && client.clientSpacingErrorsOver3ms == 1,
          "exactly 3 ms is accepted and 3.001 ms is a client-spacing violation");
    for (int cause = 0; cause < 4; ++cause) {
        SenderCadenceTracker fault;
        fault.observe(1000000,1001000,1010000,false,false,8333);
        fault.observe(1005000,1006000,1018333,cause == 0,cause <= 1, cause == 3 ? 4000 : 8333);
        check(fault.hitches == 1 && fault.hitchLateArrivals == unsigned(cause == 0) &&
              fault.hitchRenderLeadJumps == unsigned(cause == 1) && fault.hitchDisplayFloor == unsigned(cause == 2) &&
              fault.hitchOther == unsigned(cause == 3),
              "a matching hitch has one cause with late arrival ahead of render/floor attribution");
    }
}

void testSourceGapsAndLateDeliveryHaveDifferentDenominators()
{
    SenderCadenceTracker lowerSource;
    lowerSource.observe(1000000,1001000,1010000,false,false,8333);
    lowerSource.observe(1051000,1052000,1061000,false,false,8333);
    check(lowerSource.sourceStallPairs == 1 && lowerSource.hitches == 0 && lowerSource.clientSpacingPairs == 0,
          "a 51 ms source interval is a metric exclusion, never proof of an abnormal host stall");
    SenderCadenceTracker lateDelivery;
    lateDelivery.observe(1000000,1001000,1010000,false,false,8333);
    lateDelivery.observe(1010000,1052000,1061000,true,false,8333);
    check(lateDelivery.pairs == 0 && lateDelivery.clientSpacingPairs == 1 &&
          lateDelivery.clientSpacingErrorsOver3ms == 1 && lateDelivery.clientSpacingErrorUs.maximum == 41000,
          "the legacy arrival exclusion must not hide a late client interval from the client-error metric");
    lateDelivery.observe(1020000,1053000,1071000,false,false,8333);
    check(lateDelivery.presentedJerkPairs == 0, "excluded intervals cannot be bridged into a fabricated jerk pair");
    SenderCadenceTracker duplicate;
    duplicate.observe(1000000,1001000,1010000,false,false,8333);
    duplicate.observe(1010000,1011000,1020000,false,false,8333);
    duplicate.observe(1010000,1011000,1020000,false,false,8333);
    duplicate.observe(1020000,1021000,1030000,false,false,8333);
    check(duplicate.pairs == 2 && duplicate.presentedJerkPairs == 0,
          "duplicate source stamps break adjacency instead of adding samples or bridging jerk");
}

void testGraphLanesAgainstMatchingSourceFrames()
{
    Overlay::TimingGraphHistory graph;
    Vrr13::CadenceDetection cpu, display;
    uint64_t source = 1000000;
    for (uint64_t i = 0; i < 12; ++i) {
        if (i) source += i == 6 ? 51000 : 10000;
        const uint64_t target = source + 10000;
        const uint64_t enqueue = target - 4000 + (i % 2 ? 1500 : 0);
        Overlay::TimingGraphInput in;
        in.submitted = in.idValid = in.sourceTimingValid = true;
        in.submissionId = i + 1; in.sourceTimeUs = source;
        in.submissionUs = enqueue; in.targetUs = target; in.sourcePeriodUs = 10000; in.backend = 3;
        graph.record(in);
        Overlay::TimingGraphInput event;
        event.backend = 3; event.displayValid = true; event.displayId = i + 1; event.displayUs = target;
        graph.record(event);
        cpu.observe({i + 1,source,enqueue,target + 1000,0,true},500);
        display.observe({i + 1,source,target,target + 1000,0,true},500);
    }
    check(cpu.stats().deviations > 0 && display.stats().deviations == 0,
          "uneven early native enqueues do not prove uneven display when matching display events follow source");
    Overlay::TimingGraphSnapshot snapshot; graph.copyTo(snapshot);
    check(snapshot.points.size() == 12, "display feedback enriches matching frames without manufacturing submissions");
    uint64_t interval = 0;
    check(Overlay::timingGraphInterval(snapshot.points,6,Overlay::TimingGraphLane::Display,interval) && interval == 51000,
          "the display lane preserves a legitimate long source interval instead of replacing it with requested FPS");
    auto missing = snapshot.points; missing[5].displayUs = 0;
    check(!Overlay::timingGraphInterval(missing,6,Overlay::TimingGraphLane::Display,interval),
          "an absent matching OS report stays unknown rather than bridging an apparent long display interval");
    auto otherEpoch = snapshot.points; ++otherEpoch[6].generation;
    check(!Overlay::timingGraphInterval(otherEpoch,6,Overlay::TimingGraphLane::Display,interval),
          "display epochs cannot be joined into a timing disturbance");
    Vrr13::CadenceDetection badDisplay;
    for (uint64_t i = 0; i < 10; ++i) {
        const auto at = 2000000 + i * 10000;
        const auto shown = at + 10000 + (i == 5 ? 3000 : 0);
        badDisplay.observe({i+1,at,shown,shown+1000,0,true},500);
    }
    check(badDisplay.stats().deviations == 2 && badDisplay.stats().stretches == 1,
          "a real matching display delay and its catch-up remain detected even if CPU enqueue was early");
}

void testRedMarkersCannotBeHiddenByExtraBudgetOrLateTargets()
{
    Overlay::TimingGraphPoint p;
    p.sourceTimingValid = true; p.sourceTimeUs = 1000000;
    p.networkReadyUs = 1000100; p.decoderOutputUs = 1001000; p.decoderReadyUs = 1001501;
    p.bufferUs = 1000; p.inputPercentileEnabled = true; p.inputToleranceUs = 500;
    p.producerEligible = true; p.producerRequiredUs = 1501;
    p.targetUs = 2000000; p.requestedBufferUs = 20000; p.extraDelayUs = 20000;
    auto s = Overlay::bufferGraphSample(p);
    check(!s.absorbed() && s.producerMissed(),
          "late-clamped targets, requested protection and retiming cannot conceal a one-microsecond input miss");
    p.decoderReadyUs = 1001500; p.producerRequiredUs = 1500;
    s = Overlay::bufferGraphSample(p);
    check(s.absorbed() && s.producerAbsorbed(), "the same matching frame exactly on its actual input allowance is covered");
    p.decoderReadyUs = 0; s = Overlay::bufferGraphSample(p);
    check(!s.readyValid && !s.absorbed() && s.producerValid,
          "unknown GPU readiness remains distinct from independently known producer coverage");
    p.producerEligible = false;
    check(!Overlay::bufferGraphSample(p).producerMissed(), "ineligible input cannot become a red producer miss from stale fields");
}

void testDelayedDisplayFeedbackRequiresMatchingIdentity()
{
    Overlay::TimingGraphHistory graph;
    for (uint64_t id = 1; id <= 3; ++id) {
        Overlay::TimingGraphInput in;
        in.submitted = in.idValid = true;
        in.backend = 3; in.submissionId = id;
        in.submissionUs = 1000000 + id * 10000;
        in.targetUs = in.submissionUs + 4000;
        graph.record(in);
    }
    Overlay::TimingGraphInput event;
    event.displayValid = true; event.backend = 3;
    event.displayId = 999; event.displayUs = 1014000;
    graph.record(event);
    Overlay::TimingGraphSnapshot snapshot; graph.copyTo(snapshot);
    check(snapshot.points.size() == 3 && snapshot.points[0].displayUs == 0 &&
          snapshot.points[1].displayUs == 0 && snapshot.points[2].displayUs == 0,
          "an unknown OS image identity cannot provide display timing for a nearby submitted image");
    event.displayId = 1; event.backend = 2; graph.record(event);
    graph.copyTo(snapshot);
    check(snapshot.points[0].displayUs == 0, "a matching numeric ID from another backend is not the same image");
    event.backend = 3; graph.record(event); graph.copyTo(snapshot);
    check(snapshot.points[0].displayUs == 1014000 && snapshot.points[2].displayUs == 0,
          "delayed OS feedback must enrich the older matching image, not the latest submission");
    event.displayUs = 1019000; graph.record(event); graph.copyTo(snapshot);
    check(snapshot.points[0].displayUs == 1014000,
          "duplicate display feedback cannot replace the first reported display time and rewrite cadence");
    event.displayId = 3; event.displayUs = 1034000; graph.record(event); graph.copyTo(snapshot);
    uint64_t interval = 0;
    check(!Overlay::timingGraphInterval(snapshot.points, 2, Overlay::TimingGraphLane::Display, interval),
          "two known display endpoints cannot bridge the unobserved middle image");
    event.displayId = 2; event.displayUs = 1024000; graph.record(event); graph.copyTo(snapshot);
    check(Overlay::timingGraphInterval(snapshot.points, 2, Overlay::TimingGraphLane::Display, interval) && interval == 10000,
          "later matching feedback completes the known display interval without inventing an earlier observation");
}

void testNonDivisibleTailRanksAndUnknownPopulation()
{
    Distribution d;
    for (uint64_t sample : {0ULL, 1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL}) d.add(sample);
    check(d.percentileRatio(1, 2) == 3 && d.percentileRatio(2, 3) == 4,
          "non-divisible percentile ranks round upward to the observed order statistic");

    SenderCadenceTracker t;
    t.observe(1000000, 1001000, 1010000, false, false, 10000);
    t.observe(1010000, 1011000, 1020000, false, false, 10000);
    t.observe(1020000, 0, 1030000, false, false, 10000); // Unknown decode time.
    t.observe(1030000, 1031000, 1040000, false, false, 10000);
    check(t.clientSpacingPairs == 1 && t.pairs == 1 && t.presentedJerkPairs == 0,
          "unknown arrival evidence contributes no clean sample and breaks adjacency across the missing frame");

    SenderCadenceTracker missingSubmission;
    missingSubmission.observe(1000000, 1001000, 1010000, false, false, 10000);
    missingSubmission.observe(1010000, 1011000, 0, false, false, 10000);
    missingSubmission.observe(1020000, 1021000, 1030000, false, false, 10000);
    check(missingSubmission.clientSpacingPairs == 0 && missingSubmission.pairs == 0,
          "an unavailable local submission timestamp breaks cadence evidence instead of scoring a clean successor");

    SenderCadenceTracker sourceOrigin;
    sourceOrigin.observe(0, 1000, 10000, false, false, 10000);
    sourceOrigin.observe(10000, 11000, 20000, false, false, 10000);
    check(sourceOrigin.clientSpacingPairs == 1 && sourceOrigin.absoluteSpacingErrorUs.maximum == 0,
          "a zero RTP/source clock origin remains valid when local decode and submission evidence is present");
}

void testCadenceEvidenceBoundsRateStepsAndEpochs()
{
    using Detection = Vrr13::CadenceDetection;
    const auto frame = [](uint64_t id, uint64_t source, uint64_t actual,
                          uint64_t observed, uint64_t uncertainty = 0,
                          bool valid = true) {
        return Detection::Frame{id, source, actual, observed, uncertainty, valid};
    };
    {
        Detection d;
        // Independent arithmetic: 10 ms source intervals, 11 ms actual,
        // and +/- 250 us at both endpoints. The 1 ms residual's guaranteed
        // part is exactly the 500 us tolerance, so it is uncertain only.
        d.observe(frame(1, 1000000, 1001000, 2000000, 250), 500);
        d.observe(frame(2, 1010000, 1012000, 2010000, 250), 500);
        auto s = d.stats();
        check(s.intervals == 1 && s.deviations == 0 && s.uncertain == 1 &&
              s.worstErrorUs == 500,
              "uncertainty bounds classify an exact tolerance boundary as uncertain, not a proven deviation");
        // Residual grows to 1.5 ms, of which 1 ms remains after the same
        // 500 us endpoint bracket; that interval is now provably outside.
        d.observe(frame(3, 1020000, 1023500, 2020000, 250), 500);
        s = d.stats();
        check(s.intervals == 2 && s.deviations == 1 && s.uncertain == 1 &&
              s.stretches == 1,
              "a residual beyond the full endpoint uncertainty is counted as a proven stretch");
    }
    {
        Detection d;
        // A genuine source-rate transition changes both clocks equally.
        d.observe(frame(1, 5000000, 5001000, 8000000), 500);
        d.observe(frame(2, 5010000, 5011000, 8010000), 500);
        d.observe(frame(3, 5020000, 5021000, 8020000), 500);
        d.observe(frame(4, 5036000, 5037000, 8036000), 500);
        auto s = d.stats();
        check(s.intervals == 3 && s.deviations == 0,
              "a 10-to-16 ms source cadence transition is not scored as a client timing deviation");

        // Feedback with an older observation time starts a fresh evidence
        // epoch; the next frame cannot bridge back to the old sequence.
        d.observe(frame(5, 6630000, 6632500, 7170000), 500);
        d.observe(frame(6, 6640000, 6643500, 8180000), 500);
        s = d.stats();
        check(s.intervals == 1 && s.deviations == 1,
              "out-of-order feedback retires prior evidence before the new epoch is scored");
    }
    {
        Detection d;
        constexpr uint64_t base = UINT64_MAX - 100000;
        d.observe(frame(1, base, base + 1000, base + 2000), 100);
        d.observe(frame(2, base + 10000, base + 12000, base + 12000), 100);
        auto s = d.stats();
        check(s.intervals == 1 && s.deviations == 1 && s.worstErrorUs == 1000,
              "large clock origins preserve small source-relative residuals without timestamp overflow");

        // The 20-bucket rolling window is 2 seconds. Evidence older than it
        // expires without a synthetic clean interval replacing it.
        Detection expiring;
        expiring.observe(frame(1, 1000000, 1002000, 2000000), 100);
        expiring.observe(frame(2, 1010000, 1012000, 2010000), 100);
        expiring.observe(frame(3, 1020000, 1022000, 4120000), 100);
        s = expiring.stats();
        check(s.intervals == 1 && s.deviations == 0,
              "expired deviations leave only the newly observed clean interval in the rolling population");
    }
    {
        Detection d;
        d.observe(frame(1, 1000000, 1001000, 2000000), 250);
        d.observe(frame(2, 1010000, 1011000, 2010000), 250);
        d.observe(frame(2, 1020000, 1025000, 2020000), 250); // Duplicate identity.
        d.observe(frame(3, 1030000, 1036000, 2030000), 250);
        auto s = d.stats();
        check(s.intervals == 2 && s.deviations == 1,
              "duplicate feedback identity cannot replace the prior frame or add a second interval");
        d.observe(frame(4, 1040000, 1041000, 2040000, 0, false), 250);
        d.observe(frame(5, 1050000, 1051000, 2050000), 250);
        s = d.stats();
        check(s.intervals == 2,
              "invalid display evidence breaks adjacency instead of becoming a clean cadence observation");
    }
}
}

int main()
{
    bool assertionEvaluated = false;
    assert((assertionEvaluated = true));
    check(assertionEvaluated, "release tests must execute assert expressions rather than compile their evidence away");
    testTailStatisticsAndMissingDurations();
    testHistogramBoundaryRanksCannotInflateLatency();
    testSenderFidelityAndPresentedEvennessAreDifferent();
    testHitchThresholdsAndAttribution();
    testSourceGapsAndLateDeliveryHaveDifferentDenominators();
    testGraphLanesAgainstMatchingSourceFrames();
    testRedMarkersCannotBeHiddenByExtraBudgetOrLateTargets();
    testDelayedDisplayFeedbackRequiresMatchingIdentity();
    testNonDivisibleTailRanksAndUnknownPopulation();
    testCadenceEvidenceBoundsRateStepsAndEpochs();
    if (!failures) std::puts("Trace scoring, tail, source-cadence, identity, graph-lane and red-marker contracts passed");
    return failures ? 1 : 0;
}
