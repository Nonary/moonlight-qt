#include "timing.h"
#include "trace.h"
#include "config.h"
#include "smoothness.h"
#include "deadlines.h"

#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Vrr;

static void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

static std::array<int64_t, 12> planWords(const Plan& p)
{
    return {p.prepare, p.submit, p.target, p.earliest, p.latest, p.uncertainty,
        p.buffer, p.renderBudget, p.compositorLead, int64_t(p.mode), p.belowRange, p.deadline};
}

static void tracePlan(Trace* trace, Event event, uint64_t id, Ns at, const Plan& p)
{
    if (trace) trace->add(event, id, at, {p.prepare, p.submit, p.target, p.earliest, p.latest,
        p.uncertainty, p.buffer, p.renderBudget, p.compositorLead, int64_t(p.mode), p.belowRange, p.deadline});
}

static int replay(const std::string& path)
{
    std::vector<Record> records;
    require(Trace::read(path, records), "invalid or truncated capture");
    Controller controller(Config{});
    bool active = false, readingState = false;
    size_t stateSize = 0, decisions = 0, arrivals = 0, checkpoints = 0;
    std::vector<int64_t> state;
    Frame frame;
    Plan plan;
    for (const auto& r : records) {
        const auto& d = r.data;
        const auto fail = [&] (bool ok) { require(ok, "replay diverged at record " + std::to_string(r.serial)); };
        if (r.event == Event::CheckpointBegin) {
            require(d[0] > 0 && d[0] <= 262144, "invalid checkpoint size");
            readingState = true; stateSize = size_t(d[0]); state.clear(); continue;
        }
        if (r.event == Event::CheckpointData) {
            if (readingState) state.insert(state.end(), d.begin(), d.end());
            continue;
        }
        if (r.event == Event::CheckpointEnd) {
            if (!readingState) continue;
            require(state.size() >= stateSize && state.size() < stateSize + 12, "incomplete checkpoint");
            state.resize(stateSize);
            if (active) fail(controller.checkpoint() == state);
            else { require(controller.restore(state), "invalid checkpoint"); active = true; }
            ++checkpoints; readingState = false; continue;
        }
        if (!active) continue; // A ring snapshot may begin in the middle of a checkpoint/frame.
        switch (r.event) {
        case Event::Arrival:
            frame = {}; frame.id = r.id; frame.rtp = uint32_t(d[0]);
            frame.received = d[1]; frame.assembled = d[2]; frame.decoded = d[3];
            controller.arrive(frame);
            fail(frame.source == d[4] && frame.playout == d[5]); ++arrivals;
            break;
        case Event::Recovery:
            fail(d[0] == int64_t(controller.recoveryFlags()) && d[1] == controller.sourcePeriod() &&
                 d[2] == controller.excludedFrames());
            break;
        case Event::Reserve:
            fail(d[0] == controller.reserve().common() && d[1] == controller.reserve().boost() &&
                 d[2] == controller.bufferTarget() &&
                 d[3] == int64_t(controller.reserve().observations()));
            break;
        case Event::Plan:
            fail(frame.id == r.id);
            plan = controller.plan(frame, r.at);
            fail(planWords(plan) == d); ++decisions; break;
        case Event::Prepared:
            fail(frame.id == r.id);
            plan = controller.prepared(plan, r.at);
            fail(planWords(plan) == d); ++decisions; break;
        case Event::Render:
            if (d[2]) {
                if (d[10] >= 1) controller.renderCost(RenderProbe{d[4], d[5], d[6], d[7], d[8], uint64_t(d[9])}, d[1], r.at, d[3]);
                else controller.renderCost(d[1], r.at, d[3]);
                if (d[10] >= 2) controller.gpuReady(r.id, r.at);
            }
            break;
        case Event::Wake: controller.wakeError(d[1]); break;
        case Event::Submit:
            if (d[2]) controller.submitted(r.id, r.at, d[0], d[6]);
            break;
        case Event::Feedback: {
            Feedback f;
            f.id = r.id; f.observed = r.at; f.sequence = uint64_t(d[0]); f.presented = d[1];
            f.uncertainty = d[2]; f.refresh = d[3]; f.output = uint64_t(d[4]); f.flags = uint64_t(d[5]);
            f.quality = Quality(d[6]); f.outcome = Outcome(d[7]);
            fail(controller.feedback(f) == bool(d[8])); break;
        }
        default: break;
        }
    }
    require(active && decisions, "capture contains no complete replayable decisions");
    std::cout << "{\"mode\":\"exact-replay\",\"decisions\":" << decisions << ",\"arrivals\":" << arrivals
              << ",\"checkpoints\":" << checkpoints << ",\"divergences\":0}\n";
    return 0;
}

