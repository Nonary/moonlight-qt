#include "../../app/streaming/video/pyrowave/pyrowavelinkpolicy.h"
#include "../../app/streaming/video/pyrowave/pyrowavebandwidth.h"
#include "../../app/streaming/video/pyrowave/pyrowavecalibrationpolicy.h"
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

using namespace PyroWaveLink;
Result sample(int kbps, bool passing) {
    Result r;
    r.requestedKbps = kbps;
    r.expected = r.sent = r.received = 10000;
    r.senderMs = durationMs;
    r.frames = 240;
    r.worstFrameLossPercent = 0;
    r.lossPercent = passing ? 0 : 2;
    r.worstWindowLossPercent = passing ? 0 : 2;
    return r;
}
int main() {
    std::vector<int> tried;
    auto run = [&](int start, int limit) {
        tried.clear();
        return search(start, maximumKbps, [&](int rate) {
            tried.push_back(rate);
            return sample(rate, rate <= limit);
        }, [] { return false; });
    };
    auto r = run(500000, 1000000);
    CHECK(tried.front() == 500000);
    CHECK(*std::max_element(tried.begin(), tried.end()) > 1000000);
    CHECK(r.requestedKbps >= 940000 && r.requestedKbps <= 950000);
    CHECK(tried[tried.size()-1] == r.requestedKbps && tried[tried.size()-2] == r.requestedKbps);
    r = run(2000000, 800000);
    CHECK(tried.front() == 2000000 && r.requestedKbps <= 760000 && r.requestedKbps >= 750000);
    CHECK(!run(500000, 0).stable());
    // A failed search retains the last floor probe so the UI can name its
    // actual failure instead of claiming that UDP may be blocked.
    auto failed = run(500000, 0);
    CHECK(failed.requestedKbps == minimumKbps && failed.expected == 10000);
    CHECK(std::string(failed.failureReason()) == "packet loss exceeds the limit");
    failed = search(500000, maximumKbps, [](int rate) {
        auto measured = sample(rate, true);
        measured.delayP99Ms = 20;
        return measured;
    }, [] { return false; });
    CHECK(!failed.stable() && failed.received == failed.expected);
    CHECK(std::string(failed.failureReason()) == "packet delivery variation exceeds 4 ms");
    // Jitter without congestion must not hide local format support. Capacity
    // search still finds a loss boundary, leaves margin and confirms twice.
    tried.clear();
    auto capacity = search(500000, maximumKbps, [&](int rate) {
        tried.push_back(rate);
        auto measured = sample(rate, rate <= 800000);
        measured.delayP99Ms = 16;
        return measured;
    }, [] { return false; }, false);
    CHECK(capacity.capacityQualified() && !capacity.stable());
    CHECK(capacity.requestedKbps >= 750000 && capacity.requestedKbps <= 760000);
    CHECK(tried.back() == capacity.requestedKbps && tried[tried.size()-2] == capacity.requestedKbps);
    CHECK(!search(500000, maximumKbps, [](int rate) { return sample(rate, false); },
                  [] { return false; }, false).capacityQualified());
    CHECK(!search(500000, maximumKbps, [](int rate) {
        auto measured = sample(rate, true);
        measured.delayGrowthMs = 3;
        return measured;
    }, [] { return false; }, false).capacityQualified());
    CHECK(run(500000, maximumKbps).requestedKbps == 2850000);
    CHECK(run(minimumKbps, maximumKbps).requestedKbps == 2850000);
    // Ceiling-first search keeps the same 5 Mbps boundary and confirmations,
    // but needs only three probes when the known ceiling is sustainable.
    CHECK(run(500000, maximumKbps).requestedKbps == 2850000);
    int oldAttempts = tried.size();
    CHECK(run(maximumKbps, maximumKbps).requestedKbps == 2850000);
    CHECK(tried.size() == 3 && oldAttempts > int(tried.size()));
    std::printf("UDP at a sustainable 3 Gbps ceiling: %d -> %zu probes (%.1f -> %.1f seconds of sending)\n",
                oldAttempts, tried.size(), oldAttempts * durationMs / 1000.0,
                tried.size() * durationMs / 1000.0);
    // Unknown bottlenecks still get a measured boundary, never an interface-speed guess.
    CHECK(run(maximumKbps, 800000).requestedKbps == 760000);
    CHECK(tried.size() <= 13);
    // Severe random loss at every rate must never produce a suggestion.
    CHECK(!search(500000, maximumKbps, [](int rate) { return sample(rate, false); }, [] { return false; }).stable());
    // A route that deteriorates during final confirmation must back off again.
    int count = 0;
    r = search(500000, 500000, [&](int rate) {
        ++count;
        return sample(rate, count == 1 || rate <= 350000);
    }, [] { return false; });
    CHECK(r.stable() && r.requestedKbps <= 350000);
    bool stop = false;
    CHECK(!search(500000, maximumKbps, [&](int rate) { stop = true; return sample(rate, true); }, [&] { return stop; }).stable());

    Result clean = sample(100000, true);
    std::vector<int64_t> times(clean.expected);
    for (uint32_t seq = 0; seq < clean.expected; ++seq) {
        const auto tick = (uint64_t(seq+1) * durationMs + clean.expected - 1) / clean.expected - 1;
        times[seq] = 123456789 + tick * 1000;
    }
    summarize(clean, times);
    CHECK(clean.stable() && clean.delayP99Ms < 0.001);
    // PyroWave tolerates a short burst (8 packets, 1.6% of one window)...
    for (int i = 0; i < 8; ++i) times[i] = -1;
    summarize(clean, times);
    CHECK(clean.lossPercent < 0.1 && clean.worstWindowLossPercent > 1 && clean.stable());
    // ...but not loss bunched into one window above 5%, even under 2% overall.
    for (int i = 0; i < 30; ++i) times[i] = -1;
    summarize(clean, times);
    CHECK(clean.lossPercent < 2 && clean.worstWindowLossPercent > 5 && !clean.stable());
    CHECK(std::string(clean.failureReason()) == "bursty packet loss exceeds the limit");
    for (int i = 0; i < 30; ++i) {
        const auto tick = (uint64_t(i+1) * durationMs + clean.expected - 1) / clean.expected - 1;
        times[i] = 123456789 + tick * 1000;
    }
    // Tail loss is counted against the sender's expected count.
    times.assign(clean.expected, -1);
    summarize(clean, times);
    CHECK(clean.lossPercent == 100 && !clean.stable());
    // Lossless queue growth must fail too; a constant clock offset is harmless.
    for (uint32_t seq = 0; seq < clean.expected; ++seq) times[seq] = 100000 + int64_t(seq) * 202;
    summarize(clean, times);
    CHECK(!clean.stable() && clean.delayGrowthMs > 2);

    // Burst-v1 assigns sequence ranges to frames. Score the whole frame,
    // including missing tails, rather than diluting holes across intact ones.
    Result burst = sample(1120000, true);
    burst.expected = burst.sent = 240 * 800 + 17; // Unequal frame sizes.
    const auto burstTimes = [&] { return std::vector<int64_t>(burst.expected, 123456789); };
    auto packets = burstTimes();
    const auto lose = [&](unsigned frame, unsigned count, bool tail) {
        const unsigned begin = uint64_t(burst.expected) * frame / 240;
        const unsigned end = uint64_t(burst.expected) * (frame + 1) / 240;
        const unsigned first = tail ? end - count : begin + 100;
        for (unsigned seq = first; seq < first + count; ++seq) packets[seq] = -1;
    };
    // Twelve lost tail packets every frame are only 1.5% overall. This is
    // sustained tail damage, even though every 100 ms window passes.
    for (unsigned frame = 0; frame < 240; ++frame) lose(frame, 12, true);
    summarize(burst, packets, 120);
    CHECK(burst.capacityQualified() && burst.lossPercent < 2);
    CHECK(burst.frames == 240 && burst.damagedFrames == 240 && burst.missingTailFrames == 240);
    CHECK(burst.excessiveLossFrames == 0 && !paceQualified(burst));
    // The last packet arriving does not excuse repeated holes in the final
    // send group (the queue logs these as packet silence, not tail silence).
    packets = burstTimes();
    for (unsigned frame = 0; frame < 240; ++frame) {
        const unsigned end = uint64_t(burst.expected) * (frame + 1) / 240;
        for (unsigned seq = end - 24; seq < end - 12; ++seq) packets[seq] = -1;
    }
    summarize(burst, packets, 120, 150);
    CHECK(burst.lossPercent < 2 && burst.excessiveLossFrames == 0);
    CHECK(burst.missingTailFrames == 240 && !paceQualified(burst));
    // Interior holes also count against the entire frame's packet budget.
    // 3% damage on one frame in five is within the per-frame allowance.
    packets = burstTimes();
    for (unsigned frame = 0; frame < 240; frame += 5) lose(frame, 24, false);
    summarize(burst, packets, 120);
    CHECK(burst.capacityQualified() && burst.lossPercent < 1);
    CHECK(burst.excessiveLossFrames == 0 && burst.missingTailFrames == 0 && paceQualified(burst));
    // Repeated small interior holes are permitted within aggregate limits;
    // packet speed calibration targets tails and severe frame damage.
    packets = burstTimes();
    for (unsigned frame = 0; frame < 240; ++frame) lose(frame, 12, false);
    summarize(burst, packets, 120, 150);
    CHECK(burst.damagedFrames == 240 && burst.excessiveLossFrames == 0);
    CHECK(burst.missingTailFrames == 0 && paceQualified(burst));
    // A single small damaged frame is tolerated. A completely absent final
    // frame still counts and fails the aggregate 100 ms burst limit.
    packets = burstTimes();
    lose(10, 24, false);
    summarize(burst, packets, 120);
    CHECK(burst.excessiveLossFrames == 0 && paceQualified(burst));
    // One frame over 10% fails even when intact frames hide it in the
    // aggregate. Count all missing packets, regardless of their position.
    packets = burstTimes();
    lose(10, 81, false);
    summarize(burst, packets, 120, 150);
    CHECK(burst.lossPercent < 0.1 && burst.worstWindowLossPercent < 1);
    CHECK(burst.excessiveLossFrames == 1 && burst.missingTailFrames == 0 && !paceQualified(burst));
    // Exactly 10% is allowed, strictly more is not (800 packets per frame).
    burst.expected = burst.sent = 240 * 800;
    packets = burstTimes();
    lose(10, 80, false);
    summarize(burst, packets, 120, 150);
    CHECK(burst.worstFrameLossPercent == 10 && burst.excessiveLossFrames == 0 && paceQualified(burst));
    lose(10, 81, false);
    summarize(burst, packets, 120, 150);
    CHECK(burst.worstFrameLossPercent > 10 && burst.excessiveLossFrames == 1 && !paceQualified(burst));
    packets = burstTimes();
    packets.resize(uint64_t(burst.expected) * 239 / 240);
    summarize(burst, packets, 120);
    CHECK(burst.frames == 240 && burst.excessiveLossFrames == 1 && burst.missingTailFrames == 1);
    CHECK(burst.worstFrameLossPercent == 100 && !paceQualified(burst));
    // Each summary replaces the old counters, and unframed measurements
    // cannot qualify a frame-shaped search.
    packets = burstTimes();
    summarize(burst, packets, 120);
    CHECK(burst.damagedFrames == 0 && burst.worstFrameLossPercent == 0 && paceQualified(burst));
    summarize(burst, packets);
    CHECK(burst.frames == 0 && !paceQualified(burst));
    burst = sample(1120000, true);
    burst.frames = 250;
    burst.damagedFrames = 250;
    burst.worstFrameLossPercent = 1;
    CHECK(paceQualified(burst));
    burst.excessiveLossFrames = 1;
    burst.worstFrameLossPercent = 10.1;
    CHECK(!paceQualified(burst));
    burst.excessiveLossFrames = 0;
    burst.worstFrameLossPercent = 10;
    burst.missingTailFrames = 4;
    CHECK(paceQualified(burst));
    burst.missingTailFrames = 5;
    CHECK(!paceQualified(burst));
    // The latest final 1500 Mbps probe: 0.142% packets lost, only 0.833%
    // of frames over 2%, but none over 10% and no missing tail groups.
    burst = sample(1185000, true);
    burst.lossPercent = 0.142;
    burst.worstWindowLossPercent = 0.402;
    burst.damagedFrames = 21;
    burst.excessiveLossFrames = 0;
    burst.worstFrameLossPercent = 2.843;
    CHECK(burst.excessiveLossFramePercent() < 2 && burst.missingTailFramePercent() == 0);
    CHECK(paceQualified(burst));
    // A host that misses a few sends (a stalled send buffer) is measured as loss,
    // not a failed step; one that cannot send 98% is overloaded.
    clean = sample(500000, true);
    clean.sent = clean.received = 9950;
    clean.lossPercent = 0.5;
    CHECK(clean.capacityQualified() && paceQualified(clean));
    clean.sent = clean.received = 9700;
    clean.lossPercent = 3;
    CHECK(!clean.capacityQualified() && !paceQualified(clean));
    CHECK(std::string(clean.failureReason()) == "host did not send enough probe packets");
    // Loss under 2% qualifies capacity; 2% does not.
    clean = sample(500000, true);
    clean.lossPercent = clean.worstWindowLossPercent = 1.9;
    clean.received = 9810;
    CHECK(clean.capacityQualified());
    clean.lossPercent = clean.worstWindowLossPercent = 2.0;
    clean.received = 9800;
    CHECK(!clean.capacityQualified());
    CHECK(std::string(clean.failureReason()) == "packet loss exceeds the limit");
    clean = sample(500000, true);
    clean.senderMs = 2300;
    CHECK(!clean.stable());

    using namespace pyrowave::bandwidth;
    // Budget inversion for all supported packet sizes, FEC settings and FPS.
    for (int packet : {256, 1024, 1392}) for (int fec : {0, 20, 50, 100})
        for (int fps : {30, 60, 120, 240}) for (int image : {5000, 100000, 765000, 2000000}) {
            transport_t t {packet, fec, 2, 0};
            const auto wire = total_kbps(image, fps, t);
            CHECK(wire > image);
            CHECK(image_kbps(wire + 0.001, fps, t) >= image - 1);
        }
    transport_t t;
    CHECK(total_kbps(765000, 120, t) > 850000);
    CHECK(image_kbps(765000, 120, t) < 700000);

    namespace C = PyroWaveCalibration;
    CHECK(C::imageTarget(C::Minimum, 765000, 2000000) == 380000);
    CHECK(C::imageTarget(C::Recommended, 765000, 2000000) == 765000);
    CHECK(C::imageTarget(C::Recommended, 765000, 500000) == 500000);
    CHECK(C::imageTarget(C::Minimum, 5000, 2000000) == 5000);
    CHECK(C::wireTarget(C::Moderate, 950000) == 570000);
    CHECK(C::wireTarget(C::Maximum, 950000) == 950000);
    // Moderate aims for 60% of the link, never below Recommended, and steps
    // down to what calibration confirmed. On the 2.5 Gbps dock path a 1135 Mbps
    // budget gave 680 Mbps (below Recommended's ~850) when 60% applied to it.
    CHECK(C::wireTarget(C::Moderate, 1135000, 2500000, 850000) == 1135000);
    CHECK(C::wireTarget(C::Moderate, 3000000, 2500000, 850000) == 1500000);
    CHECK(C::wireTarget(C::Moderate, 3000000, 1000000, 850000) == 850000);
    CHECK(C::wireTarget(C::Moderate, 700000, 2500000, 850000) == 700000);
    CHECK(C::wireTarget(C::Maximum, 1135000, 2500000, 850000) == 1135000);
    for (int budget : {200000, 700000, 1135000, 2000000, 3000000})
        for (int link : {0, 1000000, 2500000, 10000000}) for (int recommended : {300000, 850000}) {
            const int moderate = C::wireTarget(C::Moderate, budget, link, recommended);
            CHECK(moderate <= budget);
            CHECK(moderate >= (std::min)(budget, recommended));
            CHECK(moderate <= C::wireTarget(C::Maximum, budget, link, recommended));
        }
    CHECK(C::imageQuality(C::Recommended, 500000, 765000) == C::ReducedQuality);
    CHECK(C::imageQuality(C::Recommended, 765000, 765000) == C::MeetsTarget);
    CHECK(C::imageQuality(C::Recommended, 900000, 765000) == C::MeetsTarget);
    CHECK(C::imageQuality(C::Recommended, 380000, 765000) == C::ReducedQuality);
    CHECK(C::imageQuality(C::Recommended, 375000, 765000) == C::BelowMinimum);
    CHECK(C::imageQuality(C::Minimum, 380000, 765000) == C::MeetsTarget);
    CHECK(C::imageQuality(C::Minimum, 375000, 765000) == C::BelowMinimum);
    CHECK(C::imageQuality(C::Minimum, 0, 5000) == C::BelowMinimum);
    // The measured wire rate is not image quality: reserve FEC and headers,
    // then apply Moderate's allowance before grading against the guide.
    CHECK(C::imageQuality(C::Recommended, C::imageCapacity(500000, 120, t), 765000) == C::ReducedQuality);
    CHECK(C::imageQuality(C::Minimum, C::imageCapacity(500000, 120, t), 765000) == C::MeetsTarget);
    CHECK(C::imageQuality(C::Recommended, C::imageCapacity(765000, 120, t), 765000) != C::MeetsTarget);
    CHECK(C::imageQuality(C::Moderate, C::imageCapacity(C::wireTarget(C::Moderate, 950000), 120, t), 765000) == C::ReducedQuality);
    for (int fps : {30, 60, 120, 240}) for (int guide : {5000, 100000, 765000, 2000000}) {
        const int ceiling = C::qualityProbeCeiling(guide, fps, t);
        const int confirmed = C::roundDown(ceiling * 0.95);
        CHECK(C::roundUp(total_kbps(guide, fps, t)) <= confirmed);
    }
    // Moderate's 60% allowance includes FEC and headers rather than adding
    // overhead after spending 60% on the image.
    for (int packet : {256, 1024, 1392}) for (int fec : {0, 20, 50, 100})
        for (int fps : {30, 60, 120, 240}) for (int wire : {5000, 500000, 950000, 3000000}) {
            transport_t transport {packet, fec, 2, 0};
            const int budget = C::wireTarget(C::Moderate, wire);
            const int image = C::imageCapacity(budget, fps, transport);
            if (image >= 5000) {
                CHECK(C::roundUp(total_kbps(image, fps, transport)) <= budget);
                CHECK(image * 125.0 / fps <= (fec ? 3000 : 4000) * (packet - 16) - 8);
            }
        }

    struct Cost { bool ok; double meanMs; double frameMs; bool overloaded; };
    std::vector<int> full, quick;
    const auto keepsUp = [](const Cost& c) { return c.ok && !c.overloaded && c.frameMs <= 8; };
    const auto device = [&](int requested, int guide, int floor, double fixed, double variable) {
        full.clear(); quick.clear();
        const auto cost = [&](int rate) {
            const double mean = fixed + variable * rate / 1000000;
            return Cost {true, mean, mean * 1.1, mean >= 8};
        };
        return C::searchDevice(requested, guide, floor,
            [&](int rate) { full.push_back(rate); return cost(rate); },
            [&](int rate) { quick.push_back(rate); return cost(rate); },
            keepsUp, [] { return false; });
    };
    // Healthy devices stop after one complete measurement at any target.
    for (int rate : {380000, 765000, 1500000, 2500000}) {
        const auto selected = device(rate, 765000, 100000, 2, 0);
        CHECK(selected.imageKbps == rate && keepsUp(selected.cost));
        CHECK(!selected.deviceLimited && full.size() == 1 && quick.empty());
    }
    // Noisy tails or fixed format overload must not buy a lucky pass at a
    // lower bitrate without the original 10% measurable-cost improvement.
    auto selected = device(2500000, 765000, 100000, 8.5, 0);
    CHECK(selected.imageKbps == 2500000 && !keepsUp(selected.cost));
    CHECK(full.size() == 2 && quick.size() == 1);
    selected = device(765000, 765000, 100000, 7.5, 0);
    CHECK(!selected.deviceLimited && !keepsUp(selected.cost));
    // A bitrate-sensitive device is refined to within 1/64 of its bracket,
    // including rates above the quality regression's upper end.
    selected = device(2500000, 765000, 100000, 2, 4);
    const int boundary = C::roundDown((8 / 1.1 - 2) / 4 * 1000000);
    CHECK(keepsUp(selected.cost) && selected.deviceLimited);
    CHECK(selected.imageKbps <= boundary && boundary - selected.imageKbps <= 30000);
    CHECK(full.size() <= 8);
    CHECK(std::find(full.begin(), full.end(), selected.imageKbps) != full.end());
    // Recover below the recommendation only with an effective floor probe.
    selected = device(765000, 765000, 100000, 2, 10);
    CHECK(keepsUp(selected.cost) && selected.deviceLimited && selected.imageKbps < 765000);
    CHECK(quick.size() == 1 && full.size() <= 8);
    CHECK(!device(100000, 765000, 100000, 9, 0).deviceLimited);
    // A runtime failure or cancellation cannot become a passing result.
    const auto invalid = C::searchDevice(765000, 765000, 100000,
        [](int) { return Cost {false, 0, 0, false}; },
        [](int) { std::abort(); return Cost {}; }, keepsUp, [] { return false; });
    CHECK(!invalid.cost.ok);
    const auto cancelled = C::searchDevice(765000, 765000, 100000,
        [](int) { return Cost {true, 9, 10, true}; },
        [](int) { std::abort(); return Cost {}; }, keepsUp, [] { return true; });
    CHECK(!keepsUp(cancelled.cost));
    // Frame pacing: highest loss-free pace, else the pace that lost least.
    const auto lossy = [](int kbps, double loss) {
        auto measured = sample(kbps, true);
        measured.lossPercent = measured.worstWindowLossPercent = loss;
        measured.received = uint32_t(measured.expected * (1 - loss / 100));
        measured.delayP99Ms = 30; // Bursty delivery never grades a pace probe.
        return measured;
    };
    std::vector<int> paces;
    const auto paceRun = [&](int link, int floor, auto lossAt) {
        paces.clear();
        return searchPace(link, floor, [&](int pace) { paces.push_back(pace); return lossy(pace, lossAt(pace)); },
                          [] { return false; });
    };
    // A receiver that keeps up at line rate is paced at the link after two confirmations.
    auto pace = paceRun(2500000, 1200000, [](int) { return 0.0; });
    CHECK(pace.qualified && pace.paceKbps == 2500000 && paces == std::vector<int>({2500000, 2500000, 2500000}));
    // The measured dock: whole USB transfers drop above 2.2 Gbps. The bisected
    // edge keeps a 5% margin, confirmed twice, and never probes below the floor.
    pace = paceRun(2500000, 1200000, [](int p) { return p <= 2200000 ? 0.0 : 2.0; });
    CHECK(pace.qualified && pace.paceKbps == 2050000);
    CHECK(paces[paces.size() - 1] == 2050000 && paces[paces.size() - 2] == 2050000);
    CHECK(*std::min_element(paces.begin(), paces.end()) >= 1200000 && paces.size() <= 12);
    // PyroWave tolerates loss as detail: under 2% qualifies, even at the link.
    pace = paceRun(2500000, 1200000, [](int p) { return p <= 2150000 ? 0.0 : 0.12; });
    CHECK(pace.qualified && pace.paceKbps == 2500000);
    pace = paceRun(2500000, 1200000, [](int p) { return p <= 2150000 ? 0.5 : 2.5; });
    CHECK(pace.qualified && pace.paceKbps <= 2150000 * 0.95 && pace.paceKbps >= 2000000);
    // Exactly 2% is not under the limit, and bunched loss fails on its own.
    pace = paceRun(2500000, 1200000, [](int) { return 2.0; });
    CHECK(!pace.qualified);
    pace = searchPace(2500000, 1200000, [&](int p) {
        auto measured = lossy(p, 0.5);
        measured.worstWindowLossPercent = p > 1800000 ? 6.0 : 1.0;
        return measured;
    }, [] { return false; });
    CHECK(pace.qualified && pace.paceKbps <= 1800000 * 0.95);
    // All aggregate probes pass, but recurring frame/tail damage starts at
    // 1.8 Gbps. Find that boundary and require two fresh frame-level passes.
    for (bool tail : {false, true}) {
        paces.clear();
        pace = searchPace(2500000, 1200000, [&](int p) {
            paces.push_back(p);
            auto measured = lossy(p, 0.9);
            if (p > 1800000) {
                if (tail) measured.missingTailFrames = 40;
                else { measured.excessiveLossFrames = 1; measured.worstFrameLossPercent = 10.1; }
            }
            return measured;
        }, [] { return false; });
        CHECK(pace.qualified && pace.paceKbps <= 1800000 * 0.95);
        CHECK(paces.back() == pace.paceKbps && paces[paces.size() - 2] == pace.paceKbps);
    }
    // Nothing within the limit: choose the least loss, the faster pace on ties.
    pace = paceRun(2500000, 1200000, [](int p) { return 1.0 + p / 1000000.0; });
    CHECK(!pace.qualified && pace.paceKbps == 1200000);
    pace = paceRun(2500000, 1200000, [](int) { return 3.0; });
    CHECK(!pace.qualified && pace.paceKbps == 2500000);
    // A frame that needs most of the link keeps the link: the floor never exceeds it.
    pace = paceRun(1000000, 1500000, [](int) { return 2.5; });
    CHECK(!pace.qualified && pace.paceKbps == 1000000 && paces.size() == 3);
    // A lucky single pass is not enough: failed confirmation steps down 10%.
    int calls = 0;
    pace = searchPace(2500000, 1200000, [&](int p) {
        ++calls;
        return lossy(p, p <= 1800000 || (p <= 2000000 && calls % 2) ? 0.0 : 3.0);
    }, [] { return false; });
    CHECK(pace.qualified && pace.paceKbps <= 1800000 && pace.paceKbps >= 1200000);
    // Cancellation and unusable links determine nothing.
    bool stopPace = false;
    pace = searchPace(2500000, 1200000, [&](int p) { stopPace = true; return lossy(p, 0); }, [&] { return stopPace; });
    CHECK(pace.paceKbps == 0);
    CHECK(searchPace(10000, 5000, [&](int p) { return lossy(p, 0); }, [] { return false; }).paceKbps == 0);

    // Loss is not necessarily monotonic: a failed floor and cap must not
    // hide an intermediate speed that avoids severe damage or missing tails.
    paces.clear();
    pace = searchPace(2500000, 1200000, [&](int p) {
        paces.push_back(p);
        auto measured = lossy(p, 0.5);
        if (p != 1850000) { measured.excessiveLossFrames = 1; measured.worstFrameLossPercent = 11; }
        return measured;
    }, [] { return false; });
    CHECK(pace.qualified && pace.paceKbps == 1850000);
    CHECK(paces.back() == 1850000 && paces[paces.size() - 2] == 1850000);
    // When all speeds lose tails, exhaust the 50 Mbps range and select the
    // fewest tail-damaged frames, even if a faster pace lost fewer packets.
    paces.clear();
    pace = searchPace(2500000, 1200000, [&](int p) {
        paces.push_back(p);
        auto measured = lossy(p, p == 2500000 ? 0.1 : 0.5);
        measured.missingTailFrames = p == 1750000 ? 8 : 40;
        return measured;
    }, [] { return false; });
    CHECK(!pace.qualified && pace.paceKbps == 1750000 && pace.probe.missingTailFrames == 8);
    for (int p = 1200000; p <= 2500000; p += paceStepKbps)
        CHECK(std::find(paces.begin(), paces.end(), p) != paces.end());
    CHECK(paces.back() == 1750000 && paces[paces.size() - 2] == 1750000);
    // Severe interior damage at every speed also returns an explicit fallback.
    pace = searchPace(2500000, 1200000, [&](int p) {
        auto measured = lossy(p, 0.5);
        measured.excessiveLossFrames = 1;
        measured.worstFrameLossPercent = p == 1650000 ? 11 : 20;
        return measured;
    }, [] { return false; });
    CHECK(!pace.qualified && pace.paceKbps == 1650000);
    // Invalid sending/timing is not an all-speeds-loss fallback.
    pace = searchPace(2500000, 1200000, [&](int p) {
        auto measured = lossy(p, 0.5);
        measured.senderMs = 2300;
        return measured;
    }, [] { return false; });
    CHECK(!pace.qualified && !pace.paceKbps);
    // Failed confirmation must remain visible instead of reusing a lucky pass.
    calls = 0;
    pace = searchPace(1500000, 1500000, [&](int p) {
        auto measured = lossy(p, 0.5);
        if (++calls > 1) { measured.excessiveLossFrames = 1; measured.worstFrameLossPercent = 20; }
        return measured;
    }, [] { return false; });
    CHECK(!pace.qualified && pace.probe.excessiveLossFrames == 1 && pace.probe.worstFrameLossPercent == 20);
    // Fallback ranking also includes a failed confirmation at the initially
    // clean link speed, rather than preferring that lucky first measurement.
    int linkCalls = 0;
    pace = searchPace(2500000, 1200000, [&](int p) {
        auto measured = lossy(p, 0.5);
        measured.missingTailFrames = p == 1750000 ? 8 : 40;
        if (p == 2500000 && ++linkCalls == 1) measured.missingTailFrames = 0;
        return measured;
    }, [] { return false; });
    CHECK(!pace.qualified && pace.paceKbps == 1750000);
    // A fallback that no longer produces valid measurements cannot be applied.
    int fallbackCalls = 0;
    pace = searchPace(2500000, 1200000, [&](int p) {
        auto measured = lossy(p, 0.5);
        measured.missingTailFrames = p == 1750000 ? 8 : 40;
        if (p == 1750000 && ++fallbackCalls > 1) measured.received = 0;
        return measured;
    }, [] { return false; });
    CHECK(!pace.paceKbps);
    // Cancellation while exploring alternatives discards every earlier sample.
    calls = 0;
    pace = searchPace(2500000, 1200000, [&](int p) { ++calls; return lossy(p, 3); },
                      [&] { return calls >= 4; });
    CHECK(!pace.paceKbps);
    std::puts("PyroWave link search, loss, delay and FEC budget tests passed");
}
