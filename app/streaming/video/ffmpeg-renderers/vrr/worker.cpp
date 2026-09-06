#include "worker.h"
#include "profile.h"
#include <QStandardPaths>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <exception>

namespace Vrr {
Worker::Worker(IFFmpegRenderer* renderer, Config config, QString profileKey) :
    m_Renderer(renderer), m_Controller(config), m_Smoothness(config.minInterval), m_ProfileKey(std::move(profileKey))
{
    if (config.adaptiveReserve && !m_ProfileKey.isEmpty()) {
        m_ProfilePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/vrr14-delay-profiles.json";
        Reserve cached;
        if (loadProfile(m_ProfilePath, m_ProfileKey, cached)) {
            m_Controller.loadReserve(cached.profile());
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "VRR timing history loaded: tail %.3f ms, %llu frames, %u successful sessions; proven reserve %.3f ms", cached.common() / 1e6, (unsigned long long)cached.cachedEvidence(), cached.successes(), cached.provenBuffer() / 1e6);
        }
    }
    // Read once before starting the timing thread. Repeated checkpoints retain
    // this identity after ring overwrite without hashing or disk IO per frame.
#ifdef Q_OS_LINUX
    QFile executable(QStringLiteral("/proc/self/exe"));
#else
    QFile executable(QCoreApplication::applicationFilePath());
#endif
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (executable.open(QIODevice::ReadOnly) && hash.addData(&executable)) {
        const auto bytes = hash.result();
        std::array<int64_t, 4> words{};
        for (size_t i = 0; i < words.size(); ++i) {
            uint64_t word = 0;
            for (size_t j = 0; j < 8; ++j) word |= uint64_t(uint8_t(bytes[int(i * 8 + j)])) << (j * 8);
            words[i] = int64_t(word);
        }
        m_Trace.setBuildHash(words);
    }
    m_Trace.checkpoint(m_Controller, now());
}

Worker::~Worker() { stop(); }

bool Worker::start()
{
    try { m_Thread = std::thread(&Worker::run, this); }
    catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VRR thread creation failed: %s", e.what());
        return false;
    }
    return true;
}

void Worker::stop()
{
    if (!m_Thread.joinable()) return;
    m_Stopping.store(true);
    m_Ready.notify_all();
    m_Thread.join();
    const auto deadlines = m_Deadlines.report();
    if (m_Controller.config().adaptiveReserve && !m_ProfileKey.isEmpty()) {
        const bool dropped = m_LastDeliveryFailure && now() - m_LastDeliveryFailure < Reserve::Window;
        auto validation = PresentationValidation::Unavailable;
        if (dropped || (deadlines.available && deadlines.misses * 2000 > deadlines.measured))
            validation = PresentationValidation::Failed;
        else if (deadlines.available && deadlines.duration >= Reserve::Window)
            validation = PresentationValidation::Passed;
        const bool saved = saveProfile(m_ProfilePath, m_ProfileKey, m_Controller.reserve(), validation);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "VRR bufferable timing: deadline-error tail %.3f ms, 3 ms tolerance, no fixed cushion, %llu new / %llu effective frames; release %s; profile %s",
            m_Controller.reserve().common() / 1e6, (unsigned long long)m_Controller.reserve().observations(),
            (unsigned long long)m_Controller.reserve().evidence(),
            m_Controller.reserve().canRelease() ? "enabled" : "held",
            saved ? "saved" : "unchanged");
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "VRR original presentation deadlines (+/-3 ms): %.4f%%; %llu/%llu known, %llu misses; %.1f s history; %s",
        deadlines.coverage, (unsigned long long)deadlines.measured, (unsigned long long)deadlines.submitted,
        (unsigned long long)deadlines.misses, deadlines.duration / double(Second),
        deadlines.available ? "measured" : "insufficient feedback");
    const auto smoothness = m_Smoothness.report();
    const auto session = m_Smoothness.sessionReport();
    if (session.available) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "VRR session source-cadence score (diagnostic, not deadline coverage): %.4f%%; coverage %.2f%%; hitches %llu; worst hold %.3f ms; measured intervals %llu",
        session.percent, session.coverage, (unsigned long long)session.hitches, session.worstHold / 1e6,
        (unsigned long long)session.intervals);
    if (smoothness.available) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "VRR source-cadence score (diagnostic, not deadline coverage): %.2f%%; feedback coverage %.1f%%; cadence error p95/p99 %.3f/%.3f ms; worst hold %.3f ms; hitches %llu (last %llu measured intervals)",
            smoothness.percent, smoothness.coverage, smoothness.p95Error / 1e6, smoothness.p99Error / 1e6,
            smoothness.worstHold / 1e6, (unsigned long long)smoothness.hitches, (unsigned long long)smoothness.intervals);
    }
    else SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "VRR smoothness: insufficient presentation evidence (coverage %.1f%%, %llu intervals)",
        smoothness.coverage, (unsigned long long)smoothness.intervals);
    // Decoder has already stopped submitting. No file IO occurs on the timing thread.
    const auto path = qgetenv("MOONLIGHT_VRR_CAPTURE");
    if (!path.isEmpty()) {
        const bool saved = m_Trace.save(path.toStdString());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "VRR capture %s: %s (maximum 4 MiB per session)",
                    saved ? "saved" : "failed", path.constData());
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "VRR feedback: %llu presented, %llu discarded, %llu unavailable; "
                "decisions: %llu tracking, %llu acquiring/stale, %llu beyond known lower refresh limit",
                (unsigned long long)m_Presented, (unsigned long long)m_Discarded,
                (unsigned long long)m_Unavailable, (unsigned long long)m_Tracking,
                (unsigned long long)m_Predicted, (unsigned long long)m_BelowRange);
}

