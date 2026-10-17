// SPDX-License-Identifier: GPL-3.0-or-later
// Steam Controller (2026) wire constants shared by the decoder, the HID link and the host bridge.
//
// The protocol surface towards the host is the Steam extension of moonlight-common-c
// (jlobue10/moonlight-common-c, branch mac-pyrowave-steam). The numeric contract is restated
// here under #ifndef so the client builds against a common-c that predates it and still puts
// the same values on the wire; the two parts that need code in common-c (the attributes packet
// and the two listener callbacks) compile only when the extended header is present, which
// STEAM_CONTROLLER_LI_EXTENSIONS reports.
#pragma once

#include <Limelight.h>
#include <cstdint>

#ifdef LI_CCAP_STEAM_ATTRIBUTES
#define STEAM_CONTROLLER_LI_EXTENSIONS 1
#else
#define STEAM_CONTROLLER_LI_EXTENSIONS 0
#endif

#ifndef LI_CTYPE_STEAM
#define LI_CTYPE_STEAM 0x04
#endif
#ifndef LI_CCAP_DUAL_TOUCHPAD
#define LI_CCAP_DUAL_TOUCHPAD 0x100
#endif
#ifndef LI_CCAP_GRIP_SENSE
#define LI_CCAP_GRIP_SENSE 0x200
#endif
#ifndef LI_CCAP_STICK_TOUCH
#define LI_CCAP_STICK_TOUCH 0x400
#endif
#ifndef LI_CCAP_STEAM_HAPTIC
#define LI_CCAP_STEAM_HAPTIC 0x800
#endif
#ifndef LI_CCAP_SC26_BUTTONS
#define LI_CCAP_SC26_BUTTONS 0x1000
#endif
#ifndef LI_CCAP_STEAM_SETTINGS
#define LI_CCAP_STEAM_SETTINGS 0x2000
#endif
#ifndef LI_CCAP_STEAM_ATTRIBUTES
#define LI_CCAP_STEAM_ATTRIBUTES 0x4000
#endif

#ifndef LEFT_GRIP_TOUCH_FLAG
#define LEFT_GRIP_TOUCH_FLAG  0x400000
#define RIGHT_GRIP_TOUCH_FLAG 0x800000
#endif
#ifndef LEFT_STICK_TOUCH_FLAG
#define LEFT_STICK_TOUCH_FLAG  0x1000000
#define RIGHT_STICK_TOUCH_FLAG 0x2000000
#endif
#ifndef LEFT_PAD_CLICK_FLAG
#define LEFT_PAD_CLICK_FLAG      0x04000000
#define RIGHT_PAD_CLICK_FLAG     0x08000000
#define LEFT_TRIGGER_CLICK_FLAG  0x10000000
#define RIGHT_TRIGGER_CLICK_FLAG 0x20000000
#endif

#ifndef LI_SCATTR_FIRMWARE_BUILD_TIME
#define LI_SCATTR_FIRMWARE_BUILD_TIME   0x01
#define LI_SCATTR_BOOTLOADER_BUILD_TIME 0x02
#define LI_SCATTR_BOARD_REVISION        0x04
#define LI_SCATTR_CAPABILITIES          0x08
#define LI_SCATTR_RADIO_FW_BUILD_TIME   0x10
#define LI_SCATTR_DONGLE_FW_BUILD_TIME  0x20
#define LI_SCATTR_BUILD_ID              0x40
#define LI_SCATTR_SERIALS               0x80
#define LI_SCATTR_TRANSPORT_USB  0
#define LI_SCATTR_TRANSPORT_BLE  1
#define LI_SCATTR_TRANSPORT_PUCK 2
#endif

#ifndef STEAM_HAPTIC_REPORT_MAX
#define STEAM_HAPTIC_REPORT_MAX 12
#endif
#ifndef STEAM_SETTINGS_SET_VALUES
#define STEAM_SETTINGS_SET_VALUES 0x87
#define STEAM_SETTINGS_LOAD_DEFAULTS 0x8E
#endif

