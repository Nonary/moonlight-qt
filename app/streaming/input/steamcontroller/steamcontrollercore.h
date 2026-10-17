// SPDX-License-Identifier: GPL-3.0-or-later
// Transport-agnostic Steam Controller (2026) state decoder: the port of the Android client's
// SteamControllerInput (buttons, sticks, triggers, both pads, IMU, battery). No SDL or Qt types;
// it reports decoded input to a sink and is driven by canned reports in tests/steamcontroller.
#pragma once

#include "steamcontrollerprotocol.h"
#include <cstdint>

struct SteamControllerConfig {
    enum Grips { GripsPaddles = 0, GripsDs4 = 1, GripsOff = 2 };
    bool motion = true;        // advertise GYRO+ACCEL and ask the firmware for the raw IMU
    bool splitPads = true;     // both pads as two fingers on touchpad 0 (else DUAL_TOUCHPAD)
    int grips = GripsPaddles;  // what the four grip buttons become
    bool stickStretch = false; // per-axis rim stretch (session-only extents)
};

// Receives what one controller decoded. Values are already in the Moonlight wire convention:
// sticks -32767..32767 with up positive (as sendGamepadState sends them), triggers 0..255 with
// the Android handler's 0.13 deadzone applied, touch coordinates 0..1 with (0,0) top-left,
// gyro in deg/s and accelerometer in m/s^2.
class SteamControllerSink {
public:
    virtual ~SteamControllerSink() = default;
    virtual void steamState(int buttonFlags, uint8_t leftTrigger, uint8_t rightTrigger,
                            short leftStickX, short leftStickY, short rightStickX, short rightStickY) = 0;
    virtual void steamTouch(uint8_t touchpadIndex, uint8_t eventType, uint32_t pointerId,
                            float x, float y, float pressure) = 0;
    virtual void steamMotion(uint8_t motionType, float x, float y, float z) = 0;
    virtual void steamBattery(uint8_t batteryState, uint8_t percent) = 0;
};

class SteamControllerCore {
public:
    // Button bit indices inside the 32-bit field at state offset +1
    enum Button {
        BTN_A = 0, BTN_B = 1, BTN_X = 2, BTN_Y = 3, BTN_QUICK_ACCESS = 4, BTN_RSTICK_CLICK = 5, BTN_MENU = 6,
        BTN_GRIP_R_TOP = 7, BTN_GRIP_R_BOTTOM = 8, BTN_RB = 9, BTN_DPAD_DOWN = 10, BTN_DPAD_RIGHT = 11,
        BTN_DPAD_LEFT = 12, BTN_DPAD_UP = 13, BTN_VIEW = 14, BTN_LSTICK_CLICK = 15, BTN_STEAM = 16,
        BTN_GRIP_L_TOP = 17, BTN_GRIP_L_BOTTOM = 18, BTN_LB = 19, BTN_RSTICK_TOUCH = 20, BTN_RPAD_TOUCH = 21,
        BTN_RPAD_CLICK = 22, BTN_RT_FULL = 23, BTN_LSTICK_TOUCH = 24, BTN_LPAD_TOUCH = 25, BTN_LPAD_CLICK = 26,
        BTN_LT_FULL = 27, BTN_GRIP_R_TOUCH = 28, BTN_GRIP_L_TOUCH = 29
    };

    SteamControllerCore(const SteamControllerConfig& config, SteamControllerSink* sink);

    const SteamControllerConfig& config() const { return m_Config; }
    // LI_CCAP_* set advertised in the arrival event for this configuration.
    uint16_t capabilities() const;
    // The full button set the arrival event advertises.
    static uint32_t supportedButtonFlags();

    // A state report with its report id at p[0] (offset 1): 0x42 (wired), 0x45, or 0x47 whose pads
    // sit two bytes later. Needs at least 45 bytes after the offset. nowMs drives IMU decimation.
    void handleState(const uint8_t* p, int size, int offset, bool timestampedPads, uint64_t nowMs);
    // Report 0x43: [offset] charge state, [offset+1] percent.
    void handleBattery(const uint8_t* p, int size, int offset);

    // Host request: motion reports of this type at this rate (0 = stop). Decimated like
    // Millisecond intervals round up; unchanged samples are skipped. Only rate 0 disables a sensor.
    void setMotionRate(uint8_t motionType, uint16_t reportRateHz);

    // Replay for a new announcement: the next report resends its state and pad DOWNs.
    void resetInputState();
    // Forgets pad touches without an UP (the host never saw them go down).
    void forgetTouches();
    // Sends an UP for every pad touch the host still thinks is down (link loss).
    void releaseTouches();

    bool leftPadTouched() const { return m_LeftPadTouched; }
    bool rightPadTouched() const { return m_RightPadTouched; }
    // Raw button words OR-ed since the last takeButtonsSeen(); diagnostics.
    uint32_t takeButtonsSeen() { uint32_t v = m_ButtonsSeen; m_ButtonsSeen = 0; return v; }

private:
    bool updatePad(int pad, bool wasTouched, bool touched, int16_t rawX, int16_t rawY, uint16_t rawPressure);
    void reportTouch(uint8_t touchpadIndex, uint8_t eventType, uint32_t pointerId, float x, float y, float pressure);
    void reportMotion(uint8_t type, float x, float y, float z, uint64_t nowMs);
    void calibrateSticks(float& lx, float& ly, float& rx, float& ry);

    SteamControllerConfig m_Config;
    SteamControllerSink* m_Sink;

    // Last state handed to the sink (dedup); invalid until the first report after each announce.
    bool m_ReportedStateValid = false;
    int m_ReportedButtonFlags = 0;
    float m_ReportedLeftTrigger = 0, m_ReportedRightTrigger = 0;
    float m_ReportedLeftStickX = 0, m_ReportedLeftStickY = 0, m_ReportedRightStickX = 0, m_ReportedRightStickY = 0;

    bool m_LeftPadTouched = false, m_RightPadTouched = false;
    float m_LastTouchX[2] = {0, 0}, m_LastTouchY[2] = {0, 0}, m_LastTouchPressure[2] = {0, 0};

    // Motion decimation per type (index 0 accel, 1 gyro)
    uint16_t m_MotionPeriodMs[2] = {0, 0};
    uint64_t m_LastMotionTime[2] = {0, 0};
    float m_LastMotionData[2][3] = {{0, 0, 0}, {0, 0, 0}};
    bool m_MotionSeen[2] = {false, false};

    // Stick rim stretch extents (floor 0.75, only grow, session-only)
    static constexpr float STICK_EXTENT_FLOOR = 0.75f;
    float m_LeftExtentX = STICK_EXTENT_FLOOR, m_LeftExtentY = STICK_EXTENT_FLOOR;
    float m_RightExtentX = STICK_EXTENT_FLOOR, m_RightExtentY = STICK_EXTENT_FLOOR;

    uint32_t m_ButtonsSeen = 0;
};