void Worker::submit(AVFrame* av, uint64_t receivedUs, uint64_t assembledUs)
{
    Frame frame;
    frame.decoded = now();
    const Ns liNow = Ns(LiGetMicroseconds());
    frame.received = receivedUs ? frame.decoded + (Ns(receivedUs) - liNow) * 1000 : 0;
    frame.assembled = assembledUs ? frame.decoded + (Ns(assembledUs) - liNow) * 1000 : 0;
    frame.rtp = uint32_t(av->pts);
    std::lock_guard<std::mutex> lock(m_Lock);
    frame.id = ++m_NextId;
    Queued q{av, frame};
    if (m_Stopping.load()) { drop(q, Drop::Shutdown); return; }
    // Same five-surface budget as Pacer: three queued + two GPU-owned.
    if (m_Queue.size() == QueueFrames) { drop(m_Queue.front(), Drop::Capacity); m_Queue.pop_front(); }
    m_Queue.push_back(q);
    m_Ready.notify_one();
}

void Worker::drop(Queued& q, Drop reason)
{
    // Caller owns m_Lock. Preserve input timestamps even for frames never selected.
    m_Trace.add(Event::Drop, q.timing.id, now(), {int64_t(reason), q.timing.decoded,
        q.timing.rtp, q.timing.received, q.timing.assembled, int64_t(m_Queue.size())});
    ++m_Stats.pacerDroppedFrames;
    if (reason != Drop::Shutdown) m_LastDeliveryFailure = now();
    av_frame_free(&q.frame);
}

void Worker::collectStats(VIDEO_STATS& stats)
{
    std::lock_guard<std::mutex> lock(m_Lock);
    stats.renderedFrames += m_Stats.renderedFrames;
    stats.pacerDroppedFrames += m_Stats.pacerDroppedFrames;
    stats.totalRenderTimeUs += m_Stats.totalRenderTimeUs;
    stats.totalPacerTimeUs += m_Stats.totalPacerTimeUs;
    stats.vrrActive = true;
    stats.vrrSmoothnessAvailable = m_SmoothnessReport.available;
    stats.vrrSmoothness = m_SmoothnessReport.percent;
    stats.vrrFeedbackCoverage = m_SmoothnessReport.coverage;
    stats.vrrCadenceP99Ms = m_SmoothnessReport.p99Error / 1e6;
    stats.vrrDeadlineCoverage = m_Stats.vrrDeadlineCoverage;
    stats.vrrDeadlineFeedbackCoverage = m_Stats.vrrDeadlineFeedbackCoverage;
    stats.vrrCalibrationSeconds = m_Stats.vrrCalibrationSeconds;
    stats.vrrCalibrationFrames = m_Stats.vrrCalibrationFrames;
    stats.vrrCalibrationReady = m_Stats.vrrCalibrationReady;
    stats.vrrBufferCoverage = m_Stats.vrrBufferCoverage;
    stats.vrrBufferSeconds = m_Stats.vrrBufferSeconds;
    stats.vrrBufferFrames = m_Stats.vrrBufferFrames;
    stats.vrrBufferOverloaded = m_Stats.vrrBufferOverloaded;
    // Counters are deltas; calibration remains the latest snapshot between polls.
    m_Stats.renderedFrames = m_Stats.pacerDroppedFrames = 0;
    m_Stats.totalRenderTimeUs = m_Stats.totalPacerTimeUs = 0;
}