struct Options {
    std::string input, capture, config;
    uint64_t seed = 1;
    size_t frames = 10000, skip = 0, limit = 0;
    double sourceHz = 116, displayHz = 120, floorHz = 48;
    Ns jitter = 3 * Millisecond, render = Millisecond, compositor = 500000;
    Ns feedback = 2 * Millisecond, wake = 50000;
    Ns hostVariance = 0, stall = 20 * Millisecond, initialRender = 0;
    double renderScale = 1;
    unsigned feedbackLoss = 0;
    bool fixedDisplay = false;
    size_t swapchainImages = 3;
};

static int measure(const std::string& path)
{
    std::vector<Record> records;
    require(Trace::read(path, records), "invalid or truncated capture");
    Ns minimum = Second / 120;
    bool header = false, active = false, deadlineKnown = false;
    double deadlineCoverage = 0;
    std::map<uint64_t, Ns> source, targets;
    Deadlines deadlines;
    std::unique_ptr<Smoothness> score;
    for (const auto& r : records) {
        const auto& d = r.data;
        if (!active) {
            if (r.event == Event::CheckpointBegin) header = true;
            else if (header && r.event == Event::CheckpointData && !score) {
                minimum = d[1];
                require(minimum >= Millisecond && minimum <= Second, "invalid display interval");
                score = std::make_unique<Smoothness>(minimum);
            }
            else if (header && score && r.event == Event::CheckpointEnd) active = true;
            continue;
        }
        if (r.event == Event::Reserve && d[9] == 2 && d[10] == Reserve::MissTolerance && d[5] > 0 && d[7] >= 0 && d[7] <= d[5]) {
            deadlineKnown = true; deadlineCoverage = 100.0 * (1.0 - double(d[7]) / d[5]);
        }
        if (r.event == Event::Plan) targets.emplace(r.id, d[11] ? d[11] : d[2]);
        if (r.event == Event::Arrival) source[r.id] = d[4];
        else if (r.event == Event::Submit && d[2] && source.count(r.id)) {
            score->submitted(r.id, source[r.id]); source.erase(r.id);
            if (targets.count(r.id)) deadlines.submitted(r.id, r.at, targets[r.id]);
            targets.erase(r.id);
        }
        else if (r.event == Event::Drop) { source.erase(r.id); targets.erase(r.id); }
        else if (r.event == Event::Feedback) {
            Feedback f; f.id = r.id; f.observed = r.at; f.sequence = uint64_t(d[0]);
            f.presented = d[1]; f.uncertainty = d[2]; f.refresh = d[3]; f.output = uint64_t(d[4]);
            f.flags = uint64_t(d[5]); f.quality = Quality(d[6]); f.outcome = Outcome(d[7]);
            score->feedback(f); deadlines.feedback(f);
        }
    }
    require(active, "no complete capture checkpoint");
    const auto full = score->sessionReport();
    const auto recent = score->report();
    std::cout << std::setprecision(10) << "{\"mode\":\"measured-capture\",\"smoothness_percent\":";
    if (full.available) std::cout << full.percent; else std::cout << "null";
    std::cout << ",\"feedback_coverage_percent\":" << full.coverage << ",\"intervals\":" << full.intervals
        << ",\"hitches\":" << full.hitches << ",\"worst_hold_ms\":" << double(full.worstHold) / 1e6
        << ",\"recent_cadence_error_p99_ms\":" << double(recent.p99Error) / 1e6
        << ",\"recent_smoothness_percent\":" << recent.percent
        << ",\"score_kind\":\"source_cadence_diagnostic\",\"buffer_deadline_coverage_percent\":";
    if (deadlineKnown) std::cout << deadlineCoverage; else std::cout << "null";
    const auto actual = deadlines.report();
    std::cout << ",\"presentation_deadline_coverage_percent\":";
    if (actual.available) std::cout << actual.coverage; else std::cout << "null";
    std::cout << ",\"presentation_deadline_misses\":" << actual.misses
        << ",\"presentation_deadline_measured_frames\":" << actual.measured
        << ",\"target_99_95_met\":";
    if (actual.available && actual.duration >= Reserve::Window)
        std::cout << (actual.misses * 2000 <= actual.measured ? "true" : "false");
    else std::cout << "null";
    std::cout << "}\n";
    return 0;
}

