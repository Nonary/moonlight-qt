#include "timing.h"

namespace Vrr {
Controller::Controller(Config config) : m_Config(config)
{
    m_Config.minInterval = std::clamp(config.minInterval, Second / 1000, Second);
    if (m_Config.maxInterval <= m_Config.minInterval) m_Config.maxInterval = 0;
    m_Config.maxBuffer = std::clamp(config.maxBuffer, Millisecond, 100 * Millisecond);
    m_Config.guard = std::clamp(config.guard, Ns(50000), m_Config.minInterval / 4);
    m_Config.feedbackTimeout = std::clamp(config.feedbackTimeout, 20 * Millisecond, Second);
    m_Config.minBuffer = std::clamp(config.minBuffer, Ns(0), m_Config.maxBuffer);
    m_Config.bufferAttack = std::clamp(config.bufferAttack, Ns(1000), 5 * Millisecond);
    m_Config.bufferRelease = std::clamp(config.bufferRelease, Ns(1000), Millisecond);
    m_Config.initialRender = std::clamp(config.initialRender, Ns(100000), 50 * Millisecond);
    m_Config.jitterPercentile = std::clamp(config.jitterPercentile, 50u, 100u);
    m_Config.renderPercentile = std::clamp(config.renderPercentile, 50u, 100u);
    m_Config.cadenceSlew = std::clamp(config.cadenceSlew, Ns(1000), Millisecond);
    m_Config.smoothingDelay = std::clamp(config.smoothingDelay, Ns(0), m_Config.maxBuffer);
    m_Config.reserveMax = std::clamp(config.reserveMax, Ns(0), m_Config.maxBuffer);
    m_Config.reserveBoost = std::clamp(config.reserveBoost, Ns(0), m_Config.reserveMax);
    m_Config.frameInterval = std::clamp(config.frameInterval, Second / 1000, Second);
}

void Controller::reset()
{
    *this = Controller(m_Config);
}

void Controller::arrive(Frame& f)
{
    m_RecoveryFlags = 0;
    const auto ticks = int64_t(int32_t(f.rtp - m_LastRtp));
    const Ns decodedGap = f.decoded - m_LastDecoded;
    const bool epoch = !m_LastDecoded || ticks <= 0 || ticks > 90000 ||
        decodedGap <= 0 || decodedGap > Second;
    uint64_t steps = f.id > m_LastFrameId ? f.id - m_LastFrameId : 1;
    steps = std::min<uint64_t>(steps, 1000);
    if (!epoch && steps > 1) m_RecoveryFlags |= FramesSkipped;
    bool sourceDiscontinuity = false, rateChanged = false;
    if (epoch) {
        m_Transit = {};
        m_Periods = {};
        m_Source = 0;
        m_SmoothedSource = m_SmoothedPeriod = m_CadencePeriod = m_RateCandidate = 0;
        m_Buffer = m_Config.adaptiveReserve ? std::clamp(bufferTarget(),
            m_Config.minBuffer, m_Config.maxBuffer) : Millisecond;
        m_RateCandidateCount = m_BurstRemaining = m_ClockLateFrames = 0;
        m_RecoveryFlags |= NewEpoch;
    }
    else {
        m_Source += ticks;
        const Ns interval = ticks * Second / 90000 / Ns(steps);
        const Ns previousPeriod = m_SmoothedPeriod;
        sourceDiscontinuity = m_Periods.count >= 2 && previousPeriod &&
            (interval > previousPeriod * 5 / 2 || interval < previousPeriod / 2);
        if (sourceDiscontinuity) {
            m_RecoveryFlags |= SourceDiscontinuity;
            // One stall must not become the new cadence. Three consistent
            // outliers, however, can be a genuine sustained frame-rate change.
            if (m_RateCandidate && std::abs(interval - m_RateCandidate) <= m_RateCandidate / 5)
                ++m_RateCandidateCount;
            else { m_RateCandidate = interval; m_RateCandidateCount = 1; }
            if (m_RateCandidateCount >= 3) {
                m_Periods = {};
                m_SmoothedPeriod = interval;
                m_RateCandidateCount = 0;
                rateChanged = true;
                m_RecoveryFlags |= RateChanged;
            }
        }
        else m_RateCandidateCount = 0;
        if (!sourceDiscontinuity || rateChanged) {
            m_Periods.add(interval);
            auto periods = m_Periods.values;
            std::sort(periods.begin(), periods.begin() + m_Periods.count);
            // A trimmed mean preserves alternating host cadence without letting
            // one extreme interval poison the period for the next several frames.
            const size_t trim = m_Periods.count / 8;
            Ns sum = 0;
            for (size_t i = trim; i < m_Periods.count - trim; ++i) sum += periods[i];
            m_SmoothedPeriod = sum / Ns(m_Periods.count - 2 * trim);
        }
        const Ns stall = std::max(25 * Millisecond, previousPeriod * 5 / 2);
        if (decodedGap > stall) {
            m_BurstRemaining = unsigned(std::min<Ns>(32, decodedGap /
                std::max(Millisecond, previousPeriod ? previousPeriod : interval)));
            m_RecoveryFlags |= ArrivalStall;
        }
        else if (m_BurstRemaining) {
            --m_BurstRemaining;
            m_RecoveryFlags |= BurstExcluded;
        }
    }
    f.source = m_Source / 90000 * Second + m_Source % 90000 * Second / 90000;
    if (m_Config.smoothCadence && !epoch) {
        if (sourceDiscontinuity || rateChanged || steps > 1 ||
            (m_RecoveryFlags & (ArrivalStall | BurstExcluded))) {
            m_SmoothedSource = f.source;
            m_CadencePeriod = m_SmoothedPeriod;
            m_RecoveryFlags |= PhaseRestarted;
        }
        else {
            // VRR13's damped source clock: 10% interval tracking, 20% phase
            // correction. The phase cap bounds lag; it is not an additive buffer.
            const Ns interval = ticks * Second / 90000 / Ns(steps);
            if (!m_CadencePeriod) m_CadencePeriod = m_SmoothedPeriod;
            m_CadencePeriod += std::clamp((interval - m_CadencePeriod) / 10,
                -m_Config.cadenceSlew, m_Config.cadenceSlew);
            const Ns predicted = m_SmoothedSource + m_CadencePeriod * Ns(steps);
            m_SmoothedSource = predicted + (f.source - predicted) / 5;
            m_SmoothedSource = std::clamp(m_SmoothedSource,
                f.source - std::min(m_Buffer, m_Config.smoothingDelay),
                f.source + m_Config.smoothingDelay);
        }
    }
    const Ns transit = f.decoded - f.source;
    // A confirmed cadence transition can also change the source timestamp
    // phase. Keeping the old minimum then labels the same offset as jitter on
    // every steady frame. Re-anchor once, rather than buffering that offset.
    if (rateChanged) {
        m_Transit = {};
        m_ClockLateFrames = 0;
        m_RecoveryFlags |= ClockRestarted;
    }
    const Ns baselineBefore = m_Transit.percentile(0, transit);
    const bool exclude = sourceDiscontinuity || (m_RecoveryFlags & (ArrivalStall | BurstExcluded));
    // A timestamp/route phase shift needs a new clock mapping, while isolated
    // stalls and their queued followers must not inflate ordinary jitter.
    if (!exclude && transit > baselineBefore + m_Config.maxBuffer) ++m_ClockLateFrames;
    else m_ClockLateFrames = 0;
    if (transit < baselineBefore - std::max(25 * Millisecond, 2 * m_SmoothedPeriod) ||
        m_ClockLateFrames >= 3) {
        m_Transit = {};
        m_ClockLateFrames = 0;
        m_RecoveryFlags |= ClockRestarted;
    }
    if (!m_Transit.count || !exclude) m_Transit.add(transit);
    const Ns baseline = m_Transit.percentile(0);
    // Source cadence and intentional smoothing phase are not transport jitter.
    m_TransitResidual = std::max<Ns>(0, transit - baseline);
    const Ns learned = m_Config.adaptiveReserve ?
        std::min(m_Config.reserveMax, bufferTarget() + m_Config.guard) : 0;
    const Ns capacity = bufferLimit();
    const Ns wanted = std::clamp(std::max(learned,
                                 m_Config.adaptiveReserve ? Ns(0) :
                                     m_Transit.percentile(m_Config.jitterPercentile) - baseline + m_Config.guard),
                                 std::min(m_Config.minBuffer, capacity), capacity);
    const bool release = !m_Config.adaptiveReserve ||
        m_Reserve.canRelease();
    // Equal wall-clock release at all source rates. Do not turn a recovery
    // gap or skipped frames into permission for an abrupt latency correction.
    const Ns releaseElapsed = epoch || (m_RecoveryFlags & (ArrivalStall | FramesSkipped)) ? 0 :
        std::clamp(decodedGap, Ns(0), 100 * Millisecond);
    const Ns releaseStep = m_Config.bufferRelease * releaseElapsed / m_Config.minInterval;
    m_Buffer += std::clamp(wanted - m_Buffer, release ? -releaseStep : Ns(0), m_Config.bufferAttack);
    // Hard queue-capacity correction is not an ordinary latency optimization:
    // retaining an impossible reserve until confidence returns discards frames.
    m_Buffer = std::min(m_Buffer, capacity);
    f.playout = (m_Config.smoothCadence ? m_SmoothedSource : f.source) + baseline + m_Buffer;
    m_LastDecoded = f.decoded;
    m_LastRtp = f.rtp;
    m_LastFrameId = f.id;
}

Ns Controller::bufferLimit() const
{
    const Ns period = m_SmoothedPeriod ? m_SmoothedPeriod : m_Config.frameInterval;
    const Ns typical = std::clamp(m_Render.percentile(50, m_Config.initialRender), Ns(0), period);
    // Leave a queued slot for the next arrival and budget typical preparation
    // too. A three-frame queue cannot implement a 100 ms reserve at 120 FPS.
    return std::min(m_Config.maxBuffer, Ns(QueueFrames - 1) * period - typical);
}

Plan Controller::plan(const Frame& f, Ns at) const
{
    Plan p;
    p.buffer = m_Buffer;
    p.renderBudget = std::clamp(m_Render.percentile(m_Config.renderPercentile, m_Config.initialRender),
        Ns(100000), 100 * Millisecond);
    p.compositorLead = m_Latency.percentile(50);
    // The upper render quantile controls when to START work, not a mandatory
    // wait after every decode. Use typical render cost for the playout offset;
    // actual GPU readiness below can move a late frame forward when necessary.
    const Ns renderOffset = std::clamp(m_Render.percentile(50, m_Config.initialRender), Ns(100000), 50 * Millisecond);
    p.target = f.playout + renderOffset + p.compositorLead;
    p = constrain(p, at, p.renderBudget);
    // Commit the first feasible plan, including the display floor already
    // known at admission. Later preparation/feedback cannot rewrite this goal.
    p.deadline = p.target;
    return p;
}

Plan Controller::prepared(Plan p, Ns at) const
{
    p.compositorLead = m_Latency.percentile(50);
    return constrain(p, at, 0);
}

Plan Controller::constrain(Plan p, Ns at, Ns render) const
{
    const bool fresh = m_LastScanout && at >= m_LastScanout &&
        at - m_LastScanout <= m_Config.feedbackTimeout && at >= m_LastObserved &&
        at - m_LastObserved <= m_Config.feedbackTimeout;
    p.mode = fresh ? Mode::Tracking : (m_LastScanout ? Mode::Stale : Mode::Acquiring);
    p.uncertainty = m_Config.guard + m_ClockUncertainty + m_Wake.percentile(99) +
        (m_Latency.percentile(95) - m_Latency.percentile(5));
    // Software completion timestamps are useful observations, but not precise
    // hardware scanout anchors. Preserve this uncertainty in the decision.
    if (m_Quality != Quality::Hardware) p.uncertainty += Millisecond;
    // Presentation latency varies with compositor phase and queue position.
    // Its spread is uncertainty, not an additional physical refresh interval.
    // Adding median-minus-low-tail here can turn 120 Hz into ~60 Hz when
    // feedback spans two compositor phases. The swapchain enforces scanout's
    // physical minimum; retain the measured spread in diagnostics above.
    const Ns timingGuard = m_Config.guard + m_ClockUncertainty + m_Wake.percentile(99);
    p.earliest = fresh ? m_LastScanout + m_Config.minInterval + timingGuard : 0;
    p.latest = fresh && m_Config.maxInterval ? m_LastScanout + m_Config.maxInterval - timingGuard -
        (m_Latency.percentile(95) - p.compositorLead) : 0;
    p.target = std::max({p.target, p.earliest, at + p.compositorLead});
    p.submit = p.target - p.compositorLead;
    // Bound unobserved outstanding work too. Feedback can lag or disappear;
    // a stale anchor must never release a burst into the swapchain.
    if (m_LastSubmit && !fresh) p.submit = std::max(p.submit, m_LastSubmit + m_Config.minInterval + m_Config.guard);
    p.target = p.submit + p.compositorLead;
    // GPU work can consume the playout window. Do not idle on the CPU
    // until just the render-tail allowance remains.
    p.prepare = std::max(at, p.submit - render - (render ? p.buffer : 0));
    p.belowRange = p.latest && p.target > p.latest;
    return p;
}

RenderProbe Controller::renderProbe() const
{
    return {m_LastDecoded, m_TransitResidual, m_Buffer,
        std::clamp(m_Render.percentile(50, m_Config.initialRender), Ns(100000), 50 * Millisecond),
        m_SmoothedPeriod ? m_SmoothedPeriod : m_Config.frameInterval, m_RecoveryFlags};
}

void Controller::renderCost(Ns cost, Ns observed, Ns schedulingDelay)
{
    const auto probe = renderProbe();
    renderCost(probe, cost, observed ? observed : probe.decoded + cost, schedulingDelay);
}

void Controller::renderCost(const RenderProbe& probe, Ns cost, Ns observed, Ns schedulingDelay)
{
    if (cost < 0 || cost > Second || schedulingDelay < 0 || schedulingDelay > Second) return;
    if (probe.recovery & UncertainCompletion) {
        // A descheduled observer supplies a loose upper bound, not evidence
        // that rendering itself got slower. Do not cache that scheduling delay.
        m_UnpacedReady = probe.decoded;
        m_ExpectedReady = probe.decoded - probe.residual;
        clearWorkEpisode();
        return;
    }
    const bool recovery = probe.recovery &
        (NewEpoch | SourceDiscontinuity | ArrivalStall | BurstExcluded | ClockRestarted | FramesSkipped);
    const bool exceptional = cost > std::max({25 * Millisecond, 3 * probe.typical, 2 * probe.period});
    const Ns expectedDecode = probe.decoded - probe.residual;
    if (recovery || exceptional) {
        m_UnpacedReady = probe.decoded; m_ExpectedReady = expectedDecode;
        clearWorkEpisode();
    }
    if ((probe.recovery & NewEpoch) && exceptional) return;
    const Ns backlog = std::max<Ns>(0, m_UnpacedReady - probe.decoded);
    if (!backlog) finishWorkEpisode();
    if (!recovery && !exceptional) {
        const Ns expectedBacklog = m_WorkloadOverloaded ? 0 : std::max<Ns>(0, m_ExpectedReady - expectedDecode);
        m_ExpectedReady = expectedDecode + std::min(Second, expectedBacklog + probe.typical);
        m_UnpacedReady = probe.decoded + std::min(Second,
            (m_WorkloadOverloaded ? 0 : backlog) + schedulingDelay + cost);
    }
    if (m_Config.adaptiveReserve && probe.decoded && !recovery && !exceptional && !m_WorkloadOverloaded) {
        // Normal work queued by bursty game timestamps is already part of the
        // expected schedule. Only additional receiver-side delay needs reserve.
        // Keep raw error history: current FPS can change the available recovery
        // room without waiting five minutes for old samples to expire. Score
        // this observation using its own original headroom and applied buffer.
        const Ns required = std::max<Ns>(0, m_UnpacedReady - m_ExpectedReady);
        const Ns headroom = schedulingHeadroom(probe.period, probe.typical);
        if (backlog || m_WorkCount || schedulingDelay + cost > probe.period) {
            if (m_WorkCount == m_WorkSamples.size()) {
                clearWorkEpisode(); m_WorkloadOverloaded = true;
                m_UnpacedReady = probe.decoded + schedulingDelay + cost;
            }
            else m_WorkSamples[m_WorkCount++] = {required, probe.applied, observed, headroom};
        }
        else m_Reserve.observe(required, probe.applied + headroom, observed, m_Config.guard + headroom);
    }
    // The preparation lead is learned independently from observed completion.
    // Delayed results use their original frame's probe, never the current frame.
    m_Render.add(cost);
}

void Controller::clearWorkEpisode()
{
    m_WorkCount = 0;
    m_WorkloadOverloaded = false;
}

void Controller::finishWorkEpisode()
{
    // An actual idle gap demonstrates that the demand was temporary. Preserve
    // each frame and its original time, including all sub-threshold successes.
    for (size_t i = 0; i < m_WorkCount; ++i) {
        const auto& s = m_WorkSamples[i];
        m_Reserve.observe(s.required, s.applied + s.headroom, s.observed, m_Config.guard + s.headroom);
    }
    clearWorkEpisode();
}

void Controller::wakeError(Ns error)
{
    if (error >= 0 && error <= 50 * Millisecond) m_Wake.add(error);
}

void Controller::submitted(uint64_t id, Ns at, Ns target, Ns ready)
{
    m_LastSubmit = at;
    m_Pending[m_NextPending] = {id, at, target, ready < 0 ? at : ready, 0};
    m_NextPending = (m_NextPending + 1) % m_Pending.size();
}

void Controller::learnLatency(Pending& p)
{
    if (!p.ready || !p.presented) return;
    // Async submission may precede image completion. Its GPU wait belongs to
    // preparation, not compositor delay. An observation made after scanout
    // cannot measure this component and must not manufacture a zero sample.
    if (p.ready <= p.presented) m_Latency.add(p.presented - std::max(p.submitted, p.ready));
    p = {};
}

void Controller::gpuReady(uint64_t id, Ns at)
{
    for (auto& p : m_Pending) if (id && p.id == id) {
        p.ready = at; learnLatency(p); return;
    }
}

bool Controller::feedback(const Feedback& f)
{
    if (f.outcome == Outcome::Reset) {
        m_LastScanout = m_LastObserved = m_ClockUncertainty = 0;
        m_LastSequence = m_Output = 0;
        m_Quality = Quality::Unavailable;
        m_Latency = {};
        m_Pending = {};
        return false;
    }
    Pending* pending = nullptr;
    for (auto& p : m_Pending) if (f.id && p.id == f.id) { pending = &p; break; }
    if (pending && pending->presented) return false;
    if (f.outcome != Outcome::Presented) {
        if (pending) *pending = {};
        return false;
    }
    if (f.quality == Quality::Unavailable || f.uncertainty < 0 ||
        f.uncertainty > 5 * Millisecond || f.presented <= 0 ||
        f.observed < f.presented || f.observed - f.presented > m_Config.feedbackTimeout ||
        (pending && f.presented < pending->submitted)) return false;
    if (m_Output && f.output && m_Output != f.output) {
        // Output migration invalidates phase, refresh sequence and learned compositor cost.
        m_LastScanout = 0;
        m_LastSequence = 0;
        m_Latency = {};
    }
    if (f.presented < m_LastScanout ||
        (f.sequence && m_LastSequence && f.sequence < m_LastSequence) ||
        (f.presented == m_LastScanout && !pending)) return false;
    if (pending) {
        pending->presented = f.presented;
        learnLatency(*pending);
    }
    m_LastScanout = f.presented;
    m_LastObserved = f.observed;
    m_ClockUncertainty = f.uncertainty;
    m_LastSequence = f.sequence;
    m_Output = f.output;
    m_Quality = f.quality;
    return true;
}

std::vector<int64_t> Controller::checkpoint() const
{
    std::vector<int64_t> result{17, m_Config.minInterval, m_Config.maxInterval, m_Config.maxBuffer,
        m_Config.guard, m_Config.feedbackTimeout, int64_t(m_NextPending), m_LastRtp,
        m_Source, m_LastDecoded, m_Buffer, m_LastSubmit, m_LastScanout, m_LastObserved,
        m_ClockUncertainty, int64_t(m_Output), int64_t(m_LastSequence), int64_t(m_Quality)};
    for (const auto* samples : {&m_Transit, &m_Render, &m_Latency, &m_Wake}) {
        result.push_back(int64_t(samples->count));
        result.push_back(int64_t(samples->next));
        result.insert(result.end(), samples->values.begin(), samples->values.end());
    }
    for (const auto& p : m_Pending) {
        result.insert(result.end(), {int64_t(p.id), p.submitted, p.target, p.ready, p.presented});
    }
    result.insert(result.end(), {m_Config.minBuffer, m_Config.bufferAttack, m_Config.bufferRelease,
        m_Config.initialRender, m_Config.jitterPercentile, m_Config.renderPercentile,
        m_Config.smoothCadence, m_Config.cadenceSlew, m_Config.smoothingDelay, m_SmoothedSource, m_SmoothedPeriod});
    result.insert(result.end(), {int64_t(m_Periods.count), int64_t(m_Periods.next)});
    result.insert(result.end(), m_Periods.values.begin(), m_Periods.values.end());
    result.insert(result.end(), {int64_t(m_LastFrameId), int64_t(m_RecoveryFlags), m_RateCandidate,
        m_RateCandidateCount, m_BurstRemaining, m_ClockLateFrames});
    result.insert(result.end(), {m_Config.adaptiveReserve, m_Config.reserveMax, m_Config.reserveBoost, m_TransitResidual, m_Config.frameInterval, m_UnpacedReady, m_CadencePeriod});
    result.insert(result.end(), {m_ExpectedReady, int64_t(m_WorkCount), m_WorkloadOverloaded});
    for (size_t i = 0; i < m_WorkCount; ++i) {
        const auto& s = m_WorkSamples[i];
        result.insert(result.end(), {s.required, s.applied, s.observed, s.headroom});
    }
    const auto reserveState = m_Reserve.checkpoint();
    result.insert(result.end(), reserveState.begin(), reserveState.end());
    return result;
}

bool Controller::restore(const std::vector<int64_t>& state)
{
    if (state.size() < 18 + 4 * 130 + 32 * 5 + 11 + 24 + 10 + Reserve::ProfileWords + 9 + Reserve::Seconds * 5 || state[0] != 17) return false;
    Controller c(Config{state[1], state[2], state[3], state[4], state[5]});
    if (c.m_Config.minInterval != state[1] || c.m_Config.maxInterval != state[2] ||
        c.m_Config.maxBuffer != state[3] || c.m_Config.guard != state[4] ||
        c.m_Config.feedbackTimeout != state[5] || state[6] < 0 || state[6] >= 32 ||
        state[7] < 0 || uint64_t(state[7]) > UINT32_MAX || state[17] < 0 || state[17] > 2) return false;
    c.m_NextPending = size_t(state[6]); c.m_LastRtp = uint32_t(state[7]);
    c.m_Source = state[8]; c.m_LastDecoded = state[9]; c.m_Buffer = state[10];
    c.m_LastSubmit = state[11]; c.m_LastScanout = state[12]; c.m_LastObserved = state[13];
    c.m_ClockUncertainty = state[14]; c.m_Output = uint64_t(state[15]);
    c.m_LastSequence = uint64_t(state[16]); c.m_Quality = Quality(state[17]);
    size_t i = 18;
    for (auto* samples : {&c.m_Transit, &c.m_Render, &c.m_Latency, &c.m_Wake}) {
        if (state[i] < 0 || state[i] > 128 || state[i + 1] < 0 || state[i + 1] >= 128) return false;
        samples->count = size_t(state[i++]); samples->next = size_t(state[i++]);
        for (auto& value : samples->values) value = state[i++];
    }
    for (auto& p : c.m_Pending) {
        p.id = uint64_t(state[i++]); p.submitted = state[i++]; p.target = state[i++];
        p.ready = state[i++]; p.presented = state[i++];
        if (p.ready < 0 || p.presented < 0 || (p.presented && p.presented < p.submitted)) return false;
    }
    c.m_Config.minBuffer = state[i++]; c.m_Config.bufferAttack = state[i++];
    c.m_Config.bufferRelease = state[i++]; c.m_Config.initialRender = state[i++];
    c.m_Config.jitterPercentile = unsigned(state[i++]); c.m_Config.renderPercentile = unsigned(state[i++]);
    if (state[i] != 0 && state[i] != 1) return false;
    c.m_Config.smoothCadence = bool(state[i++]); c.m_Config.cadenceSlew = state[i++];
    c.m_Config.smoothingDelay = state[i++]; c.m_SmoothedSource = state[i++]; c.m_SmoothedPeriod = state[i++];
    const auto validated = Controller(c.m_Config).config();
    if (validated.minBuffer != c.m_Config.minBuffer || validated.bufferAttack != c.m_Config.bufferAttack ||
        validated.bufferRelease != c.m_Config.bufferRelease || validated.initialRender != c.m_Config.initialRender ||
        validated.jitterPercentile != c.m_Config.jitterPercentile || validated.renderPercentile != c.m_Config.renderPercentile ||
        validated.cadenceSlew != c.m_Config.cadenceSlew || validated.smoothingDelay != c.m_Config.smoothingDelay) return false;
    if (state[i] < 0 || state[i] > 16 || state[i + 1] < 0 || state[i + 1] >= 16) return false;
    c.m_Periods.count = size_t(state[i++]); c.m_Periods.next = size_t(state[i++]);
    for (auto& value : c.m_Periods.values) value = state[i++];
    c.m_LastFrameId = uint64_t(state[i++]); c.m_RecoveryFlags = uint64_t(state[i++]);
    c.m_RateCandidate = state[i++];
    if (state[i] < 0 || state[i] > 2 || state[i + 1] < 0 || state[i + 1] > 32 ||
        state[i + 2] < 0 || state[i + 2] > 2) return false;
    c.m_RateCandidateCount = unsigned(state[i++]); c.m_BurstRemaining = unsigned(state[i++]);
    c.m_ClockLateFrames = unsigned(state[i++]);
    if (state[i] != 0 && state[i] != 1) return false;
    c.m_Config.adaptiveReserve = bool(state[i++]); c.m_Config.reserveMax = state[i++];
    c.m_Config.reserveBoost = state[i++]; c.m_TransitResidual = state[i++];
    c.m_Config.frameInterval = state[i++];
    c.m_UnpacedReady = state[i++];
    c.m_CadencePeriod = state[i++];
    if (c.m_CadencePeriod < 0 || c.m_CadencePeriod > Second) return false;
    if (c.m_UnpacedReady < 0 || c.m_UnpacedReady > c.m_LastDecoded + Second) return false;
    c.m_ExpectedReady = state[i++];
    if (c.m_ExpectedReady < 0 || c.m_ExpectedReady > c.m_LastDecoded + Second) return false;
    if (state[i] < 0 || size_t(state[i]) > c.m_WorkSamples.size() || (state[i + 1] != 0 && state[i + 1] != 1)) return false;
    c.m_WorkCount = size_t(state[i++]); c.m_WorkloadOverloaded = state[i++];
    if ((c.m_WorkloadOverloaded && c.m_WorkCount) ||
        state.size() < i + c.m_WorkCount * 4 + Reserve::ProfileWords + 9 + Reserve::Seconds * 5) return false;
    Ns previous = 0;
    for (size_t j = 0; j < c.m_WorkCount; ++j) {
        auto& s = c.m_WorkSamples[j];
        s.required = state[i++]; s.applied = state[i++]; s.observed = state[i++]; s.headroom = state[i++];
        if (s.required < 0 || s.required > 3 * Second || s.applied < 0 || s.applied > c.m_Config.maxBuffer ||
            s.observed <= 0 || s.observed < previous || s.headroom < 0 || s.headroom > Second) return false;
        previous = s.observed;
    }
    const auto reserveConfig = Controller(c.m_Config).config();
    if (reserveConfig.frameInterval != c.m_Config.frameInterval || reserveConfig.reserveMax != c.m_Config.reserveMax || reserveConfig.reserveBoost != c.m_Config.reserveBoost ||
        !c.m_Reserve.restore({state.begin() + i, state.end()})) return false;
    *this = c;
    return true;
}
}