void Worker::poll()
{
    Overlay::IOverlayRenderer::UpdateTiming overlay;
    for (int i = 0; i < Overlay::OverlayMax && m_Renderer->takeOverlayTiming(overlay); ++i) {
        m_Trace.add(Event::OverlayWork, 0, now(), {overlay.type, int64_t(overlay.revision),
            overlay.queueNs, overlay.rasterNs, overlay.dispatchNs});
    }
    Feedback f;
    // Bounded work even if a backend misbehaves. Every observation is recorded,
    // including rejected, discarded and unavailable feedback.
    for (unsigned i = 0; i < 64 && m_Renderer->pollVrr(f); ++i) {
        const bool used = m_Controller.feedback(f);
        m_Smoothness.feedback(f);
        m_Deadlines.feedback(f);
        m_Trace.add(Event::Feedback, f.id, f.observed,
            {int64_t(f.sequence), f.presented, f.uncertainty, f.refresh, int64_t(f.output),
             int64_t(f.flags), int64_t(f.quality), int64_t(f.outcome), used});
        if (f.outcome == Outcome::Presented) ++m_Presented;
        else if (f.outcome == Outcome::Discarded) ++m_Discarded;
        else ++m_Unavailable;
    }
    std::lock_guard<std::mutex> lock(m_Lock);
    m_SmoothnessReport = m_Smoothness.report(now());
    const auto deadlines = m_Deadlines.report();
    m_Stats.vrrDeadlineCoverage = deadlines.coverage;
    m_Stats.vrrDeadlineFeedbackCoverage = deadlines.feedbackCoverage;
    m_Stats.vrrCalibrationSeconds = deadlines.duration / double(Second);
    m_Stats.vrrCalibrationFrames = deadlines.measured;
    m_Stats.vrrCalibrationReady = deadlines.available;
    m_Stats.vrrBufferCoverage = m_Controller.reserve().coverage();
    m_Stats.vrrBufferSeconds = m_Controller.reserve().duration() / double(Second);
    m_Stats.vrrBufferFrames = m_Controller.reserve().validationFrames();
    m_Stats.vrrBufferOverloaded = m_Controller.workloadOverloaded();
}

void Worker::recordPlan(Event event, const Frame& f, const Plan& p, Ns at)
{
    m_Trace.add(event, f.id, at, {p.prepare, p.submit, p.target, p.earliest, p.latest,
        p.uncertainty, p.buffer, p.renderBudget, p.compositorLead, int64_t(p.mode), p.belowRange, p.deadline});
}