// Defined integer RNG, identical across standard libraries and process order.
struct Random {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    Ns bounded(Ns maximum) { return maximum > 0 ? Ns(next() % (uint64_t(maximum) + 1)) : 0; }
};

struct Input {
    Frame frame;
    Ns render = 0, compositor = 0, feedback = 0, wake = 0, cpu = 300000;
    Ns acquisitionWait = 0, stageWork = 0;
    int serviceVersion = 0;
    bool lost = false, measuredStages = false;
};

static std::vector<Input> inputs(const Options& options)
{
    std::vector<Input> result;
    if (!options.input.empty()) {
        std::vector<Record> records;
        require(Trace::read(options.input, records), "cannot read input capture");
        std::map<uint64_t, Input> frames;
        for (const auto& r : records) {
            if (r.event == Event::Arrival) {
                auto& f = frames[r.id].frame;
                f.id = r.id; f.rtp = uint32_t(r.data[0]); f.received = r.data[1];
                f.assembled = r.data[2]; f.decoded = r.data[3];
            }
            else if (r.event == Event::Drop && r.data[1] > 0) {
                auto& f = frames[r.id].frame;
                f.id = r.id; f.decoded = r.data[1]; f.rtp = uint32_t(r.data[2]);
                f.received = r.data[3]; f.assembled = r.data[4];
            }
            else if (r.event == Event::Render && r.data[2]) {
                auto& in = frames[r.id]; in.render = r.data[1]; in.serviceVersion = int(r.data[10]);
            }
            else if (r.event == Event::PrepareStages && r.data[4] > 0) {
                auto& in = frames[r.id];
                const Ns workStarted = std::max(r.data[0], r.data[1]);
                in.acquisitionWait = workStarted - r.data[0];
                in.stageWork = std::max<Ns>(0, r.data[4] - workStarted);
                in.cpu = std::clamp(r.data[2] - workStarted, Ns(0), in.stageWork);
                in.measuredStages = true;
            }
        }
        for (const auto& entry : frames) if (entry.second.frame.decoded > 0) {
            auto in = entry.second;
            // Recreate work once, not the old scheduler's image/GPU queue waits.
            if (in.serviceVersion == 2) in.render = std::max<Ns>(0, in.render - in.acquisitionWait);
            else if (in.serviceVersion < 2 && in.measuredStages) in.render = in.stageWork;
            result.push_back(in);
        }
        std::sort(result.begin(), result.end(), [](const Input& a, const Input& b) { return a.frame.decoded < b.frame.decoded; });
    }
    else {
        Random random{options.seed};
        Ns previous = 0;
        for (size_t i = 0; i < options.frames; ++i) {
            Input in;
            const Ns source = Ns(i * double(Second) / options.sourceHz) + (i % 2 ? options.hostVariance : 0);
            in.frame.id = i + 1;
            in.frame.rtp = uint32_t(source / Second * 90000 + source % Second * 90000 / Second);
            in.frame.received = Second + source + 2 * Millisecond + random.bounded(options.jitter);
            // Occasional genuine network stalls expose burst recovery and frame drops.
            if (i && i % 997 == 0) in.frame.received += options.stall;
            in.frame.assembled = in.frame.received + 300000;
            in.frame.decoded = std::max(previous + 1000, in.frame.assembled + 500000 + random.bounded(options.jitter / 4));
            previous = in.frame.decoded;
            result.push_back(in);
        }
    }
    require(options.skip < result.size(), "empty input selection");
    if (options.skip) result.erase(result.begin(), result.begin() + options.skip);
    if (options.limit && result.size() > options.limit) result.resize(options.limit);
    // Frame-indexed random environment samples are shared by ALL configurations.
    // Dropping a frame never shifts another frame's random draw.
    for (auto& in : result) {
        Random random{options.seed ^ (in.frame.id * 0x9e3779b97f4a7c15ULL)};
        const Ns fallbackRender = options.render + random.bounded(options.render / 4);
        if (!in.render) in.render = fallbackRender;
        in.render = Ns(in.render * options.renderScale);
        if (in.frame.id == 1 && options.initialRender) in.cpu = in.render = options.initialRender;
        in.compositor = options.compositor + random.bounded(options.compositor / 2);
        in.feedback = options.feedback + random.bounded(options.feedback / 4);
        in.wake = random.bounded(options.wake);
        in.lost = random.next() % 100 < options.feedbackLoss;
    }
    return result;
}

