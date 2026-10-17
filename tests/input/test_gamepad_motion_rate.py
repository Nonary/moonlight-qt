#!/usr/bin/env python3
"""Run production SDL motion configuration and dispatch against fake boundaries.

No Qt, SDL device, or network is needed. The GamepadState declaration and three
method bodies come from the shipping source, so narrowing the stored interval
or changing dispatch independently is covered. Set CXX/CXXFLAGS as needed.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


header = (ROOT / "app/streaming/input/input.h").read_text()
source = (ROOT / "app/streaming/input/gamepad.cpp").read_text()
state = block(header, "struct GamepadState {") + ";"
methods = "\n".join(block(source, marker) for marker in (
    "GamepadState*\nSdlInputHandler::findStateForGamepad(",
    "void SdlInputHandler::setMotionEventState(",
    "void SdlInputHandler::handleControllerSensorEvent(",
))
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#define SDL_VERSION_ATLEAST(a, b, c) 1
#define HAVE_STEAM_CONTROLLER_DRIVER 0
#define SDL_arraysize(a) (sizeof(a) / sizeof((a)[0]))
#define SDL_assert assert
#define SDL_TICKS_PASSED(a, b) (static_cast<int32_t>((b) - (a)) <= 0)
constexpr int MAX_GAMEPADS = 16;
constexpr int SDL_TRUE = 1, SDL_FALSE = 0;
constexpr int SDL_SENSOR_ACCEL = 1, SDL_SENSOR_GYRO = 2;
constexpr uint8_t LI_MOTION_TYPE_ACCEL = 1, LI_MOTION_TYPE_GYRO = 2;
using SDL_JoystickID = int32_t;
using SDL_TimerID = int;
struct SDL_GameController { bool enabled[3] = {}; };
struct SDL_ControllerSensorEvent {
    uint32_t timestamp;
    SDL_JoystickID which;
    int sensor;
    float data[3];
};
int SDL_GameControllerSetSensorEnabled(SDL_GameController* c, int type, int on) {
    c->enabled[type] = on;
    return 0;
}
struct Motion { uint8_t controller, type; float x; };
std::vector<Motion> sent;
int LiSendControllerMotionEvent(uint8_t controller, uint8_t type, float x, float, float) {
    sent.push_back({controller, type, x});
    return 0;
}
@STATE@
class SdlInputHandler {
public:
    GamepadState m_GamepadState[MAX_GAMEPADS] = {};
    bool m_MultiController = true;
    GamepadState* findStateForGamepad(SDL_JoystickID);
    void setMotionEventState(uint16_t, uint8_t, uint16_t);
    void handleControllerSensorEvent(SDL_ControllerSensorEvent*);
};
@METHODS@
int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name, int rate, int sensor) {
        ++checks;
        failures += !ok;
        std::printf("%s %s (rate %d, sensor %d)\n", ok ? "PASS" : "FAIL", name, rate, sensor);
    };
    for (uint16_t rate : {1, 2, 3, 60, 250, 333, 1000, 1001, 65535}) {
        for (int sensor : {SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO}) {
            for (uint32_t start : {10000u, UINT32_MAX - 10u}) {
                SdlInputHandler handler;
                SDL_GameController controller;
                auto& state = handler.m_GamepadState[2];
                state.controller = &controller;
                state.index = 2;
                state.jsId = 42;
                // Establish the prior sample so only the requested interval is tested.
                state.lastAccelEventTime = state.lastGyroEventTime = start;
                sent.clear();
                const uint32_t period = (1000u + rate - 1u) / rate;
                handler.setMotionEventState(2, static_cast<uint8_t>(sensor), rate);
                check(controller.enabled[sensor], "positive rate enables the requested sensor", rate, sensor);
                SDL_ControllerSensorEvent event {start + period - 1u, 42, sensor, {1, 2, 3}};
                handler.handleControllerSensorEvent(&event);
                check(sent.empty(), "no sample is forwarded before the requested interval", rate, sensor);
                event.timestamp = start + period;
                event.data[0] = 2;
                handler.handleControllerSensorEvent(&event);
                check(sent.size() == 1 && sent[0].controller == 2 && sent[0].type == sensor,
                      "sample is forwarded at the interval, including tick wrap", rate, sensor);
                sent.clear();
                handler.setMotionEventState(2, static_cast<uint8_t>(sensor), 0);
                event.timestamp += 2000;
                event.data[0] = 3;
                handler.handleControllerSensorEvent(&event);
                check(!controller.enabled[sensor] && sent.empty(), "zero rate disables forwarding", rate, sensor);
                check(handler.m_GamepadState[1].accelReportPeriodMs == 0 &&
                      handler.m_GamepadState[1].gyroReportPeriodMs == 0,
                      "another controller is untouched", rate, sensor);
            }
        }
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
'''.replace("@STATE@", state).replace("@METHODS@", methods)

with tempfile.TemporaryDirectory(prefix="moonlight-motion-") as directory:
    cpp = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    cpp.write_text(fixture)
    subprocess.run(shlex.split(os.environ.get("CXX", "c++")) +
                   ["-std=c++17", "-Wall", "-Wextra", "-Werror"] +
                   shlex.split(os.environ.get("CXXFLAGS", "")) +
                   [str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
