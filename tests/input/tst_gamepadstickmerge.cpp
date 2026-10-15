#include "../../app/streaming/input/gamepadstickmerge.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

using GamepadStickMerge::Stick;

struct State {
    const void* controller = nullptr;
    short index = 0;
    short lsX = 0, lsY = 0;
    short rsX = 0, rsY = 0;
};

static const int connectedController = 0;

static State state(Stick left, Stick right = {}, short index = 0)
{
    return {&connectedController, index, left.x, left.y, right.x, right.y};
}

static void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static bool same(Stick a, Stick b)
{
    return a.x == b.x && a.y == b.y;
}

static void regression()
{
    // Illustrative wire values reproduce the captured HID 143/140 versus
    // 143/0 flip when X changes by four units under the old component OR rule.
    for (short forwardX : {short(3980), short(3984)}) {
        const Stick forward {forwardX, 32767};
        const Stick idle {3983, -3213};
        std::array<State, 2> pads {state(forward), state(idle)};
        for (int order = 0; order < 2; ++order) {
            for (std::size_t origin = 0; origin < pads.size(); ++origin) {
                const auto output = GamepadStickMerge::select(pads.data(), pads.size(), pads[origin].index);
                require(same(output.left, forward), "idle X drift displaced held forward");
            }
            std::reverse(pads.begin(), pads.end());
        }
    }
}

static void vectorsAndTies()
{
    // Each stick may select a different controller, but never splice axes.
    std::array<State, 2> pads {state({30000, 0}, {-20000, 0}),
                              state({0, 31000}, {12000, 12000})};
    auto output = GamepadStickMerge::select(pads.data(), pads.size(), 0);
    require(same(output.left, {0, 31000}) && same(output.right, {-20000, 0}),
            "stick vectors were spliced or coupled");

    pads = {state({30000, 0}), state({23000, 23000})};
    require(same(GamepadStickMerge::select(pads.data(), pads.size(), 0).left, {23000, 23000}),
            "diagonal selection did not use radial strength");

    pads = {state({-30000, 0}), state({30000, 0})};
    for (int order = 0; order < 2; ++order) {
        for (std::size_t origin = 0; origin < pads.size(); ++origin) {
            require(same(GamepadStickMerge::select(pads.data(), pads.size(), pads[origin].index).left,
                         {pads[0].lsX, pads[0].lsY}), "opposing equal inputs did not retain the lowest slot");
        }
        std::reverse(pads.begin(), pads.end());
    }
    pads = {state({-31000, 0}), state({30000, 0})};
    require(same(GamepadStickMerge::select(pads.data(), pads.size(), 0).left, {-31000, 0}),
            "weaker opposing input displaced or cancelled stronger input");
}

static void lifecycle()
{
    std::array<State, 3> pads {state({0, 32767}), state({3983, -3213}), state({-32768, -32768}, {}, 1)};
    pads[0] = {}; // Same zeroed slot as handleControllerDeviceEvent removal.
    require(same(GamepadStickMerge::select(pads.data(), pads.size(), 0).left, {3983, -3213}),
            "removed controller or another player's input was retained");
    pads[0] = state({0, 32767}); // Reconnect into the freed slot.
    require(same(GamepadStickMerge::select(pads.data(), pads.size(), 0).left, {0, 32767}),
            "reconnected controller was not selected");
    pads[0].controller = nullptr; // Ignore disconnected slots even before clearing old axes.
    pads[1] = {};
    require(same(GamepadStickMerge::select(pads.data(), pads.size(), 0).left, {}),
            "no connected matching controller should produce neutral");
    const std::array<State, 2> neutral {state({}), state({})};
    require(same(GamepadStickMerge::select(neutral.data(), neutral.size(), 0).left, {}),
            "neutral controllers should remain neutral");
}

static void fullRange()
{
    require(GamepadStickMerge::squaredMagnitude({-32768, -32768}) == 2147483648LL,
            "full-scale magnitude overflowed");
    const std::array<short, 9> edges {-32768, -32767, -23000, -1, 0, 1, 23000, 32766, 32767};
    for (int x = std::numeric_limits<short>::min(); x <= std::numeric_limits<short>::max(); ++x) {
        for (short y : edges) {
            const Stick candidate {static_cast<short>(x), y};
            const long long expected = 1LL * x * x + 1LL * y * y;
            require(GamepadStickMerge::squaredMagnitude(candidate) == expected,
                    "signed axis range scored incorrectly");
        }
    }
    // Boundary vectors compete in both slot orders, for both event origins.
    for (short ax : edges) for (short ay : edges) {
        for (short bx : edges) for (short by : edges) {
            std::array<State, 2> pads {state({ax, ay}), state({bx, by})};
            for (int order = 0; order < 2; ++order) {
                const long long a = 1LL * pads[0].lsX * pads[0].lsX + 1LL * pads[0].lsY * pads[0].lsY;
                const long long b = 1LL * pads[1].lsX * pads[1].lsX + 1LL * pads[1].lsY * pads[1].lsY;
                const State& expected = pads[b > a ? 1 : 0];
                for (std::size_t origin = 0; origin < pads.size(); ++origin) {
                    const auto selected = GamepadStickMerge::select(pads.data(), pads.size(), pads[origin].index);
                    require(same(selected.left, {expected.lsX, expected.lsY}),
                            "range/ordering regression or event-origin-dependent selection");
                }
                std::reverse(pads.begin(), pads.end());
            }
        }
    }
}

int main()
{
    regression();
    vectorsAndTies();
    lifecycle();
    fullRange();
    std::puts("gamepad stick merge tests passed");
    return 0;
}
