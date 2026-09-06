#include "timing.h"
#include "smoothness.h"
#include "config.h"
#include "deadlines.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>

using namespace Vrr;

static Feedback feedback(uint64_t id, Ns presented, Ns observed)
{
    Feedback f;
    f.id = id; f.sequence = id; f.presented = presented; f.observed = observed;
    f.quality = Quality::Hardware; f.outcome = Outcome::Presented; f.uncertainty = 1000;
    return f;
}

static void timing()
{
    Config config;
    config.minInterval = 8 * Millisecond; config.maxInterval = 20 * Millisecond;
    Controller c(config);
    Frame f; f.id = 1; f.rtp = UINT32_MAX - 449; f.decoded = Second;
    c.arrive(f);
    auto p = c.plan(f, f.decoded);
    assert(p.mode == Mode::Acquiring && !p.latest);
    auto prepared = c.prepared(p, p.submit);
    assert(prepared.submit == p.submit); // Preparation reserve is charged exactly once.
    c.submitted(f.id, p.submit, p.target);
    auto fb = feedback(1, p.submit + 500000, p.submit + Millisecond);
    assert(c.feedback(fb));
    assert(!c.feedback(fb)); // Duplicates cannot re-train the latency estimate.
    Controller lateCorrelation(config);
    lateCorrelation.submitted(1, p.submit, p.target);
    auto syncOnly = fb; syncOnly.id = 0;
    assert(lateCorrelation.feedback(syncOnly));
    assert(lateCorrelation.feedback(fb)); // DXGI can resolve a frame after its sync sample.
    assert(lateCorrelation.plan(f, fb.observed).compositorLead == 500000);
    auto future = feedback(2, fb.observed + Second, fb.observed);
    assert(!c.feedback(future));
    auto delayed = feedback(2, fb.presented + Millisecond, fb.observed + Second);
    assert(!c.feedback(delayed));
    f.id = 2; f.rtp = 450; f.decoded = std::max(f.decoded + 10 * Millisecond, fb.observed + 100000);
    c.arrive(f);
    assert(f.source == 10 * Millisecond); // RTP wrap preserves source time.
    p = c.plan(f, f.decoded);
    assert(p.mode == Mode::Tracking && p.earliest >= fb.presented + config.minInterval);
    assert(p.submit >= prepared.submit + config.minInterval + config.guard);
    auto stale = c.prepared(p, f.decoded + Second);
    assert(stale.mode == Mode::Stale && stale.latest == 0);
    auto state = c.checkpoint();
    Controller restored(Config{});
    assert(restored.restore(state) && restored.checkpoint() == state);
    assert(restored.plan(f, f.decoded).submit == p.submit);
    state[6] = 1000;
    assert(!restored.restore(state));
    Feedback reset; reset.outcome = Outcome::Reset;
    c.feedback(reset);
    assert(c.plan(f, f.decoded).mode == Mode::Acquiring);
    f.rtp += 900000; f.decoded += Second;
    c.arrive(f);
    assert(f.source == 0 && f.playout >= f.decoded);
}

static void jitterAndSmoothing()
{
    Config hostConfig;
    hostConfig.minInterval = 4 * Millisecond;
    Config smoothConfig = hostConfig; smoothConfig.smoothCadence = true;
    Controller host(hostConfig), smooth(smoothConfig);
    Ns lastHost = 0, lastSmooth = 0;
    long double hostError = 0, smoothError = 0;
    uint32_t rtp = 0;
    for (unsigned i = 1; i <= 1000; ++i) {
        rtp += (i % 2) ? 540 : 1260; // Alternating 6/14 ms host cadence.
        Frame a; a.id = i; a.rtp = rtp; a.decoded = Second + Ns(rtp) * Second / 90000;
        Frame b = a;
        host.arrive(a); smooth.arrive(b);
        if (i > 200) {
            hostError += std::abs(a.playout - lastHost - 10 * Millisecond);
            smoothError += std::abs(b.playout - lastSmooth - 10 * Millisecond);
            assert(std::abs(b.playout - b.decoded) <= 2 * smoothConfig.maxBuffer);
        }
        lastHost = a.playout; lastSmooth = b.playout;
    }
    assert(smoothError < hostError / 3);
    auto state = smooth.checkpoint();
    Controller restored(Config{});
    assert(restored.restore(state) && restored.checkpoint() == state);
    // Sustained rate change adapts rather than growing a pacing backlog forever.
    for (unsigned i = 0; i < 500; ++i) {
        rtp += 1800;
        Frame f; f.rtp = rtp; f.decoded = Second + Ns(rtp) * Second / 90000;
        smooth.arrive(f);
        assert(std::abs(f.playout - f.decoded) <= 2 * smoothConfig.maxBuffer);
    }
}

static void variablePresentationAndRenderCost()
{
    Config config; config.minInterval = 8 * Millisecond;
    Controller c(config);
    Ns lastPresented = 0, lastSubmitted = 0;
    // Two presentation latency phases must not become a 15 ms minimum refresh.
    for (uint64_t i = 1; i <= 128; ++i) {
        const Ns submitted = Second + Ns(i) * 20 * Millisecond;
        const Ns presented = submitted + (i % 2 ? 2 : 9) * Millisecond;
        c.submitted(i, submitted, presented);
        assert(c.feedback(feedback(i, presented, presented + 100000)));
        lastPresented = presented; lastSubmitted = submitted;
        c.renderCost(i % 4 ? 2 * Millisecond : 8 * Millisecond);
    }
    Frame f; f.id = 129; f.rtp = 90000; f.decoded = lastPresented + 200000;
    c.arrive(f);
    const auto p = c.plan(f, f.decoded);
    assert(p.earliest <= lastPresented + config.minInterval + 100000);
    assert(p.submit >= lastSubmitted + config.minInterval);

    // A slow render tail advances preparation; it cannot force an 8 ms wait
    // on every otherwise-fast frame after the GPU has already finished.
    Controller render(config);
    for (unsigned i = 0; i < 128; ++i) render.renderCost(i % 4 ? 2 * Millisecond : 8 * Millisecond);
    f.decoded = Second; render.arrive(f);
    const auto start = render.plan(f, f.decoded);
    assert(start.renderBudget == 8 * Millisecond && start.prepare == std::max(f.decoded, start.submit - start.renderBudget - start.buffer));
    const auto ready = render.prepared(start, f.decoded + 2 * Millisecond);
    assert(ready.submit <= f.decoded + start.buffer + 2 * Millisecond);
}