static double percentile(std::vector<Ns> values, unsigned p)
{
    if (values.empty()) return 0;
    const size_t index = (values.size() - 1) * p / 100;
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index] / 1e6;
}

static int simulate(const Options& o)
{
    const auto input = inputs(o);
    Config config;
    config.minInterval = Ns(std::ceil(Second / o.displayHz));
    config.frameInterval = Ns(Second / o.sourceHz);
    config.maxInterval = o.floorHz ? Ns(Second / o.floorHz) : 0;
    if (!o.config.empty()) { std::string error; require(loadConfig(o.config, config, error), error); }
    Controller controller(config);
    Smoothness smoothness(config.minInterval);
    Deadlines deadlines;
    auto trace = o.capture.empty() ? nullptr : std::make_unique<Trace>();
    if (trace) trace->checkpoint(controller, input.front().frame.decoded);
    std::deque<size_t> queue;
    std::deque<Feedback> pendingFeedback;
    Ns at = input.front().frame.decoded;
    Ns physicalScanout = 0, lastCheckpoint = 0;
    // Images can be rendered while a preceding image awaits presentation.
    // The old model serialized every acquire behind the previous scanout,
    // inventing a one-image swapchain and hiding controller throughput bugs.
    std::vector<Ns> imageAvailable(o.swapchainImages, 0);
    size_t precedingImage = o.swapchainImages;
    uint64_t sequence = 0, displayed = 0, dropped = 0, lowerMisses = 0, tracking = 0;
    size_t next = 0;
    std::vector<Ns> latency, errors;
    struct Completion { Frame frame; RenderProbe probe; Ns started, acquired, cpu, ready, dispatch; };
    std::deque<Completion> gpu;
    Ns gpuAvailable = 0, lastGpuReady = 0;
    auto recordDrop = [&](size_t index, Drop reason) {
        ++dropped;
        const auto& f = input[index].frame;
        if (trace) trace->add(Event::Drop, f.id, at, {int64_t(reason), f.decoded, f.rtp,
            f.received, f.assembled, int64_t(queue.size())});
    };
    // Arrival and feedback are independent event streams. Virtual time jumps
    // directly to work/events; there are no timers, sleeps, or wall-clock reads.
    auto advance = [&](Ns target) {
        at = std::max(at, target);
        while (next < input.size() && input[next].frame.decoded <= at) {
            if (queue.size() == QueueFrames) { recordDrop(queue.front(), Drop::Capacity); queue.pop_front(); }
            queue.push_back(next++);
        }
    };
    auto poll = [&] {
        while (!pendingFeedback.empty() && pendingFeedback.front().observed <= at) {
            auto f = pendingFeedback.front(); pendingFeedback.pop_front();
            f.observed = at; // Delivery to application, not physical event time.
            const bool used = controller.feedback(f);
            smoothness.feedback(f); deadlines.feedback(f);
            if (trace) trace->add(Event::Feedback, f.id, at,
                {int64_t(f.sequence), f.presented, f.uncertainty, f.refresh, int64_t(f.output),
                 int64_t(f.flags), int64_t(f.quality), int64_t(f.outcome), used});
        }
    };
    auto complete = [&] {
        while (!gpu.empty() && gpu.front().ready <= at) {
            const auto c = gpu.front(); gpu.pop_front();
            Preparation preparation; preparation.token = c.frame.id;
            preparation.acquired = c.acquired;
            preparation.commandsSubmitted = c.cpu; preparation.gpuReady = c.ready;
            const Ns work = preparationWork(c.started, preparation, lastGpuReady);
            lastGpuReady = c.ready;
            controller.renderCost(c.probe, work, c.ready, c.dispatch);
            controller.gpuReady(c.frame.id, c.ready);
            if (trace) {
                trace->add(Event::PrepareStages, c.frame.id, c.ready, {c.started, c.acquired, c.cpu, c.ready - 1000, c.ready});
                trace->add(Event::Render, c.frame.id, c.ready, {c.started, work, 1, c.dispatch,
                    c.probe.decoded, c.probe.residual, c.probe.applied, c.probe.typical, c.probe.period,
                    int64_t(c.probe.recovery), 3, c.cpu});
                if (!(c.frame.id % 16)) trace->add(Event::Reserve, c.frame.id, c.ready,
                    {controller.reserve().common(), controller.reserve().boost(), controller.bufferTarget(),
                     int64_t(controller.reserve().observations()), int64_t(controller.reserve().evidence()),
                     int64_t(controller.reserve().validationFrames()), controller.reserve().reliable(),
                     int64_t(controller.reserve().misses()), controller.reserve().duration(), 2, Reserve::MissTolerance});
            }
        }
    };
    while (next < input.size() || !queue.empty()) {
        advance(at); complete(); poll();
        if (gpu.size() >= 2) { advance(gpu.front().ready); continue; }
        if (queue.empty()) { advance(input[next].frame.decoded); poll(); }
        while (queue.size() > 1 && at - input[queue.front()].frame.decoded > controller.bufferLimit() + 2 * config.minInterval) {
            recordDrop(queue.front(), Drop::Stale); queue.pop_front();
        }
        const auto in = input[queue.front()]; queue.pop_front();
        auto f = in.frame;
        if (trace && (!lastCheckpoint || at - lastCheckpoint >= 5 * Second)) {
            trace->checkpoint(controller, at); lastCheckpoint = at;
        }
        controller.arrive(f);
        if (trace) trace->add(Event::Arrival, f.id, at, {f.rtp, f.received, f.assembled, f.decoded, f.source, f.playout});
        if (trace && controller.recoveryFlags()) trace->add(Event::Recovery, f.id, at,
            {int64_t(controller.recoveryFlags()), controller.sourcePeriod(), controller.excludedFrames()});
        Plan p = controller.plan(f, at);
        tracePlan(trace.get(), Event::Plan, f.id, at, p);
        if (p.target > f.playout + p.renderBudget + p.compositorLead + config.minInterval && !queue.empty()) {
            ++dropped;
            if (trace) trace->add(Event::Drop, f.id, at, {int64_t(Drop::Stale), f.decoded, f.rtp,
                f.received, f.assembled, int64_t(queue.size())});
            continue;
        }
        const auto probe = controller.renderProbe();
        const Ns planAt = at;
        advance(std::max(p.prepare, at) + in.wake);
        controller.wakeError(std::max<Ns>(0, at - p.prepare));
        if (trace) trace->add(Event::Wake, f.id, at, {p.prepare, std::max<Ns>(0, at - p.prepare)});
        const Ns started = at;
        // Model acquisition and GPU work separately from compositor delivery.
        const size_t image = size_t(std::min_element(imageAvailable.begin(), imageAvailable.end()) - imageAvailable.begin());
        const Ns acquired = std::max(at, imageAvailable[image]);
        advance(acquired + std::min(in.cpu, in.render));
        const Ns cpuCompleted = at;
        const Ns gpuReady = std::max(gpuAvailable, at) + std::max<Ns>(0, in.render - in.cpu);
        gpuAvailable = gpuReady;
        const Ns schedulingDelay = std::max<Ns>(0, started - std::max(planAt, p.prepare));
        for (unsigned attempts = 0;; ++attempts) {
            require(attempts < 100, "scheduler failed to converge");
            poll();
            p = controller.prepared(p, at);
            tracePlan(trace.get(), Event::Prepared, f.id, at, p);
            advance(std::max(at, p.submit) + in.wake);
            poll();
            p = controller.prepared(p, at);
            tracePlan(trace.get(), Event::Prepared, f.id, at, p);
            if (p.submit <= at) break;
        }
        const Ns submitted = at;
        controller.submitted(f.id, submitted, p.deadline, 0);
        deadlines.submitted(f.id, submitted, p.deadline);
        smoothness.submitted(f.id, f.source);
        if (trace) trace->add(Event::Submit, f.id, submitted, {p.deadline, at, 1, p.submit, p.uncertainty, p.target});
        // Counterfactual display: produce NEW feedback for this configuration.
        // Historical presentation events are never fed into a retimed run.
        const Ns physicalMin = Ns(std::ceil(Second / o.displayHz));
        const Ns physicalMax = o.floorHz ? Ns(Second / o.floorHz) : 0;
        Ns candidate = std::max(submitted, gpuReady) + in.compositor;
        if (physicalScanout && physicalMax) {
            while (physicalScanout + physicalMax < candidate) { physicalScanout += physicalMax; ++sequence; }
        }
        if (o.fixedDisplay) candidate = ((candidate + physicalMin - 1) / physicalMin) * physicalMin;
        const Ns scanout = std::max(candidate, physicalScanout ? physicalScanout + physicalMin : candidate);
        if (precedingImage < imageAvailable.size()) imageAvailable[precedingImage] = scanout;
        imageAvailable[image] = INT64_MAX;
        precedingImage = image;
        physicalScanout = scanout;
        ++sequence; ++displayed;
        latency.push_back(scanout - f.decoded);
        errors.push_back(std::abs(scanout - p.deadline));
        lowerMisses += p.belowRange; tracking += p.mode == Mode::Tracking;
        if (!in.lost) {
            Feedback feedback;
            feedback.id = f.id; feedback.sequence = sequence; feedback.presented = scanout;
            feedback.observed = scanout + in.feedback; feedback.uncertainty = 1000;
            feedback.output = 1; feedback.flags = 7;
            feedback.quality = Quality::Hardware; feedback.outcome = Outcome::Presented;
            auto position = std::upper_bound(pendingFeedback.begin(), pendingFeedback.end(), feedback.observed,
                [](Ns time, const Feedback& existing) { return time < existing.observed; });
            pendingFeedback.insert(position, feedback);
        }
        gpu.push_back({f, probe, started, acquired, cpuCompleted, gpuReady, schedulingDelay});
        advance(at + 20000); // Explicit CPU submission overhead, deterministic.
    }
    while (!gpu.empty()) { advance(gpu.front().ready); complete(); poll(); }
    while (!pendingFeedback.empty()) { advance(pendingFeedback.front().observed); poll(); }
    if (trace) { trace->add(Event::Stop, 0, at); require(trace->save(o.capture), "cannot save capture"); }
    const auto score = smoothness.sessionReport();
    const auto actual = deadlines.report();
    long double sum = 0;
    for (auto value : latency) sum += value;
    std::cout << std::setprecision(10)
        << "{\"mode\":\"counterfactual-simulation\",\"seed\":" << o.seed << ",\"input_frames\":" << input.size()
        << ",\"displayed\":" << displayed << ",\"dropped\":" << dropped
        << ",\"smoothness_percent\":";
    if (score.available) std::cout << score.percent; else std::cout << "null";
    std::cout << ",\"feedback_coverage_percent\":" << score.coverage
        << ",\"latency_mean_ms\":" << (latency.empty() ? 0 : double(sum / latency.size() / 1e6))
        << ",\"latency_p95_ms\":" << percentile(latency, 95) << ",\"latency_p99_ms\":" << percentile(latency, 99)
        << ",\"learned_deadline_error_ms\":" << controller.reserve().common() / 1e6
        << ",\"reserve_target_ms\":" << controller.bufferTarget() / 1e6
        << ",\"buffer_coverage_percent\":" << controller.reserve().coverage()
        << ",\"buffer_measured_frames\":" << controller.reserve().validationFrames()
        << ",\"buffer_history_seconds\":" << controller.reserve().duration() / double(Second)
        << ",\"workload_overloaded\":" << (controller.workloadOverloaded() ? "true" : "false")
        << ",\"presentation_deadline_coverage_percent\":" << actual.coverage
        << ",\"presentation_deadline_misses\":" << actual.misses
        << ",\"prediction_error_p99_ms\":" << percentile(errors, 99)
        << ",\"hitches\":" << score.hitches << ",\"worst_hold_ms\":" << double(score.worstHold) / 1e6
        << ",\"recent_cadence_error_p99_ms\":" << double(score.p99Error) / 1e6
        << ",\"lower_range_misses\":" << lowerMisses << ",\"tracking_decisions\":" << tracking
        << ",\"virtual_seconds\":" << double(at - input.front().frame.decoded) / Second << "}\n";
    return 0;
}

