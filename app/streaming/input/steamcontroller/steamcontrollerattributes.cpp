// SPDX-License-Identifier: GPL-3.0-or-later
#include "steamcontrollerattributes.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
// GET_ATTRIBUTES_VALUES tags (SDL steam/controller_constants.h, confirmed on the wired unit)
const int TAG_CAPABILITIES = 2, TAG_FIRMWARE_BUILD_TIME = 4, TAG_RADIO_FIRMWARE_BUILD_TIME = 5,
          TAG_DONGLE_FIRMWARE_BUILD_TIME = 8, TAG_BOARD_REVISION = 9, TAG_BOOTLOADER_BUILD_TIME = 10;
const int STRING_TAG_BOARD_SERIAL = 0, STRING_TAG_UNIT_SERIAL = 1;

uint32_t u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

std::string format(const char* fmt, ...) {
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    return buffer;
}

SteamController::LogFunction s_LogFunction;
}

void SteamController::setLogFunction(LogFunction fn) { s_LogFunction = fn; }

uint64_t SteamController::nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void SteamController::logf(int level, const char* fmt, ...) {
    if (!s_LogFunction) return;
    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    s_LogFunction(level, buffer);
}

int SteamControllerAttributes::parseAttributeValues(const uint8_t* p, int length) {
    int pairs = 0;
    for (int i = 0; i + 5 <= length; i += 5, pairs++) {
        int tag = p[i];
        uint32_t value = u32(p + i + 1);
        switch (tag) {
        case TAG_CAPABILITIES:
            capabilities = value;
            validMask |= LI_SCATTR_CAPABILITIES;
            break;
        case TAG_FIRMWARE_BUILD_TIME:
            firmwareBuildTime = value;
            validMask |= LI_SCATTR_FIRMWARE_BUILD_TIME;
            break;
        case TAG_RADIO_FIRMWARE_BUILD_TIME:
            radioFirmwareBuildTime = value;
            validMask |= LI_SCATTR_RADIO_FW_BUILD_TIME;
            break;
        case TAG_DONGLE_FIRMWARE_BUILD_TIME:
            dongleFirmwareBuildTime = value;
            validMask |= LI_SCATTR_DONGLE_FW_BUILD_TIME;
            break;
        case TAG_BOARD_REVISION:
            boardRevision = value;
            validMask |= LI_SCATTR_BOARD_REVISION;
            break;
        case TAG_BOOTLOADER_BUILD_TIME:
            bootloaderBuildTime = value;
            validMask |= LI_SCATTR_BOOTLOADER_BUILD_TIME;
            break;
        default:
            // 1 product id and 11 connection interval describe the link, not the firmware
            break;
        }
    }
    return pairs;
}

bool SteamControllerAttributes::parseDeviceInfo(const uint8_t* p, int length) {
    // [sub id 00][firmware build u32][board revision u32][build id 16][unit serial 16]
    if (length < 25 || p[0] != 0) {
        return false;
    }
    std::string id = ascii(p + 9, std::min(16, length - 9));
    if (id.empty()) {
        return false;
    }
    buildId = id;
    validMask |= LI_SCATTR_BUILD_ID;
    if ((validMask & LI_SCATTR_FIRMWARE_BUILD_TIME) == 0) {
        firmwareBuildTime = u32(p + 1);
        validMask |= LI_SCATTR_FIRMWARE_BUILD_TIME;
    }
    if ((validMask & LI_SCATTR_BOARD_REVISION) == 0) {
        boardRevision = u32(p + 5);
        validMask |= LI_SCATTR_BOARD_REVISION;
    }
    if (length >= 41 && unitSerial.empty()) {
        std::string serial = ascii(p + 25, 16);
        if (!serial.empty()) {
            unitSerial = serial;
            validMask |= LI_SCATTR_SERIALS;
        }
    }
    return true;
}

