// SPDX-License-Identifier: GPL-3.0-or-later
#include "steamcontrollercore.h"

#include <cmath>
#include <cstring>

namespace {

const float TRIGGER_DEADZONE = 0.13f;   // what the Android ControllerHandler applies to driver triggers

bool bit(uint32_t field, int index) { return (field & (1u << index)) != 0; }
int16_t s16(const uint8_t* p, int i) { return (int16_t)((uint16_t)p[i] | ((uint16_t)p[i + 1] << 8)); }
uint16_t u16(const uint8_t* p, int i) { return (uint16_t)((uint16_t)p[i] | ((uint16_t)p[i + 1] << 8)); }
uint32_t u32(const uint8_t* p, int i) {
    return (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
}
float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Wire scaling: stick = round(v * 0x7FFF) clamped to the signed range; trigger = byte(v * 0xFF)
// after the deadzone, truncating as the Java (byte) cast does.
short toWireStick(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    long r = lroundf(v * 0x7FFF);
    if (r > 0x7FFF) r = 0x7FFF;
    if (r < -0x7FFF) r = -0x7FFF;
    return (short)r;
}
uint8_t toWireTrigger(float v) {
    if (v <= TRIGGER_DEADZONE) return 0;
    int t = (int)(v * 0xFF);
    return (uint8_t)(t > 255 ? 255 : t);
}

}

SteamControllerCore::SteamControllerCore(const SteamControllerConfig& config, SteamControllerSink* sink)
    : m_Config(config), m_Sink(sink)
{
}

uint16_t SteamControllerCore::capabilities() const
{
    return (uint16_t)(LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE
            | LI_CCAP_TOUCHPAD | (m_Config.splitPads ? 0 : LI_CCAP_DUAL_TOUCHPAD)
            | LI_CCAP_BATTERY_STATE | LI_CCAP_GRIP_SENSE | LI_CCAP_STICK_TOUCH
            | LI_CCAP_STEAM_HAPTIC | LI_CCAP_SC26_BUTTONS | LI_CCAP_STEAM_ATTRIBUTES
            | (m_Config.motion ? (LI_CCAP_GYRO | LI_CCAP_ACCEL) : 0));
}

uint32_t SteamControllerCore::supportedButtonFlags()
{
    return A_FLAG | B_FLAG | X_FLAG | Y_FLAG
            | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG
            | LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG
            | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG
            | PADDLE1_FLAG | PADDLE2_FLAG | PADDLE3_FLAG | PADDLE4_FLAG
            | TOUCHPAD_FLAG | MISC_FLAG
            | LEFT_GRIP_TOUCH_FLAG | RIGHT_GRIP_TOUCH_FLAG
            | LEFT_STICK_TOUCH_FLAG | RIGHT_STICK_TOUCH_FLAG
            | LEFT_PAD_CLICK_FLAG | RIGHT_PAD_CLICK_FLAG
            | LEFT_TRIGGER_CLICK_FLAG | RIGHT_TRIGGER_CLICK_FLAG;
}

void SteamControllerCore::resetInputState()
{
    m_ReportedStateValid = false;
    m_MotionSeen[0] = m_MotionSeen[1] = false;
    m_LeftExtentX = m_LeftExtentY = m_RightExtentX = m_RightExtentY = STICK_EXTENT_FLOOR;
    forgetTouches();
}

void SteamControllerCore::forgetTouches()
{
    m_LeftPadTouched = m_RightPadTouched = false;
    for (int pad = 0; pad < 2; pad++) {
        m_LastTouchX[pad] = m_LastTouchY[pad] = m_LastTouchPressure[pad] = 0;
    }
}

void SteamControllerCore::releaseTouches()
{
    if (m_LeftPadTouched) reportTouch(0, LI_TOUCH_EVENT_UP, 0, 0, 0, 0);
    if (m_RightPadTouched) reportTouch(m_Config.splitPads ? 0 : 1, LI_TOUCH_EVENT_UP, m_Config.splitPads ? 1 : 0, 0, 0, 0);
    m_LeftPadTouched = m_RightPadTouched = false;
}

void SteamControllerCore::setMotionRate(uint8_t motionType, uint16_t reportRateHz)
{
    int index = motionType == LI_MOTION_TYPE_ACCEL ? 0 : motionType == LI_MOTION_TYPE_GYRO ? 1 : -1;
    if (index < 0) return;
    m_MotionPeriodMs[index] = reportRateHz ? (uint16_t)((1000u + reportRateHz - 1) / reportRateHz) : 0;
    m_MotionSeen[index] = false;
}

void SteamControllerCore::handleState(const uint8_t* p, int size, int offset, bool timestampedPads, uint64_t nowMs)
{
    if (size - offset < 45) {
        return;
    }
    uint32_t buttons = u32(p, offset + 1);
    m_ButtonsSeen |= buttons;
    int flags = 0;
    if (bit(buttons, BTN_A)) flags |= A_FLAG;
    if (bit(buttons, BTN_B)) flags |= B_FLAG;
    if (bit(buttons, BTN_X)) flags |= X_FLAG;
    if (bit(buttons, BTN_Y)) flags |= Y_FLAG;
    if (bit(buttons, BTN_DPAD_UP)) flags |= UP_FLAG;
    if (bit(buttons, BTN_DPAD_DOWN)) flags |= DOWN_FLAG;
    if (bit(buttons, BTN_DPAD_LEFT)) flags |= LEFT_FLAG;
    if (bit(buttons, BTN_DPAD_RIGHT)) flags |= RIGHT_FLAG;
    if (bit(buttons, BTN_LB)) flags |= LB_FLAG;
    if (bit(buttons, BTN_RB)) flags |= RB_FLAG;
    if (bit(buttons, BTN_MENU)) flags |= PLAY_FLAG;
    if (bit(buttons, BTN_VIEW)) flags |= BACK_FLAG;
    if (bit(buttons, BTN_LSTICK_CLICK)) flags |= LS_CLK_FLAG;
    if (bit(buttons, BTN_RSTICK_CLICK)) flags |= RS_CLK_FLAG;
    if (bit(buttons, BTN_STEAM)) flags |= SPECIAL_FLAG;
    if (bit(buttons, BTN_QUICK_ACCESS)) flags |= MISC_FLAG;
    switch (m_Config.grips) {
    case SteamControllerConfig::GripsPaddles:
        // Xbox Elite paddle order: P1 upper right, P2 upper left, P3 lower right, P4 lower left
        if (bit(buttons, BTN_GRIP_R_TOP)) flags |= PADDLE1_FLAG;
        if (bit(buttons, BTN_GRIP_L_TOP)) flags |= PADDLE2_FLAG;
        if (bit(buttons, BTN_GRIP_R_BOTTOM)) flags |= PADDLE3_FLAG;
        if (bit(buttons, BTN_GRIP_L_BOTTOM)) flags |= PADDLE4_FLAG;
        break;
    case SteamControllerConfig::GripsDs4:
        // A DualShock 4 has no paddles; give the grips inputs the host can actually emulate
        if (bit(buttons, BTN_GRIP_L_TOP)) flags |= MISC_FLAG;       // Share
        if (bit(buttons, BTN_GRIP_R_TOP)) flags |= TOUCHPAD_FLAG;   // touchpad click
        if (bit(buttons, BTN_GRIP_L_BOTTOM)) flags |= LS_CLK_FLAG;  // L3
        if (bit(buttons, BTN_GRIP_R_BOTTOM)) flags |= RS_CLK_FLAG;  // R3
        break;
    default:
        break;
    }
    if (bit(buttons, BTN_LPAD_CLICK) || bit(buttons, BTN_RPAD_CLICK)) flags |= TOUCHPAD_FLAG;
    if (bit(buttons, BTN_LPAD_CLICK)) flags |= LEFT_PAD_CLICK_FLAG;
    if (bit(buttons, BTN_RPAD_CLICK)) flags |= RIGHT_PAD_CLICK_FLAG;
    if (bit(buttons, BTN_LT_FULL)) flags |= LEFT_TRIGGER_CLICK_FLAG;
    if (bit(buttons, BTN_RT_FULL)) flags |= RIGHT_TRIGGER_CLICK_FLAG;
    // Grip sense (held, not pressed): a Vibepollo host with the Steam Controller profile puts
    // these back on the virtual device so Steam's "gyro on grip" works; other hosts ignore them.
    if (bit(buttons, BTN_GRIP_L_TOUCH)) flags |= LEFT_GRIP_TOUCH_FLAG;
    if (bit(buttons, BTN_GRIP_R_TOUCH)) flags |= RIGHT_GRIP_TOUCH_FLAG;
    // Stick touch (capacitive, held not pressed) goes the same way.
    if (bit(buttons, BTN_LSTICK_TOUCH)) flags |= LEFT_STICK_TOUCH_FLAG;
    if (bit(buttons, BTN_RSTICK_TOUCH)) flags |= RIGHT_STICK_TOUCH_FLAG;

    // Triggers: 0..32767
    float leftTrigger = clamp01(s16(p, offset + 5) / 32767.0f);
    float rightTrigger = clamp01(s16(p, offset + 7) / 32767.0f);
    // Sticks: signed, the controller reports Y up-positive. Kept in the Android core's
    // down-positive float convention (flipped here) so the stretch below matches; the wire
    // conversion flips back because Moonlight sends up-positive (see sendGamepadState).
    float leftStickX = s16(p, offset + 9) / 32767.0f;
    float leftStickY = -s16(p, offset + 11) / 32767.0f;
    float rightStickX = s16(p, offset + 13) / 32767.0f;
    float rightStickY = -s16(p, offset + 15) / 32767.0f;
    if (m_Config.stickStretch) {
        calibrateSticks(leftStickX, leftStickY, rightStickX, rightStickY);
    }
    // The firmware streams at 250 Hz whether or not anything moved; the host keeps the last state.
    if (!(m_ReportedStateValid && flags == m_ReportedButtonFlags
          && leftTrigger == m_ReportedLeftTrigger && rightTrigger == m_ReportedRightTrigger
          && leftStickX == m_ReportedLeftStickX && leftStickY == m_ReportedLeftStickY
          && rightStickX == m_ReportedRightStickX && rightStickY == m_ReportedRightStickY)) {
        m_ReportedStateValid = true;
        m_ReportedButtonFlags = flags;
        m_ReportedLeftTrigger = leftTrigger;
        m_ReportedRightTrigger = rightTrigger;
        m_ReportedLeftStickX = leftStickX;
        m_ReportedLeftStickY = leftStickY;
        m_ReportedRightStickX = rightStickX;
        m_ReportedRightStickY = rightStickY;
        m_Sink->steamState(flags, toWireTrigger(leftTrigger), toWireTrigger(rightTrigger),
                           toWireStick(leftStickX), toWireStick(-leftStickY),
                           toWireStick(rightStickX), toWireStick(-rightStickY));
    }

    // Touchpads: normalised 0..1 with (0,0) top-left, as SDL does; pressure 0..1
    int padShift = timestampedPads ? 2 : 0;
    bool lTouch = bit(buttons, BTN_LPAD_TOUCH);
    bool rTouch = bit(buttons, BTN_RPAD_TOUCH);
    m_LeftPadTouched = updatePad(0, m_LeftPadTouched, lTouch, s16(p, offset + 17 + padShift),
                                 s16(p, offset + 19 + padShift), u16(p, offset + 21 + padShift));
    m_RightPadTouched = updatePad(1, m_RightPadTouched, rTouch, s16(p, offset + 23 + padShift),
                                  s16(p, offset + 25 + padShift), u16(p, offset + 27 + padShift));

    if (m_Config.motion) {
        // Raw IMU: gyro full scale 2000 deg/s, accel 2 g. Axis order follows SDL's Triton driver.
        // The six words sit at the same place in 0x42, 0x45 and 0x47.
        const float gyroScale = 2000.0f / 32768.0f;
        const float accelScale = 2.0f * 9.80665f / 32768.0f;
        int16_t gx = s16(p, offset + 39), gy = s16(p, offset + 41), gz = s16(p, offset + 43);
        int16_t ax = s16(p, offset + 33), ay = s16(p, offset + 35), az = s16(p, offset + 37);
        reportMotion(LI_MOTION_TYPE_GYRO, gx * gyroScale, gz * gyroScale, -gy * gyroScale, nowMs);
        reportMotion(LI_MOTION_TYPE_ACCEL, ax * accelScale, az * accelScale, -ay * accelScale, nowMs);
    }
}

bool SteamControllerCore::updatePad(int pad, bool wasTouched, bool touched, int16_t rawX, int16_t rawY, uint16_t rawPressure)
{
    // Split mode: both pads are fingers 0/1 on touchpad 0, each confined to its half.
    // Dual mode: pad 0/1 with finger 0 each (needs a host that knows LI_CCAP_DUAL_TOUCHPAD).
    uint8_t touchpadIndex = m_Config.splitPads ? 0 : (uint8_t)pad;
    uint32_t pointerId = m_Config.splitPads ? (uint32_t)pad : 0;
    if (touched) {
        float x = rawX / 65536.0f + 0.5f;
        float y = -rawY / 65536.0f + 0.5f;
        if (m_Config.splitPads) {
            x = pad == 0 ? x * 0.5f : 0.5f + x * 0.5f;
        }
        float pressure = clamp01(rawPressure / 32768.0f);
        if (wasTouched && x == m_LastTouchX[pad] && y == m_LastTouchY[pad] && pressure == m_LastTouchPressure[pad]) {
            // A resting finger produced 250 identical MOVE packets per second per pad.
            return true;
        }
        m_LastTouchX[pad] = x;
        m_LastTouchY[pad] = y;
        m_LastTouchPressure[pad] = pressure;
        reportTouch(touchpadIndex, wasTouched ? LI_TOUCH_EVENT_MOVE : LI_TOUCH_EVENT_DOWN, pointerId, x, y, pressure);
    }
    else if (wasTouched) {
        reportTouch(touchpadIndex, LI_TOUCH_EVENT_UP, pointerId, 0, 0, 0);
    }
    return touched;
}

void SteamControllerCore::reportTouch(uint8_t touchpadIndex, uint8_t eventType, uint32_t pointerId, float x, float y, float pressure)
{
    m_Sink->steamTouch(touchpadIndex, eventType, pointerId, x, y, pressure);
}

void SteamControllerCore::reportMotion(uint8_t type, float x, float y, float z, uint64_t nowMs)
{
    int index = type == LI_MOTION_TYPE_ACCEL ? 0 : 1;
    if (m_MotionPeriodMs[index] == 0) {
        return;
    }
    float data[3] = {x, y, z};
    if (m_MotionSeen[index]) {
        if (nowMs < m_LastMotionTime[index] + m_MotionPeriodMs[index]) {
            return;
        }
        if (memcmp(data, m_LastMotionData[index], sizeof(data)) == 0) {
            return;
        }
    }
    m_MotionSeen[index] = true;
    memcpy(m_LastMotionData[index], data, sizeof(data));
    m_LastMotionTime[index] = nowMs;
    m_Sink->steamMotion(type, x, y, z);
}

void SteamControllerCore::handleBattery(const uint8_t* p, int size, int offset)
{
    if (size - offset < 2) {
        return;
    }
    // Charge state follows the firmware's EChargeState: 0 reset, 1 discharging, 2 charging,
    // 3 source validate, 4 done.
    int chargeState = p[offset];
    int percent = p[offset + 1] > 100 ? 100 : p[offset + 1];
    uint8_t state;
    switch (chargeState) {
    case 1: state = LI_BATTERY_STATE_DISCHARGING; break;
    case 2:
    case 3: state = LI_BATTERY_STATE_CHARGING; break;
    case 4: state = LI_BATTERY_STATE_FULL; break;
    default: state = LI_BATTERY_STATE_UNKNOWN; break;
    }
    m_Sink->steamBattery(state, (uint8_t)percent);
}

// Rescales both sticks to the rim: each axis is divided by the largest deflection seen on it
// (floor 0.75, only grows) and the result is clamped to the unit circle.
void SteamControllerCore::calibrateSticks(float& leftStickX, float& leftStickY, float& rightStickX, float& rightStickY)
{
    float lx = fabsf(leftStickX), ly = fabsf(leftStickY);
    float rx = fabsf(rightStickX), ry = fabsf(rightStickY);
    if (lx > m_LeftExtentX) m_LeftExtentX = lx;
    if (ly > m_LeftExtentY) m_LeftExtentY = ly;
    if (rx > m_RightExtentX) m_RightExtentX = rx;
    if (ry > m_RightExtentY) m_RightExtentY = ry;
    float x = leftStickX / m_LeftExtentX, y = leftStickY / m_LeftExtentY;
    float m = hypotf(x, y);
    if (m > 1.0f) { x /= m; y /= m; }
    leftStickX = x; leftStickY = y;
    x = rightStickX / m_RightExtentX; y = rightStickY / m_RightExtentY;
    m = hypotf(x, y);
    if (m > 1.0f) { x /= m; y /= m; }
    rightStickX = x; rightStickY = y;
}
