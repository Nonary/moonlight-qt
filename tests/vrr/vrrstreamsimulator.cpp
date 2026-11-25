#include "vrrstreamsimulator.h"

#include "vrrproductionsession.h"
#include "../../app/streaming/video/ffmpeg-renderers/dxgiwaitable.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrframedroppolicy.h"

#include <algorithm>
#include <optional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace VrrSim {
namespace {

using Time = uint64_t;
// Production uses zero to mean "no timestamp". Observations subtract this origin.
constexpr Time Origin = 10000000;
constexpr Time Never = std::numeric_limits<Time>::max();

// Reuse capacity across every arrival and reconnect. In particular, MSVC's
// std::deque allocates one block per element for elements larger than 16 bytes.
template<class T>
class Fifo {
public:
    size_t size() const { return count; }
    const T& operator[](size_t i) const { return slots[(head + i) & (slots.size() - 1)]; }
    void clear() { head = count = 0; }
    void push(T value) {
        if (count == slots.size()) {
            std::vector<T> grown(std::max<size_t>(16, count * 2));
            for (size_t i = 0; i < count; ++i) grown[i] = (*this)[i];
            slots.swap(grown);
            head = 0;
        }
        slots[(head + count++) & (slots.size() - 1)] = std::move(value);
    }
    T pop() {
        auto front = slots[head];
        head = (head + 1) & (slots.size() - 1);
        --count;
        return front;
    }
private:
    std::vector<T> slots;
    size_t head = 0, count = 0;
};

struct Frame {
    int number = 0;
    uint32_t rtp = 0;
    Time source = 0, receive = 0, assembled = 0, decode = 0, prepare = 0;
    Time output = 0, ready = 0, dequeued = 0;
    PacedFrame paced() const {
        PacedFrame f(nullptr, number, rtp, true, output);
        f.setDeliveryTimeline(receive, assembled, output);
        f.setDecoderOutputComplete(false);
        f.noteDecodeCompletedUs(ready);
        return f;
    }
};
struct Flip {
    Time at;
    Observation snapshot;
};

struct Replacement {
    uint64_t frame, session;
    Time sourceUs, confirmedUs, prepareUs;
};

class Simulation {
public:
    Simulation(const Scenario& scenario, const Options& options, PresentObserver observer, void* context,
               std::vector<Replacement>* replacements = nullptr) :
        scenario(scenario), options(options), observer(observer), context(context),
        replacements(replacements),
        waitableDxgi(scenario.dxgiWaitable && options.canLatch), rng(scenario.seed),
        config(vrrProductionSession(options.latencyMode, scenario.streamHz, scenario.displayHz, options.smooth))
    {
        if (options.targetHundredths) {
            config.timingOptions.targetHundredths = options.targetHundredths;
            config = vrrProductionInputConfig(std::move(config));
        }
        params = vrrTimingParametersForSession(config);
        if (options.fixedBufferUs >= 0) {
            // Run the ordinary observer, scheduling and learning paths. Pin the
            // actuator's entire range, including startup and source-rate bands.
            const Time fixed = Time(options.fixedBufferUs);
            params.sourcePlayoutDelayUs = params.playoutDelayMinimumUs = params.playoutDelayMaximumUs = fixed;
            params.playoutDelayStartUs = fixed;
            params.playoutDelayStartPeriodPerMille = params.playoutDelayMaximumPeriodPerMille = 0;
            // A trial may exceed the user's adaptive preset. The decoded-queue
            // budget is still real. Reject an infeasible setting at the point
            // of use rather than reporting it as a trial at the requested value.
            params.playoutDelayCapSourcePeriodPerMille = std::max(params.playoutDelayCapSourcePeriodPerMille,
                                                                   fixed * 1000);
        }
        controller = std::make_unique<VrrTimingController>(config, options.canLatch, params);
    }