void Worker::finishFrame(InFlight& f)
{
    const Ns completed = f.preparation.gpuReady;
    const bool precise = !f.preparation.token ||
        completed - std::max(f.preparation.gpuNotReady, f.cpuCompleted) <= 500000;
    if (!precise) f.probe.recovery |= UncertainCompletion;
    const Ns work = preparationWork(f.started, f.preparation, m_LastGpuReady);
    m_LastGpuReady = std::max(m_LastGpuReady, precise ? completed : f.preparation.gpuNotReady);
    m_Trace.add(Event::PrepareStages, f.q.timing.id, completed,
        {f.started, f.preparation.acquired, f.preparation.commandsSubmitted,
         f.preparation.gpuNotReady, completed});
    m_Trace.add(Event::Render, f.q.timing.id, completed,
        {f.started, work, 1, f.dispatch, f.probe.decoded,
         f.probe.residual, f.probe.applied, f.probe.typical, f.probe.period,
         int64_t(f.probe.recovery), 3, f.cpuCompleted});
    m_Controller.renderCost(f.probe, work, completed, f.dispatch);
    m_Controller.gpuReady(f.q.timing.id, completed);
    if (!(f.q.timing.id % 16)) m_Trace.add(Event::Reserve, f.q.timing.id, completed,
        {m_Controller.reserve().common(), m_Controller.reserve().boost(),
         m_Controller.bufferTarget(),
         int64_t(m_Controller.reserve().observations()), int64_t(m_Controller.reserve().evidence()),
         int64_t(m_Controller.reserve().validationFrames()), m_Controller.reserve().reliable(),
         int64_t(m_Controller.reserve().misses()), m_Controller.reserve().duration(), 2, Reserve::MissTolerance});
    if (f.presented) {
        std::lock_guard<std::mutex> lock(m_Lock);
        ++m_Stats.renderedFrames;
        m_Stats.totalPacerTimeUs += uint64_t((std::max<Ns>(0, f.started - f.q.timing.decoded) +
            std::max<Ns>(0, f.submitted - completed)) / 1000);
        m_Stats.totalRenderTimeUs += uint64_t((completed - f.started) / 1000);
    }
    av_frame_free(&f.q.frame); // The image-completion dependency has signalled.
}

void Worker::drainPreparation()
{
    Preparation ready;
    while (m_Renderer->pollVrrPreparation(ready)) {
        const auto it = std::find_if(m_InFlight.begin(), m_InFlight.end(),
            [&](const InFlight& f) { return f.preparation.token == ready.token; });
        if (it == m_InFlight.end()) continue;
        if (!ready.gpuReady) {
            // Keep decoder surfaces alive until cleanup drains outstanding GPU work.
            SDL_Event event{}; event.type = SDL_RENDER_DEVICE_RESET; SDL_PushEvent(&event);
            m_Stopping.store(true); continue;
        }
        it->preparation = ready;
        finishFrame(*it);
        m_InFlight.erase(it);
    }
}

