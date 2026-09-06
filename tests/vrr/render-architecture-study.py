#!/usr/bin/env python3
"""Build isolated scheduling prototypes; never modify the application controller.

Run from the repository root. Generated C++ retains the production controller
and queue model except for the explicit replacements below. Set VRR_STUDY_MODE:
0 = production, 1 = shared deadline with readiness reserve, 2 = shared deadline
with work-conserving preparation, 3 = mode 2 with execution-independent smoothing,
4 = mode 2 plus a bounded offscreen preparation stage and final swapchain copy.
Preparation costs and GPU readiness observations remain attached to each frame.
These are CPU submission simulations, not GPU-completion or scanout predictions.
"""

from pathlib import Path
import argparse
import shutil


def replace_once(text, before, after):
    assert text.count(before) == 1, (before[:100], text.count(before))
    return text.replace(before, after, 1)


def generate(destination):
    destination.mkdir(parents=True, exist_ok=True)
    controller = Path('app/streaming/video/ffmpeg-renderers/pacer/vrr')
    tests = Path('tests/vrr')
    for name in ['vrrtimingcontroller.h', 'vrrtypes.h']:
        shutil.copyfile(controller / name, destination / name)
    for name in ['vrrreplayconfig.h', 'vrrreplayconfig.cpp', 'vrrqueuesim.cpp']:
        text = (tests / name).read_text().replace(
            '../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h',
            'vrrtimingcontroller.h')
        (destination / name).write_text(text)

    text = (controller / 'vrrtimingcontroller.cpp').read_text()
    # Keep the study's vrr13 baseline after the September 5 trial becomes the
    # session default. The explicit prototypes below select the architecture.
    text = text.replace('parameters.playoutSharedRenderDeadline = 1;',
                        'parameters.playoutSharedRenderDeadline = 0;')
    # The original architecture study predates the readiness-floor follow-up.
    # Normalize this optional block before applying the historical prototypes.
    marker = '    // A late-ready frame still needs time to prepare.'
    if marker in text:
        start = text.index(marker)
        end = text.index('    targetUs = std::max(\n        targetUs,', start)
        text = text[:start] + text[end:]
        text = text.replace('saturatingAdd(readyLeadUs,',
                            'saturatingAdd(presentationLeadUs,')
    text = replace_once(text, '#include <algorithm>', '#include <algorithm>\n#include <cstdlib>')
    text = replace_once(text, 'namespace {', '''namespace {
int studyMode()
{
    static const int mode = std::getenv("VRR_STUDY_MODE") ?
        std::atoi(std::getenv("VRR_STUDY_MODE")) : 0;
    return mode;
}
''')
    text = replace_once(text,
        '    const uint64_t leadUs = saturatingAdd(presentationLeadUs,',
        '''    // Experimental: preparation consumes the playout window rather
    // than unconditionally extending it. Keep learned render-start headroom.
    const uint64_t mappedLeadUs = timestampPlayout && studyMode() != 0 ?
        0 : presentationLeadUs;
    const uint64_t leadUs = saturatingAdd(mappedLeadUs,''')
    start = text.index('    uint64_t targetUs = saturatingAdd(')
    end = text.index('    const uint64_t unflooredTargetUs = targetUs;', start)
    region = text[start:end].replace('presentationLeadUs', 'mappedLeadUs')
    region = replace_once(region,
        '''        saturatingAdd(nowUs,
                      saturatingAdd(mappedLeadUs,''',
        '''        saturatingAdd(nowUs,
                      saturatingAdd(timestampPlayout && studyMode() == 1 ?
                                        presentationLeadUs : mappedLeadUs,''')
    text = text[:start] + region + text[end:]
    text = replace_once(text,
        '        m_LastSmoothedBasisUs = targetUs > leadUs ? targetUs - leadUs : 0;',
        '''        m_LastSmoothedBasisUs = studyMode() == 3 ?
            addSigned(saturatingAdd(m_SourceTimeUs, playoutDelayUs), smoothingUs) :
            (targetUs > leadUs ? targetUs - leadUs : 0);''')
    (destination / 'vrrtimingcontroller.cpp').write_text(text)

    text = (destination / 'vrrqueuesim.cpp').read_text()
    text = replace_once(text, '        result["count"] =', '''        long double sum = 0;
        for (auto value : values) sum += value;
        result["mean"] = values.empty() ? 0.0 : static_cast<double>(sum / values.size());
        result["p99_9"] = static_cast<qint64>(percentile(999));
        result["count"] =''')
    text = replace_once(text, '    Distribution presentationJerk;', '''    Distribution presentationJerk;
    Distribution intervalChange;
    Distribution deadlineMiss;
    Distribution gpuToSubmit;
    uint64_t priorIntervalUs = 0;
    uint64_t missesOver1ms = 0;
    uint64_t missesOver2ms = 0;
    uint64_t readinessViolations = 0;
    uint64_t outputStepsOver2ms = 0;
    uint64_t outputStepsOver4ms = 0;
    const uint64_t extraWorkUs = std::getenv("VRR_STUDY_EXTRA_WORK_US") ?
        std::strtoull(std::getenv("VRR_STUDY_EXTRA_WORK_US"), nullptr, 10) : 0;
    const uint64_t spikeEvery = std::getenv("VRR_STUDY_SPIKE_EVERY") ?
        std::strtoull(std::getenv("VRR_STUDY_SPIKE_EVERY"), nullptr, 10) : 0;''')
    text = replace_once(text,
        '''        const uint64_t preparationUs = input.preparationUs != 0 ?
            input.preparationUs : medianPreparationUs;''',
        '''        const uint64_t preparationUs = (input.preparationUs != 0 ?
            input.preparationUs : medianPreparationUs) +
            (spikeEvery == 0 || input.sequence % spikeEvery == 0 ? extraWorkUs : 0);''')
    text = replace_once(text, '        ++presented;', '''        ++presented;
        const uint64_t lateUs = preparationEndUs > decision.targetUs ?
            preparationEndUs - decision.targetUs : 0;
        deadlineMiss.values.push_back(lateUs);
        missesOver1ms += lateUs > 1000;
        missesOver2ms += lateUs > 2000;
        readinessViolations += submissionUs < input.gpuReadyUs ||
            submissionUs < input.decodeUs;
        if (input.gpuReadyUs != 0) {
            gpuToSubmit.values.push_back(submissionUs - input.gpuReadyUs);
        }''')
    text = replace_once(text,
        '            presentationIntervals.values.push_back(presentationInterval);',
        '''            presentationIntervals.values.push_back(presentationInterval);
            if (priorIntervalUs != 0) {
                const uint64_t change = static_cast<uint64_t>(std::llabs(
                    static_cast<int64_t>(presentationInterval) -
                    static_cast<int64_t>(priorIntervalUs)));
                intervalChange.values.push_back(change);
                outputStepsOver2ms += change > 2000;
                outputStepsOver4ms += change > 4000;
            }
            priorIntervalUs = presentationInterval;''')
    text = replace_once(text,
        '    result["resolved_controller"] =',
        '''    result["output_interval_change_us"] = intervalChange.json();
    result["preparation_deadline_miss_us"] = deadlineMiss.json();
    result["gpu_observation_to_submission_us"] = gpuToSubmit.json();
    result["preparation_misses_over_1ms"] = static_cast<qint64>(missesOver1ms);
    result["preparation_misses_over_2ms"] = static_cast<qint64>(missesOver2ms);
    result["readiness_violations"] = static_cast<qint64>(readinessViolations);
    result["output_steps_over_2ms"] = static_cast<qint64>(outputStepsOver2ms);
    result["output_steps_over_4ms"] = static_cast<qint64>(outputStepsOver4ms);
    result["architecture_mode"] = std::getenv("VRR_STUDY_MODE") ?
        std::atoi(std::getenv("VRR_STUDY_MODE")) : 0;
    result["extra_preparation_work_us"] = static_cast<qint64>(extraWorkUs);
    result["spike_every"] = static_cast<qint64>(spikeEvery);
    result["resolved_controller"] =''')
    text = replace_once(text, '    auto admitThrough =', '''    const bool pipeline = std::getenv("VRR_STUDY_MODE") &&
        std::atoi(std::getenv("VRR_STUDY_MODE")) == 4;
    const uint64_t copyCostUs = std::getenv("VRR_STUDY_COPY_US") ?
        std::strtoull(std::getenv("VRR_STUDY_COPY_US"), nullptr, 10) : 500;
    // Two offscreen textures: one owned by the presentation stage, one
    // rendering or prepared. The producer queue retains its original bound.
    bool renderBusy = false;
    bool prepared = false;
    size_t renderingIndex = 0;
    size_t preparedIndex = 0;
    uint64_t renderDoneUs = 0;
    uint64_t stageClockUs = nowUs;
    const uint64_t never = std::numeric_limits<uint64_t>::max();
    auto nextStageEvent = [&]() {
        uint64_t event = nextArrival < capture.frames.size() ?
            capture.frames[nextArrival].arrivalUs : never;
        if (renderBusy) event = std::min(event, renderDoneUs);
        if (!renderBusy && !prepared && !queue.empty()) {
            const auto& frame = capture.frames[queue.front()];
            event = std::min(event, std::max(stageClockUs,
                std::max(frame.arrivalUs, std::max(frame.decodeUs, frame.gpuReadyUs))));
        }
        return event;
    };
    auto admitThrough =''')
    text = replace_once(text,
        '''    auto admitThrough = [&](uint64_t boundaryUs) {
        while''',
        '''    auto admitThrough = [&](uint64_t boundaryUs) {
        if (pipeline) {
            for (;;) {
                const uint64_t eventUs = nextStageEvent();
                if (eventUs == never || eventUs > boundaryUs) break;
                stageClockUs = std::max(stageClockUs, eventUs);
                while (nextArrival < capture.frames.size() &&
                        capture.frames[nextArrival].arrivalUs <= eventUs) {
                    if (queue.size() >= queueCapacity) {
                        queue.pop_front();
                        ++capacityDrops;
                    }
                    queue.push_back(nextArrival++);
                    maximumQueueDepth = std::max<uint64_t>(maximumQueueDepth, queue.size());
                    queueDepth.values.push_back(queue.size());
                }
                if (renderBusy && renderDoneUs <= eventUs) {
                    renderBusy = false;
                    prepared = true;
                    preparedIndex = renderingIndex;
                }
                if (!renderBusy && !prepared && !queue.empty()) {
                    const auto& frame = capture.frames[queue.front()];
                    const uint64_t readyUs = std::max(frame.arrivalUs,
                        std::max(frame.decodeUs, frame.gpuReadyUs));
                    if (readyUs <= stageClockUs) {
                        renderingIndex = queue.front();
                        queue.pop_front();
                        // Keep ALL recorded preparation work, including its
                        // acquisition overhead, then charge a separate final
                        // copy. Extra work models unmeasured offscreen/fence
                        // cost; it is never assumed to be free GPU rendering.
                        const uint64_t workUs = (frame.preparationUs != 0 ?
                            frame.preparationUs : medianPreparationUs) +
                            (spikeEvery == 0 || frame.sequence % spikeEvery == 0 ?
                                extraWorkUs : 0);
                        renderDoneUs = addSaturated(stageClockUs, workUs);
                        renderBusy = true;
                    }
                }
            }
            return;
        }
        while''')
    text = replace_once(text,
        '''    while (nextArrival < capture.frames.size() || !queue.empty()) {
        if (queue.empty()) {
            nowUs = std::max(nowUs, capture.frames[nextArrival].arrivalUs);
        }
        admitThrough(nowUs);
        if (queue.empty()) continue;

        const InputFrame& input = capture.frames[queue.front()];
        queue.pop_front();''',
        '''    while (nextArrival < capture.frames.size() || !queue.empty() ||
            (pipeline && (renderBusy || prepared))) {
        size_t selectedIndex = 0;
        if (pipeline) {
            admitThrough(nowUs);
            while (!prepared) {
                const uint64_t eventUs = nextStageEvent();
                if (eventUs == never) std::abort();
                nowUs = std::max(nowUs, eventUs);
                admitThrough(nowUs);
            }
            selectedIndex = preparedIndex;
            prepared = false;
            stageClockUs = std::max(stageClockUs, nowUs);
        }
        else {
            if (queue.empty()) {
                nowUs = std::max(nowUs, capture.frames[nextArrival].arrivalUs);
            }
            admitThrough(nowUs);
            if (queue.empty()) continue;
            selectedIndex = queue.front();
            queue.pop_front();
        }
        const InputFrame& input = capture.frames[selectedIndex];''')
    text = replace_once(text,
        '''        const uint64_t preparationUs = (input.preparationUs != 0 ?''',
        '''        const uint64_t preparationUs = pipeline ? copyCostUs : (input.preparationUs != 0 ?''')
    text = replace_once(text,
        '    result["spike_every"] = static_cast<qint64>(spikeEvery);',
        '''    result["spike_every"] = static_cast<qint64>(spikeEvery);
    result["offscreen_stage"] = pipeline;
    result["offscreen_texture_capacity"] = pipeline ? 2 : 0;
    result["final_copy_cost_us"] = static_cast<qint64>(pipeline ? copyCostUs : 0);''')
    (destination / 'vrrqueuesim.cpp').write_text(text)
    (destination / 'study.pro').write_text('''TEMPLATE = app
TARGET = architecture-sim
QT -= gui
CONFIG += console c++17 link_pkgconfig
PKGCONFIG += libavutil
SOURCES += vrrqueuesim.cpp vrrreplayconfig.cpp vrrtimingcontroller.cpp
''')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    generate(parser.parse_args().destination)