    Counts run() {
        enterPhase();
        for (;;) {
            const Time delivery = network.size() ? network[0].assembled : Never;
            const Time decodeStart = compressed.size() ? std::max(now, decoderFree) : Never;
            const Time flip = pendingFlips.size() ? pendingFlips[0].at : Never;
            now = std::min({phaseEnd, nextHost, delivery, decodeStart, workerEvent, flip});
            if (now == Never) break;
            // At a shared timestamp accept a complete source/decoder batch
            // before dispatching the worker. Thus a real successor can satisfy
            // even the last instant of its replacement wait.
            while (pendingFlips.size() && pendingFlips[0].at == now) displayFlip();
            if (phaseEnd == now) enterPhase();
            if (nextHost == now) hostFrame();
            while (network.size() && network[0].assembled == now) {
                const auto arrived = network.pop();
                // VideoDepacketizer rejects the overflowing unit and flushes
                // all waiting units. Each generated picture is independently
                // decodable, so the following unit can begin recovery.
                if (compressed.size() == 15) {
                    counts.decodeDrops += compressed.size() + 1;
                    compressed.clear();
                }
                else compressed.push(arrived);
            }
            while (compressed.size() && decoderFree <= now) decodeFrame();
            if (state == State::Replacing && decoded.size()) workerEvent = now;
            if (state == State::Idle && decoded.size()) dequeue();
            if (workerEvent == now) advanceWorker();
        }
        return counts;
    }

private:
    enum class State { Idle, DecodeFence, Replacing, ReservedFence, RenderStart, PresentAdmission, PrepareComplete, Submit };
    const Scenario& scenario;
    Options options;
    PresentObserver observer;
    void* context;
    std::vector<Replacement>* replacements;
    const bool waitableDxgi;
    uint32_t rng;
    VrrSessionConfig config;
    VrrTimingParameters params;
    std::unique_ptr<VrrTimingController> controller;
    Counts counts;
    Fifo<Frame> network, compressed, decoded;
    Fifo<Flip> pendingFlips;
    Frame active, reserved;
    State state = State::Idle;
    VrrTimingDecision decision;
    Time now = Origin, phaseEnd = Origin, nextHost = Never, disturbance = Never;
    Time workerEvent = Never, preparationStarted = 0, prepared = 0;
    Time encoderFree = 0;
    Time decoderFree = 0, lastAssembly = 0, lastShown = 0, lastSource = 0;
    Time firstDecision = 0, lastDelaySample = 0;
    std::optional<Time> savedSeed;
    std::vector<Time> settledDelays;
    uint64_t session = 0;
    size_t phaseIndex = 0;
    const Phase* phase = nullptr;
    Time periodRemainder = 0, rtpRemainder = 0, framesInPhase = 0;
    uint32_t rtp = 0xFFF00000u;
    bool spikePending = false;

    Time nextPeriod() {
        if (phase->alternatingFirstUs)
            return framesInPhase % 2 ? phase->alternatingFirstUs : phase->alternatingSecondUs;
        const Time numerator = 1000000 + periodRemainder;
        periodRemainder = numerator % unsigned(phase->hostHz);
        return numerator / unsigned(phase->hostHz);
    }

    void enterPhase() {
        if (phaseIndex == scenario.phases.size()) {
            phaseEnd = nextHost = Never;
            return;
        }
        const auto& incoming = scenario.phases[phaseIndex++];
        const bool wasRunning = phase != nullptr;
        const bool cadenceChanged = wasRunning &&
            (phase->hostHz != incoming.hostHz || phase->alternatingFirstUs != incoming.alternatingFirstUs ||
             phase->alternatingSecondUs != incoming.alternatingSecondUs);
        phase = &incoming;
        phaseEnd = now + phase->durationUs;
        disturbance = now;
        spikePending = false;
        framesInPhase = 0;
        if (phase->reconnect && wasRunning) reconnect();
        if (!lastSource) nextHost = now;
        else if (cadenceChanged) {
            periodRemainder = 0;
            nextHost = std::max(now, lastSource + nextPeriod());
        }
        // Advance the RTP clock independently of client arrival/wait times.
        // The low 32 bits wrap naturally including on a backwards jump.
        rtp += uint32_t(phase->rtpJumpUs * 9 / 100);
    }

