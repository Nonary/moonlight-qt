#include <QFile>
#include "assertions.h"
#include "overlaymanager.h"
#include "performancestats.h"
#include "../../app/streaming/video/clientpacingwarning.h"
#include "../../app/streaming/video/pyrowave/pyrowavepacketlosswarning.h"
#include <QCoreApplication>
#include <QDir>
#include <cassert>
#include <future>
#include <iostream>
#include <vector>
#include <array>
#include <cmath>
#include <limits>

using namespace Overlay;
using namespace std::chrono_literals;

class Presenter : public IOverlayRenderer {
public:
    explicit Presenter(OverlayManager& manager) : manager(manager) {}
    void notifyOverlayUpdated(OverlayType type) override {
        assert(std::this_thread::get_id() != producer);
        SDL_Surface* surface = manager.getUpdatedOverlaySurface(type);
        const auto text = manager.getOverlayText(type);
        const bool enabled = manager.isOverlayEnabled(type);
        std::unique_lock<std::mutex> guard(lock);
        if (type == OverlayDebug) {
            last = text;
            height = surface ? surface->h : 0;
            width = surface ? surface->w : 0;
            bottomTransparent = false;
            statsInk = statsShadow = 0;
            colors = {};
            rowColors = {};
            minInkY.fill(std::numeric_limits<int>::max()); maxInkY.fill(-1);
            if (surface && surface->format->format == SDL_PIXELFORMAT_ARGB8888) {
                const auto* bottom = reinterpret_cast<const Uint32*>(static_cast<const Uint8*>(surface->pixels) + (surface->h - 1) * surface->pitch);
                Uint8 r, g, b, a;
                SDL_GetRGBA(bottom[surface->w - 1], surface->format, &r, &g, &b, &a);
                bottomTransparent = a == 0;
                // Cadence, clipped spikes, incoming stages and buffer inks.
                const std::array<Uint32, 10> palette{
                    SDL_MapRGBA(surface->format, 190, 198, 210, 255),
                    SDL_MapRGBA(surface->format, 65, 215, 250, 255),
                    SDL_MapRGBA(surface->format, 245, 110, 220, 255),
                    SDL_MapRGBA(surface->format, 255, 95, 65, 255),
                    SDL_MapRGBA(surface->format, 255, 185, 70, 255),
                    SDL_MapRGBA(surface->format, 160, 145, 255, 255),
                    SDL_MapRGBA(surface->format, 135, 145, 160, 255),
                    SDL_MapRGBA(surface->format, 85, 225, 135, 255),
                    SDL_MapRGBA(surface->format, 145, 225, 180, 255),
                    SDL_MapRGBA(surface->format, 245, 230, 90, 255)};
                const double scale = manager.getOverlayFontSize(OverlayDebug) / 20.0;
                const auto S = [scale](int v) { return int(std::lround(v * scale)); };
                for (int y = 0; y < surface->h; ++y) {
                    const auto row = reinterpret_cast<const Uint32*>(static_cast<const Uint8*>(surface->pixels) + y * surface->pitch);
                    for (int x = 0; x < surface->w; ++x) {
                        statsInk += row[x] == SDL_MapRGBA(surface->format, 212, 255, 0, 255);
                        statsShadow += row[x] == SDL_MapRGBA(surface->format, 0, 0, 0, 255);
                        for (size_t k = 0; k < palette.size(); ++k) {
                            colors[k] += row[x] == palette[k];
                            for (int graphRow = 0; graphRow <= TimingGraphLayout::BufferLane; ++graphRow) {
                                if (x >= S(TimingGraphLayout::Left) && y >= S(TimingGraphLayout::plotTop(graphRow)) &&
                                    y <= S(TimingGraphLayout::plotBottom(graphRow)) + S(2) && row[x] == palette[k]) {
                                    ++rowColors[graphRow][k];
                                    minInkY[graphRow] = std::min(minInkY[graphRow], y);
                                    maxInkY[graphRow] = std::max(maxInkY[graphRow], y);
                                }
                            }
                        }
                    }
                }
                const auto preview = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_PREVIEW");
                if (!preview.isEmpty()) {
                    assert(SDL_SaveBMP(surface, qPrintable(preview)) == 0);
                    // Inspect the real alpha composition over bright video,
                    // not a transparent surface implicitly viewed on black.
                    auto* bright = SDL_CreateRGBSurfaceWithFormat(0, surface->w, surface->h, 32, SDL_PIXELFORMAT_ARGB8888);
                    assert(bright);
                    SDL_FillRect(bright, nullptr, SDL_MapRGBA(bright->format, 255, 255, 255, 255));
                    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
                    assert(SDL_BlitSurface(surface, nullptr, bright, nullptr) == 0);
                    assert(SDL_SaveBMP(bright, qPrintable(preview + ".bright.bmp")) == 0);
                    SDL_FreeSurface(bright);
                }
            }
        }
        SDL_FreeSurface(surface);
        ++calls;
        if (type == OverlayDebug && enabled && block) {
            entered = true;
            ready.notify_all();
            ready.wait(guard, [&] { return !block; });
        }
        ready.notify_all();
    }
    void awaitText(const std::string& text) {
        std::unique_lock<std::mutex> guard(lock);
        assert(ready.wait_for(guard, 3s, [&] { return last == text; }));
    }
    void awaitSize(int expectedWidth) {
        std::unique_lock<std::mutex> guard(lock);
        assert(ready.wait_for(guard, 3s, [&] { return width == expectedWidth; }));
    }
    OverlayManager& manager;
    const std::thread::id producer = std::this_thread::get_id();
    std::mutex lock;
    std::condition_variable ready;
    bool block = false, entered = false, bottomTransparent = false;
    unsigned calls = 0, statsInk = 0, statsShadow = 0;
    std::string last;
    int height = 0, width = 0;
    std::array<unsigned, 10> colors{};
    std::array<std::array<unsigned, 10>, TimingGraphLayout::Lanes + 1> rowColors{};
    std::array<int, TimingGraphLayout::Lanes + 1> minInkY{}, maxInkY{};
};