static void spikeRecovery()
{
    for (bool smoothing : {false, true}) {
        Config config; config.smoothCadence = smoothing;
        Controller c(config);
        uint64_t id = 0;
        uint32_t rtp = 0;
        Ns time = Second;
        auto feed = [&](Ns sourceStep, Ns decodedStep, unsigned skipped = 0) {
            id += 1 + skipped;
            rtp += uint32_t(sourceStep * 90000 / Second);
            time += decodedStep;
            Frame f; f.id = id; f.rtp = rtp; f.decoded = time;
            c.arrive(f);
            c.renderCost(Millisecond);
            return f;
        };
        for (int i = 0; i < 200; ++i) feed(10 * Millisecond, 10 * Millisecond);
        const auto before = c.checkpoint();
        feed(60 * Millisecond, 60 * Millisecond);
        assert(c.recoveryFlags() & ArrivalStall);
        assert(c.sourcePeriod() == 10 * Millisecond);
        // Checkpoint replay must preserve an in-progress recovery episode.
        Controller restored(config);
        assert(restored.restore(c.checkpoint()) && restored.checkpoint() == c.checkpoint());
        for (int i = 0; i < 20; ++i) {
            auto f = feed(10 * Millisecond, 10 * Millisecond);
            assert(c.sourcePeriod() == 10 * Millisecond);
            assert(f.playout - f.decoded <= config.smoothingDelay + 10 * Millisecond + config.guard);
        }
        // Locally skipped frames preserve source cadence instead of teaching
        // the estimator that each skipped interval is a slower frame rate.
        feed(40 * Millisecond, 40 * Millisecond, 3);
        assert(c.sourcePeriod() == 10 * Millisecond);

        // A sustained 20 FPS interval really is a rate change; then return to
        // 100 FPS. Neither direction may become a permanently rejected outlier.
        for (int i = 0; i < 6; ++i) feed(50 * Millisecond, 50 * Millisecond);
        assert(c.sourcePeriod() == 50 * Millisecond);
        for (int i = 0; i < 6; ++i) feed(10 * Millisecond, 10 * Millisecond);
        assert(c.sourcePeriod() == 10 * Millisecond);

        // Arrival stall plus queued followers: exclude the whole backlog from
        // jitter learning, not only the first delayed frame.
        Controller burst(config);
        assert(burst.restore(before));
        Ns source = 0, lastDecode = 0;
        for (unsigned i = 201; i <= 400; ++i) {
            source = Ns(i) * 10 * Millisecond;
            Ns decoded = Second + source + (i == 201 ? 80 * Millisecond : 0);
            decoded = std::max(decoded, lastDecode + 100000);
            lastDecode = decoded;
            Frame f; f.id = i; f.rtp = i * 900; f.decoded = decoded;
            burst.arrive(f);
            auto p = burst.plan(f, decoded);
            assert(p.buffer <= config.smoothingDelay + 10 * Millisecond + config.guard);
        }
    }
}

static void adaptiveReserve()
{
    Reserve r;
    for (int i = 0; i < 300; ++i) r.observe(Millisecond, 1500000);
    assert(r.common() == Millisecond);
    r.observe(12 * Millisecond, 1500000);
    assert(r.common() == 12 * Millisecond); // Rare tail must survive quiet frames.
    for (int i = 0; i < 8; ++i) r.observe(3 * Millisecond, 1500000);
    assert(r.target(100 * Millisecond) >= 2 * Millisecond);
    Reserve copy;
    assert(copy.restore(r.checkpoint()) && copy.checkpoint() == r.checkpoint());
    Reserve cached;
    assert(cached.loadProfile(r.profile()));
    assert(cached.common() == r.common() && cached.boost() == 0 && cached.observations() == 0);
    for (int i = 0; i < 100000; ++i) cached.observe(500000, 4 * Millisecond);
    assert(cached.target(2 * Millisecond) <= 3 * Millisecond);

    Config config;
    Controller c(config);
    Ns time = Second;
    for (unsigned i = 1; i <= 600; ++i) {
        Frame f; f.id = i; f.rtp = i * 900; f.decoded = time;
        c.arrive(f);
        auto before = c.plan(f, time);
        c.renderCost((i % 10 < 3 ? 4 : 2) * Millisecond);
        auto ready = c.prepared(before, time + 2 * Millisecond);
        assert(ready.submit <= time + config.maxBuffer + config.initialRender + 4 * Millisecond);
        time += 10 * Millisecond;
    }
    assert(c.reserve().common() == 2 * Millisecond); // Store raw error; apply current headroom to the target.
    Controller restored(config);
    assert(restored.restore(c.checkpoint()) && restored.checkpoint() == c.checkpoint());
}