    void reconnect() {
        auto history = controller->playoutHistory().profile();
        if (const auto seed = controller->startDelayForCache(settledDelays)) savedSeed = *seed;
        // Match the worker's same-key profile and settled-delay load. All
        // transport/decode/pacer ownership belongs to the closing session.
        controller = std::make_unique<VrrTimingController>(config, options.canLatch, params);
        controller->loadPlayoutHistory(history);
        if (savedSeed) controller->seedPlayoutDelayStart(*savedSeed);
        counts.reconnectDrops += network.size() + compressed.size() + decoded.size() + pendingFlips.size() +
                                 (state != State::Idle) + (state == State::ReservedFence);
        network.clear(); compressed.clear(); decoded.clear(); pendingFlips.clear();
        settledDelays.clear();
        state = State::Idle;
        workerEvent = Never;
        encoderFree = decoderFree = lastAssembly = lastShown = firstDecision = lastDelaySample = lastSource = 0;
        rtpRemainder = periodRemainder = 0;
        rtp = 0xFFF00000u;
        ++session;
    }

    void hostFrame() {
        if (!spikePending && disturbance <= now) {
            disturbance = phase->disturbancePeriodUs ?
                disturbance + ((now - disturbance) / phase->disturbancePeriodUs + 1) * phase->disturbancePeriodUs : Never;
            spikePending = true;
            if (phase->hostPauseUs) {
                nextHost += phase->hostPauseUs;
                return;
            }
        }
        Frame frame;
        frame.number = int(++counts.produced);
        const Time previousSource = lastSource;
        if (lastSource) {
            const Time ticks = (now - lastSource) * (1000000 + phase->clockDriftPpm) * 9 + rtpRemainder;
            rtp += uint32_t(ticks / 100000000);
            rtpRemainder = ticks % 100000000;
        }
        lastSource = frame.source = now;
        frame.rtp = rtp;
        // Exactly one random draw per source frame, independent of drops or
        // controller decisions. Jitter is uniform additional transport delay.
        rng = 1664525u * rng + 1013904223u;
        const Time transportUs = phase->networkDelayUs + (uint64_t(rng) % (phase->jitterUs + 1)) +
                                 (spikePending ? phase->networkSpikeUs : 0);
        // Serial encoder service is independent of the capture clock. A slow
        // encode queues later captures, which then send one service time apart
        // until the encoder catches up. Transport starts at the send clock.
        const Time previousSend = encoderFree;
        const Time encodeStart = std::max(now, encoderFree);
        const Time encodeUs = phase->encodeUs + (spikePending ? phase->encoderStallUs : 0);
        if (encodeStart >= Never - encodeUs - transportUs)
            throw std::invalid_argument("encoder backlog exceeds the virtual clock range");
        encoderFree = encodeStart + encodeUs;
        counts.maxCaptureToSendUs = std::max(counts.maxCaptureToSendUs, encoderFree - now);
        if (previousSend && encoderFree - previousSend < now - previousSource)
            ++counts.compressedHostSendIntervals;
        frame.receive = encoderFree + transportUs;
        // Access units leave reassembly in source order, including a burst of
        // several completed units behind a single delayed predecessor.
        frame.assembled = lastAssembly = std::max(lastAssembly, frame.receive);
        frame.decode = phase->decodeUs;
        frame.prepare = phase->prepareUs;
        network.push(frame);
        spikePending = false;
        ++framesInPhase;
        nextHost = now + nextPeriod();
    }

    void decodeFrame() {
        auto frame = compressed.pop();
        frame.output = now;
        frame.ready = decoderFree = now + frame.decode;
        if (decoded.size() == controller->queuedFrameCapacity()) {
            decoded.pop();
            ++counts.queueDrops;
        }
        decoded.push(frame);
    }

    void dequeue() {
        const Time protection = controller->playoutDelayUs() +
            std::min(controller->trajectoryStats().carryUs, controller->sourcePeriodUs());
        while (decoded.size() > 1 && VrrFrameDropPolicy::beforeDecodeWait(decoded[0].paced(), decoded[1].paced(),
                decoded[0].output, now, controller->sourcePeriodUs(), protection)) {
            decoded.pop();
            ++counts.queueDrops;
        }
        active = decoded.pop();
        active.dequeued = now;
        state = State::DecodeFence;
        workerEvent = std::max(now, active.ready);
    }

