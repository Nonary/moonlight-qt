#include "../../app/streaming/video/ffmpeg-renderers/dxgipresent.h"

#include <cstdio>

namespace {
struct FakeSwapChain
{
    unsigned int interval = 99;
    unsigned int flags = 99;
    unsigned int calls = 0;
    long result = 0;

    long Present(unsigned int newInterval, unsigned int newFlags)
    {
        interval = newInterval;
        flags = newFlags;
        ++calls;
        return result;
    }
};
}

int main()
{
    constexpr unsigned int allowTearing = 0x200;
    FakeSwapChain swapChain;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    const auto submit = [&](DxgiPresentParameters parameters,
                            unsigned int interval, unsigned int flags) {
        const auto previousCalls = swapChain.calls;
        check(parameters.present(swapChain) == swapChain.result,
              "native Present result must propagate unchanged");
        check(swapChain.calls == previousCalls + 1,
              "one frame must issue exactly one native Present");
        check(swapChain.interval == interval && swapChain.flags == flags,
              "native Present received the wrong interval or flags");
        check(swapChain.interval == parameters.syncInterval &&
                  swapChain.flags == parameters.flags,
              "native arguments must match the parameters reported in telemetry");
    };

    // Switch both ways: the latched interval must not be silently replaced by
    // zero, and tearing must never carry over into a synchronized submission.
    submit(DxgiPresentParameters::adaptive(true, allowTearing), 1, 0);

    // Compatibility changes only protected presents; adaptive VRR retains
    // the tearing flag. Switching either direction must not carry flags over.
    submit(DxgiPresentParameters::adaptive(true, allowTearing, true), 0, 0);
    submit(DxgiPresentParameters::adaptive(false, allowTearing, true), 0, allowTearing);
    submit(DxgiPresentParameters::adaptive(true, allowTearing, true), 0, 0);
    check(DxgiPresentParameters{0, 0}.protectedPresentation(allowTearing),
          "interval-zero without tearing must remain a protected presentation");
    check(!DxgiPresentParameters{0, allowTearing}.protectedPresentation(allowTearing),
          "adaptive tearing must not be attributed to the protected mode");
    check(DxgiPresentParameters{1, 0}.protectedPresentation(allowTearing),
          "ordinary interval-one protection must retain its classification");
    submit(DxgiPresentParameters::adaptive(false, allowTearing), 0, allowTearing);
    submit(DxgiPresentParameters::adaptive(true, allowTearing), 1, 0);

    // Preserve the legacy software-paced caller's explicit interval-zero path.
    submit({0, 0}, 0, 0);
    submit({0, allowTearing}, 0, allowTearing);

    // Both errors and non-display success statuses belong to the caller.
    swapChain.result = -1;
    submit(DxgiPresentParameters::adaptive(true, allowTearing), 1, 0);
    submit(DxgiPresentParameters::adaptive(true, allowTearing, true), 0, 0);
    swapChain.result = 1;
    submit(DxgiPresentParameters::adaptive(false, allowTearing), 0, allowTearing);
    return failures == 0 ? 0 : 1;
}