static TimingGraphSnapshot testTimingGraph()
{
    TimingGraphHistory history;
    TimingGraphSnapshot snapshot;
    auto& points = snapshot.points;
    points.reserve(TimingGraphHistory::Capacity);
    TimingGraphInput in;
    in.submitted = true; in.idValid = true; in.backend = 2;
    in.submissionUs = in.targetUs = 1000000; in.sourcePeriodUs = 10000;
    in.bufferUs = in.requestedBufferUs = 2500; in.toleranceUs = 500;
    in.extraDelayUs = 1250;
    in.submissionId = UINT64_MAX - 1000; // Preserve full-width identity.
    history.record(in);
    const auto firstId = in.submissionId;
    in.submissionId++; in.submissionUs += 12000; in.targetUs += 10000;
    in.displayValid = true; in.displayId = firstId; in.displayUs = 1002000;
    history.record(in);
    history.copyTo(snapshot);
    assert(points.size() == 2 && points[0].displayUs == 1002000 && points[1].displayUs == 0);
    assert(points[1].submissionIntervalUs == 12000 && points[1].targetIntervalUs == 10000);
    assert(points[1].extraDelayUs == 1250 && points[1].bufferUs == 2500);
    // Extra holds never enlarge the buffer or the stage miss threshold.
    auto noExtra = points[1]; noExtra.extraDelayUs = 0;
    assert(bufferGraphSample(noExtra).bufferLimitUs == bufferGraphSample(points[1]).bufferLimitUs);
    // Invalid samples, duplicate feedback, and pre-submission timestamps must
    // never become new display events or attach to the reporting frame.
    in.submitted = false; in.displayUs = 1003000; history.record(in);
    in.displayId++; in.displayUs = 1001000; history.record(in);
    in.displayUs = 1015000; in.displayValid = false; history.record(in);
    history.copyTo(snapshot);
    assert(points[0].displayUs == 1002000 && points[1].displayUs == 0);
    in.discontinuity = true; in.submitted = true; in.submissionUs += 10000; in.targetUs += 10000;
    in.submissionId++; in.displayValid = true; in.displayId = firstId; in.displayUs = 1004000;
    history.record(in); history.copyTo(snapshot);
    assert(points.back().breakBefore && points.back().submissionIntervalUs == 0 && points[0].displayUs == 1002000);
    // Fixed storage wraps; the snapshot holds the drawn frames plus one
    // predecessor and keeps individual hitches rather than averages.
    in.discontinuity = false;
    for (size_t i = 0; i < TimingGraphHistory::Capacity + 100; ++i) {
        in.submissionId++; in.submissionUs += 10000 + (i % 23 == 0 ? 3000 : 0); in.targetUs += 10000;
        in.bufferUs = i % 60 < 30 ? 2500 : 4500;
        in.requestedBufferUs = i % 60 < 20 ? 2500 : 5000;
        in.sourceTimingValid = i % 80 != 0;
        in.sourceTimeUs = in.targetUs - in.bufferUs;
        in.cadenceRetimingUs = -500;
        in.networkReadyUs = in.sourceTimeUs - 1500 + (i % 23 == 0 ? 3000 : 0);
        in.decoderOutputUs = in.sourceTimeUs + (i % 23 == 0 ? 5000 : 500);
        in.decoderReadyUs = i % 9 == 0 ? 0 : in.decoderOutputUs + 1000;
        in.displayId = in.submissionId; in.displayUs = in.submissionUs + 2000 + (i % 11 == 0 ? 2000 : 0);
        history.record(in);
    }
    history.copyTo(snapshot);
    assert(points.size() == TimingGraphHistory::SnapshotPoints);
    bool hitch = false, jump = false;
    for (size_t i = 1; i < points.size(); ++i) {
        hitch |= points[i].submissionIntervalUs == 13000;
        jump |= points[i].bufferUs != points[i-1].bufferUs;
    }
    assert(hitch && jump);
    assert(points.back().sourceTimeUs == in.sourceTimeUs &&
           points.back().decoderReadyUs == in.decoderReadyUs &&
           points.back().cadenceRetimingUs == in.cadenceRetimingUs);
    assert(snapshot.hitches.observedFrames > points.size());
    return snapshot;
}

static void testBufferCoverage()
{
    TimingGraphPoint p;
    p.sourceTimingValid = true;
    p.sourceTimeUs = 1000000;
    p.cadenceRetimingUs = 500;
    p.networkReadyUs = 999500;
    p.decoderOutputUs = 1001000;
    p.decoderReadyUs = 1003000;
    p.bufferUs = 2500;
    auto sample = bufferGraphSample(p);
    assert(sample.valid && sample.networkValid && sample.outputValid && sample.readyValid);
    assert(sample.networkUs == -1000 && sample.decoderOutputUs == 500);
    assert(sample.decoderReadyUs == 2500 && sample.absorbed());
    // No tolerance/flattening at the coverage boundary. A late-clamped final
    // target or an unapplied buffer request must not hide a missed deadline.
    ++p.decoderReadyUs;
    p.targetUs = 2000000;
    p.requestedBufferUs = 9000;
    assert(!bufferGraphSample(p).absorbed());
    p.cadenceRetimingUs = -500;
    assert(bufferGraphSample(p).decoderReadyUs == 3501);
    p.decoderReadyUs = 0;
    sample = bufferGraphSample(p);
    assert(sample.outputValid && !sample.readyValid && !sample.absorbed());
    p.decoderReadyUs = p.decoderOutputUs - 1;
    assert(!bufferGraphSample(p).readyValid);
    p.decoderOutputUs = p.networkReadyUs - 1;
    assert(!bufferGraphSample(p).outputValid && !bufferGraphSample(p).readyValid);
    p.sourceTimingValid = false;
    assert(!bufferGraphSample(p).valid);
    p.sourceTimingValid = true; p.sourceTimeUs = 0;
    assert(!bufferGraphSample(p).valid);
}

// Causal producer coverage is independent of raw queue/GPU-stage coverage.
static void testAdaptiveProducerCoverage()
{
    TimingGraphHistory history;
    TimingGraphInput in;
    in.submitted=true; in.submissionUs=1020000; in.targetUs=1010000;
    in.sourceTimingValid=true; in.sourceTimeUs=1000000; in.sourcePeriodUs=10000;
    in.bufferUs=2500; in.requestedBufferUs=9000; in.toleranceUs=500;
    in.networkReadyUs=1000500; in.decoderOutputUs=1008000; in.decoderReadyUs=1009000;
    in.inputPercentileEnabled=true; in.producerEligible=true; in.producerRequiredUs=2000; in.inputToleranceUs=750;
    in.inputTargetUs=1500; in.inputTargetValid=true; in.inputTargetCapped=true;
    in.inputProbabilityPpm=995000; in.inputWindowUs=200000000; in.inputCoverageWindowUs=10000000;
    in.inputSamples=301; in.inputAbsorbed=297; in.inputMissed=4; in.trajectoryCarryUs=6690;
    history.record(in); TimingGraphSnapshot snapshot; history.copyTo(snapshot);
    assert(snapshot.points.size()==1);
    const auto& p=snapshot.points[0];
    assert(p.producerRequiredUs==in.producerRequiredUs && p.producerEligible && p.inputPercentileEnabled);
    assert(p.inputTargetUs==1500 && p.inputTargetUs!=p.bufferUs && p.inputTargetUs!=p.requestedBufferUs);
    assert(p.inputTargetValid && p.inputTargetCapped && p.inputProbabilityPpm==995000 && p.inputWindowUs==200000000 && p.inputCoverageWindowUs==10000000 && p.inputToleranceUs==750);
    assert(p.inputSamples==301 && p.inputAbsorbed==297 && p.inputMissed==4 && p.trajectoryCarryUs==6690);
    auto sample=bufferGraphSample(p);
    assert(sample.producerAbsorbed() && !sample.producerMissed());
    assert(sample.readyValid && !sample.absorbed()); // Own backlog may make the raw stage late.
    assert(snapshot.hitches.missed==1 && snapshot.hitches.worstExcessUs==5750); // Legacy raw counter preserved.

    for(int64_t required : {-100LL,3190LL,3250LL,3251LL}) {
        auto point=p; point.producerRequiredUs=required;
        point.targetUs=2000000; point.requestedBufferUs=30000; point.toleranceUs=1000;
        sample=bufferGraphSample(point);
        assert(sample.producerUs==double(required));
        assert(sample.producerAbsorbed()==(required<=3250));
        assert(sample.producerMissed()==(required>3250)); // 690us, exactly750us accepted,751us is the exact crossing.
        point.decoderReadyUs=0; sample=bufferGraphSample(point);
        assert(!sample.readyValid && sample.producerValid); // Unknown GPU completion is not unknown causal input.
        assert(sample.producerMissed()==(required>3250));
    }
    // Producer eligibility is authoritative. Loss, overload, and controller reset
    // arrive as ineligible, even if historical target/evidence fields remain valid.
    for(int invalidReason=0;invalidReason<3;++invalidReason) {
        auto point=p; point.producerEligible=false; point.producerRequiredUs=99999;
        sample=bufferGraphSample(point);
        assert(!sample.producerValid && !sample.producerAbsorbed() && !sample.producerMissed());
        assert(sample.readyValid && !sample.absorbed());
    }
    auto zero=p;zero.inputToleranceUs=0;zero.producerRequiredUs=2501;
    assert(bufferGraphSample(zero).producerMissed());
    auto huge=p;huge.bufferUs=std::numeric_limits<uint64_t>::max();huge.inputToleranceUs=750;
    huge.producerRequiredUs=std::numeric_limits<int64_t>::max();
    assert(bufferGraphSample(huge).producerAbsorbed() && !bufferGraphSample(huge).producerMissed());

    auto point=p; point.sourceTimingValid=false;
    const auto invalidSource=bufferGraphSample(point);
    assert(!invalidSource.valid && !invalidSource.producerValid && !invalidSource.producerMissed());
    assert(invalidSource.bufferUs==2500 && invalidSource.bufferLimitUs==3250); // Known budget is not promoted into known timing.
    point=p; point.sourceTimeUs=0; assert(!bufferGraphSample(point).producerValid);
    point=p; point.inputPercentileEnabled=false; assert(!bufferGraphSample(point).producerValid);

    // Legacy history keeps its exact raw threshold. Adaptive coverage adds
    // tolerance, and a definition change clears incompatible rolling totals.
    BufferHitchHistory raw;
    point=p; point.inputPercentileEnabled=false; raw.record(point);
    const auto legacy=raw.snapshot();
    point.submissionUs+=10000; point.inputPercentileEnabled=true;
    point.producerEligible=false; point.producerRequiredUs=-20000; raw.record(point);
    const auto adaptive=raw.snapshot();
    assert(legacy.missed==1 && legacy.measuredFrames==1 && legacy.worstExcessUs==6500);
    assert(adaptive.missed==1 && adaptive.measuredFrames==1 && adaptive.worstExcessUs==5750);
    assert(adaptive.observedFrames==1 && adaptive.unknown==legacy.unknown);
}