static void calibrationReliability()
{
    Reserve r;
    const Ns interval = Second / 60;
    assert(r.target(100 * Millisecond, interval) == interval);
    for (int i = 0; i < 20000; ++i) r.observe(250000, interval);
    assert(r.canRelease() && !r.reliable()); // Release need not wait for the retention window.
    assert(r.target(100 * Millisecond, interval) == 0);
    // A 0.1% tail defeats p90/p95 and short windows. Evaluate predictions
    // BEFORE observing each frame, over multiple complete learning horizons.
    unsigned misses = 0, measured = 0;
    for (int i = 0; i < 1000000; ++i) {
        const Ns required = i % 1000 == 0 ? 12 * Millisecond : 250000;
        const Ns prediction = r.target(100 * Millisecond, interval);
        if (i >= 100000) { ++measured; misses += required > prediction + Reserve::MissTolerance; }
        r.observe(required, prediction);
    }
    assert(uint64_t(misses) * 2000 <= measured);
    assert(r.evidence() > 35800 && r.evidence() <= 36001);
    assert(r.reliable() && r.common() == 12 * Millisecond);
    Reserve warm;
    assert(warm.loadProfile(r.profile()));
    assert(warm.cachedEvidence() == r.evidence() && !warm.reliable());
    assert(warm.target(100 * Millisecond, interval) == interval);
    for (unsigned i = 0; i < 36100; ++i) warm.observe(250000, interval);
    assert(warm.reliable() && warm.target(100 * Millisecond, interval) == 0);
    // A deterioration immediately adds pressure; sustained misses close the
    // release gate. Restoring a checkpoint must reproduce both behaviors.
    for (int i = 0; i < 32; ++i) warm.observe(20 * Millisecond, 12 * Millisecond);
    assert(!warm.reliable() && warm.target(100 * Millisecond, interval) >= interval);
    Reserve replay;
    assert(replay.restore(warm.checkpoint()) && replay.checkpoint() == warm.checkpoint());
    for (int i = 0; i < 40000; ++i) {
        warm.observe(250000, 20 * Millisecond);
        replay.observe(250000, 20 * Millisecond);
    }
    assert(warm.reliable() && replay.checkpoint() == warm.checkpoint());
    auto invalid = r.profile(); invalid[0] = 2;
    assert(!warm.loadProfile(invalid));
    invalid = r.profile(); invalid[1] = INT64_MAX;
    assert(!warm.loadProfile(invalid));

    // Cold-start allowance credits the unused source interval.
    for (unsigned fps : {30u, 60u, 75u, 100u, 120u}) {
        Config config; config.frameInterval = Second / fps; config.minInterval = Second / 144;
        Controller controller(config);
        Ns previous = 0;
        for (unsigned i = 1; i <= 300 * fps + 2000; ++i) {
            Frame f; f.id = i; f.rtp = i * (90000 / fps);
            f.decoded = Second + Ns(i) * Second / fps;
            controller.arrive(f);
            const auto p = controller.plan(f, f.decoded);
            if (i <= 2 * fps) assert(p.buffer >= config.minInterval);
            if (previous) assert(p.buffer >= previous - config.bufferRelease * (Second / fps + 1) / config.minInterval);
            controller.renderCost(Millisecond);
            previous = p.buffer;
        }
        assert(previous == config.minBuffer);
        Controller copy(config);
        assert(copy.restore(controller.checkpoint()) && copy.checkpoint() == controller.checkpoint());
    }
}

static void drainingBuffer()
{
    Config config; config.frameInterval = config.minInterval = 10 * Millisecond;
    Controller c(config);
    Ns buffer = 0, completed = 0;
    for (unsigned i = 1; i <= 41000; ++i) {
        // Following quiet calibration, add recurring slow rendering work.
        const Ns cost = i > 40000 && i % 2 ? 14 * Millisecond : Millisecond;
        Frame f; f.id = i; f.rtp = i * 900;
        f.decoded = Second + Ns(i) * 10 * Millisecond;
        c.arrive(f);
        auto p = c.plan(f, f.decoded);
        if (i == 40000) assert(p.buffer == config.minBuffer);
        if (i > 40001 && !c.reserve().canRelease()) assert(p.buffer >= buffer);
        completed = std::max(completed + 100000, f.decoded + cost);
        c.renderCost(cost, completed);
        buffer = p.buffer;
    }
    assert(buffer >= 10 * Millisecond && buffer <= 10 * Millisecond + Reserve::Bin); // 14 ms work - 1 ms typical - 3 ms tolerance.
    Config capped; capped.maxBuffer = 5 * Millisecond;
    Controller bounded(capped);
    Frame f; f.id = 1; f.rtp = 900; f.decoded = Second;
    bounded.arrive(f);
    assert(bounded.plan(f, f.decoded).buffer <= capped.maxBuffer);
}

static void cadenceAndSchedulingSlack()
{
    // Deadline errors are already relative to actual scheduling headroom.
    Reserve r;
    for (unsigned i = 1; i <= 37000; ++i) r.observe(14 * Millisecond, 100 * Millisecond);
    assert(r.reliable() && r.common() == 14 * Millisecond);
    Ns previous = 0;
    for (unsigned fps : {30u, 60u, 75u, 100u, 120u}) {
        const Ns expected = 14 * Millisecond - Reserve::MissTolerance;
        assert(r.target(100 * Millisecond, Second / fps) == expected);
        assert(expected >= previous); previous = expected;
    }
    Config config; config.frameInterval = Second / 100; config.minInterval = Second / 144;
    Controller c(config);
    uint32_t rtp = 0; uint64_t id = 0; Ns at = Second;
    for (unsigned fps : {100u, 75u, 120u, 60u}) {
        for (unsigned i = 0; i < 600; ++i) {
            rtp += 90000 / fps; at += Second / fps;
            Frame f; f.id = ++id; f.rtp = rtp; f.decoded = at;
            c.arrive(f); c.renderCost(Millisecond);
            if (i > 32) assert(std::abs(c.sourcePeriod() - Second / fps) <= 1);
        }
    }
    // Existing refresh scheduling delay overlaps the jitter allowance: the
    // controller takes the later deadline, rather than adding both delays.
    Config spare; spare.minInterval = 20 * Millisecond;
    Controller withReserve(spare);
    Config plain = spare; plain.adaptiveReserve = false;
    Controller withoutReserve(plain);
    Frame a; a.id = 1; a.rtp = 900; a.decoded = Second;
    Frame b = a;
    auto fb = feedback(0, Second, Second);
    assert(withReserve.feedback(fb) && withoutReserve.feedback(fb));
    withReserve.arrive(a); withoutReserve.arrive(b);
    assert(withReserve.plan(a, Second).submit == withoutReserve.plan(b, Second).submit);
}