void Worker::run()
{
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    Ns lastCheckpoint = 0;
    AVFrame* failedFrame = nullptr;
    while (!m_Stopping.load()) {
        drainPreparation(); poll();
        if (m_InFlight.size() >= 2) {
            // Three queued + two GPU-owned frames preserve the five-surface cap.
            waitUntil(now() + 100000, m_Stopping); continue;
        }
        Queued q;
        {
            std::unique_lock<std::mutex> lock(m_Lock);
            if (m_Queue.empty()) {
                m_Ready.wait_for(lock, std::chrono::milliseconds(1), [&] { return m_Stopping.load() || !m_Queue.empty(); });
                if (m_Queue.empty()) continue;
            }
            if (m_Stopping.load()) break;
            while (m_Queue.size() > 1 && now() - m_Queue.front().timing.decoded >
                   m_Controller.bufferLimit() + 2 * m_Controller.config().minInterval) {
                drop(m_Queue.front(), Drop::Stale); m_Queue.pop_front();
            }
            q = m_Queue.front(); m_Queue.pop_front();
        }
        const Ns checkpointAt = now();
        if (!lastCheckpoint || checkpointAt - lastCheckpoint >= 5 * Second) {
            m_Trace.checkpoint(m_Controller, checkpointAt); lastCheckpoint = checkpointAt;
        }
        m_Controller.arrive(q.timing);
        m_Trace.add(Event::Arrival, q.timing.id, now(), {q.timing.rtp, q.timing.received,
            q.timing.assembled, q.timing.decoded, q.timing.source, q.timing.playout});
        if (m_Controller.recoveryFlags()) m_Trace.add(Event::Recovery, q.timing.id, now(),
            {int64_t(m_Controller.recoveryFlags()), m_Controller.sourcePeriod(), m_Controller.excludedFrames()});
        Ns at = now();
        Plan p = m_Controller.plan(q.timing, at);
        recordPlan(Event::Plan, q.timing, p, at);
        if (p.target > q.timing.playout + p.renderBudget + p.compositorLead + m_Controller.config().minInterval) {
            std::lock_guard<std::mutex> lock(m_Lock);
            if (!m_Queue.empty()) { drop(q, Drop::Stale); continue; }
        }
        InFlight flight{}; flight.q = q; flight.probe = m_Controller.renderProbe();
        const Ns planAt = at;
        if (!waitUntil(p.prepare, m_Stopping)) {
            std::lock_guard<std::mutex> lock(m_Lock); drop(q, Drop::Shutdown); break;
        }
        at = now();
        m_Controller.wakeError(std::max<Ns>(0, at - p.prepare));
        m_Trace.add(Event::Wake, q.timing.id, at, {p.prepare, std::max<Ns>(0, at - p.prepare)});
        flight.started = at;
        flight.dispatch = std::max<Ns>(0, at - std::max(planAt, p.prepare));
        const bool prepared = m_Renderer->prepareVrr(q.frame, m_Stopping, flight.preparation);
        flight.cpuCompleted = now();
        if (!prepared) {
            m_Trace.add(Event::Drop, q.timing.id, now(), {int64_t(m_Stopping.load() ? Drop::Shutdown : Drop::PrepareFailed)});
            { std::lock_guard<std::mutex> lock(m_Lock); ++m_Stats.pacerDroppedFrames;
              if (!m_Stopping.load()) m_LastDeliveryFailure = now(); }
            failedFrame = q.frame;
            if (!m_Stopping.load()) { SDL_Event event{}; event.type = SDL_RENDER_DEVICE_RESET; SDL_PushEvent(&event); }
            m_Stopping.store(true); break;
        }
        if (!flight.preparation.token) flight.preparation.gpuReady = flight.cpuCompleted;
        while (!m_Stopping.load()) {
            poll(); at = now(); p = m_Controller.prepared(p, at);
            recordPlan(Event::Prepared, q.timing, p, at);
            if (!waitUntil(p.submit, m_Stopping)) break;
            poll(); at = now();
            const Plan check = m_Controller.prepared(p, at);
            recordPlan(Event::Prepared, q.timing, check, at);
            if (check.submit > at) { p = check; continue; }
            p = check;
            flight.presented = m_Renderer->presentVrr(q.timing.id, flight.submitted);
            m_Trace.add(Event::Submit, q.timing.id, flight.submitted,
                {p.deadline, now(), flight.presented, p.submit, p.uncertainty, p.target, flight.preparation.gpuReady});
            if (flight.presented) {
                m_Controller.submitted(q.timing.id, flight.submitted, p.deadline, flight.preparation.gpuReady);
                m_Deadlines.submitted(q.timing.id, flight.submitted, p.deadline);
                m_Smoothness.submitted(q.timing.id, q.timing.source);
                if (p.mode == Mode::Tracking) ++m_Tracking; else ++m_Predicted;
                if (p.belowRange) ++m_BelowRange;
            }
            break;
        }
        flight.plan = p;
        if (!flight.presented) {
            std::lock_guard<std::mutex> lock(m_Lock);
            ++m_Stats.pacerDroppedFrames;
            if (!m_Stopping.load()) m_LastDeliveryFailure = now();
            m_Trace.add(Event::Drop, q.timing.id, now(),
                {int64_t(m_Stopping.load() ? Drop::Shutdown : Drop::PresentFailed)});
        }
        if (flight.preparation.token) m_InFlight.push_back(flight);
        else finishFrame(flight);
    }
    // Cleanup waits for image dependencies, including failed/cancelled work.
    m_Renderer->cleanupRenderContext();
    drainPreparation(); poll();
    for (auto& f : m_InFlight) {
        // Timeout observations are not calibration data. The GPU has now drained.
        if (f.presented) { std::lock_guard<std::mutex> lock(m_Lock); ++m_Stats.renderedFrames; }
        av_frame_free(&f.q.frame);
    }
    m_InFlight.clear(); av_frame_free(&failedFrame);
    std::lock_guard<std::mutex> lock(m_Lock);
    while (!m_Queue.empty()) { drop(m_Queue.front(), Drop::Shutdown); m_Queue.pop_front(); }
}
}
