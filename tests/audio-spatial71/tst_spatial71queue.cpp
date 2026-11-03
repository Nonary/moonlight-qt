#include "../../app/streaming/audio/renderers/spatialaudioqueue.h"

#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

namespace {
std::size_t checks = 0;
void check(bool result, const char* expression, int line)
{
    ++checks;
    if (!result) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

std::vector<float> frames(std::size_t start, std::size_t count)
{
    std::vector<float> result(count * Spatial71::ChannelCount);
    for (std::size_t f = 0; f < count; ++f) {
        for (std::size_t c = 0; c < Spatial71::ChannelCount; ++c) {
            result[f * Spatial71::ChannelCount + c] = static_cast<float>((start + f) * 8 + c);
        }
    }
    return result;
}

void formatAndRouting()
{
    constexpr std::array<std::uint32_t, 8> expected = {2, 4, 8, 16, 128, 256, 32, 64};
    static_assert(Spatial71::SampleRate == 48000, "Timing tests require 48 kHz");
    static_assert(Spatial71::QueueCapacityFrames == 1440, "Maximum PCM backlog is 30 ms");
    CHECK(Spatial71::StaticObjectTypes == expected);
    std::uint32_t mask = 0;
    for (auto type : Spatial71::StaticObjectTypes) {
        CHECK((mask & type) == 0);
        mask |= type;
    }
    CHECK(mask == 0x1FE && mask == Spatial71::StaticObjectMask);
    const auto source = frames(0, 23);
    for (std::size_t channel = 0; channel < 8; ++channel) {
        std::array<float, 25> destination{};
        destination.front() = destination.back() = -1.0f;
        CHECK(Spatial71::deinterleave(source.data(), 23, channel, destination.data() + 1));
        for (std::size_t f = 0; f < 23; ++f) {
            CHECK(destination[f + 1] == static_cast<float>(8 * f + channel));
        }
        CHECK(destination.front() == -1.0f && destination.back() == -1.0f);
    }
    std::array<float, 1> untouched = {17.0f};
    CHECK(!Spatial71::deinterleave(source.data(), 1, 8, untouched.data()));
    CHECK(!Spatial71::deinterleave(nullptr, 1, 0, untouched.data()));
    CHECK(!Spatial71::deinterleave(source.data(), 1, 0, nullptr));
    CHECK(!Spatial71::deinterleave(source.data(), std::numeric_limits<std::size_t>::max(), 0, untouched.data()));
    CHECK(Spatial71::deinterleave(nullptr, 0, 0, nullptr));
    CHECK(untouched[0] == 17.0f);

    for (int bytes : {-1, 1, 31, 33, INT_MAX}) {
        std::size_t count = 123;
        CHECK(!Spatial71::bytesToFrames(bytes, Spatial71::MaxDecodeFrames, count));
        CHECK(count == 0);
    }
    for (const std::size_t count : {std::size_t(0), std::size_t(1), std::size_t(240), Spatial71::MaxDecodeFrames}) {
        std::size_t actual = 0;
        CHECK(Spatial71::bytesToFrames(static_cast<int>(count * 32), Spatial71::MaxDecodeFrames, actual));
        CHECK(actual == count);
    }
    std::size_t actual = 123;
    CHECK(!Spatial71::bytesToFrames(static_cast<int>((Spatial71::MaxDecodeFrames + 1) * 32), Spatial71::MaxDecodeFrames, actual));
    CHECK(actual == 0);
    std::puts("PASS channel-to-static-object mapping, deinterleave guards, packet-size validation");
}

void silenceWrapAndOverflow()
{
    const auto storage = std::make_unique<Spatial71::FrameQueue>();
    auto& queue = *storage;
    CHECK(queue.push(nullptr, 0) == 0);
    CHECK(queue.pop(nullptr, 0) == 0);
    std::vector<float> output(480 * 8, -1.0f);
    CHECK(queue.pop(output.data(), 480) == 0);
    for (float value : output) CHECK(value == 0.0f);
    CHECK(queue.statistics().missingFrames == 480);

    const auto input = frames(0, 4000);
    CHECK(queue.push(input.data(), 1200) == 0);
    output.resize(1000 * 8);
    CHECK(queue.pop(output.data(), 1000) == 1000);
    CHECK(std::memcmp(output.data(), input.data(), 1000 * 8 * sizeof(float)) == 0);
    CHECK(queue.push(input.data() + 1200 * 8, 1240) == 0);
    CHECK(queue.size() == 1440);
    CHECK(queue.push(input.data() + 2440 * 8, 17) == 17);
    output.resize(1500 * 8);
    CHECK(queue.pop(output.data(), 1500) == 1440);
    CHECK(std::memcmp(output.data(), input.data() + 1017 * 8, 1440 * 8 * sizeof(float)) == 0);
    for (std::size_t i = 1440 * 8; i < output.size(); ++i) CHECK(output[i] == 0.0f);
    CHECK(queue.size() == 0);
    CHECK(queue.statistics().highWaterFrames == 1440);
    CHECK(queue.statistics().droppedFrames == 17);
    CHECK(queue.statistics().missingFrames == 540);
    CHECK(queue.statistics().consumedFrames == 2440);
    CHECK(queue.statistics().submittedFrames == 2457);

    CHECK(queue.push(input.data(), 100) == 0);
    CHECK(queue.push(input.data(), 4000) == 2660); // 100 older queued + 2560 input prefix
    output.resize(1440 * 8);
    CHECK(queue.pop(output.data(), 1440) == 1440);
    CHECK(std::memcmp(output.data(), input.data() + 2560 * 8, output.size() * sizeof(float)) == 0);
    queue.clear();
    CHECK(queue.size() == 0);
    const auto reset = queue.statistics();
    CHECK(reset.submittedFrames == 0 && reset.consumedFrames == 0 && reset.droppedFrames == 0 && reset.missingFrames == 0 && reset.highWaterFrames == 0);
    output.assign(8, -1.0f);
    CHECK(queue.pop(output.data(), 1) == 0);
    for (float value : output) CHECK(value == 0.0f);
    std::puts("PASS FIFO wrap, bounded complete-frame overflow, zero underflow, reset epochs");
}

void exactFloatPreservation()
{
    // Storage/routing is not a DSP transform. Verify bitwise preservation of
    // ordinary amplitudes, signed zero, denormals and IEEE diagnostic values.
    constexpr std::array<std::uint32_t, 8> bits = {0U, 0x80000000U, 0x3D000000U, 0xBD000000U, 1U, 0x7FC00001U, 0x7F800000U, 0xFF800000U};
    static_assert(sizeof(float) == sizeof(std::uint32_t), "32-bit PCM");
    std::array<float, 8> input{}, output{};
    std::memcpy(input.data(), bits.data(), sizeof(bits));
    const auto storage = std::make_unique<Spatial71::FrameQueue>();
    auto& queue = *storage;
    CHECK(queue.push(input.data(), 1) == 0);
    CHECK(queue.pop(output.data(), 1) == 1);
    CHECK(std::memcmp(input.data(), output.data(), sizeof(bits)) == 0);
    for (std::size_t c = 0; c < 8; ++c) {
        float result = 0;
        CHECK(Spatial71::deinterleave(output.data(), 1, c, &result));
        CHECK(std::memcmp(&result, bits.data() + c, sizeof(result)) == 0);
    }
    std::puts("PASS bit-exact PCM queue and channel extraction (no gain, clipping, or upmix)");
}

void modelStress()
{
    const auto storage = std::make_unique<Spatial71::FrameQueue>();
    auto& queue = *storage;
    std::deque<std::array<float, 8>> model;
    std::uint32_t random = 0x7151U;
    std::size_t nextFrame = 0;
    std::uint64_t submitted = 0, consumed = 0, dropped = 0, missing = 0;
    std::size_t highWater = 0;
    constexpr std::array<std::size_t, 12> quanta = {0, 1, 17, 128, 240, 441, 480, 512, 960, 1440, 1441, 5760};
    for (std::size_t operation = 0; operation < 10000; ++operation) {
        random = random * 1664525U + 1013904223U;
        const auto count = quanta.at((random >> 8) % quanta.size());
        if ((random & 3U) != 0U) {
            const auto input = frames(nextFrame, count);
            nextFrame += count;
            submitted += count;
            const auto beforeDropped = dropped;
            for (std::size_t f = 0; f < count; ++f) {
                std::array<float, 8> frame{};
                std::copy_n(input.data() + f * 8, 8, frame.data());
                model.push_back(frame);
                if (model.size() > 1440) { model.pop_front(); ++dropped; }
            }
            CHECK(queue.push(input.data(), count) == dropped - beforeDropped);
            highWater = (std::max)(highWater, model.size());
        }
        else {
            const auto realFrames = (std::min)(count, model.size());
            std::vector<float> output(count * 8 + 2, -9.0f);
            CHECK(queue.pop(output.data() + 1, count) == realFrames);
            for (std::size_t f = 0; f < count; ++f) {
                if (!model.empty()) {
                    CHECK(std::memcmp(output.data() + 1 + f * 8, model.front().data(), 8 * sizeof(float)) == 0);
                    model.pop_front();
                }
                else {
                    for (std::size_t c = 0; c < 8; ++c) CHECK(output[1 + f * 8 + c] == 0.0f);
                }
            }
            CHECK(output.front() == -9.0f && output.back() == -9.0f);
            consumed += realFrames;
            missing += count - realFrames;
        }
        CHECK(queue.size() == model.size() && queue.size() <= 1440);
        const auto stats = queue.statistics();
        CHECK(stats.submittedFrames == submitted && stats.consumedFrames == consumed);
        CHECK(stats.droppedFrames == dropped && stats.missingFrames == missing);
        CHECK(stats.highWaterFrames == highWater);
        CHECK(stats.submittedFrames == stats.consumedFrames + stats.droppedFrames + queue.size());
    }
    std::puts("PASS 10000 deterministic mixed producer/consumer operations against independent deque model");
}
} // namespace

int main()
{
    formatAndRouting();
    silenceWrapAndOverflow();
    exactFloatPreservation();
    modelStress();
    std::printf("PASS all offline Spatial71 queue tests (%zu checks); no audio device was opened\n", checks);
    return 0;
}