static void combinedDeadlineError()
{
    Config config; config.frameInterval = config.minInterval = 10 * Millisecond;
    Controller c(config);
    for (unsigned i = 1; i <= 1000; ++i) {
        const bool tail = i % 100 == 0;
        Frame f; f.id = i; f.rtp = i * 900;
        // Same frame experiences 3 ms arrival/decode jitter, 2 ms extra GPU
        // work, and 1 ms involuntary scheduler delay: a paired 6 ms error.
        f.decoded = Second + Ns(i) * 10 * Millisecond + (tail ? 3 * Millisecond : 0);
        c.arrive(f);
        const Ns cost = tail ? 3 * Millisecond : Millisecond;
        c.renderCost(cost, f.decoded + cost + (tail ? Millisecond : 0), tail ? Millisecond : 0);
    }
    assert(c.reserve().common() == 6 * Millisecond);
    assert(c.reserve().target(config.reserveBoost, config.frameInterval) == 3 * Millisecond);
    // Disjoint component spikes must not be added as separate tail quantiles.
    Controller disjoint(config);
    for (unsigned i = 1; i <= 1000; ++i) {
        Frame f; f.id = i; f.rtp = i * 900;
        f.decoded = Second + Ns(i) * 10 * Millisecond + (i % 100 == 0 ? 3 * Millisecond : 0);
        disjoint.arrive(f);
        const Ns cost = i % 100 == 50 ? 4 * Millisecond : Millisecond;
        disjoint.renderCost(cost, f.decoded + cost);
    }
    assert(disjoint.reserve().common() == 3 * Millisecond);
}

static void provenStartupPadding()
{
    Reserve prior;
    for (unsigned i = 0; i < 37000; ++i) prior.observe(0, Millisecond);
    auto words = prior.profile(); words[2] = 3; words[3] = 3 * Millisecond;
    for (bool smoothing : {false, true}) {
        Config config; config.frameInterval = 10 * Millisecond; config.smoothCadence = smoothing;
        Controller c(config); assert(c.loadReserve(words));
        for (unsigned i = 1; i <= 30100; ++i) {
            Frame f; f.id = i; f.rtp = i * 900; f.decoded = Second + Ns(i) * 10 * Millisecond;
            c.arrive(f); c.renderCost(Millisecond);
        }
        // The saved reserve excludes guard/smoothing padding, so successive
        // warm sessions cannot repeatedly add those allowances to themselves.
        assert(c.reserve().successfulBuffer() <= 3 * Millisecond);
        assert(c.reserve().successfulBuffer() >= config.minBuffer - config.guard);
    }
}

static void startupStallAndQueueCapacity()
{
    Config config; config.frameInterval = Second / 120;
    Controller c(config);
    Frame first; first.id = 1; first.rtp = 750; first.decoded = Second;
    c.arrive(first); c.renderCost(285 * Millisecond, first.decoded + 285 * Millisecond);
    assert(!c.reserve().observations() && c.reserve().common() == 0);
    // Mirrors the actual capture: cold GPU work, skipped decoded frames and
    // recovery backlog must not become a five-minute 100 ms jitter tail.
    for (unsigned i = 36; i <= 400; ++i) {
        Frame f; f.id = i; f.rtp = i * 750; f.decoded = Second + Ns(i - 1) * Second / 120;
        c.arrive(f);
        const Ns delay = c.recoveryFlags() ? 90 * Millisecond : 0;
        c.renderCost(4 * Millisecond, f.decoded + 4 * Millisecond + delay, delay);
        assert(c.plan(f, f.decoded).buffer <= c.bufferLimit());
    }
    assert(c.reserve().common() < 10 * Millisecond);
    Reserve corruptPrior;
    for (unsigned i = 0; i < 37000; ++i) corruptPrior.observe(100 * Millisecond, 100 * Millisecond);
    for (unsigned fps : {30u, 60u, 75u, 100u, 120u}) {
        config.frameInterval = Second / fps;
        Controller bounded(config); assert(bounded.loadReserve(corruptPrior.profile()));
        for (unsigned i = 1; i < 100; ++i) {
            Frame f; f.id = i; f.rtp = i * (90000 / fps); f.decoded = Second + Ns(i) * Second / fps;
            bounded.arrive(f);
            assert(bounded.plan(f, f.decoded).buffer <= bounded.bufferLimit());
            bounded.renderCost(4 * Millisecond);
            assert(bounded.bufferLimit() <= 2 * config.frameInterval);
        }
    }
}

static SmoothnessReport score(bool hitch, bool loss, bool microseconds)
{
    Smoothness smooth(8 * Millisecond);
    Ns visible = Second;
    for (uint64_t i = 1; i <= 250; ++i) {
        smooth.submitted(i, Ns(i) * 8 * Millisecond);
        visible += hitch && i % 20 == 0 ? 24 * Millisecond : 8 * Millisecond;
        if (microseconds) visible += (i % 2) ? 1000 : -1000;
        if (!loss || i % 3) smooth.feedback(feedback(i, visible, visible + Millisecond));
    }
    return smooth.report();
}

static void smoothness()
{
    auto clean = score(false, false, false);
    auto noise = score(false, false, true);
    auto rough = score(true, false, false);
    auto missing = score(false, true, false);
    assert(clean.available && clean.percent == 100);
    assert(noise.available && noise.percent == 100);
    assert(rough.available && rough.percent < 95 && rough.hitches > 0);
    assert(!missing.available && missing.coverage < 90);
    Smoothness empty(8 * Millisecond);
    for (uint64_t i = 1; i < 100; ++i) empty.submitted(i, Ns(i) * 8 * Millisecond);
    assert(!empty.report().available); // CPU submission cannot manufacture smoothness.
    Smoothness session(8 * Millisecond);
    Ns at = Second;
    for (unsigned i = 1; i <= 600; ++i) {
        session.submitted(i, Ns(i) * 8 * Millisecond);
        at += (i == 10 ? 32 : 8) * Millisecond;
        session.feedback(feedback(i, at, at));
    }
    assert(session.report().hitches == 0 && session.report().worstHold == 8 * Millisecond);
    assert(session.sessionReport().hitches == 1 && session.sessionReport().worstHold == 32 * Millisecond);
}