int main(int argc, char** argv)
{
    try {
        require(argc >= 2, "usage: vrr-lab replay CAPTURE | simulate [--input CAPTURE] [--config FILE] [--seed N] [--frames N] [--capture FILE]");
        const std::string mode = argv[1];
        if (mode == "replay") { require(argc == 3, "replay requires exactly one capture"); return replay(argv[2]); }
        if (mode == "measure") { require(argc == 3, "measure requires exactly one capture"); return measure(argv[2]); }
        require(mode == "simulate", "unknown mode");
        Options o;
        for (int i = 2; i < argc; i += 2) {
            require(i + 1 < argc, "option missing value");
            const std::string key = argv[i], value = argv[i + 1];
            if (key == "--input") o.input = value;
            else if (key == "--capture") o.capture = value;
            else if (key == "--config") o.config = value;
            else {
                size_t end = 0;
                const double number = std::stod(value, &end);
                require(end == value.size() && std::isfinite(number) && number >= 0 && number <= 1000000000, "invalid numeric option");
                if (key == "--seed") o.seed = uint64_t(number);
                else if (key == "--frames") o.frames = size_t(number);
                else if (key == "--skip") o.skip = size_t(number);
                else if (key == "--limit") o.limit = size_t(number);
                else if (key == "--source-hz") o.sourceHz = number;
                else if (key == "--display-hz") o.displayHz = number;
                else if (key == "--floor-hz") o.floorHz = number;
                else if (key == "--stall-us") o.stall = Ns(number * 1000);
                else if (key == "--jitter-us") o.jitter = Ns(number * 1000);
                else if (key == "--initial-render-us") o.initialRender = Ns(number * 1000);
                else if (key == "--render-us") o.render = Ns(number * 1000);
                else if (key == "--compositor-us") o.compositor = Ns(number * 1000);
                else if (key == "--feedback-us") o.feedback = Ns(number * 1000);
                else if (key == "--wake-us") o.wake = Ns(number * 1000);
                else if (key == "--host-variance-us") o.hostVariance = Ns(number * 1000);
                else if (key == "--render-scale") o.renderScale = number;
                else if (key == "--swapchain-images") {
                    require(number >= 2 && number <= 8 && number == std::floor(number), "swapchain-images must be an integer from 2 to 8");
                    o.swapchainImages = size_t(number);
                }
                else if (key == "--feedback-loss-percent") o.feedbackLoss = unsigned(number);
                else if (key == "--fixed-display") o.fixedDisplay = number != 0;
                else throw std::runtime_error("unknown option: " + key);
            }
        }
        require(o.frames >= 1 && o.frames <= 10000000 && o.sourceHz >= 1 && o.sourceHz <= 1000 &&
            o.displayHz >= 1 && o.displayHz <= 1000 && o.floorHz < o.displayHz && o.feedbackLoss <= 100 &&
            o.jitter <= Second && o.render <= Second && o.compositor <= Second && o.feedback <= Second && o.wake <= Second &&
            o.hostVariance < Second / o.sourceHz && o.renderScale >= 0 && o.renderScale <= 10,
            "option outside supported range");
        return simulate(o);
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
