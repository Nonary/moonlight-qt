#!/usr/bin/env python3
"""Exercise production gamepad mouse cleanup with recorded host input events."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>
constexpr int BUTTON_LEFT = 1, BUTTON_X2 = 5;
constexpr int BUTTON_ACTION_PRESS = 7, BUTTON_ACTION_RELEASE = 8;
std::vector<std::pair<int, int>> events;
std::vector<int> stoppedTimers;
void LiSendMouseButtonEvent(int action, int button) { events.emplace_back(action, button); }
void SDL_RemoveTimer(int timer) { stoppedTimers.push_back(timer); }
struct Session {
    static Session* get() { static Session instance; return &instance; }
    void notifyMouseEmulationMode(bool active) { assert(!active); }
};
struct GamepadState { int mouseEmulationTimer = 0; uint8_t mouseButtonsDown = 0; };
struct SdlInputHandler {
    std::array<GamepadState, 16> m_GamepadState {};
    void sendGamepadMouseButton(GamepadState*, uint8_t, bool);
    void stopGamepadMouseEmulation(GamepadState*);
};
'''

TESTS = r'''
int main() {
    for (uint8_t button = BUTTON_LEFT; button <= BUTTON_X2; button++) {
        SdlInputHandler handler;
        auto* state = &handler.m_GamepadState[0];
        state->mouseEmulationTimer = button;
        events.clear(); stoppedTimers.clear();
        handler.sendGamepadMouseButton(state, button, true);
        handler.stopGamepadMouseEmulation(state); // toggle/removal/destruction share this path
        assert((events == std::vector<std::pair<int, int>> {
            {BUTTON_ACTION_PRESS, button}, {BUTTON_ACTION_RELEASE, button}}));
        assert(state->mouseButtonsDown == 0 && state->mouseEmulationTimer == 0);
        assert(stoppedTimers == std::vector<int>{button});
        handler.stopGamepadMouseEmulation(state);
        handler.sendGamepadMouseButton(state, button, false); // late button-up after stopping
        assert(events.size() == 2 && stoppedTimers.size() == 1);
    }
    {
        SdlInputHandler handler;
        auto* a = &handler.m_GamepadState[0];
        auto* b = &handler.m_GamepadState[1];
        events.clear();
        handler.sendGamepadMouseButton(a, BUTTON_LEFT, true);
        handler.sendGamepadMouseButton(b, BUTTON_LEFT, true);
        handler.stopGamepadMouseEmulation(a);
        assert(events.size() == 1); // b still owns the click
        handler.stopGamepadMouseEmulation(b);
        assert(events.size() == 2 && events.back().first == BUTTON_ACTION_RELEASE);
    }
    {
        SdlInputHandler handler;
        auto* state = &handler.m_GamepadState[0];
        events.clear();
        handler.sendGamepadMouseButton(state, BUTTON_LEFT, false); // held before entering mode
        handler.stopGamepadMouseEmulation(state);
        assert(events.empty()); // never release a click we did not inject
        handler.sendGamepadMouseButton(state, BUTTON_LEFT, true);
        handler.sendGamepadMouseButton(state, BUTTON_X2, true);
        handler.stopGamepadMouseEmulation(state);
        assert(events.size() == 4 && state->mouseButtonsDown == 0);
    }
    std::cout << "7 gamepad mouse cleanup scenarios passed\n";
}
'''


def main():
    source = (ROOT / 'app/streaming/input/gamepad.cpp').read_text()
    handlers = '\n'.join(function(source, signature) for signature in (
        'void SdlInputHandler::sendGamepadMouseButton(',
        'void SdlInputHandler::stopGamepadMouseEmulation(',
    ))
    with tempfile.TemporaryDirectory(prefix='gamepad-mouse-cleanup-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(PRELUDE + handlers + TESTS)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