static void sourcePhaseAndExactQuantile()
{
    // Captured failure: a short RTP step accompanies a long decode interval,
    // followed by steady 52 ms source AND decode intervals at a shifted phase.
    // Once that sustained cadence is recognized, the old phase is not jitter.
    for (bool smoothing : {false, true}) {
        Config cfg; cfg.frameInterval = 16 * Millisecond; cfg.smoothCadence = smoothing;
        Controller c(cfg); uint64_t id = 0; uint32_t rtp = 0; Ns at = Second;
        auto feed = [&](Ns src, Ns decode) {
            rtp += uint32_t(src * 90000 / Second); at += decode;
            Frame f; f.id = ++id; f.rtp = rtp; f.decoded = at;
            c.arrive(f); c.renderCost(4 * Millisecond, at + 4 * Millisecond);
            return c.plan(f, at).buffer;
        };
        for (unsigned i = 0; i < 300; ++i) feed(16 * Millisecond, 16 * Millisecond);
        feed(2 * Millisecond, 52 * Millisecond);
        for (unsigned i = 0; i < 20; ++i) {
            const Ns buffer = feed(52 * Millisecond, 52 * Millisecond);
            assert(buffer <= cfg.frameInterval + cfg.guard + (smoothing ? cfg.smoothingDelay : 0));
        }
        assert(c.reserve().common() < Millisecond);
        // Return to the prior phase and cadence without retaining a 50 ms tail.
        feed(76 * Millisecond, 26 * Millisecond);
        for (unsigned i = 0; i < 300; ++i) feed(16 * Millisecond, 16 * Millisecond);
        assert(c.reserve().common() <= (smoothing ? cfg.smoothingDelay : 0) + Reserve::Bin);
    }
    Reserve exact;
    // At 4,292 samples, two errors are permitted by 99.95%; three are not.
    for (unsigned i = 0; i < 4290; ++i) exact.observe(Millisecond, 2 * Millisecond);
    exact.observe(50 * Millisecond, 2 * Millisecond);
    exact.observe(50 * Millisecond, 2 * Millisecond);
    assert(exact.common() == Millisecond && !exact.failing());
    assert(exact.target(100 * Millisecond, 10 * Millisecond) == 0);
    exact.observe(50 * Millisecond, 2 * Millisecond);
    assert(exact.common() == 50 * Millisecond && exact.failing());
}

static void tolerantDeadlineAndWorkload()
{
    Reserve r;
    r.observe(5 * Millisecond - 1, 2 * Millisecond);
    r.observe(5 * Millisecond, 2 * Millisecond);
    assert(r.misses() == 0); // Exactly +3 ms beyond the actual buffer still passes.
    r.observe(5 * Millisecond + 1, 2 * Millisecond);
    assert(r.misses() == 1 && r.validationFrames() == 3);
    for (unsigned fps : {30u, 60u, 75u, 100u, 120u}) {
        Config cfg; cfg.frameInterval = Second / fps;
        Controller immediate(cfg), paced(cfg);
        for (unsigned i = 1; i <= fps * 310; ++i) {
            Frame f; f.id = i; f.rtp = uint32_t(uint64_t(i) * 90000 / fps);
            f.decoded = Second + Ns(i) * Second / fps;
            Frame g = f;
            immediate.arrive(f); paced.arrive(g);
            // Reporting work completed 45 ms later must not become 45 ms of jitter.
            const Ns cost = (i % 200 == 0 ? 7 : 4) * Millisecond;
            immediate.renderCost(cost, f.decoded + cost);
            paced.renderCost(cost, g.decoded + cost + 45 * Millisecond);
        }
        assert(immediate.reserve().common() == paced.reserve().common());
        assert(paced.reserve().common() <= 3 * Millisecond + Reserve::Bin);
        assert(paced.reserve().reliable());
        // RTP quantization can round the 3 ms tail into one additional histogram bin.
        assert(paced.reserve().target(cfg.reserveBoost, cfg.frameInterval) <= Reserve::Bin);
    }
    // FIFO backlog drains using the actual interval: 12 ms work overruns 120 FPS,
    // but fits 75 FPS. No fixed 120 FPS subtraction or frame-time double counting.
    Ns tails[2]{};
    unsigned index = 0;
    for (unsigned fps : {120u, 75u}) {
        Config cfg; cfg.frameInterval = Second / fps;
        Controller c(cfg);
        for (unsigned i = 1; i <= 1000; ++i) {
            Frame f; f.id = i; f.rtp = uint32_t(uint64_t(i) * 90000 / fps);
            f.decoded = Second + Ns(i) * Second / fps;
            c.arrive(f); c.renderCost(12 * Millisecond);
        }
        tails[index++] = c.reserve().common();
    }
    assert(tails[0] == 0 && tails[1] <= Reserve::Bin); // Sustained overload must not become cached jitter.
}

static void cadenceEvidence()
{
    Smoothness variable(Second / 144), gaps(Second / 144);
    Ns source = 0, shown = Second;
    for (uint64_t i = 1; i <= 1000; ++i) {
        const Ns step = i < 300 ? Second / 120 : i < 600 ? Second / 75 : (i % 2 ? 9 : 13) * Millisecond;
        source += step; shown += step;
        variable.submitted(i, source); variable.feedback(feedback(i, shown, shown));
        gaps.submitted(i, source);
        if (i % 20) gaps.feedback(feedback(i, shown, shown));
    }
    assert(variable.report().available && variable.report().percent == 100 && variable.report().hitches == 0);
    assert(gaps.report().available && gaps.report().percent == 100 && gaps.report().hitches == 0);
    assert(gaps.report().coverage < 100); // Missing evidence stays visible as coverage.
    // Known local drops are different from missing feedback and still show up.
    Smoothness dropped(8 * Millisecond);
    Ns at = Second; uint64_t id = 0;
    for (unsigned i = 0; i < 100; ++i) {
        const unsigned step = i % 10 ? 1 : 3;
        id += step; at += Ns(step) * 8 * Millisecond;
        dropped.submitted(id, Ns(id) * 8 * Millisecond);
        dropped.feedback(feedback(id, at, at));
    }
    assert(dropped.report().hitches > 0 && dropped.report().percent < 100);
    // Explicit compositor discards are evidence of lost frames, not feedback loss.
    Smoothness discarded(8 * Millisecond);
    for (uint64_t i = 1; i <= 100; ++i) {
        const Ns at = Second + Ns(i) * 8 * Millisecond;
        discarded.submitted(i, Ns(i) * 8 * Millisecond);
        auto f = feedback(i, at, at);
        if (i % 10 == 0) f.outcome = Outcome::Discarded;
        discarded.feedback(f);
    }
    assert(discarded.report().hitches > 0 && discarded.report().percent < 100);
    // RTP re-epoch is not an invented long visual hold.
    variable.submitted(1001, 0); shown += 10 * Millisecond;
    variable.feedback(feedback(1001, shown, shown));
    assert(variable.report().hitches == 0);
}

