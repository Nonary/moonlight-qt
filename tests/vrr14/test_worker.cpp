#include "worker.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

class Presenter : public IFFmpegRenderer {
public:
    Presenter() : IFFmpegRenderer(RendererType::Unknown) {}
    bool initialize(PDECODER_PARAMETERS) override { return true; }
    bool prepareDecoderContext(AVCodecContext*, AVDictionary**) override { return true; }
    void renderFrame(AVFrame*) override { assert(false); }
    bool prepareVrr(AVFrame*, const std::atomic<bool>& stopping, Vrr::Preparation& timing) override {
        owner = std::this_thread::get_id();
        preparing.store(true);
        timing.acquired = timing.commandsSubmitted = timing.gpuNotReady = Vrr::now();
        const bool ready = Vrr::waitUntil(Vrr::now() + (block ? Vrr::Second : 100000), stopping);
        timing.gpuReady = Vrr::now();
        return ready;
    }
    bool presentVrr(uint64_t id, Vrr::Ns& at) override {
        assert(owner == std::this_thread::get_id());
        at = Vrr::now();
        feedback = {}; feedback.id = feedback.sequence = id;
        feedback.presented = feedback.observed = at;
        feedback.quality = Vrr::Quality::Hardware; feedback.outcome = Vrr::Outcome::Presented;
        pending = true;
        return true;
    }
    bool pollVrr(Vrr::Feedback& result) override {
        if (!pending) return false;
        result = feedback; result.observed = Vrr::now(); pending = false; return true;
    }
    void cleanupRenderContext() override { cleaned = true; }
    bool block = false, cleaned = false, pending = false;
    std::atomic<bool> preparing{false};
    std::thread::id owner;
    Vrr::Feedback feedback;
};

class AsyncPresenter : public Presenter {
public:
    bool prepareVrr(AVFrame*, const std::atomic<bool>&, Vrr::Preparation& timing) override {
        owner = std::this_thread::get_id();
        timing.token = ++next;
        timing.acquired = timing.commandsSubmitted = timing.gpuNotReady = Vrr::now();
        completions.push_back(timing);
        maxInFlight = std::max(maxInFlight, completions.size());
        return true;
    }
    bool presentVrr(uint64_t id, Vrr::Ns& at) override {
        assert(owner == std::this_thread::get_id());
        at = Vrr::now();
        Vrr::Feedback f; f.id = f.sequence = id;
        f.quality = Vrr::Quality::Hardware; f.outcome = Vrr::Outcome::Presented;
        feedbacks.push_back(f); ++submitted;
        return true;
    }
    bool pollVrrPreparation(Vrr::Preparation& result) override {
        if (!release || completions.empty()) return false;
        result = completions.front(); completions.pop_front();
        result.gpuReady = Vrr::now();
        return true;
    }
    bool pollVrr(Vrr::Feedback& result) override {
        if (!release || feedbacks.empty()) return false;
        result = feedbacks.front(); feedbacks.pop_front();
        result.presented = result.observed = Vrr::now();
        return true;
    }
    void cleanupRenderContext() override { release = true; cleaned = true; }
    std::atomic<bool> release{false};
    std::atomic<unsigned> submitted{0};
    size_t maxInFlight = 0;
    uint64_t next = 0;
    std::deque<Vrr::Preparation> completions;
    std::deque<Vrr::Feedback> feedbacks;
};

static AVFrame* frame(unsigned i, std::atomic<unsigned>& freed)
{
    auto* f = av_frame_alloc();
    f->pts = i * 180;
    f->buf[0] = av_buffer_create(static_cast<uint8_t*>(av_malloc(1)), 1,
        [](void* context, uint8_t* data) { ++*static_cast<std::atomic<unsigned>*>(context); av_free(data); }, &freed, 0);
    assert(f->buf[0]);
    return f;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir captures;
    assert(captures.isValid());
    const auto capture = captures.filePath("worker.vrr14");
    qputenv("MOONLIGHT_VRR_CAPTURE", capture.toUtf8());
    SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS);
    std::atomic<unsigned> freed{0};
    Presenter presenter;
    Vrr::Config config; config.minInterval = 2 * Vrr::Millisecond;
    {
        Vrr::Worker worker(&presenter, config);
        assert(worker.start());
        for (unsigned i = 0; i < 100; ++i) worker.submit(frame(i, freed), 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        worker.stop();
        if (!qgetenv("VRR_TEST_LAB").isEmpty()) {
            assert(QProcess::execute(QString::fromUtf8(qgetenv("VRR_TEST_LAB")), {"replay", capture}) == 0);
        }
        VIDEO_STATS stats{};
        worker.collectStats(stats);
        assert(stats.renderedFrames + stats.pacerDroppedFrames == 100);
        assert(stats.pacerDroppedFrames > 0 && presenter.cleaned && freed == 100);
        VIDEO_STATS again{};
        worker.collectStats(again);
        assert(!again.renderedFrames && !again.pacerDroppedFrames); // No double-counted deltas.
    }
    Presenter blocked;
    blocked.block = true;
    {
        Vrr::Worker worker(&blocked, config);
        assert(worker.start());
        worker.submit(frame(1, freed), 0, 0);
        const auto deadline = Vrr::now() + Vrr::Second;
        while (!blocked.preparing.load() && Vrr::now() < deadline) std::this_thread::yield();
        assert(blocked.preparing.load());
        const auto before = Vrr::now();
        worker.stop();
        assert(Vrr::now() - before < 100 * Vrr::Millisecond);
        assert(blocked.cleaned && freed == 101);
    }
    AsyncPresenter asynchronous;
    {
        Vrr::Worker worker(&asynchronous, config);
        assert(worker.start());
        worker.submit(frame(1, freed), 0, 0);
        worker.submit(frame(2, freed), 0, 0);
        const auto deadline = Vrr::now() + Vrr::Second;
        while (asynchronous.submitted.load() < 2 && Vrr::now() < deadline) std::this_thread::yield();
        assert(asynchronous.submitted == 2); // Both submitted before either GPU completion.
        assert(freed == 101); // Neither decoder surface may be reused yet.
        worker.stop(); // Drains dependencies before freeing either surface.
        assert(asynchronous.cleaned && freed == 103 && asynchronous.maxInFlight == 2);
        VIDEO_STATS stats{}; worker.collectStats(stats);
        assert(stats.renderedFrames == 2 && stats.pacerDroppedFrames == 0);
        if (!qgetenv("VRR_TEST_LAB").isEmpty()) {
            assert(QProcess::execute(QString::fromUtf8(qgetenv("VRR_TEST_LAB")), {"replay", capture}) == 0);
        }
    }
    SDL_Quit();
    std::cout << "VRR worker capacity, frame ownership, stats deltas and cancellation checks passed\n";
}