namespace SteamController {

constexpr uint16_t VENDOR_VALVE = 0x28DE;
constexpr uint16_t PRODUCT_WIRED = 0x1302;   // controller on a USB cable (HID interface 0)
constexpr uint16_t PRODUCT_PUCK = 0x1304;    // wireless receiver ("Proteus"), controllers on HID interfaces 2..5
constexpr uint16_t VENDOR_USAGE_PAGE = 0xFF00;

// Feature report 1 is the control channel: [01][type][len][payload...], 64 bytes, zero padded.
constexpr uint8_t FEATURE_REPORT_ID = 0x01;
constexpr int FEATURE_REPORT_SIZE = 64;
constexpr uint8_t CMD_SET_SETTINGS_VALUES = 0x87;
constexpr uint8_t CMD_GET_ATTRIBUTES_VALUES = 0x83;
constexpr uint8_t CMD_GET_STRING_ATTRIBUTE = 0xAE;
constexpr uint8_t CMD_GET_DEVICE_INFO = 0xF2;
// Steam's length byte on GET_STRING_ATTRIBUTE (the firmware reads only the tag).
constexpr uint8_t STRING_ATTRIBUTE_REQUEST_LENGTH = 0x15;

// Settings written at link setup (ids as in SDL's controller_constants.h).
constexpr uint8_t SETTING_LIZARD_MODE = 9;
constexpr uint8_t SETTING_IMU_MODE = 48;
constexpr uint8_t SETTING_STEAM_WATCHDOG_ENABLE = 71;
constexpr uint8_t IMU_MODE_RAW = 0x18;   // RAW_ACCEL | RAW_GYRO

// Input reports (numbered; byte 0 is the id)
constexpr uint8_t IN_STATE_WIRED = 0x42;        // 54 bytes
constexpr uint8_t IN_BATTERY = 0x43;            // 15 bytes
constexpr uint8_t IN_HAPTIC_ACK = 0x44;         // ignored
constexpr uint8_t IN_STATE = 0x45;              // 46 bytes
constexpr uint8_t IN_WIRELESS_EVENT = 0x46;     // [1] 1 disconnect, 2 connect
constexpr uint8_t IN_STATE_TIMESTAMPED = 0x47;  // 46 bytes, pads shifted by 2
constexpr uint8_t IN_WIRELESS_EVENT_ALT = 0x79;
constexpr uint8_t IN_RADIO_DIAGNOSTICS = 0x7B;  // ignored
constexpr int STATE_SIZE_WIRED = 54;
constexpr int STATE_SIZE_WIRELESS = 46;
constexpr int BATTERY_SIZE = 15;

// Output (haptic) reports and their exact lengths including the id
constexpr uint8_t OUT_RUMBLE = 0x80;
constexpr uint8_t OUT_PULSE = 0x81;
constexpr uint8_t OUT_HAPTIC_COMMAND = 0x82;
constexpr uint8_t OUT_LFO_TONE = 0x83;
constexpr uint8_t OUT_LOG_SWEEP = 0x84;
constexpr uint8_t OUT_SCRIPT = 0x85;

constexpr int outputReportSize(int id)
{
    return id == OUT_RUMBLE ? 10 : id == OUT_PULSE ? 8 : id == OUT_HAPTIC_COMMAND ? 4 :
           id == OUT_LFO_TONE ? 10 : id == OUT_LOG_SWEEP ? 9 : id == OUT_SCRIPT ? 4 : 0;
}

// Logged once per link with the attributes and shown in the preference's help text.
constexpr const char* FIRMWARE_UPDATE_WARNING =
    "NEVER start a controller firmware update from Steam while streaming."
    " The update would target the virtual controller on the host, not the real controller; it cannot succeed"
    " and may leave Steam confused. Update firmware with the controller connected directly to a PC running Steam.";

// Logging sink shared by the SDL-free parts. Levels follow SDL_LogPriority numbering so the
// app can bind it to SDL_Log*; tests capture lines.
enum LogLevel { LogInfo = 3, LogWarn = 4 };
typedef void (*LogFunction)(int level, const char* message);
void setLogFunction(LogFunction fn);
void logf(int level, const char* format, ...);
// Monotonic milliseconds for the link timers (std::chrono::steady_clock).
uint64_t nowMs();

} // namespace SteamController