static void config(const char* directory)
{
    const std::string path = std::string(directory) + "/config-test.conf";
    Config c; std::string error;
    { std::ofstream f(path); f << "guard_us=100\nsmooth_cadence=1\n"; }
    assert(loadConfig(path, c, error) && c.guard == 100000 && c.smoothCadence);
    { std::ofstream f(path); f << "guard_us=nan\n"; }
    assert(!loadConfig(path, c, error));
    { std::ofstream f(path); f << "guard_us=100\nguard_us=200\n"; }
    assert(!loadConfig(path, c, error));
    { std::ofstream f(path); f << "buffer_attack_us=0\n"; }
    assert(!loadConfig(path, c, error));
    { std::ofstream f(path); f << "typo=1\n"; }
    assert(!loadConfig(path, c, error));
    { std::ofstream f(path); f << "buffer_max_us=2000\n"; }
    c = Config{};
    assert(loadConfig(path, c, error) && c.reserveMax == 2 * Millisecond && c.reserveBoost == 2 * Millisecond);
}

static void asynchronousMeasurements()
{
    Config cfg; cfg.frameInterval = 10 * Millisecond;
    Controller immediate(cfg), delayed(cfg);
    struct Sample { RenderProbe probe; Ns cost, at; };
    std::vector<Sample> samples;
    for (unsigned i = 1; i <= 1000; ++i) {
        Frame f; f.id = i; f.rtp = i * 900;
        f.decoded = Second + Ns(i) * 10 * Millisecond + (i % 37 == 0 ? 2 * Millisecond : 0);
        Frame g = f;
        immediate.arrive(f); delayed.arrive(g);
        samples.push_back({delayed.renderProbe(), Millisecond, f.decoded + Millisecond});
        immediate.renderCost(Millisecond, f.decoded + Millisecond);
        if (i >= 3) {
            const auto& s = samples[i - 3];
            delayed.renderCost(s.probe, s.cost, s.at);
        }
    }
    for (size_t i = samples.size() - 2; i < samples.size(); ++i) {
        const auto& s = samples[i]; delayed.renderCost(s.probe, s.cost, s.at);
    }
    assert(delayed.reserve().observations() == immediate.reserve().observations());
    assert(delayed.reserve().common() == immediate.reserve().common());

    Preparation p; p.token = 1; p.commandsSubmitted = Second + Millisecond;
    p.gpuReady = Second + 12 * Millisecond;
    // 1 ms CPU + 5 ms new GPU work; the first 6 ms of GPU occupancy belongs
    // to the previous frame, not another 6 ms service demand for this frame.
    assert(preparationWork(Second, p, Second + 7 * Millisecond) == 6 * Millisecond);
    assert(preparationWork(Second, p, Second - Millisecond) == 12 * Millisecond);
    p.acquired = Second + 4 * Millisecond; p.commandsSubmitted = Second + 5 * Millisecond;
    assert(preparationWork(Second, p, Second + 7 * Millisecond) == 6 * Millisecond);

    const auto observations = immediate.reserve().observations();
    const auto tail = immediate.reserve().common();
    auto uncertain = immediate.renderProbe(); uncertain.recovery |= UncertainCompletion;
    immediate.renderCost(uncertain, 20 * Millisecond, uncertain.decoded + 20 * Millisecond);
    assert(immediate.reserve().observations() == observations && immediate.reserve().common() == tail);

    for (bool feedbackFirst : {false, true}) {
        Controller c(cfg);
        c.submitted(1, Second, Second + 9 * Millisecond, 0);
        const auto fb = feedback(1, Second + 9 * Millisecond, Second + 10 * Millisecond);
        if (feedbackFirst) { assert(c.feedback(fb)); assert(!c.feedback(fb)); }
        Controller restored(cfg);
        assert(restored.restore(c.checkpoint()) && restored.checkpoint() == c.checkpoint());
        c.gpuReady(1, Second + 8 * Millisecond);
        if (!feedbackFirst) assert(c.feedback(fb));
        Frame f; f.id = 2; f.rtp = 900; f.decoded = Second + 20 * Millisecond;
        c.arrive(f);
        assert(c.plan(f, f.decoded).compositorLead == Millisecond);
        assert(!c.feedback(fb));
    }
}