    Time protectedDelay() const {
        return decision.playoutDelayUs + std::min(decision.trajectory.carryUs, decision.sourcePeriodUs);
    }

    void finish() {
        state = State::Idle;
        workerEvent = decoded.size() ? now : Never;
    }

    void beforeRender() {
        if (decoded.size() && VrrFrameDropPolicy::beforeRender(decision, controller->displayPeriodUs(),
                now - active.output - (std::max(active.dequeued, active.ready) - active.dequeued),
                controller->latencyFixActive(), protectedDelay(), params.playoutLateRecovery != 0)) {
            controller->noteSubmission(false, false, 0);
            ++counts.queueDrops;
            finish();
        }
        else {
            state = State::RenderStart;
            workerEvent = std::max(now, decision.renderStartUs);
        }
    }

    void advanceWorker() {
        switch (state) {
        case State::Idle:
            workerEvent = Never;
            break;
        case State::DecodeFence: {
            auto frame = active.paced();
            frame.noteDecodeSyncWaitUs(now - active.dequeued);
            // observeDecodeReadiness returns both the exact fence and the
            // collection bound. The exact timestamp wins, as in the worker.
            frame.noteGpuReadyUs(now);
            decision = controller->schedule(frame, now);
            if (options.fixedBufferUs >= 0 && decision.playoutDelayUs != uint64_t(options.fixedBufferUs))
                throw FixedBufferUnavailable("fixed buffer exceeds the controller's queue budget");
            if (!firstDecision) firstDecision = now;
            if (now - firstDecision >= 30000000 && now - lastDelaySample >= 1000000 && settledDelays.size() < 36000) {
                lastDelaySample = now;
                settledDelays.push_back(decision.playoutDelayUs);
            }
            if (decision.trajectory.resyncRecommended) {
                const auto wait = decision.replacementWaitUs ? decision.replacementWaitUs :
                    params.playoutCeilingPhaseReplacement ? std::min<Time>(20000, 2 * decision.sourcePeriodUs) : 0;
                // This backend supplies asynchronous decoder outputs. An
                // immediate check cannot prove their decode fence. A timed
                // reservation can wait for a queue arrival, take ownership,
                // and then collect that specific fence (even after timeout).
                if (wait && !options.deferReplacements) {
                    state = State::Replacing;
                    workerEvent = decoded.size() ? now : now + ((wait + 999) / 1000) * 1000;
                }
                else {
                    controller->noteTrajectoryDeferred();
                    beforeRender();
                }
            }
            else beforeRender();
            break;
        }
        case State::Replacing:
            if (decoded.size()) {
                reserved = decoded.pop();
                reserved.dequeued = now;
                state = State::ReservedFence;
                workerEvent = std::max(now, reserved.ready);
            }
            else {
                controller->noteTrajectoryDeferred();
                beforeRender();
            }
            break;
        case State::ReservedFence:
            if (replacements)
                replacements->push_back({uint64_t(active.number), session, active.source - Origin,
                                         now - Origin, active.prepare});
            controller->noteTrajectoryResyncDrop();
            controller->noteSubmission(false, false, 0);
            ++counts.replacements;
            active = reserved;
            state = State::DecodeFence;
            workerEvent = now;
            break;
        case State::RenderStart:
            if (decoded.size() && VrrFrameDropPolicy::afterRenderWait(decision, active.output, now,
                    controller->latencyFixActive(), std::max(active.ready, active.dequeued) - active.dequeued,
                    protectedDelay())) {
                if (controller->latencyFixActive()) controller->noteSubmission(false, false, 0);
                else controller->rebase();
                ++counts.queueDrops;
                finish();
            }
            else {
                preparationStarted = now;
                // The waitable swap chain admits preparation only when a
                // display flip frees a slot. Keep the worker occupied while
                // arrivals continue through the real decoded-queue policy.
                if (waitableDxgi && pendingFlips.size() >= DxgiWaitable::MaximumFrameLatency) {
                    state = State::PresentAdmission;
                    workerEvent = std::min(pendingFlips[0].at, now + DxgiWaitable::AdmissionTimeoutUs);
                }
                else {
                    state = State::PrepareComplete;
                    workerEvent = now + active.prepare;
                }
            }
            break;
        case State::PresentAdmission:
            counts.presenterAdmissionWaitUs += now - preparationStarted;
            if (pendingFlips.size() >= DxgiWaitable::MaximumFrameLatency) {
                controller->notePreparationDuration(now - preparationStarted, 0, now);
                controller->noteSubmission(false, false, 0);
                ++counts.presenterDrops;
                finish();
            }
            else {
                state = State::PrepareComplete;
                workerEvent = now + active.prepare;
            }
            break;
        case State::PrepareComplete:
            prepared = now;
            controller->notePreparationDuration(now - preparationStarted, 0, now);
            controller->noteSchedulerDelays(0, 0, true);
            controller->noteSpacingDeficit(0);
            state = State::Submit;
            workerEvent = std::max({now, decision.targetUs, controller->earliestSubmissionUs()});
            break;
        case State::Submit:
            submit();
            finish();
            break;
        }
    }