bool SteamControllerAttributes::parseStringAttribute(const uint8_t* p, int length) {
    if (length < 2) {
        return false;
    }
    std::string text = ascii(p + 1, length - 1);
    if (text.empty()) {
        return false;
    }
    switch (p[0]) {
    case STRING_TAG_UNIT_SERIAL:
        unitSerial = text;
        break;
    case STRING_TAG_BOARD_SERIAL:
        boardSerial = text;
        break;
    default:
        return false;
    }
    validMask |= LI_SCATTR_SERIALS;
    return true;
}

std::string SteamControllerAttributes::describe() const {
    if (isEmpty()) {
        return "no firmware attributes read (the host keeps its defaults)";
    }
    auto date = [this](uint32_t unixSeconds, int bit) {
        return (validMask & bit) != 0 ? isoDate(unixSeconds) : std::string("n/a");
    };
    return "firmware build " + date(firmwareBuildTime, LI_SCATTR_FIRMWARE_BUILD_TIME)
            + format(" (0x%08x)", firmwareBuildTime)
            + ", bootloader " + date(bootloaderBuildTime, LI_SCATTR_BOOTLOADER_BUILD_TIME)
            + ", board rev " + ((validMask & LI_SCATTR_BOARD_REVISION) != 0 ? format("%u", boardRevision) : std::string("n/a"))
            + ", capabilities " + ((validMask & LI_SCATTR_CAPABILITIES) != 0 ? format("0x%x", capabilities) : std::string("n/a"))
            + ", build id " + ((validMask & LI_SCATTR_BUILD_ID) != 0 ? buildId : std::string("n/a"))
            + ", serial " + (unitSerial.empty() ? std::string("n/a") : unitSerial)
            + (boardSerial.empty() ? std::string() : " (board " + boardSerial + ")")
            + ", radio fw " + date(radioFirmwareBuildTime, LI_SCATTR_RADIO_FW_BUILD_TIME)
            + ", dongle fw " + date(dongleFirmwareBuildTime, LI_SCATTR_DONGLE_FW_BUILD_TIME);
}

std::string SteamControllerAttributes::isoDate(uint32_t unixSeconds) {
    uint64_t seconds = unixSeconds;
    int64_t days = (int64_t)(seconds / 86400), rest = (int64_t)(seconds % 86400);
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    int64_t z = days + 719468;
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1;
    int64_t month = mp < 10 ? mp + 3 : mp - 9;
    int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    return format("%04lld-%02lld-%02lld %02lld:%02lldZ", (long long)year, (long long)month, (long long)day,
                  (long long)(rest / 3600), (long long)((rest % 3600) / 60));
}

std::string SteamControllerAttributes::ascii(const uint8_t* p, int max) {
    std::string out;
    for (int i = 0; i < max && (int)out.size() < MAX_STRING; i++) {
        int c = p[i];
        if (c == 0) {
            break;
        }
        out.push_back(c >= 0x20 && c <= 0x7E ? (char)c : '?');
    }
    return out;
}

#if STEAM_CONTROLLER_LI_EXTENSIONS
LI_STEAM_CONTROLLER_ATTRIBUTES SteamControllerAttributes::toLi() const {
    LI_STEAM_CONTROLLER_ATTRIBUTES li;
    memset(&li, 0, sizeof(li));
    li.transport = transport;
    li.validMask = validMask;
    li.firmwareBuildTime = firmwareBuildTime;
    li.bootloaderBuildTime = bootloaderBuildTime;
    li.boardRevision = boardRevision;
    li.capabilities = capabilities;
    li.radioFirmwareBuildTime = radioFirmwareBuildTime;
    li.dongleFirmwareBuildTime = dongleFirmwareBuildTime;
    strncpy(li.buildId, buildId.c_str(), sizeof(li.buildId) - 1);
    strncpy(li.unitSerial, unitSerial.c_str(), sizeof(li.unitSerial) - 1);
    strncpy(li.boardSerial, boardSerial.c_str(), sizeof(li.boardSerial) - 1);
    return li;
}
#endif