static void originalDeadlines()
{
    Config cfg; Controller c(cfg);
    Frame f; f.id = 1; f.rtp = 900; f.decoded = Second;
    c.arrive(f);
    const auto p = c.plan(f, f.decoded);
    assert(p.deadline == p.target);
    const auto late = c.prepared(p, p.target + 10 * Millisecond);
    assert(late.target > p.target && late.deadline == p.deadline);
    Deadlines measured;
    measured.submitted(f.id, late.submit, p.deadline);
    auto fb = feedback(f.id, late.target, late.target + Millisecond);
    measured.feedback(fb); measured.feedback(fb);
    assert(measured.report().measured == 1 && measured.report().misses == 1);

    Deadlines boundaries;
    for (unsigned i = 1; i <= 4; ++i) {
        const Ns at = Second + Ns(i) * 10 * Millisecond;
        boundaries.submitted(i, at, at + 5 * Millisecond);
        const Ns error = i <= 2 ? 3 * Millisecond : 3 * Millisecond + 1;
        const Ns presented = at + 5 * Millisecond + (i % 2 ? error : -error);
        boundaries.feedback(feedback(i, presented, at + 10 * Millisecond));
    }
    assert(boundaries.report().measured == 4 && boundaries.report().misses == 2);
    boundaries.submitted(5, 2 * Second, 2 * Second);
    auto invalid = feedback(5, 2 * Second - 1, 2 * Second);
    boundaries.feedback(invalid); // Before submission.
    invalid = feedback(5, 2 * Second + Millisecond, 2 * Second);
    boundaries.feedback(invalid); // Future timestamp.
    invalid = feedback(5, 2 * Second, 2 * Second); invalid.uncertainty = Millisecond;
    boundaries.feedback(invalid);
    assert(boundaries.report().measured == 4 && boundaries.report().submitted == 5);
    assert(!boundaries.report().available && boundaries.report().feedbackCoverage == 80);
    boundaries.feedback(feedback(5, 2 * Second, 2 * Second));
    assert(boundaries.report().measured == 5);
    boundaries.submitted(6, 303 * Second, 303 * Second);
    boundaries.feedback(feedback(6, 303 * Second, 303 * Second));
    assert(boundaries.report().measured == 1 && boundaries.report().misses == 0);
    assert(boundaries.report().duration == 0);
}

static void bufferableDelaysOnly()
{
    // Four milliseconds of queued normal work caused entirely by host cadence
    // is part of the expected schedule, not new receiver jitter. Smoothing mode
    // must not change calibration, even after the startup buffer has drained.
    Config cfg; cfg.frameInterval = 20 * Millisecond;
    Config smoothed = cfg; smoothed.smoothCadence = true;
    Controller host(cfg), smooth(smoothed);
    Ns source = 0;
    for (unsigned i = 1; i <= 2500; ++i) {
        source += (i % 2 ? 12 : 28) * Millisecond;
        Frame f; f.id = i; f.rtp = uint32_t(source * 90000 / Second); f.decoded = Second + source;
        Frame g = f;
        host.arrive(f); smooth.arrive(g);
        assert(host.renderProbe().residual == 0 && smooth.renderProbe().residual == 0);
        assert(host.plan(f, f.decoded).buffer == smooth.plan(g, g.decoded).buffer);
        host.renderCost(16 * Millisecond); smooth.renderCost(16 * Millisecond);
    }
    assert(host.reserve().common() == 0 && smooth.reserve().common() == 0);
    assert(host.reserve().observations() > 2400 && smooth.reserve().observations() == host.reserve().observations());
    Frame last; last.decoded = Second + source;
    assert(host.plan(last, last.decoded).buffer == cfg.minBuffer);

    // Genuine receiver jitter at otherwise identical host timestamps is learned
    // identically with smoothing enabled or disabled, across FPS and headroom.
    for (unsigned fps : {30u, 60u, 75u, 100u, 120u}) {
        Config a; a.frameInterval = Second / fps; a.minInterval = a.frameInterval;
        Config b = a; b.smoothCadence = true;
        Controller plain(a), filtered(b);
        for (unsigned i = 1; i <= 3000; ++i) {
            Frame f; f.id = i; f.rtp = uint32_t(uint64_t(i) * 90000 / fps);
            f.decoded = Second + Ns(f.rtp) * Second / 90000 + (i % 100 == 0 ? 6 * Millisecond : 0);
            Frame g = f;
            plain.arrive(f); filtered.arrive(g);
            assert(plain.plan(f, f.decoded).buffer == filtered.plan(g, g.decoded).buffer);
            plain.renderCost(Millisecond); filtered.renderCost(Millisecond);
        }
        // Rational RTP intervals can add 1 ns to the clock residual, rounded
        // upward into the next 250 us histogram bin.
        assert(plain.reserve().common() >= 6 * Millisecond &&
               plain.reserve().common() <= 6 * Millisecond + Reserve::Bin);
        assert(filtered.reserve().common() == plain.reserve().common());
        assert(plain.reserve().target(a.reserveBoost, a.frameInterval) >= 3 * Millisecond &&
               plain.reserve().target(a.reserveBoost, a.frameInterval) <= 3 * Millisecond + Reserve::Bin);
    }

    // A temporary processing burst is learned once it drains. A permanently
    // oversubscribed processor never teaches the reserve to store its backlog.
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        Config c; c.frameInterval = 10 * Millisecond; c.minInterval = c.frameInterval;
        Controller controller(c);
        Ns completed = 0;
        auto feed = [&](unsigned id, Ns cost) {
            Frame f; f.id = id; f.rtp = id * 900; f.decoded = Second + Ns(id) * 10 * Millisecond;
            completed = std::max(completed, f.decoded) + cost;
            controller.arrive(f); controller.renderCost(cost, completed);
            return f;
        };
        for (unsigned i = 1; i <= 400; ++i) feed(i, Millisecond);
        const auto before = controller.reserve().observations();
        feed(401, 14 * Millisecond);
        assert(controller.reserve().observations() == before);
        Controller copy(c); assert(copy.restore(controller.checkpoint()) && copy.checkpoint() == controller.checkpoint());
        if (scenario == 1) {
            for (unsigned i = 402; i <= 1400; ++i) feed(i, 14 * Millisecond);
            assert(controller.workloadOverloaded());
            assert(controller.reserve().observations() == before && controller.reserve().common() == 0);
            assert(controller.reserve().target(c.reserveBoost, c.frameInterval) == 0);
            assert(copy.restore(controller.checkpoint()) && copy.checkpoint() == controller.checkpoint());
            for (unsigned i = 1401; i <= 1600; ++i) feed(i, Millisecond);
            assert(!controller.workloadOverloaded() && controller.reserve().common() == 0);
            assert(controller.reserve().observations() > before);
        }
        else if (scenario == 2) {
            feed(403, Millisecond); // A dropped frame created this apparent idle gap.
            assert(controller.recoveryFlags() & FramesSkipped);
            assert(controller.reserve().observations() == before && controller.reserve().common() == 0);
            feed(404, Millisecond);
            assert(controller.reserve().observations() == before + 1);
        }
        else {
            for (unsigned i = 402; i <= 403; ++i) {
                auto f = feed(i, Millisecond);
                copy.arrive(f); copy.renderCost(Millisecond, completed);
                assert(copy.checkpoint() == controller.checkpoint());
            }
            assert(controller.reserve().observations() == before + 3);
            assert(controller.reserve().common() == 13 * Millisecond);
            assert(controller.reserve().target(c.reserveBoost, c.frameInterval) == 10 * Millisecond);
        }
    }
}