    void displayFlip() {
        const auto flip = pendingFlips.pop();
        Vrr13::PresentationObservation feedback;
        feedback.timeKind = Vrr13::PresentationTimeKind::DisplayEvent;
        feedback.sampleValid = true;
        feedback.sampleId = flip.snapshot.frame;
        feedback.sampleTime = feedback.observed = now;
        controller->notePresentation(feedback);
        ++counts.presented;
        if (observer) observer(context, flip.snapshot);
    }

    void submit() {
        // A pending predecessor or a still-busy minimum scanout interval
        // cannot authorize an immediate flip. Model those observable parts of
        // DXGI's native guard without assuming a fixed refresh grid.
        const bool nativeLatch = waitableDxgi && params.nativeFlipProtection &&
            !decision.latchedPresentation && lastShown && now < lastShown + controller->displayPeriodUs();
        if (nativeLatch) controller->noteNativeFlipProtection(0);
        const bool latched = decision.latchedPresentation || nativeLatch;
        controller->noteSubmission(true, false, now, now);
        Vrr13::PresentationObservation feedback;
        feedback.smoothness = controller->smoothnessSample(decision);
        feedback.submitted = feedback.idValid = true;
        feedback.latched = latched;
        feedback.id = uint64_t(active.number);
        feedback.observed = feedback.submission = now;
        feedback.ready = prepared;
        feedback.deadline = decision.originalScanoutUs;
        controller->notePresentation(feedback);
        const Time shown = std::max(now, lastShown +
            (latched && lastShown ? controller->displayPeriodUs() : 0));
        lastShown = shown;
        if (waitableDxgi)
            counts.maxPendingPresents = std::max<uint64_t>(counts.maxPendingPresents, pendingFlips.size() + 1);
        Observation ob;
        ob.frame = feedback.id;
        if (!observer) {
            pendingFlips.push({shown, ob});
            return;
        }
        const auto stats = controller->intervalStats();
        ob.session = session;
        ob.sourceUs = active.source - Origin;
        ob.readyUs = active.ready - Origin;
        ob.outputUs = active.output - Origin;
        ob.dequeuedUs = active.dequeued - Origin;
        ob.submittedUs = now - Origin;
        ob.shownUs = shown - Origin;
        ob.preparationStartedUs = preparationStarted - Origin;
        ob.bufferUs = decision.playoutDelayUs;
        ob.minimumBufferUs = stats.update.minimumUs;
        ob.intendedIntervalUs = stats.update.intendedIntervalUs;
        ob.intervalErrorUs = stats.update.intervalErrorUs;
        ob.eligibleInterval = ob.intendedIntervalUs != 0;
        ob.clientEligibleInterval = stats.update.clientInterval;
        ob.calibrated = stats.initialCalibrationComplete;
        ob.scoreAvailable = stats.averageValid && stats.evaluatedUs != 0;
        ob.clientScoreAvailable = ob.scoreAvailable && stats.clientEvaluatedUs != 0;
        ob.scorePercent = stats.qualityPercent();
        ob.clientScorePercent = stats.clientQualityPercent();
        pendingFlips.push({shown, ob});
    }
};

void validate(const Scenario& scenario, const Options& options)
{
    if (scenario.streamHz <= 0 || scenario.streamHz > 1000 ||
        scenario.displayHz <= 0 || scenario.displayHz > 1000 || scenario.phases.empty() ||
        options.latencyMode < 0 || options.latencyMode > 2 || options.fixedBufferUs < -1 ||
        options.fixedBufferUs > 1000000 ||
        (options.targetHundredths && (options.targetHundredths < 9000 || options.targetHundredths > 9999)))
        throw std::invalid_argument("invalid stream scenario or policy");
    constexpr Time dayUs = 86400000000ULL;
    Time totalUs = 0, possibleFrames = 0;
    for (const auto& phase : scenario.phases) {
        if (!phase.durationUs || phase.durationUs > dayUs - totalUs ||
            phase.hostHz <= 0 || phase.hostHz > 1000 ||
            std::max({phase.jitterUs, phase.networkDelayUs, phase.decodeUs, phase.prepareUs,
                      phase.encodeUs, phase.encoderStallUs,
                      phase.alternatingFirstUs, phase.alternatingSecondUs, phase.disturbancePeriodUs,
                      phase.networkSpikeUs, phase.hostPauseUs}) > dayUs ||
            bool(phase.alternatingFirstUs) != bool(phase.alternatingSecondUs) ||
            phase.clockDriftPpm <= -1000000 || phase.clockDriftPpm >= 1000000 ||
            phase.rtpJumpUs < -int64_t(dayUs) || phase.rtpJumpUs > int64_t(dayUs))
            throw std::invalid_argument("invalid stream phase");
        totalUs += phase.durationUs;
        // PacedFrame takes an int identity. Bound all generated frames before
        // running, including a partial alternating pair at a phase boundary.
        possibleFrames += phase.alternatingFirstUs ?
            2 * (phase.durationUs / (phase.alternatingFirstUs + phase.alternatingSecondUs) + 1) :
            1 + (phase.durationUs * unsigned(phase.hostHz)) / 1000000;
        if (possibleFrames > uint64_t(std::numeric_limits<int>::max()))
            throw std::invalid_argument("stream exceeds available production frame identities");
    }
}

Time difference(Time a, Time b)
{
    return std::max(a, b) - std::min(a, b);
}

struct SourceRange {
    Time begin, end;
};

GapMetrics measureGaps(const std::vector<Observation>& timeline, const std::vector<SourceRange>& ranges)
{
    GapMetrics metrics;
    std::vector<Time> jerk;
    Time previousInterval = 0;
    for (size_t i = 1; i < timeline.size(); ++i) {
        const auto& frame = timeline[i];
        const auto& previous = timeline[i - 1];
        if (frame.session != previous.session) {
            previousInterval = 0;
            continue;
        }
        const Time interval = frame.shownUs - previous.shownUs;
        if (ranges.empty() || std::any_of(ranges.begin(), ranges.end(), [&](const SourceRange& range) {
            return range.begin <= frame.sourceUs && frame.sourceUs < range.end;
        })) {
            metrics.worstGapUs = std::max(metrics.worstGapUs, interval);
            if (previousInterval) {
                const Time change = difference(interval, previousInterval);
                jerk.push_back(change);
                metrics.worstJerkUs = std::max(metrics.worstJerkUs, change);
                metrics.jerkOver2Ms += change > 2000;
            }
        }
        previousInterval = interval;
    }
    if (!jerk.empty()) {
        const auto p99 = jerk.end() - 1 - jerk.size() / 100;
        std::nth_element(jerk.begin(), p99, jerk.end());
        metrics.jerkP99Us = *p99;
    }
    return metrics;
}

bool preservesJerk(Time before, Time gap, Time after, Time period)
{
    const Time oldLeft = difference(before, gap), oldRight = difference(gap, after);
    const Time newLeft = difference(before, gap - period);
    const Time newMiddle = difference(gap - period, period);
    const Time newRight = difference(period, after);
    return std::max({newLeft, newMiddle, newRight}) <= std::max(oldLeft, oldRight) &&
        (newLeft > 2000) + (newMiddle > 2000) + (newRight > 2000) <= (oldLeft > 2000) + (oldRight > 2000);
}

} // namespace