static void testBufferHitchHistory()
{
    BufferHitchHistory history;
    TimingGraphPoint p;
    p.sourceTimingValid = true; p.sourceTimeUs = 900000;
    p.networkReadyUs = 899999; p.decoderOutputUs = 900500;
    p.bufferUs = 2000; p.toleranceUs = 1000; p.submissionUs = 1000000; p.decoderReadyUs = 901000;
    history.record(p); // Exactly 1 ms is not a hitch.
    p.submissionUs += 100000; ++p.decoderReadyUs; history.record(p);
    p.submissionUs += 100000; p.decoderReadyUs = 902000; history.record(p); // Exactly covered.
    p.submissionUs += 100000; ++p.decoderReadyUs; history.record(p); // One us missed.
    p.submissionUs += 100000; p.decoderReadyUs = 0; history.record(p); // Completion unknown, no known hitch.
    p.submissionUs += 100000; p.decoderOutputUs = 901001; history.record(p); // Known late output.
    p.submissionUs += 100000; p.sourceTimingValid = false; history.record(p);
    p.submissionUs += 100000; p.sourceTimingValid = true;
    p.networkReadyUs = 901010; p.decoderOutputUs = 900500; history.record(p); // Invalid decoder order.
    history.record(p); // Duplicate must not inflate totals.
    const auto stats = history.snapshot();
    assert(stats.absorbed == 2 && stats.missed == 1 && stats.unknown == 2);
    assert(stats.worstExcessUs == 1 && stats.observedFrames == 8 && stats.measuredFrames == 4);
    // Read-time expiry also works if delivery stops. Expire the worst miss
    // with its bucket, never retain a session-wide maximum in a rolling row.
    const auto expired = history.snapshot(121400000);
    assert(expired.absorbed == 0 && expired.missed == 0 && expired.unknown == 2);
    assert(expired.worstExcessUs == 0 && expired.observedFrames == 3);
    assert(history.snapshot(122000000).observedFrames == 0);
    p.submissionUs = 500000; p.sourceTimeUs = 400000;
    p.networkReadyUs = 399999; p.decoderOutputUs = 400500; p.decoderReadyUs = 402001;
    history.record(p); // A restarted local clock starts a new history.
    assert(history.snapshot().observedFrames == 1 && history.snapshot().missed == 1);

    for (uint64_t tolerance : {250ULL, 500ULL, 1000ULL, 1750ULL, 2000ULL}) {
        BufferHitchHistory profile;
        TimingGraphPoint frame;
        frame.sourceTimingValid = true; frame.sourceTimeUs = 1000000;
        frame.networkReadyUs = 999999; frame.decoderOutputUs = 1000001;
        frame.toleranceUs = tolerance; frame.bufferUs = tolerance + 1000;
        frame.submissionUs = 2000000; frame.decoderReadyUs = frame.sourceTimeUs + tolerance;
        profile.record(frame);
        assert(profile.snapshot().absorbed == 0); // Exactly the selected tolerance is accepted.
        frame.submissionUs += 10000; ++frame.decoderReadyUs; profile.record(frame);
        assert(profile.snapshot().absorbed == 1); // One us beyond any preset/custom tolerance counts.
        frame.submissionUs += 10000; frame.decoderReadyUs = frame.sourceTimeUs + frame.bufferUs + 1;
        profile.record(frame);
        assert(profile.snapshot().missed == 1 && profile.snapshot().worstExcessUs == 1);
        frame.submissionUs += 10000; frame.toleranceUs = tolerance + 250;
        profile.record(frame);
        assert(profile.snapshot().observedFrames == 1 && profile.snapshot().missed == 1);
        assert(profile.snapshot().toleranceUs == frame.toleranceUs); // Never mix hitch definitions.
    }

    TimingGraphHistory graph;
    TimingGraphInput in;
    in.submitted = true; in.sourceTimingValid = true; in.bufferUs = 2500; in.toleranceUs = 500;
    in.sourcePeriodUs = 10000;
    for (unsigned i = 1; i <= 20000; ++i) {
        in.submissionUs = in.targetUs = 1000000 + uint64_t(i) * 10000;
        in.sourceTimeUs = in.submissionUs - 5000;
        in.networkReadyUs = in.sourceTimeUs - 1000;
        in.decoderOutputUs = in.sourceTimeUs + 500;
        in.decoderReadyUs = in.sourceTimeUs + (i % 10 ? 2000 : 3000);
        in.discontinuity = i == 15000; // A new phase must not erase prior counts.
        graph.record(in);
    }
    TimingGraphSnapshot snapshot;
    graph.copyTo(snapshot, in.submissionUs);
    assert(snapshot.points.size() == TimingGraphHistory::SnapshotPoints);
    assert(snapshot.hitches.observedFrames >= 11990 && snapshot.hitches.observedFrames <= 12000);
    assert(snapshot.hitches.absorbed > 10000 && snapshot.hitches.missed > 1000);
    assert(snapshot.hitches.worstExcessUs == 500);
    in.submitted = false; graph.record(in); // Feedback-only updates are not input frames.
    graph.copyTo(snapshot, in.submissionUs);
    assert(snapshot.hitches.absorbed + snapshot.hitches.missed == snapshot.hitches.observedFrames);
    graph.copyTo(snapshot, in.submissionUs + BufferHitchStats::WindowUs);
    assert(snapshot.hitches.observedFrames == 0 && !snapshot.points.empty());
}