static void availableSchedulingHeadroom()
{
    for (unsigned display : {60u, 90u, 144u}) {
        Ns previousTarget = -1;
        for (unsigned fps : {30u, 60u, 75u, 100u, 120u}) {
            Config cfg; cfg.frameInterval = Second / fps; cfg.minInterval = Second / display;
            Controller clean(cfg), loaded(cfg);
            Ns completed = 0;
            for (unsigned i = 1; i <= 60 * fps; ++i) {
                Frame f; f.id = i; f.rtp = uint32_t(uint64_t(i) * 90000 / fps);
                f.decoded = Second + Ns(f.rtp) * Second / 90000;
                clean.arrive(f);
                const Ns buffer = clean.plan(f, f.decoded).buffer;
                assert(buffer <= std::min(cfg.frameInterval, cfg.minInterval) + cfg.guard);
                if (i >= 30 * fps) assert(buffer == cfg.minBuffer);
                clean.renderCost(Millisecond);
                // Identical receiver jitter and preparation spikes at every FPS.
                // Higher frame rates have less room to recover the same work.
                f.decoded += i % 100 == 0 ? 6 * Millisecond : 0;
                const Ns cost = i % 90 == 0 ? 14 * Millisecond : Millisecond;
                completed = std::max(completed, f.decoded) + cost;
                loaded.arrive(f); loaded.renderCost(cost, completed);
            }
            // Rational RTP conversion can shift the raw tail by one 250 us
            // histogram bin; it must not create an FPS-sized reserve increase.
            assert(loaded.bufferTarget() + Reserve::Bin >= previousTarget);
            previousTarget = loaded.bufferTarget();
            if (fps == 30) assert(loaded.bufferTarget() == 0);
            Controller restored(cfg);
            assert(restored.restore(loaded.checkpoint()));
            assert(restored.checkpoint() == loaded.checkpoint());
        }
    }
    // The tolerance is applied AFTER the available recovery interval, once.
    // At 50 FPS on a 100 Hz display, 10 ms is available. With a 0.5 ms
    // applied buffer, 13.5 ms excess is allowed; one nanosecond more misses.
    for (Ns extra : {Ns(0), Ns(1)}) {
        Config cfg; cfg.frameInterval = 20 * Millisecond; cfg.minInterval = 10 * Millisecond;
        Controller c(cfg);
        RenderProbe p; p.decoded = Second; p.residual = 13500000 + extra;
        p.applied = 500000; p.typical = Millisecond; p.period = 20 * Millisecond;
        c.renderCost(p, Millisecond, p.decoded + Millisecond);
        assert(c.reserve().validationFrames() == 1);
        assert(c.reserve().misses() == uint64_t(extra));
    }
    // An existing five-minute tail is reused at new content FPS; its raw
    // history stays intact while the target immediately credits the new gap.
    Reserve history;
    for (unsigned i = 0; i < 37000; ++i) history.observe(12 * Millisecond, 12 * Millisecond);
    Config dynamic; dynamic.minInterval = Second / 144;
    Controller changing(dynamic); assert(changing.loadReserve(history.profile()));
    Ns source = 0; uint64_t id = 0;
    for (unsigned fps : {120u, 30u, 120u}) {
        for (unsigned i = 0; i < 6 * fps; ++i) {
            source += Second / fps;
            Frame f; f.id = ++id; f.rtp = uint32_t(source * 90000 / Second);
            f.decoded = Second + source;
            changing.arrive(f); changing.renderCost(Millisecond);
        }
        assert(changing.reserve().common() == 12 * Millisecond);
        if (fps == 30) assert(changing.bufferTarget() == 0);
        else assert(changing.bufferTarget() > 7 * Millisecond);
    }
    // Slow normal preparation consumes recovery room even on a fast display.
    Config cfg; cfg.minInterval = 5 * Millisecond;
    Controller c(cfg);
    assert(c.schedulingHeadroom(20 * Millisecond, 12 * Millisecond) == 8 * Millisecond);
    assert(c.schedulingHeadroom(20 * Millisecond, 21 * Millisecond) == 0);
}

static void toleranceWithoutCushion()
{
    Reserve r;
    for (unsigned i = 0; i < 600; ++i) r.observe(Ns(i % 4) * Millisecond, 0);
    assert(r.observations() == 600 && r.validationFrames() == 600 && r.misses() == 0);
    assert(r.common() == 3 * Millisecond && r.target(100 * Millisecond) == 0 && r.canRelease());
    r.observe(3 * Millisecond + 1, 0);
    assert(r.misses() == 1 && r.observations() == 601 && !r.canRelease());
    assert(r.target(100 * Millisecond) == Reserve::Bin); // Not a whole startup frame.
    Reserve guarded;
    guarded.observe(3050000, 50000, Second, 50000); // Guard is part of the actual buffer.
    assert(guarded.misses() == 0);
    guarded.observe(3050001, 50000, Second + Millisecond, 50000);
    assert(guarded.misses() == 1);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    timing(); jitterAndSmoothing(); variablePresentationAndRenderCost(); spikeRecovery(); adaptiveReserve(); calibrationReliability(); drainingBuffer(); cadenceAndSchedulingSlack(); combinedDeadlineError(); provenStartupPadding(); startupStallAndQueueCapacity(); smoothness(); sourcePhaseAndExactQuantile(); tolerantDeadlineAndWorkload(); cadenceEvidence(); asynchronousMeasurements(); originalDeadlines(); availableSchedulingHeadroom(); bufferableDelaysOnly(); toleranceWithoutCushion(); config(argv[1]);
    std::cout << "VRR timing, RTP wrap, feedback validation, checkpoint, smoothing, perceptual proxy and config checks passed\n";
}