Counts simulate(const Scenario& scenario, const Options& options, PresentObserver observer, void* context)
{
    validate(scenario, options);
    return Simulation(scenario, options, observer, context).run();
}

GapRecoveryReference measureGapRecoveryReference(const Scenario& scenario, const Options& options)
{
    validate(scenario, options);
    if (!scenario.dxgiWaitable || !options.canLatch || scenario.streamHz != scenario.displayHz)
        throw std::invalid_argument("gap recovery reference requires a waitable presenter at matched stream and display rates");

    std::vector<Observation> baseline;
    std::vector<Replacement> replacements;
    const auto append = [](void* context, const Observation& frame) {
        static_cast<std::vector<Observation>*>(context)->push_back(frame);
    };
    const Counts counts = Simulation(scenario, options, append, &baseline, &replacements).run();
    const Time period = 1000000 / scenario.displayHz;
    std::vector<SourceRange> ranges;
    Time phaseStart = 0;
    for (const auto& phase : scenario.phases) {
        if (phase.encoderStallUs)
            ranges.push_back({phaseStart, phaseStart + phase.durationUs});
        phaseStart += phase.durationUs;
    }
    GapRecoveryReference result;
    result.baselineDrops = counts.produced - counts.presented;
    result.baselineFramesPreserved = counts.presented;
    result.baseline = measureGaps(baseline, ranges);
    std::vector<Observation> filled;
    filled.reserve(baseline.size());
    bool filledPreviousGap = false;
    for (size_t i = 0; i < baseline.size(); ++i) {
        const auto& before = baseline[i];
        filled.push_back(before);
        if (filledPreviousGap) {
            filledPreviousGap = false;
            continue;
        }
        // Score the two neighboring intervals as well. Two adjacent insertions
        // would change the same jerk sample, invalidating this local check.
        if (i == 0 || i + 2 >= baseline.size())
            continue;
        const auto& after = baseline[i + 1];
        if (before.session != baseline[i - 1].session || before.session != after.session ||
                after.session != baseline[i + 2].session || after.frame <= before.frame + 1)
            continue;
        const Time gap = after.shownUs - before.shownUs;
        const Time sourceSpan = after.sourceUs - before.sourceUs;
        const Time expectedSpan = (after.frame - before.frame) * 1000000 / scenario.displayHz;
        if (gap < 3 * period || difference(sourceSpan, expectedSpan) > 1 ||
                !preservesJerk(before.shownUs - baseline[i - 1].shownUs, gap,
                               baseline[i + 2].shownUs - after.shownUs, period))
            continue;

        const Time slot = after.shownUs - period;
        if (slot >= after.submittedUs)
            continue;
        // Use the actual replacement reservation and a render window before
        // the next baseline preparation. The prior flip is already displayed.
        // Finish the added flip before the next submission so it occupies no
        // pending-present capacity needed by the baseline.
        const Time latestFinish = std::min(slot, after.preparationStartedUs);
        auto replacement = std::lower_bound(replacements.begin(), replacements.end(), after.frame,
            [](const Replacement& drop, uint64_t frame) { return drop.frame < frame; });
        while (replacement != replacements.begin()) {
            --replacement;
            if (replacement->frame <= before.frame)
                break;
            const Time start = std::max(replacement->confirmedUs, before.shownUs);
            if (replacement->session != before.session || start > latestFinish ||
                    replacement->prepareUs > latestFinish - start)
                continue;
            Observation extra;
            extra.frame = replacement->frame;
            extra.session = replacement->session;
            extra.sourceUs = replacement->sourceUs;
            extra.shownUs = slot;
            filled.push_back(extra);
            ++result.insertedFrames;
            result.oldestInsertedFrameAgeUs = std::max(result.oldestInsertedFrameAgeUs, slot - extra.sourceUs);
            filledPreviousGap = true;
            break;
        }
    }
    result.filledDrops = result.baselineDrops - result.insertedFrames;
    result.filled = measureGaps(filled, ranges);
    return result;
}

} // namespace VrrSim
