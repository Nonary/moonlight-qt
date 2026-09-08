#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/prediction.h"

#include <cstdio>
#include <vector>

namespace {
using Prediction = Vrr13::PresentationPrediction;
using Observation = Vrr13::PresentationObservation;
using Sample = Vrr13::SmoothnessFeedback::Sample;
int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

Observation submission(uint64_t id, uint64_t at, bool latched, bool dxgi = true)
{
    Observation o;
    o.submitted = o.idValid = true;
    o.id = id;
    o.submission = o.ready = at;
    o.deadline = at + 2000;
    o.observed = at;
    o.latched = latched;
    o.dxgi = dxgi;
    o.smoothness = {id, 0, at, 6000, 0, 0, true};
    return o;
}

Observation statistics(uint64_t id, uint64_t refresh, uint64_t sync,
                       uint64_t at, uint64_t observed, uint64_t uncertainty = 10)
{
    Observation o;
    o.dxgi = o.sampleValid = true;
    o.sampleId = id;
    o.presentRefresh = refresh;
    o.syncRefresh = sync;
    o.sampleTime = at;
    o.observed = observed;
    o.uncertainty = uncertainty;
    return o;
}

void testDelayedTransitionAndModeLead()
{
    Prediction prediction(true);
    std::vector<Sample> samples;
    const auto collect = [&](const Sample& s, uint64_t) { samples.push_back(s); };
    prediction.observe(submission(656, 5106432, true), collect);
    prediction.observe(submission(657, 5115211, false), collect);
    expect(prediction.measured() == 0 && prediction.floor(5115211, 6944, 100) == 0,
           "later native evidence must not be available before it is observed");

    const auto prior = statistics(656, 101, 101, 5112168, 5115300);
    prediction.observe(prior, collect);
    expect(samples.size() == 1 && samples[0].frame == 656 && samples[0].at == 5112168,
           "the previous synchronized frame must remain matchable after an adaptive submission");
    expect(prediction.lead(5115300, true) == 5736 &&
               prediction.lead(5115300, false) == 0 && prediction.lead(5115300) == 0,
           "synchronized latency must not become the adaptive presentation estimate");
    expect(prediction.floor(5115300, 6944, 100) == 5119212,
           "the shared floor must retain the latest matched refresh across modes");
    expect(uint64_t(5115211 - 5106432) > 6944 && uint64_t(5115211 - 5112168) < 6944,
           "the recorded CPU gap and matched refresh gap are distinct observations");
    prediction.observe(prior, collect);
    expect(prediction.measured() == 1 && samples.size() == 1,
           "repeated statistics must not match one submitted frame twice");

    prediction.observe(statistics(657, 102, 102, 5117211, 5117300), collect);
    expect(prediction.lead(5117300, false) == 2000 && prediction.lead(5117300, true) == 5736,
           "each native mode must retain its own latency samples");
    expect(samples.size() == 2 && samples[0].presentationEpoch != samples[1].presentationEpoch,
           "delayed callbacks must carry the epoch of their submitted frame");
    prediction.observe(submission(658, 5119000, true), collect);
    expect(prediction.lead(5119000) == 5736,
           "the default lookup must select the most recently submitted native mode");
}

void testSharedAnchorsAndUncertainty()
{
    for (bool uncertainAnchor : {false, true}) {
        Prediction prediction(true);
        std::vector<Sample> samples;
        const auto collect = [&](const Sample& s, uint64_t) { samples.push_back(s); };
        prediction.observe(submission(1, 1000000, true), collect);
        prediction.observe(statistics(99, 99, 10, 1004000, 1005000,
                                      uncertainAnchor ? 480 : 20), collect);
        prediction.observe(submission(2, 1006000, false), collect);
        prediction.observe(statistics(1, 10, 11, 1007000, 1008000,
                                      uncertainAnchor ? 20 : 480), collect);
        expect(samples.size() == 1 && samples[0].frame == 1 && samples[0].at == 1004000,
               "a refresh anchor collected before a mode change must bind a delayed identity afterward");
        expect(samples.size() == 1 && samples[0].uncertainty == 480,
               "a matched sample must retain the greater anchor or identity-binding uncertainty");
    }

    Prediction delayedAnchor(true);
    uint64_t matchedUncertainty = 0;
    const auto collect = [&](const Sample& s, uint64_t) { matchedUncertainty = s.uncertainty; };
    delayedAnchor.observe(submission(1, 1000000, true), collect);
    delayedAnchor.observe(statistics(1, 10, 9, 1003000, 1004000, 480), collect);
    delayedAnchor.observe(submission(2, 1006000, false), collect);
    delayedAnchor.observe(statistics(99, 99, 10, 1005000, 1007000, 20), collect);
    expect(delayedAnchor.measured() == 1 && matchedUncertainty == 480,
           "a later precise anchor must not erase uncertainty retained with an earlier identity binding");

    Prediction invalid(true);
    invalid.observe(submission(1, 1000000, true));
    invalid.observe(statistics(1, 10, 10, 1004000, 1005000, 501));
    expect(invalid.measured() == 0,
           "unacceptably uncertain feedback must not populate an anchor or latency bank");
    invalid.observe(statistics(1, 10, 11, 1006000, 1007000));
    expect(invalid.measured() == 0,
           "later identity binding must not recover an anchor that was rejected for uncertainty");
}

void testMissingIdAndReset()
{
    Prediction prediction(true);
    std::vector<Sample> samples;
    const auto collect = [&](const Sample& s, uint64_t) { samples.push_back(s); };
    prediction.observe(submission(1, 1000000, true), collect);
    auto missingId = submission(2, 1007000, false);
    missingId.idValid = false;
    prediction.observe(missingId, collect);
    prediction.observe(statistics(1, 1, 1, 1004000, 1008000), collect);
    expect(prediction.lead(1008000) == 0 && prediction.lead(1008000, true) == 4000,
           "a mode change with a missing present ID must still select the correct latency bank");
    prediction.observe(submission(3, 1009000, false), collect);
    prediction.observe(statistics(3, 3, 3, 1011000, 1012000), collect);
    expect(samples.size() == 2 && samples[0].presentationEpoch != samples[1].presentationEpoch,
           "an unidentifiable submitted frame must not hide a mode-transition epoch");

    prediction.observe(submission(4, 1013000, true), collect);
    prediction.reset();
    prediction.observe(statistics(4, 4, 4, 1015000, 1016000), collect);
    expect(prediction.measured() == 0 && prediction.floor(1016000, 6944, 100) == 0 &&
               prediction.lead(1016000, true) == 0 && prediction.lead(1016000, false) == 0,
           "a real reset must invalidate pending IDs, anchors, and both latency banks");
    prediction.observe(submission(5, 1020000, true), collect);
    prediction.observe(submission(6, 1028000, false), collect);
    prediction.observe(statistics(5, 5, 5, 1024000, 1029000), collect);
    expect(prediction.measured() == 1 && prediction.lead(1029000, true) == 4000,
           "reset must retain the configured preservation policy for the new epoch");
}

void testPerModeFreshnessAndOutstandingPresent()
{
    Prediction prediction(true);
    prediction.observe(submission(1, 1000000, true));
    prediction.observe(statistics(1, 1, 1, 1002000, 1003000));
    prediction.observe(submission(2, 1080000, false));
    prediction.observe(statistics(2, 2, 2, 1084000, 1085000));
    expect(prediction.lead(1110000, true) == 0 && prediction.lead(1110000, false) == 4000,
           "fresh adaptive feedback must not revive an expired synchronized latency bank");
    expect(prediction.floor(1110000, 6944, 100) == 1091044,
           "the shared floor must use its own latest matched observation");
    prediction.observe(submission(3, 1111000, true));
    prediction.observe(statistics(2, 2, 2, 1084000, 1112000));
    expect(prediction.measured() == 2,
           "statistics for an earlier frame cannot discharge a newer outstanding present");
    expect(prediction.floor(1300000, 6944, 100) == 0 && prediction.lead(1300000, false) == 0,
           "stale evidence must stop controlling all timing estimates");
}

void testCancelledObservationKeepsPendingIdentity()
{
    Prediction prediction(true);
    prediction.observe(submission(1, 1000000, true));
    Observation cancelled;
    cancelled.observed = 1002000;
    prediction.observe(cancelled);
    prediction.observe(submission(2, 1008000, false));
    prediction.observe(statistics(1, 1, 1, 1004000, 1009000));
    expect(prediction.measured() == 1 && prediction.lead(1009000, true) == 4000,
           "an empty cancelled outcome must not masquerade as a backend change and erase pending DXGI frames");
}

void testMatchedEpochSmoothness()
{
    Vrr13::SmoothnessFeedback feedback;
    const auto sample = [](uint64_t frame, uint64_t at, uint64_t intended, uint64_t epoch) {
        Sample s{frame, at, intended, 6000, 0, 0, true};
        s.presentationEpoch = epoch;
        return s;
    };
    feedback.observe(sample(1, 1000000, 1000000, 1), 1000000, true);
    feedback.observe(sample(2, 1010000, 1010000, 1), 1010000, true);
    expect(feedback.samples() == 1 && feedback.misses() == 0,
           "adjacent same-epoch native samples must score smoothness normally");
    feedback.observe(sample(3, 1025000, 1020000, 2), 1025000, true);
    expect(feedback.samples() == 1 && feedback.misses() == 0,
           "the first matched frame after a mode transition must establish a new baseline");
    feedback.observe(sample(2, 1010000, 1010000, 1), 1026000, true);
    auto staleInvalid = sample(2, 1010000, 1010000, 1);
    staleInvalid.eligible = false;
    feedback.observe(staleInvalid, 1026000, true);
    feedback.observe(sample(4, 1035000, 1030000, 2), 1035000, true);
    expect(feedback.samples() == 2 && feedback.misses() == 0,
           "an out-of-order old epoch must not erase the newer smoothness sequence");
    feedback.observe(sample(5, 1050000, 1040000, 2), 1050000, true);
    expect(feedback.samples() == 3 && feedback.misses() == 1,
           "same-epoch client spacing errors must remain observable after a mode transition");
}

void testLegacyAndOtherBackendUnchanged()
{
    for (bool dxgi : {false, true}) {
        Prediction prediction(!dxgi);
        prediction.observe(submission(1, 1000000, true, dxgi));
        prediction.observe(submission(2, 1008000, false, dxgi));
        auto feedback = statistics(1, 1, 1, 1004000, 1009000);
        feedback.dxgi = dxgi;
        prediction.observe(feedback);
        expect(prediction.measured() == 0,
               "default legacy and non-DXGI policies must retain reset-on-switch behavior");
        feedback = statistics(2, 2, 2, 1010000, 1011000);
        feedback.dxgi = dxgi;
        prediction.observe(feedback);
        expect(prediction.measured() == 1 && prediction.lead(1011000) == 2000,
               "legacy and non-DXGI policies must still learn ordinary same-mode feedback");
    }
}

void testBoundedUnmatchedHistory()
{
    expect(sizeof(Prediction) < 65536,
           "native history must remain a bounded small per-controller allocation");
    Prediction pending(true);
    for (uint64_t id = 1; id <= 2048; ++id)
        pending.observe(submission(id, 1000000 + id, (id % 2) != 0));
    pending.observe(statistics(1, 1, 1, 1004000, 1005000));
    expect(pending.measured() == 0,
           "unmatched submissions beyond retained capacity must not be resurrected");
    pending.observe(statistics(2048, 2048, 2048, 1004000, 1005000));
    expect(pending.measured() == 1,
           "bounded history must continue matching the newest submissions after overflow");

    Prediction anchors(true);
    anchors.observe(submission(1, 1000000, true));
    anchors.observe(submission(2, 1000001, false));
    for (uint64_t refresh = 1; refresh <= 2048; ++refresh)
        anchors.observe(statistics(99, 99, refresh, 1001000 + refresh, 1004000));
    anchors.observe(statistics(1, 1, 2049, 1004000, 1005000));
    expect(anchors.measured() == 0,
           "a delayed identity cannot use an anchor that has fallen outside retained capacity");
    anchors.observe(statistics(2, 2048, 2050, 1005000, 1006000));
    expect(anchors.measured() == 1,
           "bounded anchor history must still resolve recent refresh identities");
}
}

int main()
{
    testDelayedTransitionAndModeLead();
    testSharedAnchorsAndUncertainty();
    testMissingIdAndReset();
    testPerModeFreshnessAndOutstandingPresent();
    testCancelledObservationKeepsPendingIdentity();
    testMatchedEpochSmoothness();
    testLegacyAndOtherBackendUnchanged();
    testBoundedUnmatchedHistory();
    if (!failures) std::puts("VRR presentation feedback tests passed");
    return failures ? 1 : 0;
}
