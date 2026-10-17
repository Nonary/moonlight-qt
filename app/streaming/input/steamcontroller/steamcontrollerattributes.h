// SPDX-License-Identifier: GPL-3.0-or-later
// The real Steam Controller (2026)'s firmware attributes, read by the link at setup and forwarded
// to a Vibepollo host (LI_CCAP_STEAM_ATTRIBUTES), whose virtual Steam Controller echoes them to
// Steam. Steam then shows the real unit's firmware build and offers a firmware update only when
// the real controller needs one. Port of the Android client's SteamControllerAttributes.
//
// Parsers take the payload that follows the feature report's [id][type][len]:
// GET_ATTRIBUTES_VALUES 0x83 as (tag u8, value u32 LE) pairs, GET_DEVICE_INFO 0xF2 sub 0 as
// [00][firmware build u32][board revision u32][build id 16][unit serial 16], GET_STRING_ATTRIBUTE
// 0xAE as [tag][chars]. Strings keep at most 15 printable ASCII characters.
#pragma once

#include "steamcontrollerprotocol.h"
#include <string>

struct SteamControllerAttributes {
    static const int MAX_STRING = 15;

    uint8_t transport = LI_SCATTR_TRANSPORT_USB;   // LI_SCATTR_TRANSPORT_*
    uint8_t validMask = 0;                          // LI_SCATTR_* bits
    uint32_t firmwareBuildTime = 0, bootloaderBuildTime = 0, boardRevision = 0, capabilities = 0;
    uint32_t radioFirmwareBuildTime = 0, dongleFirmwareBuildTime = 0;
    std::string buildId, unitSerial, boardSerial;

    SteamControllerAttributes() = default;
    explicit SteamControllerAttributes(uint8_t transport) : transport(transport) {}

    // True when no query produced a value; nothing is worth sending then.
    bool isEmpty() const { return validMask == 0; }

    // Returns how many (tag, value) pairs were taken from a GET_ATTRIBUTES_VALUES reply payload.
    int parseAttributeValues(const uint8_t* p, int length);
    // Returns true when a GET_DEVICE_INFO sub 0 payload supplied the build id.
    bool parseDeviceInfo(const uint8_t* p, int length);
    // Returns true when a GET_STRING_ATTRIBUTE payload ([tag][chars]) supplied a serial.
    bool parseStringAttribute(const uint8_t* p, int length);

    // The stream-log line: what was read and will reach the host.
    std::string describe() const;

    // Unix seconds (unsigned 32-bit) as "2026-07-07 23:04Z"; plain arithmetic, no time-zone data.
    static std::string isoDate(uint32_t unixSeconds);
    // Printable ASCII up to the first NUL, at most MAX_STRING characters; other bytes become '?'.
    static std::string ascii(const uint8_t* p, int max);

#if STEAM_CONTROLLER_LI_EXTENSIONS
    LI_STEAM_CONTROLLER_ATTRIBUTES toLi() const;
#endif
};