static void testLaneIntervals()
{
    TimingGraphPoints points(4);
    for (size_t i = 0; i < points.size(); ++i) {
        points[i].submissionUs = 1000000 + i * 10000;
        points[i].displayUs = points[i].submissionUs + 20000 + (i == 2 ? 3000 : 0);
        points[i].targetIntervalUs = 10000;
        points[i].submissionIntervalUs = i ? 10000 : 0;
        points[i].backend = 2;
    }
    using Lane = TimingGraphLane;
    uint64_t interval = 0;
    assert(timingGraphInterval(points, 0, Lane::Target, interval) && interval == 10000);
    assert(!timingGraphInterval(points, 0, Lane::Submit, interval));
    assert(!timingGraphInterval(points, 0, Lane::Display, interval)); // No predecessor in the snapshot.
    assert(timingGraphInterval(points, 2, Lane::Display, interval) && interval == 13000);
    assert(timingGraphInterval(points, 3, Lane::Display, interval) && interval == 7000);
    // A frame without OS feedback removes both intervals that touch it,
    // instead of drawing one long interval across the unobserved frame.
    auto missing = points; missing[2].displayUs = 0;
    assert(!timingGraphInterval(missing, 2, Lane::Display, interval));
    assert(!timingGraphInterval(missing, 3, Lane::Display, interval));
    auto rebased = points; rebased[2].breakBefore = true;
    assert(!timingGraphInterval(rebased, 2, Lane::Display, interval));
    rebased = points; rebased[3].generation++;
    assert(!timingGraphInterval(rebased, 3, Lane::Display, interval));
    rebased = points; rebased[3].backend++;
    assert(!timingGraphInterval(rebased, 3, Lane::Display, interval));
    rebased = points; rebased[3].displayKind = TimingGraphDisplayKind::ConfirmedRefresh;
    assert(!timingGraphInterval(rebased, 3, Lane::Display, interval));
    rebased = points; rebased[3].displayUs = rebased[2].displayUs;
    assert(!timingGraphInterval(rebased, 3, Lane::Display, interval));
    assert(!timingGraphInterval(points, points.size(), Lane::Target, interval));
}

static void testConfirmedDisplayRefresh()
{
    const auto submission = [](uint64_t id, uint64_t at, uint32_t backend = 1) {
        TimingGraphInput in;
        in.submitted = in.idValid = true;
        in.submissionId = id; in.submissionUs = in.targetUs = at;
        in.preparationReadyUs = at - 100;
        in.backend = backend;
        return in;
    };
    const auto reference = [](uint64_t id, uint64_t present, uint64_t sync,
                              uint64_t time, uint64_t observed, uint64_t uncertainty = 0) {
        TimingGraphInput in;
        in.backend = 1; in.refreshValid = true;
        in.refreshReportedId = id; in.refreshPresentSequence = present; in.refreshSyncSequence = sync;
        in.refreshTimeUs = time; in.refreshObservedUs = observed; in.refreshUncertaintyUs = uncertainty;
        return in;
    };
    const auto points = [](const TimingGraphHistory& history) {
        TimingGraphSnapshot snapshot;
        history.copyTo(snapshot);
        return snapshot.points;
    };
    {
        TimingGraphHistory history;
        history.record(submission(10, 1000000));
        history.record(submission(11, 1010000));
        history.record(reference(10, 52, 51, 1001000, 1011000));
        assert(points(history)[0].displayUs == 0); // A nearby refresh is not the same refresh.
        history.record(reference(10, 52, 52, 1002000, 1012000));
        auto p = points(history);
        assert(p.size() == 2 && p[0].displayUs == 1002000 && p[1].displayUs == 0);
        assert(p[0].displayKind == TimingGraphDisplayKind::ConfirmedRefresh);
        history.record(reference(10, 52, 52, 1002000, 1012000)); // Exact duplicate is idempotent.
        history.record(reference(10, 53, 53, 1004000, 1013000)); // Repeat scanout is not a new image.
        p = points(history);
        assert(p.size() == 2 && p[0].displayUs == 1002000 && p[0].presentRefreshSequence == 52);
        history.record(reference(11, 54, 54, 1012000, 1013000));
        p = points(history);
        uint64_t interval = 0;
        assert(timingGraphInterval(p, 1, TimingGraphLane::Display, interval) && interval == 10000);
        history.record(submission(12, 1020000));
        history.record(submission(13, 1030000));
        history.record(reference(13, 56, 56, 1032000, 1033000));
        p = points(history);
        assert(!p[2].displayUs && p[3].displayUs == 1032000);
        assert(!timingGraphInterval(p, 2, TimingGraphLane::Display, interval));
        assert(!timingGraphInterval(p, 3, TimingGraphLane::Display, interval)); // Never bridge a missing image.
        TimingGraphInput event;
        event.backend = 1; event.displayValid = true; event.displayId = 11; event.displayUs = 1012500;
        history.record(event);
        p = points(history);
        assert(p[1].displayUs == 1012500 && p[1].displayKind == TimingGraphDisplayKind::DisplayEvent);
        assert(!timingGraphInterval(p, 1, TimingGraphLane::Display, interval)); // Separate evidence kinds.
        event.displayUs = 1013000; history.record(event);
        assert(points(history)[1].displayUs == 1012500); // Actual events retain their first timestamp.
    }
    {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        history.record(reference(1, 7, 6, 1001000, 1002000));
        history.record(reference(2, 8, 7, 1010000, 1011000));
        assert(!points(history)[0].displayUs); // Image was already reported displayed before this anchor.
    }
    {
        TimingGraphHistory history;
        history.record(submission(20, 2000000));
        history.record(reference(19, 80, 81, 2002000, 2003000)); // Anchor before the image's refresh report.
        assert(!points(history)[0].displayUs);
        history.record(reference(20, 81, 82, 2004000, 2005000));
        assert(points(history)[0].displayUs == 2002000); // Join to the earlier anchor, not this row's QPC.
        history.record(reference(20, 81, 82, 2004000, 2006000));
        assert(points(history)[0].displayUs == 2002000);
    }
    struct InvalidReference {
        uint64_t time, observed, prepared, uncertainty;
    };
    const InvalidReference invalid[] = {
        {999999, 1002000, 999900, 0},   // Before submission.
        {1001000, 1002000, 1001500, 0}, // Before verified preparation completion.
        {1002000, 1001999, 999900, 0},  // Future timestamp.
        {1001000, 1101001, 999900, 0},  // Stale anchor.
        {1100001, 1100001, 999900, 0},  // Implausibly late relative to this image's readiness.
        {1001000, 1002000, 999900, 501}, // Clock conversion bracket too broad.
    };
    for (const auto& bad : invalid) {
        TimingGraphHistory history;
        auto in = submission(1, 1000000); in.preparationReadyUs = bad.prepared;
        history.record(in);
        history.record(reference(1, 7, 7, bad.time, bad.observed, bad.uncertainty));
        assert(!points(history)[0].displayUs);
    }
    {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        auto raw = reference(1, 7, 7, 1001000, 1002000); raw.refreshValid = false;
        history.record(raw);
        assert(!points(history)[0].displayUs); // Query success/correlation validity is supplied explicitly.
    }
    {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        history.record(reference(1, 7, 7, 1001000, 1101000, 500));
        assert(points(history)[0].displayUs == 1001000); // Inclusive freshness/uncertainty bounds.
    }
    {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        history.record(reference(1, 7, 6, 1001000, 1002000)); // Pending image-to-refresh mapping.
        history.record(reference(2, 8, 7, 1102001, 1102001));
        assert(!points(history)[0].displayUs); // Expired mapping cannot recover through a later anchor.
    }
    {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        history.record(reference(0, 6, 7, 1001000, 1002000, 501)); // Uncertain anchor is not retained.
        history.record(reference(1, 7, 8, 1002000, 1003000));
        assert(!points(history)[0].displayUs);
    }
    {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        history.record(reference(1, 7, 7, 1001000, 1002000));
        history.record(reference(1, 7, 7, 1001500, 1003000)); // Contradictory timestamp for the same refresh.
        assert(!points(history)[0].displayUs);
        history.record(reference(1, 7, 7, 1001000, 1004000));
        assert(!points(history)[0].displayUs); // A convenient later duplicate cannot repair the contradiction.
    }
    for (bool disjoint : {false, true}) {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        history.record(reference(1, 7, 6, 1001000, 1002000));
        TimingGraphInput boundary;
        boundary.backend = 1;
        boundary.refreshDisjoint = disjoint;
        boundary.discontinuity = !disjoint;
        history.record(boundary);
        history.record(reference(1, 7, 7, 1002000, 1003000));
        assert(!points(history)[0].displayUs); // Old pending IDs belong to the retired epoch.
        history.record(submission(2, 1010000));
        history.record(reference(2, 7, 8, 1011000, 1012000));
        assert(!points(history)[1].displayUs); // The old epoch's anchor was cleared as well.
    }
    {
        TimingGraphHistory history;
        history.record(submission(100, 1000000));
        history.record(reference(100, 7, 6, 1001000, 1002000));
        auto unavailable = submission(101, 1005000); unavailable.idValid = false;
        history.record(unavailable);
        history.record(submission(1, 1010000)); // Counter reset hidden behind an unavailable ID query.
        const auto p = points(history);
        assert(p[2].breakBefore && p[2].generation != p[0].generation);
        history.record(reference(1, 7, 7, 1011000, 1012000));
        assert(!points(history)[0].displayUs && points(history)[2].displayUs == 1011000);
    }
    {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        history.record(reference(1, 7, 6, 1001000, 1002000));
        history.record(reference(1, 7, 5, 1001500, 1003000)); // Refresh-clock regression.
        history.record(reference(1, 7, 7, 1002000, 1004000));
        assert(!points(history)[0].displayUs);
    }
    {
        TimingGraphHistory history;
        history.record(submission(100, 1000000));
        history.record(reference(100, 7, 6, 1001000, 1002000));
        history.record(reference(99, 7, 7, 1002000, 1003000)); // Displayed present counter regression.
        history.record(reference(100, 7, 8, 1003000, 1004000));
        assert(!points(history)[0].displayUs);
    }
    {
        TimingGraphHistory history;
        history.record(submission(1, 1000000));
        history.record(reference(1, 7, 6, 1001000, 1002000));
        history.record(submission(2, 1010000, 2)); // Backend change clears pending references and anchors.
        auto raw = reference(2, 7, 7, 1011000, 1012000); raw.backend = 2;
        history.record(raw);
        assert(!points(history)[0].displayUs && points(history)[1].displayUs == 1011000);
    }
}

int main(int argc, char** argv)
{
    testLaneIntervals();
    testConfirmedDisplayRefresh();
    testBufferCoverage();
    testAdaptiveProducerCoverage();
    testBufferHitchHistory();
    {
        PyroWavePacketLossWarning warning;
        uint64_t now = 1000000;
        // 100 FPS, 100 packets/frame: each window has 30,000 data packets.
        const auto window = [&](unsigned lossPercent) {
            bool visible = false;
            for (unsigned frame = 0; frame < 300; ++frame) {
                visible = warning.observe(now, true, 100, lossPercent);
                now += 10000;
            }
            return visible;
        };
        assert(!window(1));  // Isolated holes do not warn.
        assert(!window(15));
        assert(!window(15)); // Two complete moderate-loss windows required.
        assert(window(10));
        assert(window(5));  // Retain through the hysteresis band.
        assert(!window(0)); // A complete recovery window clears.
        assert(!window(30));
        assert(window(0));  // One complete high-loss window is sufficient.
        assert(!window(0));
        assert(!window(100));
        assert(window(100));
        assert(!warning.observe(now, false, 100, 100)); // Preference/codec disables.
        assert(!window(100));
        assert(window(100));
        now += 3000000;
        assert(!warning.observe(now, true, 100, 100)); // Restart after a reporting gap.
        assert(!warning.observe(now - 1, true, 100, 100)); // Restart after clock reversal.
    }
    {
        PyroWavePacketLossWarning warning;
        uint64_t now = 1000000;
        // Every frame has a hole, but only 0.1% of packets are missing.
        for (unsigned frame = 0; frame < 1200; ++frame, now += 10000)
            assert(!warning.observe(now, true, 1000, 1));
        // A third of frames lose every packet. Those tiny frames must not
        // outweigh the intact large frames (100 / 200100 packets per window).
        for (unsigned frame = 0; frame < 1200; ++frame, now += 10000)
            assert(!warning.observe(now, true, frame % 3 ? 1000 : 1, frame % 3 ? 0 : 1));
        // Conversely, a few large lossy frames can dominate the packet count
        // even when most delivered frames are intact.
        warning = {};
        for (unsigned frame = 0; frame < 300; ++frame, now += 10000)
            assert(!warning.observe(now, true, frame % 10 ? 1 : 1000, frame % 10 ? 0 : 300));
        // 9000 / 30270 = 29.73%, just below the immediate threshold.
        assert(!warning.observe(now, true, 1, 0));
        warning = {};
        for (unsigned frame = 0; frame < 300; ++frame, now += 10000)
            assert(!warning.observe(now, true, frame % 10 ? 1 : 1000, frame % 10 ? 0 : 310));
        assert(warning.observe(now, true, 1, 0)); // 9300 / 30270 = 30.72%.
        assert(warning.observe(now + 10000, true, 0, 0)); // No invented recovery sample.
    }
    {
        using Reason = ClientPacingWarning::Reason;
        ClientPacingWarning warning;
        for (uint64_t t = 1; t <= 19; ++t)
            assert(warning.observe(t * 1000000, true, true, true, false, true, 98.0) == Reason::None);
        assert(warning.observe(20000000, true, true, false, false, true, 98.0) == Reason::None);
        for (uint64_t t = 21; t <= 40; ++t)
            assert(warning.observe(t * 1000000, true, true, true, false, true, 98.0) == Reason::None);
        assert(warning.observe(41000000, true, true, true, false, true, 98.0) == Reason::BufferLimit);
        for (uint64_t t = 42; t <= 45; ++t)
            assert(warning.observe(t * 1000000, true, true, true, false, true, 98.0) == Reason::BufferLimit);
        assert(warning.observe(46000000, true, true, true, false, true, 98.0) == Reason::None);
        // A reporting gap and a fresh 20-second streak cannot bypass the session cooldown.
        assert(warning.observe(50000000, true, true, true, false, true, 98.0) == Reason::None);
        for (uint64_t t = 51; t <= 72; ++t)
            assert(warning.observe(t * 1000000, true, true, true, true, true, 98.0) == Reason::None);
        // The warning becomes eligible again after 20 minutes of session time.
        assert(warning.observe(1240000000, true, true, true, true, true, 98.0) == Reason::None);
        for (uint64_t t = 1241; t < 1260; ++t)
            assert(warning.observe(t * 1000000, true, true, true, true, true, 98.0) == Reason::None);
        assert(warning.observe(1260000000, true, true, true, true, true, 98.0) == Reason::ProcessingOverload);
        assert(warning.observe(1261000000, false, true, true, true, true, 98.0) == Reason::None);
        for (const double quality : {99.01, 99.5, 100.0}) {
            ClientPacingWarning healthy;
            for (uint64_t t = 1; t <= 12; ++t)
                assert(healthy.observe(t * 1000000, true, true, true, true, true, quality) == Reason::None);
        }
        ClientPacingWarning recovering;
        for (uint64_t t = 1; t <= 19; ++t)
            assert(recovering.observe(t * 1000000, true, true, true, false, true, 100.0) == Reason::None);
        assert(recovering.observe(20000000, true, true, true, false, true, 99.0) == Reason::None);
        assert(recovering.observe(21000000, true, true, true, false, true, 99.0) == Reason::None);
        assert(recovering.observe(22000000, true, true, true, false, true, 99.0) == Reason::BufferLimit);
        for (const bool overloaded : {false, true}) {
            ClientPacingWarning threshold;
            for (uint64_t t = 1; t <= 20; ++t)
                threshold.observe(t * 1000000, true, true, true, overloaded, true, 99.0);
            assert(threshold.observe(21000000, true, true, true, overloaded, true, 99.0) != Reason::None);
            assert(threshold.observe(22000000, true, true, true, overloaded, true, 99.01) == Reason::None);
            assert(threshold.observe(23000000, true, true, true, overloaded, true, 99.0) == Reason::None);
        }
        // Even sustained processing overload must not warn below the cap.
        ClientPacingWarning belowCap;
        for (uint64_t t = 1; t <= 12; ++t)
            assert(belowCap.observe(t * 1000000, true, true, false, true, true, 95.0) == Reason::None);
        for (uint64_t t = 13; t <= 32; ++t)
            belowCap.observe(t * 1000000, true, true, true, true, true, 99.0);
        assert(belowCap.observe(33000000, true, true, true, true, true, 99.0) == Reason::ProcessingOverload);
        assert(belowCap.observe(34000000, true, true, false, true, true, 95.0) == Reason::None);
        const auto limited = ClientPacingWarning::message(Reason::BufferLimit, true, true, false);
        assert(limited.find("HEVC") != std::string::npos && limited.find("Try Smooth") != std::string::npos);
        assert(limited.find("network") == std::string::npos);
        assert(ClientPacingWarning::message(Reason::BufferLimit, false, true, true).find("HEVC") == std::string::npos);
        assert(ClientPacingWarning::message(Reason::BufferLimit, true, false, true).find("Try Smooth") == std::string::npos);
        assert(ClientPacingWarning::message(Reason::BufferLimit, true, false, true).find("HEVC") == std::string::npos);
        assert(ClientPacingWarning::message(Reason::ProcessingOverload, true, true, false).find("Try Smooth") == std::string::npos);
    }
    QCoreApplication app(argc, argv);
    assert(argc == 2 && QDir::setCurrent(argv[1])); // Directory containing ModeSeven.ttf.
    assert(SDL_Init(SDL_INIT_TIMER) == 0);
    const auto graph = testTimingGraph();
    {
        OverlayManager manager;
        manager.setStatusMessage(StatusSource::Network, "network loss");
        manager.setStatusMessage(StatusSource::ClientPacing, "client pacing");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "network loss\n\nclient pacing");
        manager.setStatusMessage(StatusSource::Mouse, "mouse");
        manager.setStatusMessage(StatusSource::Network, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "mouse");
        manager.setStatusMessage(StatusSource::Mouse, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "client pacing");
        manager.setStatusMessage(StatusSource::Network, "network loss");
        manager.setStatusMessage(StatusSource::ClientPacing, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "network loss");
        manager.setStatusMessage(StatusSource::Network, "");
        assert(!manager.isOverlayEnabled(OverlayStatusUpdate));
        manager.setStatusMessage(StatusSource::Network, PyroWavePacketLossWarning::Message);
        manager.setStatusMessage(StatusSource::PacketLoss, PyroWavePacketLossWarning::Message);
        assert(manager.getOverlayText(OverlayStatusUpdate) == PyroWavePacketLossWarning::Message);
        manager.setStatusMessage(StatusSource::Network, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == PyroWavePacketLossWarning::Message);
        manager.setStatusMessage(StatusSource::ClientPacing, "client pacing");
        manager.setStatusMessage(StatusSource::Mouse, "mouse");
        manager.setStatusMessage(StatusSource::PacketLoss, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "mouse");
        manager.setStatusMessage(StatusSource::Mouse, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "client pacing");
        manager.setStatusMessage(StatusSource::ClientPacing, "");
        const auto statusColor = manager.getOverlayColor(OverlayStatusUpdate);
        assert(statusColor.r > 0 && statusColor.g == 0 && statusColor.b == 0);
        assert(!manager.isOverlayEnabled(OverlayStatusUpdate));
        Presenter old(manager), replacement(manager);
        manager.setOverlayRenderer(&old);
        manager.updateOverlayText(OverlayDebug, "initial");
        manager.setOverlayState(OverlayDebug, true);
        old.awaitText("initial");
        {
            std::lock_guard<std::mutex> guard(old.lock);
            old.block = true;
        }
        manager.updateOverlayText(OverlayDebug, "blocked upload");
        {
            std::unique_lock<std::mutex> guard(old.lock);
            assert(old.ready.wait_for(guard, 3s, [&] { return old.entered; }));
        }
        // Producers must complete while the renderer callback remains blocked.
        // Superseded requests are coalesced instead of building an unbounded queue.
        auto producer = std::async(std::launch::async, [&] {
            for (unsigned i = 0; i < 1000; ++i)
                manager.updateOverlayText(OverlayDebug, std::to_string(i).c_str());
            manager.setOverlayState(OverlayDebug, false);
            manager.updateOverlayText(OverlayDebug, "latest");
            manager.setOverlayState(OverlayDebug, true);
        });
        assert(producer.wait_for(1s) == std::future_status::ready);
        producer.get();
        auto detached = std::async(std::launch::async, [&] { manager.setOverlayRenderer(nullptr); });
        assert(detached.wait_for(30ms) == std::future_status::timeout);
        {
            std::lock_guard<std::mutex> guard(old.lock);
            old.block = false;
            old.ready.notify_all();
        }
        assert(detached.wait_for(3s) == std::future_status::ready);
        detached.get();
        manager.setOverlayRenderer(&replacement);
        replacement.awaitText("latest");
        assert(!manager.isTimingGraphEnabled());
        manager.setTimingGraphState(true);
        manager.updateOverlayText(OverlayDebug, "Live timing graph\nIncoming smoothness: 99.8%", graph);
        replacement.awaitText("Live timing graph\nIncoming smoothness: 99.8%");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(replacement.height > TimingGraphLayout::Height);
            // Each series is drawn only in its own lane, so drift between
            // planned, submitted and displayed cadence is never overdrawn.
            for (int lane = 0; lane < TimingGraphLayout::Lanes; ++lane)
                for (int ink = 0; ink < 3; ++ink)
                    assert((replacement.rowColors[lane][ink] > 100) == (lane == ink));
            assert(replacement.rowColors[1][3] > 0); // Submission hitches beyond the axis are marked.
            assert(replacement.rowColors[2][3] > 0); // So are display spikes.
            const auto& incoming = replacement.rowColors[TimingGraphLayout::BufferLane];
            for (int ink = 4; ink < 8; ++ink) assert(incoming[ink] > 10);
            assert(incoming[3] > 0); // Exact decoder/buffer crossings have red X markers.
            for (int ink = 0; ink < 3; ++ink) assert(incoming[ink] == 0);
        }
        const auto adaptiveGraph=[](int64_t producer,bool eligible,bool gpuKnown) {
            TimingGraphSnapshot snapshot;
            for(unsigned i=0;i<240;++i) {
                TimingGraphPoint p;
                p.sourceTimeUs=1000000+uint64_t(i)*10000; p.sourceTimingValid=true;
                p.sourcePeriodUs=p.targetIntervalUs=p.submissionIntervalUs=10000;
                p.submissionUs=p.targetUs=p.sourceTimeUs+10000;
                p.bufferUs=2500; p.requestedBufferUs=9000;
                p.networkReadyUs=p.sourceTimeUs+500; p.decoderOutputUs=p.sourceTimeUs+8000;
                p.decoderReadyUs=gpuKnown?p.sourceTimeUs+9000:0;
                p.inputPercentileEnabled=true; p.producerEligible=eligible; p.producerRequiredUs=producer; p.inputToleranceUs=750; p.toleranceUs=750;
                p.inputTargetUs=1500; p.inputTargetValid=true; p.inputProbabilityPpm=995000; p.inputWindowUs=200000000;
                snapshot.points.push_back(p);
            }
            return snapshot;
        };
        for(const auto scenario : {0,1,2,3}) {
            const bool eligible=scenario!=3;
            const auto name=std::string("Adaptive causal coverage ")+std::to_string(scenario);
            manager.updateOverlayText(OverlayDebug,name.c_str(),adaptiveGraph(scenario==2?3251:scenario==1?3250:3190,eligible,scenario!=1));
            replacement.awaitText(name.c_str());
            std::lock_guard<std::mutex> guard(replacement.lock);
            const auto& incoming=replacement.rowColors[TimingGraphLayout::BufferLane];
            assert((incoming[3]>0)==(scenario==2)); // Raw late stage alone never paints a causal red X.
            assert(incoming[6]>100); // Raw stage remains visible independently of causal coverage.
            assert(incoming[7]>100); // Applied buffer stays readable even beneath dense exact-crossing markers.
            assert((incoming[5]>100)==(eligible && scenario!=2)); // Dense red X marks cover the producer line only when every frame misses.
            assert((incoming[8]>100)==(scenario!=2)); // B+T allowance band is independent of actual B.
            assert(incoming[9]>100); // Learned quantile remains distinct from both B and B+T.
        }
        int normalWidth, normalHeight;
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            normalWidth = replacement.width; normalHeight = replacement.height;
        }
        assert(manager.getOverlayFontSize(OverlayDebug) == 20);
        const auto preview = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_PREVIEW");
        if (!preview.isEmpty()) qputenv("MOONLIGHT_OVERLAY_TEST_PREVIEW", qPrintable(preview + ".4k.bmp"));
        manager.setOutputSize(3840, 2160);
        replacement.awaitSize(int(std::lround(normalWidth * 1.3)));
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(manager.getOverlayFontSize(OverlayDebug) == 26);
            assert(manager.getOverlayFontSize(OverlayStatusUpdate) == 36);
            assert(std::abs(replacement.height - normalHeight * 1.3) < 4);
        }
        // Capture the lanes at both output sizes.
        qunsetenv("MOONLIGHT_OVERLAY_TEST_PREVIEW");
        manager.setOutputSize(1920, 1080);
        replacement.awaitSize(normalWidth);
        assert(manager.getOverlayFontSize(OverlayDebug) == 20);
        assert(replacement.height == normalHeight);
        // The inherited VRR-room credit must match the controller, not just T.
        for (const auto excess : {499, 500, 501}) {
            auto credited = adaptiveGraph(2500 + 1000 + excess, true, true);
            for (auto& point : credited.points) {
                point.inputToleranceUs = 500;
                point.inputVrrHeadroomUs = 1000;
            }
            const auto sample = bufferGraphSample(credited.points.back());
            assert(sample.producerMissed() == (excess > 500));
            assert(sample.producerUs == 2500 + excess);
            const auto name = std::string("VRR-room coverage boundary ") + std::to_string(excess);
            manager.updateOverlayText(OverlayDebug, name.c_str(), credited);
            replacement.awaitText(name);
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert((replacement.rowColors[TimingGraphLayout::BufferLane][3] > 0) == (excess > 500));
        }
        const auto previewFolder = qEnvironmentVariable("MOONLIGHT_READABILITY_PREVIEW_DIR");
        for (const bool fourK : {false, true}) {
            for (const bool graphOnly : {false, true}) {
                const int width = fourK ? 3840 : 1280, height = fourK ? 2160 : 800;
                // The normal overlay: Moonlight's stats, then the VRR summary.
                const std::string moonlight = std::string("Video stream: ") +
                    (fourK ? "3840x2160" : "1280x800") + " 110.00 FPS (Codec: PyroWave 10-bit HDR 4:4:4)\n"
                    "Incoming frame rate from network: 110.00 FPS\n"
                    "Decoding frame rate: 110.00 FPS\n"
                    "Rendering frame rate: 110.00 FPS\n"
                    "Host processing latency min/max/average: 1.0/3.2/1.8 ms\n"
                    "Frames dropped by your network connection: 0.05%\n"
                    "Frames dropped by client pacing: 0.00%\n"
                    "Average network latency: 2 ms (variance: 1 ms)\n"
                    "Average decoding time: 3.85 ms (waiting for the decoder 0.02 ms)\n"
                    + PerformanceStats::formatClientTiming(11503, 8010, 1285, 1) +
                    "Incoming smoothness (host): 99.50%\n";
                PerformanceStats::Snapshot summary;
                summary.vrr = summary.adaptive = true; summary.bufferUs = 5700;
                summary.smoothingUs = 1250;
                summary.targetPpm = 999500; summary.toleranceUs = 500;
                summary.nowUs = 5000000; summary.input.samples = summary.input.recentEligible = 22000;
                summary.input.recentMissed = 11; summary.input.targetUs = 5420; summary.input.windowUs = 300000000;
                summary.input.tailUncertain = false;
                summary.input.reportingWindowUs=10000000;summary.input.reportingEligible=1100;
                summary.input.reportingAbsorbed=20;summary.input.reportingMissed=2;
                auto coherent = adaptiveGraph(6959, true, true);
                Vrr13::PresentTiming matchedDisplay;
                BufferHitchHistory stageHistory;
                uint64_t displayTime = 1000000 + 18 * 9091 + 10000;
                for (size_t i = 0; i < coherent.points.size(); ++i) {
                    auto& point = coherent.points[i];
                    point.sourceTimeUs = 1000000 + i * 9091;
                    point.sourcePeriodUs = point.targetIntervalUs = point.submissionIntervalUs = 9091;
                    point.targetUs = point.submissionUs = point.sourceTimeUs + 10000;
                    point.networkReadyUs = point.sourceTimeUs + 500;
                    point.decoderOutputUs = point.sourceTimeUs + 8000;
                    point.decoderReadyUs = point.sourceTimeUs + 9000;
                    point.bufferUs = summary.bufferUs; point.extraDelayUs = summary.smoothingUs;
                    point.inputTargetUs = summary.input.targetUs;
                    point.toleranceUs = point.inputToleranceUs = summary.toleranceUs;
                    point.inputVrrHeadroomUs = 760;
                    point.inputProbabilityPpm = summary.targetPpm; point.inputWindowUs = summary.input.windowUs;
                    point.inputCoverageWindowUs = summary.input.reportingWindowUs;
                    point.inputSamples = summary.input.reportingEligible;
                    point.inputAbsorbed = summary.input.reportingAbsorbed;
                    point.inputMissed = summary.input.reportingMissed;
                    // Actual synthetic display identities drive both the plot
                    // and the summary observer; neither uses input coverage.
                    if (i >= 19) {
                        displayTime += 9091;
                        if (i == 40 || i == 80 || i == 120) displayTime += 501;
                        if (i == 41 || i == 81 || i == 121) displayTime -= 501;
                        point.displayUs = displayTime;
                        matchedDisplay.observe({i + 1, point.submissionUs, displayTime, point.targetUs,
                            i == 120 ? uint64_t(1) : uint64_t(0), point.submissionUs + 1500},
                            {summary.toleranceUs, 8333, 9091, 0});
                    }
                    stageHistory.record(point);
                }
                summary.present = matchedDisplay.stats();
                // 0.501 ms display-added steps count; within uncertainty they do not.
                assert(summary.present.misses == 4);
                summary.nowUs = coherent.points.back().submissionUs + 1500;
                coherent.hitches = stageHistory.snapshot(summary.nowUs);
                const auto text = moonlight + PerformanceStats::formatVrr(summary) + (graphOnly ? " " : "");
                const auto name = std::string(fourK ? "4k" : "deck") + (graphOnly ? "-graph" : "-stats-graph");
                if (!previewFolder.isEmpty()) qputenv("MOONLIGHT_OVERLAY_TEST_PREVIEW", qPrintable(previewFolder + "/" + QString::fromStdString(name) + ".bmp"));
                manager.setOutputSize(width, height);
                manager.setOverlayState(OverlayDebug, !graphOnly);
                manager.updateOverlayText(OverlayDebug, text.c_str(), coherent);
                replacement.awaitText(text);
                std::lock_guard<std::mutex> guard(replacement.lock);
                assert(replacement.width <= width && replacement.height <= height);
                if (!graphOnly) assert(replacement.bottomTransparent && replacement.statsInk > 0 && replacement.statsShadow > 0);
                assert(replacement.height >= (fourK ? 704 : TimingGraphLayout::Height));
                if (graphOnly) assert(replacement.height == (fourK ? 705 : TimingGraphLayout::Height));
                else assert(replacement.height > (fourK ? 705 : TimingGraphLayout::Height));
                std::cout << "Readability " << name << ": " << replacement.width << "x" << replacement.height
                          << " fits " << width << "x" << height << "\n";
            }
        }
        qunsetenv("MOONLIGHT_OVERLAY_TEST_PREVIEW");
        manager.setOutputSize(1920, 1080);
        manager.setOverlayState(OverlayDebug, true);
        // Variation within the flat zone is drawn on the reference line;
        // larger variation is drawn at its true height.
        const auto jitter = [](uint64_t amplitudeUs) {
            TimingGraphSnapshot snapshot;
            auto& points = snapshot.points;
            for (unsigned i = 0; i < 300; ++i) {
                TimingGraphPoint point;
                point.sourcePeriodUs = point.targetIntervalUs = point.submissionIntervalUs = 10000;
                point.targetUs = point.submissionUs = 1000000 + i * 10000;
                point.displayUs = point.submissionUs + 2000 + (i % 2 ? amplitudeUs : 0);
                points.push_back(point);
            }
            return snapshot;
        };
        manager.updateOverlayText(OverlayDebug, "Unnoticeable variation", jitter(900));
        replacement.awaitText("Unnoticeable variation");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            for (int lane = 0; lane < TimingGraphLayout::Lanes; ++lane)
                assert(replacement.maxInkY[lane] - replacement.minInkY[lane] <= 2);
            assert(replacement.rowColors[2][2] > 100);
        }
        manager.updateOverlayText(OverlayDebug, "Noticeable variation", jitter(1500));
        replacement.awaitText("Noticeable variation");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(replacement.maxInkY[2] - replacement.minInkY[2] >= 30);
            assert(replacement.maxInkY[0] - replacement.minInkY[0] <= 2);
            assert(replacement.maxInkY[1] - replacement.minInkY[1] <= 2);
            for (const auto& row : replacement.rowColors) assert(row[3] == 0);
        }
        // A backend without DisplayEvent feedback must leave the display lane
        // empty rather than painting submission timestamps as presentation.
        auto withoutDisplay = graph;
        for (auto& point : withoutDisplay.points) point.displayUs = 0;
        manager.updateOverlayText(OverlayDebug, "No OS display feedback", std::move(withoutDisplay));
        replacement.awaitText("No OS display feedback");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(replacement.rowColors[0][0] > 100 && replacement.rowColors[1][1] > 100);
            assert(replacement.rowColors[2][2] == 0);
        }
        // Graph-only refreshes redraw without new stats text.
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            replacement.rowColors = {};
        }
        manager.updateTimingGraph(graph);
        {
            std::unique_lock<std::mutex> guard(replacement.lock);
            assert(replacement.ready.wait_for(guard, 3s, [&] { return replacement.rowColors[2][2] > 100; }));
        }
        // The graph and stats text toggle independently. The graph shows on
        // its own while the stats text is hidden.
        manager.setOverlayState(OverlayDebug, false);
        assert(manager.isOverlayEnabled(OverlayDebug) && !manager.isStatsEnabled() && manager.isTimingGraphEnabled());
        manager.updateTimingGraph(graph);
        {
            std::unique_lock<std::mutex> guard(replacement.lock);
            assert(replacement.ready.wait_for(guard, 3s, [&] {
                return replacement.height == TimingGraphLayout::Height && replacement.rowColors[2][2] > 100;
            }));
        }
        // Export a graph-only adaptive software surface for inspection; this
        // allocates SDL pixels only and never creates a native window/renderer.
        const auto adaptivePreview=qEnvironmentVariable("MOONLIGHT_VRR_TEST_EXPORT_ADAPTIVE_GRAPH");
        if(!adaptivePreview.isEmpty()) qputenv("MOONLIGHT_OVERLAY_TEST_PREVIEW",qPrintable(adaptivePreview));
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            replacement.rowColors={};
        }
        auto adaptiveOnly=adaptiveGraph(3190,true,true);
        for(auto& p:adaptiveOnly.points) {p.trajectoryCarryUs=4443;p.inputSamples=23142;p.inputAbsorbed=6200;p.inputMissed=123;}
        manager.updateTimingGraph(std::move(adaptiveOnly));
        {
            std::unique_lock<std::mutex> guard(replacement.lock);
            assert(replacement.ready.wait_for(guard,3s,[&] {
                const auto& incoming=replacement.rowColors[TimingGraphLayout::BufferLane];
                return replacement.height==TimingGraphLayout::Height && incoming[5]>100 && incoming[6]>100 && incoming[3]==0;
            }));
        }
        if(!adaptivePreview.isEmpty()) {assert(QFile::exists(adaptivePreview));qunsetenv("MOONLIGHT_OVERLAY_TEST_PREVIEW");}
        // Stats text alone ignores graph snapshots while the graph is off.
        manager.setTimingGraphState(false);
        assert(!manager.isOverlayEnabled(OverlayDebug));
        manager.setOverlayState(OverlayDebug, true);
        manager.updateOverlayText(OverlayDebug, "Stats only", graph);
        manager.updateTimingGraph(graph);
        {
            // Text state can advance between publication and the renderer's
            // callback. Wait for the actual text-only surface, not a newer
            // text string read alongside an earlier hide notification.
            std::unique_lock<std::mutex> guard(replacement.lock);
            assert(replacement.ready.wait_for(guard, 3s, [&] {
                return replacement.last == "Stats only" && replacement.bottomTransparent && replacement.height > 0 &&
                    replacement.height < TimingGraphLayout::Height;
            }));
            for (const auto& row : replacement.rowColors)
                for (auto count : row) assert(count == 0);
        }
        // A row longer than the wrap width must retain its shadow instead of
        // triggering the old unoutlined fallback for the whole stats block.
        const std::string wrapped = "Wrapped stats: " + std::string(150, 'M') + " end";
        if (!previewFolder.isEmpty()) qputenv("MOONLIGHT_OVERLAY_TEST_PREVIEW", qPrintable(previewFolder + "/wrapped-stats.bmp"));
        manager.updateOverlayText(OverlayDebug, wrapped.c_str());
        replacement.awaitText(wrapped);
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(replacement.height > manager.getOverlayFontSize(OverlayDebug) * 2);
            assert(replacement.bottomTransparent && replacement.statsInk > 0 && replacement.statsShadow > 0);
        }
        qunsetenv("MOONLIGHT_OVERLAY_TEST_PREVIEW");
        manager.setOverlayState(OverlayDebug, false);
        manager.updateOverlayText(OverlayDebug, "hidden");
        replacement.awaitText("hidden"); // Updating disabled text cannot cancel the hide request.
        assert(!manager.isOverlayEnabled(OverlayDebug));
        manager.setOverlayRenderer(nullptr);
        IOverlayRenderer::UpdateTiming timing;
        bool measured = false;
        while (replacement.takeOverlayTiming(timing)) {
            assert(timing.queueNs >= 0 && timing.rasterNs >= 0 && timing.dispatchNs >= 0);
            measured = true;
        }
        assert(measured && old.calls < 20);
    }
    SDL_Quit();
    std::cout << "Overlay worker isolation, coalescing, renderer lifetime, replacement and timing checks passed\n";
}
