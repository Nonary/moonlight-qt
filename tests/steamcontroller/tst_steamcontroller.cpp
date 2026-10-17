// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises the production Steam Controller (2026) decoder, attribute parser and HID link with a
// fake transport and a virtual clock. No SDL, no device: the byte sequences are the Android
// client's regression harness (tools/regression/test_steam_usb.py), the real unit's replies and
// the Android BLE driver's settings relay and haptic knobs (SteamControllerBle.java).
#include "../../app/streaming/input/steamcontroller/steamcontrollerlink.h"
#include "../../app/streaming/input/steamcontroller/steamcontrollerhost.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace SteamController;

// Network boundary only: execute the real registry and link bodies without a stream.
extern "C" {
int LiSendControllerArrivalEvent(uint8_t, uint16_t, uint8_t, uint32_t, uint16_t) { return 0; }
int LiSendMultiControllerEvent(short, short, int, unsigned char, unsigned char, short, short, short, short) { return 0; }
int LiSendControllerTouchEvent2(uint8_t, uint8_t, uint8_t, uint32_t, float, float, float) { return 0; }
int LiSendControllerMotionEvent(uint8_t, uint8_t, float, float, float) { return 0; }
int LiSendControllerBatteryEvent(uint8_t, uint8_t, uint8_t) { return 0; }
#if STEAM_CONTROLLER_LI_EXTENSIONS
int LiSendSteamControllerAttributesEvent(uint8_t, const LI_STEAM_CONTROLLER_ATTRIBUTES*) { return 0; }
#endif
}

static int g_Checks, g_Failed;

static void check(bool condition, const char* name)
{
    g_Checks++;
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) g_Failed++;
}

static bool near(float a, float b) { return a - b < 0.0001f && b - a < 0.0001f; }

static std::vector<uint8_t> hex(const char* s)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        unsigned v;
        sscanf(s + i, "%2x", &v);
        out.push_back((uint8_t)v);
    }
    return out;
}

static void put16(std::vector<uint8_t>& b, int off, int v) { b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8); }

// ----- log capture -----
static std::vector<std::string> g_Log;
static void captureLog(int, const char* message) { g_Log.push_back(message); }
static int logCount(const char* needle)
{
    int n = 0;
    for (const std::string& line : g_Log) if (line.find(needle) != std::string::npos) n++;
    return n;
}
static std::string logLine(const char* needle)
{
    for (const std::string& line : g_Log) if (line.find(needle) != std::string::npos) return line;
    return std::string();
}
static std::string lastLogLine(const char* needle)
{
    for (auto it = g_Log.rbegin(); it != g_Log.rend(); ++it) if (it->find(needle) != std::string::npos) return *it;
    return std::string();
}
static bool has(const std::string& line, const char* needle) { return line.find(needle) != std::string::npos; }

// ----- fake HID -----
struct FakeState {
    std::vector<std::vector<uint8_t>> writes;     // output reports
    std::vector<std::vector<uint8_t>> features;   // feature report sends (64 bytes each)
    int gets = 0;
    int lastQuery = -1;
    std::map<int, std::vector<uint8_t>> replies;  // query key -> GET_REPORT reply (01 type len payload)
    bool failWrite = false;
    bool failFeature = false;
    bool open = false;
    bool vendor = true;   // the vendor collection answers a bare GET_REPORT of feature 1; mouse/keyboard collections do not

    static int queryKey(const uint8_t* b) { int t = b[1]; return t == 0x83 ? 0x83 : (t << 8) | b[3]; }
    // The wired unit's replies (libvirtualgamepad captures/sc26-steam-handshake.txt).
    void realUnit() {
        replies[0x83] = hex("01831e010213000002000000000a2ef9d26804e3854d6a094a0000000ba00f0000");
        replies[0xF200] = hex("01f22900e3854d6a4a0000003236653166376566313935350000000046584139393632343033373837000000");
        replies[0xAE01] = hex("01ae140146584139393632343033373837000000000000");
        replies[0xAE00] = hex("01ae14004d584132473632333036354434000000000000");
    }
    std::vector<std::vector<uint8_t>> settingsWrites() const {
        std::vector<std::vector<uint8_t>> out;
        for (const auto& f : features) if (f[1] == 0x87) out.push_back(f);
        return out;
    }
    std::vector<std::vector<uint8_t>> queries() const {
        std::vector<std::vector<uint8_t>> out;
        for (const auto& f : features) if (f[1] != 0x87) out.push_back(f);
        return out;
    }
    int countWrites(uint8_t id) const { int n = 0; for (const auto& w : writes) if (w[0] == id) n++; return n; }
    bool wrote(const std::vector<uint8_t>& report) const {
        for (const auto& w : writes) if (w == report) return true;
        return false;
    }
};

class FakeDevice final : public SteamHidDevice {
public:
    explicit FakeDevice(std::shared_ptr<FakeState> state) : m_State(std::move(state)) { m_State->open = true; }
    ~FakeDevice() override { m_State->open = false; }
    int read(uint8_t*, int, int) override { return 0; }
    int write(const uint8_t* data, int length) override {
        m_State->writes.emplace_back(data, data + length);
        return m_State->failWrite ? length - 1 : length;
    }
    int sendFeatureReport(const uint8_t* data, int length) override {
        m_State->features.emplace_back(data, data + length);
        if (data[1] != 0x87) m_State->lastQuery = FakeState::queryKey(data);
        return m_State->failFeature ? length - 1 : length;
    }
    int getFeatureReport(uint8_t* buffer, int length) override {
        m_State->gets++;
        if (m_State->lastQuery < 0) {
            if (!m_State->vendor || length < 3) return -1;
            buffer[0] = 1; buffer[1] = 0; buffer[2] = 0;
            return 3;
        }
        auto it = m_State->replies.find(m_State->lastQuery);
        if (it == m_State->replies.end()) return -1;
        int n = (int)it->second.size() < length ? (int)it->second.size() : length;
        memcpy(buffer, it->second.data(), n);
        return n;
    }
private:
    std::shared_ptr<FakeState> m_State;
};

class FakeTransport final : public SteamHidTransport {
public:
    std::vector<SteamHidDeviceInfo> infos;
    std::map<std::string, std::shared_ptr<FakeState>> states;
    std::vector<std::string> opened;
    std::vector<std::string> unopenable;
    std::vector<std::string> probeFails;   // paths whose GET_REPORT fails (not the vendor collection)

    SteamHidDeviceInfo add(const std::string& path, uint16_t product, int iface, uint16_t usagePage) {
        SteamHidDeviceInfo info;
        info.path = path; info.vendorId = 0x28DE; info.productId = product; info.interfaceNumber = iface; info.usagePage = usagePage;
        infos.push_back(info);
        states[path] = std::make_shared<FakeState>();
        return info;
    }
    std::shared_ptr<FakeState> state(const std::string& path) { return states[path]; }
    std::vector<SteamHidDeviceInfo> enumerate() override { return infos; }
    std::unique_ptr<SteamHidDevice> open(const SteamHidDeviceInfo& info) override {
        for (const auto& u : unopenable) if (u == info.path) return nullptr;
        opened.push_back(info.path);
        auto st = states[info.path];
        for (const auto& p : probeFails) if (p == info.path) st->vendor = false;
        return std::unique_ptr<SteamHidDevice>(new FakeDevice(st));
    }
};

// ----- listener -----
struct Events : SteamControllerLink::Listener {
    struct Touch { int link; uint8_t pad, event; uint32_t ptr; float x, y, p; };
    struct Motion { int link; uint8_t type; float x, y, z; };
    std::vector<std::pair<int, int>> connected, disconnected;   // (link id, generation)
    int states = 0, flags = 0; uint8_t lt = 0, rt = 0; short lsX = 0, lsY = 0, rsX = 0, rsY = 0; int lastStateLink = -1;
    std::vector<Touch> touch;
    std::vector<Motion> motion;
    int battery = -1, percent = -1;
    std::vector<std::pair<int, SteamControllerAttributes>> attrs;

    void linkConnected(SteamControllerLink& l, int g) override { connected.push_back({l.id(), g}); }
    void linkDisconnected(SteamControllerLink& l, int g) override { disconnected.push_back({l.id(), g}); }
    void linkState(SteamControllerLink& l, int, int f, uint8_t a, uint8_t b, short x, short y, short rx, short ry) override {
        states++; flags = f; lt = a; rt = b; lsX = x; lsY = y; rsX = rx; rsY = ry; lastStateLink = l.id();
    }
    void linkTouch(SteamControllerLink& l, int, uint8_t pad, uint8_t ev, uint32_t ptr, float x, float y, float p) override {
        touch.push_back({l.id(), pad, ev, ptr, x, y, p});
    }
    void linkMotion(SteamControllerLink& l, int, uint8_t type, float x, float y, float z) override { motion.push_back({l.id(), type, x, y, z}); }
    void linkBattery(SteamControllerLink&, int, uint8_t s, uint8_t p) override { battery = s; percent = p; }
    void linkAttributes(SteamControllerLink& l, int, const SteamControllerAttributes& a) override { attrs.push_back({l.id(), a}); }
    int connectedCount(int link) const { int n = 0; for (auto& c : connected) if (c.first == link) n++; return n; }
    int disconnectedCount(int link) const { int n = 0; for (auto& c : disconnected) if (c.first == link) n++; return n; }
};

// ----- report builders (the Android harness's) -----
static std::vector<uint8_t> state(int id)
{
    std::vector<uint8_t> r(id == 0x42 ? 54 : 46, 0);
    r[0] = (uint8_t)id; r[1] = 3;
    uint32_t buttons = 1 | (1 << 6) | (1 << 7) | (1 << 14) | (1 << 20) | (1 << 21) | (1 << 25) | (1 << 28) | (1 << 29);
    for (int i = 0; i < 4; i++) r[i + 2] = (uint8_t)(buttons >> (8 * i));
    put16(r, 6, 32767); put16(r, 8, 16384); put16(r, 10, 16384); put16(r, 12, -16384); put16(r, 14, -32767); put16(r, 16, 32767);
    int p = id == 0x47 ? 20 : 18;
    if (id == 0x47) put16(r, 18, 0xabcd);
    put16(r, p, -16384); put16(r, p + 2, 16384); put16(r, p + 4, 16384); put16(r, p + 6, 16384); put16(r, p + 8, -16384); put16(r, p + 10, 8192);
    put16(r, 34, 16384); put16(r, 36, -8192); put16(r, 38, 4096); put16(r, 40, 16384); put16(r, 42, -8192); put16(r, 44, 4096);
    return r;
}
static std::vector<uint8_t> imu(int id)
{
    std::vector<uint8_t> r = state(id);
    put16(r, 34, 100); put16(r, 36, -200); put16(r, 38, 16384); put16(r, 40, -3000); put16(r, 42, 2000); put16(r, 44, 1000);
    return r;
}
static std::vector<uint8_t> pulse(int side, int count) { return std::vector<uint8_t>{0x81, (uint8_t)side, 4, 0, 0, 0, (uint8_t)count, 0}; }
static std::vector<uint8_t> motorStop() { return std::vector<uint8_t>{0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }

static uint64_t g_Now = 100000;

// Runs the service loop until nothing is due, like the Android harness's Handler.runReady().
static void run(SteamControllerManager& m)
{
    for (int i = 0; i < 10000; i++) {
        uint64_t next = m.service(g_Now);
        if (next > g_Now) return;
    }
    throw std::runtime_error("service spin");
}
static void input(SteamControllerLink& link, const std::vector<uint8_t>& r, int n = -1)
{
    link.handleReport(r.data(), n < 0 ? (int)r.size() : n, g_Now);
}
static std::shared_ptr<SteamControllerLink> slot(SteamControllerManager& m, int iface)
{
    for (auto& l : m.links()) if (l->info().interfaceNumber == iface) return l;
    throw std::runtime_error("no such slot");
}

// ===== tests =====

static void testAttributes()
{
    SteamControllerAttributes a(LI_SCATTR_TRANSPORT_PUCK);
    check(a.isEmpty() && a.describe() == "no firmware attributes read (the host keeps its defaults)", "empty attributes describe the default");
    std::vector<uint8_t> v = hex("010213000002000000000a2ef9d26804e3854d6a094a0000000ba00f0000");
    check(a.parseAttributeValues(v.data(), (int)v.size()) == 6, "0x83 reply yields six (tag, u32) pairs");
    check(a.firmwareBuildTime == 0x6A4D85E3 && a.bootloaderBuildTime == 0x68D2F92E && a.boardRevision == 0x4A && a.capabilities == 0,
          "0x83 tags 4, 10, 9 and 2 decode; 1 and 11 are ignored");
    check(a.validMask == (LI_SCATTR_FIRMWARE_BUILD_TIME | LI_SCATTR_BOOTLOADER_BUILD_TIME | LI_SCATTR_BOARD_REVISION | LI_SCATTR_CAPABILITIES),
          "0x83 alone sets exactly its four valid bits");
    std::vector<uint8_t> f2 = hex("00e3854d6a4a0000003236653166376566313935350000000046584139393632343033373837000000");
    check(a.parseDeviceInfo(f2.data(), (int)f2.size()) && a.buildId == "26e1f7ef1955" && a.unitSerial == "FXA9962403787",
          "0xF2 sub 0 supplies the build id and the unit serial");
    std::vector<uint8_t> board = hex("004d584132473632333036354434000000000000");
    check(a.parseStringAttribute(board.data(), (int)board.size()) && a.boardSerial == "MXA2G623065D4", "0xAE tag 0 supplies the board serial");
    std::vector<uint8_t> unit = hex("0146584139393632343033373837000000000000");
    check(a.parseStringAttribute(unit.data(), (int)unit.size()) && a.unitSerial == "FXA9962403787", "0xAE tag 1 supplies the unit serial");
    check(a.validMask == (LI_SCATTR_FIRMWARE_BUILD_TIME | LI_SCATTR_BOOTLOADER_BUILD_TIME | LI_SCATTR_BOARD_REVISION | LI_SCATTR_CAPABILITIES | LI_SCATTR_BUILD_ID | LI_SCATTR_SERIALS),
          "the real unit's four replies set six valid bits (no radio or dongle build time)");
    std::string d = a.describe();
    check(d.find("firmware build 2026-07-07 23:04Z (0x6a4d85e3), bootloader 2025-09-23") == 0 && d.find("board rev 74") != std::string::npos
          && d.find("capabilities 0x0") != std::string::npos && d.find("build id 26e1f7ef1955") != std::string::npos
          && d.find("serial FXA9962403787 (board MXA2G623065D4)") != std::string::npos && d.find("radio fw n/a, dongle fw n/a") != std::string::npos,
          "describe() shows ISO dates and every field as the Android client logs it");
    // Edge cases
    SteamControllerAttributes b(LI_SCATTR_TRANSPORT_USB);
    std::vector<uint8_t> shortInfo = hex("00e3854d6a4a000000323665");
    check(!b.parseDeviceInfo(shortInfo.data(), (int)shortInfo.size()) && b.isEmpty(), "a 0xF2 payload under 25 bytes is rejected");
    std::vector<uint8_t> noSerial = hex("00e3854d6a4a00000032366531663765663139353500000000000000");
    check(b.parseDeviceInfo(noSerial.data(), (int)noSerial.size()) && b.unitSerial.empty() && (b.validMask & LI_SCATTR_SERIALS) == 0
          && b.firmwareBuildTime == 0x6A4D85E3 && b.boardRevision == 0x4A,
          "a 25..40 byte 0xF2 payload supplies build id, firmware time and board revision but no serial");
    uint8_t odd[] = {1, 'A', 0x01, 'B', 0x7F, 0};
    check(SteamControllerAttributes::ascii(odd + 1, 5) == "A?B?", "ascii() stops at NUL and replaces non-printable bytes with '?'");
    uint8_t longName[20]; memset(longName, 'x', sizeof(longName));
    check(SteamControllerAttributes::ascii(longName, 20).size() == 15, "ascii() keeps at most 15 characters");
    uint8_t badTag[] = {7, 'A', 'B'};
    check(!b.parseStringAttribute(badTag, 3) && b.boardSerial.empty(), "an unknown string tag is ignored");
    check(SteamControllerAttributes::isoDate(0) == "1970-01-01 00:00Z" && SteamControllerAttributes::isoDate(0x68D2F92E) == "2025-09-23 19:46Z", "isoDate handles the epoch and the bootloader time");
#if STEAM_CONTROLLER_LI_EXTENSIONS
    LI_STEAM_CONTROLLER_ATTRIBUTES li = a.toLi();
    check(li.transport == LI_SCATTR_TRANSPORT_PUCK && li.validMask == a.validMask && li.firmwareBuildTime == 0x6A4D85E3
          && strcmp(li.buildId, "26e1f7ef1955") == 0 && strcmp(li.unitSerial, "FXA9962403787") == 0 && strcmp(li.boardSerial, "MXA2G623065D4") == 0
          && li.reserved[0] == 0 && li.reserved[1] == 0, "toLi() fills the common-c struct, NUL padded");
#endif
}

struct CoreSink : SteamControllerSink {
    int states = 0, flags = 0; uint8_t lt = 0, rt = 0; short lsX = 0, lsY = 0, rsX = 0, rsY = 0;
    std::vector<Events::Touch> touch; std::vector<Events::Motion> motion; int battery = -1, percent = -1;
    void steamState(int f, uint8_t a, uint8_t b, short x, short y, short rx, short ry) override { states++; flags = f; lt = a; rt = b; lsX = x; lsY = y; rsX = rx; rsY = ry; }
    void steamTouch(uint8_t pad, uint8_t ev, uint32_t ptr, float x, float y, float p) override { touch.push_back({0, pad, ev, ptr, x, y, p}); }
    void steamMotion(uint8_t type, float x, float y, float z) override { motion.push_back({0, type, x, y, z}); }
    void steamBattery(uint8_t s, uint8_t p) override { battery = s; percent = p; }
};

static void testCore()
{
    SteamControllerConfig cfg;   // defaults: motion, split pads, paddles, no stretch
    check(SteamControllerCore(cfg, nullptr).capabilities() == (LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE | LI_CCAP_TOUCHPAD | LI_CCAP_BATTERY_STATE
          | LI_CCAP_GRIP_SENSE | LI_CCAP_STICK_TOUCH | LI_CCAP_STEAM_HAPTIC | LI_CCAP_SC26_BUTTONS | LI_CCAP_STEAM_ATTRIBUTES | LI_CCAP_GYRO | LI_CCAP_ACCEL),
          "default capabilities: no DUAL_TOUCHPAD in split mode, motion on, no STEAM_SETTINGS");
    SteamControllerConfig dual = cfg; dual.splitPads = false; dual.motion = false;
    check(SteamControllerCore(dual, nullptr).capabilities() == (LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE | LI_CCAP_TOUCHPAD | LI_CCAP_DUAL_TOUCHPAD
          | LI_CCAP_BATTERY_STATE | LI_CCAP_GRIP_SENSE | LI_CCAP_STICK_TOUCH | LI_CCAP_STEAM_HAPTIC | LI_CCAP_SC26_BUTTONS | LI_CCAP_STEAM_ATTRIBUTES),
          "dual pads advertise DUAL_TOUCHPAD; motion off drops GYRO and ACCEL");
    check(SteamControllerCore::supportedButtonFlags() == (uint32_t)(A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG
          | LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG | PADDLE1_FLAG | PADDLE2_FLAG | PADDLE3_FLAG | PADDLE4_FLAG
          | TOUCHPAD_FLAG | MISC_FLAG | LEFT_GRIP_TOUCH_FLAG | RIGHT_GRIP_TOUCH_FLAG | LEFT_STICK_TOUCH_FLAG | RIGHT_STICK_TOUCH_FLAG
          | LEFT_PAD_CLICK_FLAG | RIGHT_PAD_CLICK_FLAG | LEFT_TRIGGER_CLICK_FLAG | RIGHT_TRIGGER_CLICK_FLAG), "supported buttons are the Android core's full set");

    CoreSink sink;
    SteamControllerConfig dualMotion = cfg; dualMotion.splitPads = false;
    SteamControllerCore core(dualMotion, &sink);
    core.setMotionRate(LI_MOTION_TYPE_GYRO, 250);
    core.setMotionRate(LI_MOTION_TYPE_ACCEL, 250);
    std::vector<uint8_t> s = state(0x42);
    for (int n = 1; n < 45; n++) core.handleState(s.data(), n, 1, false, 0);
    check(sink.states == 0 && sink.touch.empty(), "a state shorter than 45 bytes after the id is ignored");
    core.handleState(s.data(), 54, 1, false, 0);
    const int expectedFlags = A_FLAG | PLAY_FLAG | BACK_FLAG | PADDLE1_FLAG | LEFT_GRIP_TOUCH_FLAG | RIGHT_GRIP_TOUCH_FLAG | RIGHT_STICK_TOUCH_FLAG;
    check(sink.states == 1 && sink.flags == expectedFlags, "buttons, paddles, start/back and capacitive sensors map to the wire flags");
    check(sink.lt == 255 && sink.rt == 127, "triggers scale to bytes (32767 -> 255, 16384 -> 127)");
    check(sink.lsX == 16384 && sink.lsY == -16384 && sink.rsX == -32767 && sink.rsY == 32767,
          "sticks reach the wire with the controller's up-positive sign (as sendGamepadState sends SDL pads)");
    check(sink.touch.size() == 2 && sink.touch[0].pad == 0 && sink.touch[0].ptr == 0 && sink.touch[0].event == LI_TOUCH_EVENT_DOWN
          && near(sink.touch[0].x, .25f) && near(sink.touch[0].y, .25f) && near(sink.touch[0].p, .5f)
          && sink.touch[1].pad == 1 && sink.touch[1].ptr == 0 && near(sink.touch[1].x, .75f) && near(sink.touch[1].y, .75f) && near(sink.touch[1].p, .25f),
          "dual mode: both touchpads keep independent positions and pressure on their own index");
    check(sink.motion.size() == 2 && sink.motion[0].type == LI_MOTION_TYPE_GYRO && near(sink.motion[0].x, 1000) && near(sink.motion[0].y, 250) && near(sink.motion[0].z, 500)
          && sink.motion[1].type == LI_MOTION_TYPE_ACCEL && near(sink.motion[1].x, 9.80665f), "gyro and accelerometer units and axes match the host protocol");
    core.handleState(s.data(), 54, 1, false, 4);
    check(sink.states == 1 && sink.touch.size() == 2 && sink.motion.size() == 2, "unchanged buttons, touch moves and IMU words are deduplicated");

    // Split mode
    CoreSink split;
    SteamControllerCore scored(cfg, &split);
    scored.handleState(s.data(), 54, 1, false, 0);
    check(split.touch.size() == 2 && split.touch[0].pad == 0 && split.touch[0].ptr == 0 && near(split.touch[0].x, .125f) && near(split.touch[0].y, .25f)
          && split.touch[1].pad == 0 && split.touch[1].ptr == 1 && near(split.touch[1].x, .875f) && near(split.touch[1].y, .75f),
          "split mode: pads are fingers 0/1 on touchpad 0, each squeezed into its half");
    std::vector<uint8_t> released = state(0x42);
    released[5] &= (uint8_t)~0x02;   // bit 25 left pad touch
    scored.handleState(released.data(), 54, 1, false, 0);
    check(split.touch.size() == 3 && split.touch[2].event == LI_TOUCH_EVENT_UP && split.touch[2].ptr == 0, "lifting a pad sends UP for its finger");
    scored.releaseTouches();
    check(split.touch.size() == 4 && split.touch[3].event == LI_TOUCH_EVENT_UP && split.touch[3].ptr == 1 && split.touch[3].pad == 0, "releaseTouches sends UP for the pad still down");
    scored.releaseTouches();
    check(split.touch.size() == 4, "releaseTouches is idempotent");

    // 0x45 and 0x47 layouts
    CoreSink wl;
    SteamControllerCore wcore(dualMotion, &wl);
    wcore.setMotionRate(LI_MOTION_TYPE_GYRO, 250);
    std::vector<uint8_t> s45 = state(0x45), s47 = state(0x47);
    wcore.handleState(s45.data(), 46, 1, false, 0);
    wcore.resetInputState();
    wcore.handleState(s47.data(), 46, 1, true, 4);
    check(wl.touch.size() == 4 && near(wl.touch[0].x, .25f) && near(wl.touch[2].x, .25f) && near(wl.touch[3].x, .75f) && near(wl.touch[3].p, .25f)
          && wl.states == 2 && wl.flags == expectedFlags, "0x45 and timestamped 0x47 reports decode without shifting pad or IMU fields");
    std::vector<uint8_t> i42 = imu(0x42), i45 = imu(0x45), i47 = imu(0x47);
    CoreSink im; SteamControllerCore icore(dualMotion, &im);
    icore.setMotionRate(LI_MOTION_TYPE_GYRO, 250); icore.setMotionRate(LI_MOTION_TYPE_ACCEL, 250);
    icore.handleState(i42.data(), 54, 1, false, 0);
    CoreSink im2; SteamControllerCore icore2(dualMotion, &im2);
    icore2.setMotionRate(LI_MOTION_TYPE_GYRO, 250); icore2.setMotionRate(LI_MOTION_TYPE_ACCEL, 250);
    icore2.handleState(i47.data(), 46, 1, true, 0);
    check(im.motion.size() == 2 && im2.motion.size() == 2 && near(im.motion[0].x, im2.motion[0].x) && near(im.motion[0].z, im2.motion[0].z) && near(im.motion[1].y, im2.motion[1].y),
          "a wired 0x42 report and a puck 0x47 frame with the same IMU words report the same vectors");
    check(near(im.motion[0].x, -3000 * (2000.0f / 32768.0f)) && near(im.motion[0].y, 1000 * (2000.0f / 32768.0f)) && near(im.motion[0].z, -2000 * (2000.0f / 32768.0f))
          && near(im.motion[1].x, 100 * (2.0f * 9.80665f / 32768.0f)) && near(im.motion[1].y, 16384 * (2.0f * 9.80665f / 32768.0f)) && near(im.motion[1].z, 200 * (2.0f * 9.80665f / 32768.0f)),
          "the IMU words map as x, z, -y at 2000 deg/s and 2 g full scale");

    // Motion gating and decimation
    CoreSink gated; SteamControllerCore gcore(dualMotion, &gated);
    gcore.handleState(i42.data(), 54, 1, false, 0);
    check(gated.motion.empty(), "no motion is reported until the host asks for it");
    gcore.setMotionRate(LI_MOTION_TYPE_GYRO, 100);
    for (int t = 0; t <= 12; t += 4) {
        std::vector<uint8_t> r = imu(0x42); put16(r, 40, (int16_t)(-3000 + t)); gcore.handleState(r.data(), 54, 1, false, t);
    }
    check(gated.motion.size() == 2 && gated.motion[0].type == LI_MOTION_TYPE_GYRO, "a 100 Hz request decimates the 250 Hz stream (t=0 and t=12 of 0,4,8,12)");
    gcore.setMotionRate(LI_MOTION_TYPE_GYRO, 0);
    gcore.handleState(i45.data(), 46, 1, false, 100);
    check(gated.motion.size() == 2, "rate 0 stops motion reports");

    // Clicks
    CoreSink cs; SteamControllerCore ccore(cfg, &cs);
    std::vector<uint8_t> clicks = state(0x42); clicks[5] |= 4; clicks[4] |= 128;
    ccore.handleState(clicks.data(), 54, 1, false, 0);
    check((cs.flags & (LEFT_PAD_CLICK_FLAG | RIGHT_PAD_CLICK_FLAG | LEFT_TRIGGER_CLICK_FLAG | RIGHT_TRIGGER_CLICK_FLAG | TOUCHPAD_FLAG))
          == (LEFT_PAD_CLICK_FLAG | RIGHT_TRIGGER_CLICK_FLAG | TOUCHPAD_FLAG), "left pad and right trigger clicks are independent and keep TOUCHPAD_FLAG");
    ccore.handleState(s.data(), 54, 1, false, 0);
    check((cs.flags & (LEFT_PAD_CLICK_FLAG | RIGHT_TRIGGER_CLICK_FLAG | TOUCHPAD_FLAG)) == 0, "click releases survive deduplication");
    std::vector<uint8_t> all = state(0x42);
    uint32_t every = 0x3FFFFFFF; for (int i = 0; i < 4; i++) all[i + 2] = (uint8_t)(every >> (8 * i));
    ccore.handleState(all.data(), 54, 1, false, 0);
    check((cs.flags & (MISC_FLAG | SPECIAL_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | LB_FLAG | RB_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | X_FLAG | Y_FLAG | B_FLAG
          | PADDLE1_FLAG | PADDLE2_FLAG | PADDLE3_FLAG | PADDLE4_FLAG | LEFT_STICK_TOUCH_FLAG | LEFT_TRIGGER_CLICK_FLAG | RIGHT_PAD_CLICK_FLAG))
          == (MISC_FLAG | SPECIAL_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | LB_FLAG | RB_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | X_FLAG | Y_FLAG | B_FLAG
          | PADDLE1_FLAG | PADDLE2_FLAG | PADDLE3_FLAG | PADDLE4_FLAG | LEFT_STICK_TOUCH_FLAG | LEFT_TRIGGER_CLICK_FLAG | RIGHT_PAD_CLICK_FLAG),
          "every button bit has a wire flag (QAM -> MISC, Steam -> SPECIAL, four paddles)");

    // Grip modes
    SteamControllerConfig ds4 = cfg; ds4.grips = SteamControllerConfig::GripsDs4;
    CoreSink d; SteamControllerCore dcore(ds4, &d);
    std::vector<uint8_t> grips = state(0x42);
    uint32_t gb = (1 << 7) | (1 << 8) | (1 << 17) | (1 << 18); for (int i = 0; i < 4; i++) grips[i + 2] = (uint8_t)(gb >> (8 * i));
    dcore.handleState(grips.data(), 54, 1, false, 0);
    check(d.flags == (MISC_FLAG | TOUCHPAD_FLAG | LS_CLK_FLAG | RS_CLK_FLAG), "ds4 grips: L4 Share, R4 touchpad click, L5 L3, R5 R3");
    SteamControllerConfig off = cfg; off.grips = SteamControllerConfig::GripsOff;
    CoreSink o; SteamControllerCore ocore(off, &o);
    ocore.handleState(grips.data(), 54, 1, false, 0);
    check(o.flags == 0, "grips off: the four grip buttons send nothing");

    // Trigger deadzone and stick clamping
    CoreSink tz; SteamControllerCore tcore(cfg, &tz);
    std::vector<uint8_t> tr = state(0x42); put16(tr, 6, 4000); put16(tr, 8, 4300); put16(tr, 10, -32768); put16(tr, 12, 32767);
    tcore.handleState(tr.data(), 54, 1, false, 0);
    check(tz.lt == 0 && tz.rt == 33 && tz.lsX == -32767 && tz.lsY == 32767, "trigger deadzone 0.13 and stick clamp to +/-32767");

    // Stick stretch
    SteamControllerConfig stretch = cfg; stretch.stickStretch = true;
    CoreSink st; SteamControllerCore scoreS(stretch, &st);
    std::vector<uint8_t> half = state(0x42); put16(half, 10, 16384); put16(half, 12, 0); put16(half, 14, 0); put16(half, 16, 0);
    scoreS.handleState(half.data(), 54, 1, false, 0);
    check(st.lsX == 21845 && st.lsY == 0, "stretch divides by the 0.75 floor extent (0.5 -> 0.667)");
    std::vector<uint8_t> full = state(0x42); put16(full, 10, 32767); put16(full, 12, 0); put16(full, 14, 0); put16(full, 16, 0);
    scoreS.handleState(full.data(), 54, 1, false, 0);
    scoreS.handleState(half.data(), 54, 1, false, 0);
    check(st.lsX == 16384, "after full deflection the extent is 1.0 and 0.5 passes unchanged");
    std::vector<uint8_t> diag = state(0x42); put16(diag, 10, 32767); put16(diag, 12, 32767); put16(diag, 14, 0); put16(diag, 16, 0);
    scoreS.handleState(diag.data(), 54, 1, false, 0);
    check(st.lsX == 23170 && st.lsY == 23170, "stretched diagonals are clamped to the unit circle");
    scoreS.resetInputState();
    scoreS.handleState(half.data(), 54, 1, false, 0);
    check(st.lsX == 21845, "resetInputState forgets the session extents");

    // Battery
    CoreSink bs; SteamControllerCore bcore(cfg, &bs);
    uint8_t batt[15] = {0x43, 4, 100};
    bcore.handleBattery(batt, 15, 1);
    check(bs.battery == LI_BATTERY_STATE_FULL && bs.percent == 100, "charge state 4 is FULL");
    batt[1] = 1; batt[2] = 200; bcore.handleBattery(batt, 15, 1);
    check(bs.battery == LI_BATTERY_STATE_DISCHARGING && bs.percent == 100, "charge state 1 is DISCHARGING and the percentage is capped at 100");
    batt[1] = 3; batt[2] = 42; bcore.handleBattery(batt, 15, 1);
    check(bs.battery == LI_BATTERY_STATE_CHARGING && bs.percent == 42, "charge states 2 and 3 are CHARGING");
    batt[1] = 0; bcore.handleBattery(batt, 15, 1);
    check(bs.battery == LI_BATTERY_STATE_UNKNOWN, "charge state 0 (not measured yet) is UNKNOWN");
}

static void testLink()
{
    SteamControllerConfig cfg; cfg.splitPads = false;   // the Android harness's configuration
    FakeTransport io;
    // Puck in shuffled descriptor order, plus the receiver's own interfaces and the mouse/keyboard
    // collections of slot 2 (Windows lists them as separate paths).
    io.add("puck/if0", 0x1304, 0, 0xFF00);
    io.add("puck/if5", 0x1304, 5, 0xFF00);
    io.add("puck/if1", 0x1304, 1, 0xFF00);
    io.add("puck/if3", 0x1304, 3, 0xFF00);
    io.add("puck/if2/col1", 0x1304, 2, 0x0001);
    io.add("puck/if2/col2", 0x1304, 2, 0x0001);
    io.add("puck/if2", 0x1304, 2, 0xFF00);
    io.add("puck/if4", 0x1304, 4, 0xFF00);
    io.add("other-valve", 0x1305, 0, 0xFF00);   // a Valve PID the driver does not know
    io.add("old-dongle", 0x1142, 1, 0xFF00);
    io.probeFails = {"puck/if2/col1", "puck/if2/col2"};
    SteamHidDeviceInfo other; other.path = "other"; other.vendorId = 0x054c; other.productId = 0x1304; other.interfaceNumber = 2; other.usagePage = 0xFF00;
    io.infos.push_back(other);
    Events e;
    SteamControllerManager m(io, cfg, 500, &e);
    int opened = 0, closing = 0;
    m.onLinkOpened = [&](const std::shared_ptr<SteamControllerLink>&) { opened++; };
    m.onLinkClosing = [&](const std::shared_ptr<SteamControllerLink>&) { closing++; };
    run(m);
    auto links = m.links();
    bool slots = links.size() == 4;
    for (auto& l : links) slots &= l->isPuck() && l->info().interfaceNumber >= 2 && l->info().interfaceNumber <= 5 && l->info().usagePage == 0xFF00;
    check(slots && opened == 4, "only the puck's vendor collections on interfaces 2..5 are opened, in shuffled descriptor order");
    check(io.state("puck/if2/col1")->gets == 1 && !io.state("puck/if2/col1")->open && io.state("puck/if2")->gets == 0,
          "a non-vendor collection is probed once with a feature report 1 read and refused; the 0xff00 path is taken without a probe");
    check(e.connected.empty() && io.state("puck/if5")->features.empty() && io.state("puck/if5")->writes.empty(),
          "an empty receiver creates no phantom gamepads and receives no controller writes");
    check(logCount("opened puck/if5") == 1 && logLine("opened puck/if5").find("usage page 0xff00") != std::string::npos, "what was picked is logged");
    run(m);
    check(m.links().size() == 4 && opened == 4, "a second rescan does not reopen or duplicate links");

    auto s2 = slot(m, 2), s3 = slot(m, 3), s4 = slot(m, 4), s5 = slot(m, 5);
    check(s5->name() == "USB slot 5" && s5->slot() == 3 && s2->slot() == 0, "puck slots are named by interface and numbered 0..3");
    for (int id : {0x42, 0x45, 0x47}) { std::vector<uint8_t> r = state(id); for (int n = 1; n < (int)r.size(); n++) input(*s3, r, n); }
    input(*s3, {0x40, 2}); input(*s3, {0x79, 2}, 1);
    check(e.connected.empty(), "truncated state and wireless reports and lizard reports cannot announce a controller");

    // Connect on the first full state report
    io.state("puck/if5")->realUnit();
    input(*s5, state(0x42));
    check(e.connected.size() == 1 && e.connected[0].first == s5->id() && e.connected[0].second == 1 && s5->connected() && s5->generation() == 1,
          "the first valid report discovers a preconnected controller in the correct slot");
    check(e.states == 1 && e.lastStateLink == s5->id() && e.flags == (A_FLAG | PLAY_FLAG | BACK_FLAG | PADDLE1_FLAG | LEFT_GRIP_TOUCH_FLAG | RIGHT_GRIP_TOUCH_FLAG | RIGHT_STICK_TOUCH_FLAG)
          && e.lsX == 16384 && e.lsY == -16384 && e.lt == 255 && e.rt == 127, "the connecting report's state reaches the listener");
    run(m);
    auto st5 = io.state("puck/if5");
    auto cfgs = st5->settingsWrites();
    check(cfgs.size() == 1 && cfgs[0].size() == 64 && cfgs[0][0] == 1 && cfgs[0][1] == 0x87 && cfgs[0][2] == 9 && cfgs[0][3] == 9 && cfgs[0][4] == 0
          && cfgs[0][5] == 0 && cfgs[0][6] == 48 && cfgs[0][7] == 0x18 && cfgs[0][8] == 0 && cfgs[0][9] == 71 && cfgs[0][10] == 0 && cfgs[0][11] == 0,
          "settings use feature report 1 with lizard off, raw IMU and the firmware watchdog left disabled");
    check(logCount("link settings") == 1 && logLine("link settings").find("Steam Controller USB slot 5: link settings (feature report 1, interface 5): lizard mode 0, IMU mode 0x18, Steam watchdog 0 -> accepted") != std::string::npos,
          "the connect-time settings write is logged once with its triples and result");
    auto q = st5->queries();
    check(q.size() == 4 && q[0][1] == 0x83 && q[0][2] == 0 && q[1][1] == 0xF2 && q[1][2] == 1 && q[1][3] == 0
          && q[2][1] == 0xAE && q[2][2] == 0x15 && q[2][3] == 1 && q[3][1] == 0xAE && q[3][2] == 0x15 && q[3][3] == 0
          && st5->features.size() == 5 && st5->features[0][1] == 0x87 && st5->gets == 4,
          "after the settings write the slot asks 83 00, F2 01 00, AE 15 01 and AE 15 00, each followed by one GET_REPORT");
    check(e.attrs.size() == 1 && e.attrs[0].first == s5->id() && e.attrs[0].second.transport == LI_SCATTR_TRANSPORT_PUCK
          && e.attrs[0].second.validMask == (LI_SCATTR_FIRMWARE_BUILD_TIME | LI_SCATTR_BOOTLOADER_BUILD_TIME | LI_SCATTR_BOARD_REVISION | LI_SCATTR_CAPABILITIES | LI_SCATTR_BUILD_ID | LI_SCATTR_SERIALS),
          "the puck slot reports one attribute set, transport puck, with the six fields the wired unit answers");
    const SteamControllerAttributes real = e.attrs.empty() ? SteamControllerAttributes() : e.attrs[0].second;
    check(real.firmwareBuildTime == 0x6A4D85E3 && real.bootloaderBuildTime == 0x68D2F92E && real.boardRevision == 0x4A && real.capabilities == 0
          && real.buildId == "26e1f7ef1955" && real.unitSerial == "FXA9962403787" && real.boardSerial == "MXA2G623065D4",
          "the real unit's replies decode to its build times, board revision, build id and both serials");
    check(s5->attributes().validMask == real.validMask && s5->attributes().unitSerial == "FXA9962403787", "the link keeps the attributes so a later arrival can send them");
    check(logCount("firmware build") == 1 && logLine("firmware build").find("Steam Controller puck (USB slot 5): firmware build 2026-07-07 23:04Z (0x6a4d85e3), bootloader 2025-09-23") != std::string::npos
          && logLine("firmware build").find("serial FXA9962403787 (board MXA2G623065D4)") != std::string::npos, "the stream log shows what is forwarded, with ISO dates");
    check(logCount(FIRMWARE_UPDATE_WARNING) == 1, "the firmware-update warning is logged verbatim with the attributes");
    check(e.touch.size() == 2 && near(e.touch[0].x, .25f) && near(e.touch[1].x, .75f), "pads of the connecting report reach the listener");
    check(e.motion.empty(), "no motion until the host enables it");
    s5->setMotionRate(LI_MOTION_TYPE_GYRO, 250);
    s5->setMotionRate(LI_MOTION_TYPE_ACCEL, 250);
    input(*s5, imu(0x42));
    check(e.motion.size() == 2 && near(e.motion[0].x, -3000 * (2000.0f / 32768.0f)), "motion flows once enabled for the slot");

    // Replay for an arrival: attributes and battery again, state and pad DOWNs again
    uint8_t battery[15] = {0x43, 4, 100};
    input(*s5, std::vector<uint8_t>(battery, battery + 15), 14);
    check(e.battery == -1, "a truncated battery report is ignored");
    input(*s5, std::vector<uint8_t>(battery, battery + 15));
    check(e.battery == LI_BATTERY_STATE_FULL && e.percent == 100 && s5->hasBattery(), "battery state and level are forwarded and cached");
    e.battery = -1; size_t attrsBefore = e.attrs.size(); int statesBefore = e.states; size_t touchesBefore = e.touch.size();
    s5->replayForArrival(s5->generation());
    check(e.attrs.size() == attrsBefore + 1 && e.attrs.back().second.unitSerial == "FXA9962403787" && e.battery == LI_BATTERY_STATE_FULL,
          "replayForArrival re-sends the cached attributes and battery");
    s5->replayForArrival(99);
    check(e.attrs.size() == attrsBefore + 1, "replayForArrival ignores a stale generation");
    s5->requestInputReset();
    input(*s5, imu(0x42));
    check(e.states == statesBefore + 1 && e.touch.size() == touchesBefore + 2 && e.touch.back().event == LI_TOUCH_EVENT_DOWN,
          "an input reset resends the held state and pad DOWNs without new attribute queries");
    check(st5->gets == 4, "replay and reset keep the one-query-per-link attribute budget");

    // Other slots and formats
    input(*s2, state(0x45)); input(*s4, state(0x47));
    check(e.connectedCount(s2->id()) == 1 && e.connectedCount(s4->id()) == 1 && e.touch.size() == touchesBefore + 6, "0x45 and 0x47 connect their own slots");
    run(m);
    check(io.state("puck/if2")->settingsWrites().size() == 1 && io.state("puck/if4")->settingsWrites().size() == 1 && io.state("puck/if3")->features.empty(),
          "each connected slot gets its own settings write; an idle slot gets none");
    input(*s3, state(0x42)); run(m);
    check(e.connected.size() == 4, "four concurrent controllers have independent identities");

    // Keep-alive cadence
    size_t before = st5->settingsWrites().size();
    g_Now += 999; run(m);
    check(st5->settingsWrites().size() == before, "the keep-alive waits a full second");
    g_Now += 1; run(m);
    check(st5->settingsWrites().size() == before + 1 && logCount("USB slot 5: link settings") == 1, "the keep-alive repeats the settings write without logging it again");
    check(st5->gets == 4, "later keep-alives do not repeat the attribute queries");

    // Haptics
    st5->writes.clear();
    for (int id = 0x80; id <= 0x85; id++) {
        int n = outputReportSize(id);
        std::vector<uint8_t> r(n, 0); r[0] = (uint8_t)id; r[1] = 1; if (id == 0x81) r[6] = 1;
        s5->steamHaptic(r.data(), n, g_Now); run(m);
        char name[96]; snprintf(name, sizeof(name), "native haptic report 0x%02x reaches only its slot unchanged", id);
        check(!st5->writes.empty() && st5->writes.back() == r && io.state("puck/if4")->writes.empty(), name);
    }
    size_t sent = st5->writes.size();
    uint8_t shortReport[2] = {0x81, 1}; s5->steamHaptic(shortReport, 2, g_Now);
    uint8_t zeros[64] = {0}; s5->steamHaptic(zeros, 64, g_Now);
    for (int side = 3; side < 256; side++) { auto p = pulse(side, 0); s5->steamHaptic(p.data(), (int)p.size(), g_Now); }
    uint8_t cmd[4] = {0x82, 3, 1, 0}; s5->steamHaptic(cmd, 4, g_Now);
    run(m);
    check(st5->writes.size() == sent && s5->queuedOutputs() == 0, "invalid haptic ids, lengths and sides cause no writes");

    // Wireless disconnect and reconnect
    st5->replies.clear(); e.attrs.clear(); int getsBefore = st5->gets;
    auto p01 = pulse(0, 1); s5->steamHaptic(p01.data(), (int)p01.size(), g_Now);
    size_t touchesAtDisconnect = e.touch.size();
    input(*s5, {0x79, 1});
    check(!s5->connected() && e.disconnected.size() == 1 && e.disconnected[0].first == s5->id() && e.disconnected[0].second == 1,
          "a wireless disconnect retires only its slot and reports the retired generation");
    check(e.touch.size() == touchesAtDisconnect + 2 && e.touch.back().event == LI_TOUCH_EVENT_UP, "link loss releases the touches the host still thinks are down");
    check(s5->attributes().isEmpty() && !s5->hasBattery(), "a disconnect forgets the attributes and battery (the slot may host another controller next)");
    input(*s5, state(0x42)); st5->writes.clear(); run(m);
    check(s5->connected() && s5->generation() == 3 && e.connectedCount(s5->id()) == 2, "reconnect announces a new generation");
    check(st5->countWrites(0x81) == 0, "queued feedback from a disconnected generation is not replayed after reconnect");
    check(st5->gets == getsBefore + 4 && e.attrs.empty() && s5->attributes().isEmpty() && logCount("(USB slot 5): no firmware attributes read") == 1,
          "reconnect asks again; a unit that answers no query is reported to nobody and keeps no attributes");
    check(e.states > statesBefore + 1, "reconnect resends held input");

    // Bounded queue with stop priority
    std::vector<uint8_t> zeroDuration = pulse(0, 5); zeroDuration[2] = 0;
    s5->steamHaptic(zeroDuration.data(), (int)zeroDuration.size(), g_Now);
    for (int i = 0; i < 100; i++) { auto p = pulse(1, 1); s5->steamHaptic(p.data(), (int)p.size(), g_Now); }
    check(s5->queuedOutputs() <= 32, "effect overload keeps the output queue bounded");
    st5->writes.clear(); run(m);
    check(st5->wrote(zeroDuration), "a zero-duration motor stop survives effect overload");

    // Rumble hold
    st5->writes.clear();
    s5->rumble(1234, 5678, g_Now); run(m);
    check(st5->writes.size() == 1 && st5->writes[0] == std::vector<uint8_t>{0x80, 0, 0, 0, 0xd2, 0x04, 0, 0x2e, 0x16, 0}, "host rumble becomes a 0x80 report with the speeds at bytes 4-5 and 7-8");
    g_Now += 40; run(m);
    check(st5->countWrites(0x80) == 2, "rumble refreshes at 40 ms without host reissuance");
    g_Now += 500; run(m);
    check(st5->wrote(motorStop()) && !st5->writes.empty() && st5->writes.back() == motorStop(), "the rumble hold deadline sends the motors off");
    size_t afterStop = st5->writes.size(); g_Now += 100; run(m);
    check(st5->writes.size() == afterStop, "after the hold expires nothing more is sent");
    s5->rumble(1, 1, g_Now); run(m); g_Now += 40; s5->rumble(0, 0, g_Now); run(m); g_Now += 100; run(m);
    check(st5->writes.back() == motorStop() && st5->writes.size() == afterStop + 2, "a host stop ends the hold immediately");

    // A tolerated transfer failure must not discard a motor-stop command.
    std::vector<uint8_t> stop = pulse(0, 0);
    st5->writes.clear(); st5->failWrite = true;
    s5->steamHaptic(stop.data(), (int)stop.size(), g_Now); run(m);
    check(s5->connected() && s5->queuedOutputs() == 1, "a failed stop remains queued while the slot tolerates a transient error");
    size_t attempts = st5->writes.size(); run(m);
    check(st5->writes.size() == attempts, "a failed stop retry waits instead of spinning");
    for (int i = 0; i < 100; i++) { auto p = pulse(1, 1); s5->steamHaptic(p.data(), (int)p.size(), g_Now); }
    check(s5->queuedOutputs() <= 32, "a retained stop shares the bounded output queue under overload");
    st5->failWrite = false; st5->writes.clear(); g_Now += 40; run(m);
    check(!st5->writes.empty() && st5->writes[0] == stop, "a recovered link sends the retained stop before later effects");
    st5->failWrite = true; s5->steamHaptic(stop.data(), (int)stop.size(), g_Now); run(m);
    input(*s5, {0x79, 1}); input(*s5, state(0x42));
    st5->failWrite = false; st5->writes.clear(); g_Now += 40; run(m);
    check(!st5->wrote(stop), "disconnect cancels a pending stop retry before a new controller generation");
    // Stops retry autonomously; a retained stop is one strike however often it is retried, and the
    // retry backs off (40, 80, 160 ms ...) so a stalled pipe does not spend the failure budget.
    st5->failWrite = true; st5->writes.clear();
    s5->steamHaptic(stop.data(), (int)stop.size(), g_Now); run(m);
    g_Now += 40; run(m);
    check(s5->connected(), "two failed stop attempts retain the slot");
    g_Now += 80; run(m);
    size_t stopTries = st5->writes.size();
    check(stopTries == 3 && s5->connected(), "a third failed stop retry is not a third strike");
    g_Now += 100; run(m);
    check(st5->writes.size() == stopTries, "stop retries back off after each failure");
    g_Now += 60; run(m);
    check(st5->writes.size() == stopTries + 1 && s5->connected(), "the backed-off retry fires at 160 ms and the slot is retained");
    st5->failWrite = false; g_Now += 1000; input(*s5, state(0x42)); run(m);
    check(st5->wrote(stop) && s5->queuedOutputs() == 0, "the retained stop is delivered once the pipe recovers");
    // Latest 0x80 state supersedes an obsolete stop, even while it awaits retry.
    st5->failWrite = true; s5->rumble(0, 0, g_Now); run(m);
    s5->rumble(1, 2, g_Now); st5->failWrite = false; st5->writes.clear(); g_Now += 40; run(m);
    bool obsoleteStop = false;
    for (auto& w : st5->writes) if (w[0] == 0x80 && w[4] == 0 && w[7] == 0) obsoleteStop = true;
    check(!obsoleteStop, "new rumble supersedes an obsolete pending rumble stop");
    s5->rumble(0, 0, g_Now); run(m);
    // Effects: three failures retire the slot, with a reconnect backoff.
    st5->failWrite = true;
    { auto p = pulse(0, 1); s5->steamHaptic(p.data(), (int)p.size(), g_Now); run(m); }
    check(s5->connected(), "one failed output transfer does not retire the slot");
    for (int i = 0; i < 2; i++) { auto p = pulse(0, 1); s5->steamHaptic(p.data(), (int)p.size(), g_Now); run(m); }
    check(!s5->connected() && logCount("retiring the slot") >= 1, "three consecutive failed output transfers retire the affected slot");
    input(*s5, state(0x42));
    check(!s5->connected(), "failed output has a reconnect backoff");
    st5->failWrite = false; g_Now += 1000; input(*s5, state(0x42)); run(m);
    check(s5->connected(), "later state recovers after transient output failure");
    // Keep-alive failures
    st5->failFeature = true; g_Now += 1000; run(m);
    check(s5->connected() && logCount("write failed") == 1, "one feature keep-alive failure does not retire the slot and is logged once");
    for (int i = 0; i < 2; i++) { g_Now += 1000; run(m); }
    check(!s5->connected(), "three consecutive keep-alive failures retire the slot");
    st5->failFeature = false; g_Now += 1000; input(*s5, state(0x42)); run(m);
    check(s5->connected(), "state reconnects after a feature failure");

    // Stale state
    g_Now += 2999; s5->checkStale(g_Now);
    check(s5->connected(), "a slot stays until 3 s without a state report");
    g_Now += 1; s5->checkStale(g_Now);
    check(!s5->connected() && logCount("controller gone") == 1, "silent input expires after 3 s");
    input(*s5, state(0x42)); run(m);

    // Lost device: dropped without the restore writes
    auto st4 = io.state("puck/if4");
    st4->writes.clear(); st4->features.clear();
    s4->markLost(); run(m);
    check(m.links().size() == 3 && closing == 1 && !st4->open && st4->writes.empty() && st4->features.empty() && e.disconnectedCount(s4->id()) == 1,
          "a device whose reads fail is dropped and closed without restore writes, and its controller is reported gone");
    io.infos.erase(std::remove_if(io.infos.begin(), io.infos.end(), [](const SteamHidDeviceInfo& i) { return i.path == "puck/if3" || i.path == "puck/if4"; }), io.infos.end());
    g_Now += 2000; run(m);
    check(m.links().size() == 2 && closing == 2 && e.disconnectedCount(s3->id()) == 1, "a path that is no longer enumerated is dropped at the next rescan");

    // Stop sequence
    st5->writes.clear(); st5->features.clear(); auto st2 = io.state("puck/if2"); st2->writes.clear();
    size_t openedBefore = io.opened.size();
    m.stop(g_Now);
    check(st5->writes.size() == 5 && st5->writes[0] == motorStop() && st5->writes[1] == std::vector<uint8_t>{0x81, 0, 0, 0, 0, 0, 0, 0}
          && st5->writes[2] == std::vector<uint8_t>{0x82, 0, 0, 0} && st5->writes[3] == std::vector<uint8_t>{0x81, 1, 0, 0, 0, 0, 0, 0} && st5->writes[4] == std::vector<uint8_t>{0x82, 1, 0, 0},
          "stop sends the motor stop and both pad pulse/command stops");
    check(st5->features.size() == 1 && st5->features[0][1] == 0x87 && st5->features[0][3] == 9 && st5->features[0][4] == 1 && st5->features[0][6] == 48 && st5->features[0][7] == 0
          && st5->features[0][9] == 71 && st5->features[0][10] == 0, "stop restores lizard mode, disables the raw IMU and leaves the watchdog disabled");
    check(st2->writes.size() == 5 && !st5->open && !st2->open && m.links().empty() && closing == 4 && e.disconnectedCount(s5->id()) >= 1,
          "every connected slot gets the stop sequence, the devices close and the links are gone");
    run(m);
    check(io.opened.size() == openedBefore, "a stopped manager does not reopen devices");
}

static void testWired()
{
    SteamControllerConfig cfg; cfg.motion = false;
    FakeTransport io;
    io.add("wired/hidraw3", 0x1302, 0, 0x0001);   // Linux: one node, the first collection's usage
    io.add("wired/unknown", 0x1302, -1, 0);      // no interface or usage information at all
    io.add("wired/mouse", 0x1302, 0, 0x0001);
    io.probeFails = {"wired/mouse"};
    io.unopenable = {"wired/unknown"};
    io.state("wired/hidraw3")->replies[0x83] = hex("01831e010213000002000000000a2ef9d26804e3854d6a094a0000000ba00f0000");
    Events e;
    SteamControllerManager m(io, cfg, 0, &e);   // unlimited rumble hold
    run(m);
    check(m.links().size() == 1 && m.links()[0]->info().path == "wired/hidraw3" && !m.links()[0]->isPuck() && m.links()[0]->name() == "USB slot 0",
          "a cable-connected unit (PID 0x1302) is found by probing when the usage page is not the vendor page");
    check(logCount("cannot open wired/unknown") == 1 && logCount("udev") == 1, "a device that cannot be opened is logged with the permission hint");
    auto ws = m.links()[0]; auto wi = io.state("wired/hidraw3");
    int probes = wi->gets;
    input(*ws, state(0x42)); run(m);
    auto cfgs = wi->settingsWrites();
    check(cfgs.size() == 1 && cfgs[0][3] == 9 && cfgs[0][4] == 0 && cfgs[0][6] == 48 && cfgs[0][7] == 0 && cfgs[0][9] == 71 && cfgs[0][10] == 0,
          "with motion off the cable link asks for lizard off and IMU mode 0");
    check(logLine("USB slot 0: link settings").find("Steam Controller USB slot 0: link settings (feature report 1, interface 0): lizard mode 0, IMU mode 0x0, Steam watchdog 0 -> accepted") != std::string::npos, "the wired settings line names interface 0 and IMU mode 0");
    check(e.attrs.size() == 1 && e.attrs[0].second.transport == LI_SCATTR_TRANSPORT_USB
          && e.attrs[0].second.validMask == (LI_SCATTR_FIRMWARE_BUILD_TIME | LI_SCATTR_BOOTLOADER_BUILD_TIME | LI_SCATTR_BOARD_REVISION | LI_SCATTR_CAPABILITIES)
          && e.attrs[0].second.buildId.empty() && e.attrs[0].second.unitSerial.empty() && wi->gets == probes + 4,
          "a cable-connected unit reports transport usb; when only 0x83 answers the mask carries just its four fields");
    check(logCount("Steam Controller usb (USB slot 0): firmware build") == 1, "the wired attribute line says usb");
    check(ws->capabilities() == (LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE | LI_CCAP_TOUCHPAD | LI_CCAP_BATTERY_STATE | LI_CCAP_GRIP_SENSE | LI_CCAP_STICK_TOUCH
          | LI_CCAP_STEAM_HAPTIC | LI_CCAP_SC26_BUTTONS | LI_CCAP_STEAM_ATTRIBUTES), "the wired link's capabilities follow its configuration (split pads, no motion)");
    // Unlimited hold: the rumble repeats until the host stops it
    wi->writes.clear(); ws->rumble(100, 200, g_Now); run(m);
    for (int i = 0; i < 100; i++) { g_Now += 40; run(m); }
    check(wi->countWrites(0x80) == 101 && !wi->wrote(motorStop()), "rumble hold 0 repeats the effect indefinitely");
    ws->rumble(0, 0, g_Now); run(m); g_Now += 200; run(m);
    check(wi->writes.back() == motorStop() && wi->countWrites(0x80) == 102, "the host's stop ends an unlimited hold");

    // A transient first settings failure must not permanently skip the one-time attributes.
    FakeTransport rio;
    rio.add("wired/r", 0x1302, 0, 0xFF00);
    rio.state("wired/r")->realUnit();
    rio.state("wired/r")->failFeature = true;
    Events re;
    SteamControllerManager rm(rio, cfg, 500, &re);
    run(rm);
    auto rs = rm.links()[0]; auto ri = rio.state("wired/r");
    input(*rs, state(0x42)); run(rm);
    check(rs->connected() && re.attrs.empty() && ri->gets == 0 && logCount("write failed") >= 1, "a rejected initial setup keeps the slot live and defers the attributes");
    ri->failFeature = false; g_Now += 1000; run(rm);
    check(re.attrs.size() == 1 && ri->gets == 4 && rs->attributes().unitSerial == "FXA9962403787", "the first successful keep-alive after a setup failure reads and forwards the attributes");
    g_Now += 1000; run(rm);
    check(ri->gets == 4, "later successful keep-alives do not repeat the attribute queries");
    // Summary line
    for (int i = 0; i < 25; i++) input(*rs, state(0x42));
    g_Now += 10000; run(rm);
    check(logCount("USB slot 0: 2 reports/s, pads touched L yes R yes") == 1, "the 10 s summary counts reports and touched pads");
    rm.stop(g_Now);
    m.stop(g_Now);
}

static void testMotionRateBounds()
{
    const struct { uint16_t rate; uint16_t period; } cases[] = {
        {1, 1000}, {2, 500}, {3, 334}, {250, 4}, {1000, 1}, {1001, 1}, {65535, 1}
    };
    for (const auto& c : cases) {
        for (uint8_t type : {LI_MOTION_TYPE_ACCEL, LI_MOTION_TYPE_GYRO}) {
            CoreSink sink;
            SteamControllerCore core(SteamControllerConfig{}, &sink);
            core.setMotionRate(type, c.rate);
            auto report = imu(0x42);
            core.handleState(report.data(), (int)report.size(), 1, false, 0);
            check(sink.motion.size() == 1, "every positive motion rate enables the requested sensor");
            put16(report, type == LI_MOTION_TYPE_ACCEL ? 34 : 40, 1234);
            core.handleState(report.data(), (int)report.size(), 1, false, c.period - 1);
            check(sink.motion.size() == 1, "motion never runs ahead of the requested interval");
            core.handleState(report.data(), (int)report.size(), 1, false, c.period);
            check(sink.motion.size() == 2, "motion resumes at the full-width interval boundary");
            core.setMotionRate(type, 0);
            put16(report, type == LI_MOTION_TYPE_ACCEL ? 34 : 40, 2345);
            core.handleState(report.data(), (int)report.size(), 1, false, c.period * 2);
            check(sink.motion.size() == 2, "zero motion rate disables the sensor after any positive rate");
        }
    }
}

static void testOpenRetry()
{
    FakeTransport io;
    io.add("transient/usb", PRODUCT_WIRED, 0, VENDOR_USAGE_PAGE);
    io.unopenable.push_back("transient/usb");
    Events events;
    SteamControllerManager manager(io, SteamControllerConfig{}, 500, &events);
    manager.service(0);
    check(manager.links().empty(), "a failed HID open creates no link");
    io.unopenable.clear();
    manager.service(SteamControllerManager::RESCAN_MS - 1);
    check(manager.links().empty(), "HID open retry waits for the bounded rescan interval");
    manager.service(SteamControllerManager::RESCAN_MS);
    check(manager.links().size() == 1, "a transient HID open failure recovers without unplugging the device");
    manager.stop(SteamControllerManager::RESCAN_MS);
}

static void testHostFeedbackGeneration()
{
    SteamControllerHost host;
    SteamControllerHost::setActiveGamepadMask(1);
    auto device = std::make_shared<FakeState>();
    SteamHidDeviceInfo info;
    info.productId = PRODUCT_PUCK;
    info.interfaceNumber = 2;
    auto link = std::make_shared<SteamControllerLink>(1000, info,
        std::unique_ptr<SteamHidDevice>(new FakeDevice(device)), SteamControllerConfig{}, 500, &host);
    input(*link, state(0x45));
    host.announce(0, link, link->generation());
    auto effect = pulse(0, 1);
    SteamControllerHost::clSteamHaptic(0, (uint8_t)effect.size(), effect.data());
    check(link->queuedOutputs() == 1, "announced generation accepts host haptic feedback");

    // Reader events can run ahead of the SDL main thread. The registry still
    // names the first generation while the same receiver slot hosts a new one.
    input(*link, {0x79, 1});
    input(*link, {0x79, 2});
    SteamControllerHost::clSteamHaptic(0, (uint8_t)effect.size(), effect.data());
    check(link->queuedOutputs() == 0, "old arrival cannot send a haptic to a reconnected puck slot");
    SteamControllerHost::rumble(0, 100, 200);
    check(link->queuedOutputs() == 0, "old arrival cannot start rumble on a reconnected puck slot");
    check(!SteamControllerHost::setMotionEventState(0, LI_MOTION_TYPE_GYRO, 250),
          "old arrival cannot change a reconnected slot's motion configuration");

    host.retire(link, 0, 0);
    SteamControllerHost::setActiveGamepadMask(1);
    host.announce(0, link, link->generation());
    SteamControllerHost::clSteamHaptic(0, (uint8_t)effect.size(), effect.data());
    check(link->queuedOutputs() == 1, "new arrival enables feedback for the replacement generation");
    check(SteamControllerHost::setMotionEventState(0, LI_MOTION_TYPE_GYRO, 250),
          "new arrival enables motion requests for the replacement generation");
    host.shutdown();
    check(!SteamControllerHost::rumble(0, 100, 200), "session shutdown retires the feedback registry");
    link->stop(g_Now);
}

// A 64-byte feature report 1 frame from its leading bytes.
static std::vector<uint8_t> frame64(const char* leading)
{
    std::vector<uint8_t> f = hex(leading);
    f.resize(64, 0);
    return f;
}
static std::shared_ptr<SteamControllerLink> byPath(SteamControllerManager& m, const char* path)
{
    for (auto& l : m.links()) if (l->info().path == path) return l;
    throw std::runtime_error(std::string("no link for ") + path);
}

// The Bluetooth LE transport (PID 0x1303, HID-over-GATT): same protocol as USB plus the host
// settings relay. Byte sequences are the Android BLE driver's (SteamControllerBle.java).
static void testBle()
{
    const std::vector<uint8_t> usbSettings = frame64("018709090000301800470000");      // lizard 0, IMU 0x18, watchdog 0
    const std::vector<uint8_t> usbStopSettings = frame64("018709090100300000470000");  // lizard 1, IMU 0
    SteamControllerConfig cfg;   // defaults: motion on, split pads, no knobs
    FakeTransport io;
    // Windows/macOS: one path per top-level collection of the HID-over-GATT device, interface -1
    // (a Bluetooth HID device has no USB interface); the vendor collection is the 0xFF00 one.
    io.add("ble/a/col1", 0x1303, -1, 0x0001);
    io.add("ble/a/col2", 0x1303, -1, 0x0001);
    io.add("ble/a/col3", 0x1303, -1, 0xFF00);
    // Linux: the single uhid hidraw node of a second paired controller, first collection's usage.
    io.add("ble/b/hidraw7", 0x1303, 0, 0x0001);
    io.add("ble/odd", 0x1303, 1, 0xFF00);   // no Bluetooth HID device has interface 1
    io.probeFails = {"ble/a/col1", "ble/a/col2"};
    io.state("ble/a/col3")->realUnit();
    Events e;
    SteamControllerManager m(io, cfg, 500, &e);
    run(m);
    auto links = m.links();
    bool allBle = links.size() == 2;
    for (auto& l : links) allBle &= l->isBle() && !l->isPuck() && l->transport() == SteamControllerLink::TransportBle;
    check(allBle && io.opened.size() == 4 && !io.state("ble/odd")->open,
          "both Bluetooth LE controllers (PID 0x1303, interface -1 and 0) are opened by usage page or by probing; interface 1 is not a candidate");
    SteamHidDeviceInfo odd; odd.vendorId = 0x28DE; odd.productId = 0x1303; odd.interfaceNumber = -1;
    SteamHidDeviceInfo odd2 = odd; odd2.interfaceNumber = 2;
    check(SteamControllerManager::isCandidate(odd) && !SteamControllerManager::isCandidate(odd2), "isCandidate takes 0x1303 with interface -1 or 0 only");
    auto a = byPath(m, "ble/a/col3"), b = byPath(m, "ble/b/hidraw7");
    check(a->name() == "Bluetooth LE slot 0" && b->name() == "Bluetooth LE slot 1" && a->slot() == 0 && b->slot() == 1
          && strcmp(a->transportName(), "Bluetooth LE") == 0 && a->attributeTransport() == LI_SCATTR_TRANSPORT_BLE,
          "BLE links take the lowest free slot number, are named 'Bluetooth LE slot N' and report transport BLE");
    check(has(logLine("opened ble/a/col3"), "(product 0x1303, interface -1, usage page 0xff00; usage page 0xff00) as Bluetooth LE slot 0")
          && has(logLine("opened ble/b/hidraw7"), "accepted a feature report 1 read) as Bluetooth LE slot 1"),
          "the opened lines name the collection pick and the Bluetooth LE slot");
    check(a->capabilities() == (SteamControllerCore(cfg, nullptr).capabilities() | LI_CCAP_STEAM_SETTINGS)
          && (a->capabilities() & LI_CCAP_STEAM_SETTINGS) != 0 && (SteamControllerCore(cfg, nullptr).capabilities() & LI_CCAP_STEAM_SETTINGS) == 0,
          "a BLE link advertises the USB capability set plus STEAM_SETTINGS");

    // Connect: report 0x45 (45 bytes after the id, same offsets as 0x42/0x47)
    input(*a, state(0x45));
    check(e.connected.size() == 1 && a->connected() && e.states == 1 && e.touch.size() == 2
          && logCount("Steam Controller Bluetooth LE slot 0: controller connected (Bluetooth LE, link generation 1)") == 1,
          "a 0x45 report connects the BLE link and its state and pads reach the listener");
    check(has(logLine("Bluetooth LE slot 0: haptics:"), "0x82 gain boost +0 dB (ceiling +6 dB), 0x81 pulse on-time x1.0, master gain setting not written"),
          "the BLE link logs its haptic knobs once per connect (defaults off)");
    run(m);
    auto st = io.state("ble/a/col3");
    check(st->settingsWrites().size() == 1 && st->settingsWrites()[0] == usbSettings, "the BLE connect settings are byte-identical to USB (01 87 09 09 00 00 30 18 00 47 00 00)");
    check(has(logLine("Bluetooth LE slot 0: link settings"), "link settings (feature report 1, interface 0): lizard mode 0, IMU mode 0x18, Steam watchdog 0 -> accepted"),
          "the BLE settings line reads like the USB one");
    auto q = st->queries();
    check(q.size() == 4 && q[0][1] == 0x83 && q[0][2] == 0 && q[1][1] == 0xF2 && q[1][2] == 1 && q[1][3] == 0
          && q[2][1] == 0xAE && q[2][2] == 0x15 && q[2][3] == 1 && q[3][1] == 0xAE && q[3][2] == 0x15 && q[3][3] == 0 && st->gets == 4,
          "the BLE attribute queries are the USB ones (83 00, F2 01 00, AE 15 01, AE 15 00), one GET_REPORT each");
    check(e.attrs.size() == 1 && e.attrs[0].second.transport == LI_SCATTR_TRANSPORT_BLE && e.attrs[0].second.unitSerial == "FXA9962403787"
          && logCount("Steam Controller ble (Bluetooth LE slot 0): firmware build 2026-07-07") == 1,
          "the attributes packet carries transport BLE (1) and the real unit's values");
    uint8_t battery[15] = {0x43, 1, 77};
    input(*a, std::vector<uint8_t>(battery, battery + 15));
    check(e.battery == LI_BATTERY_STATE_DISCHARGING && e.percent == 77 && a->hasBattery(), "a 0x43 report over BLE sets the battery; without one it stays unknown");
    g_Now += 1000; run(m);
    check(st->settingsWrites().size() == 2 && st->settingsWrites()[1] == usbSettings && st->gets == 4, "the BLE keep-alive repeats the same frame every second");

    // Host settings relay: 0x87 minus the reserved ids
    st->features.clear();
    const uint8_t triples[] = {9, 0, 0, 48, 0x18, 0, 71, 0, 0, 49, 2, 0, 52, 0xff, 0xff, 53, 0xff, 0xff, 41, 0x34, 0x12, 70, 5, 0, 79, 1, 0};
    check(a->hostSettings(0x87, 9, triples, g_Now) && a->queuedSettings() == 1 && a->queuedOutputs() == 1, "a BLE link takes a host 0x87 write into the output queue as a settings entry");
    run(m);
    check(st->features.size() == 1 && st->features[0] == frame64("018709293412460500" "4f0100"),
          "the kept triples go out as feature report 1 `01 87 09 29 34 12 46 05 00 4f 01 00`; 9, 48, 71, 49, 52 and 53 are dropped");
    check(logCount("Steam Controller Bluetooth LE slot 0: host settings 0x87: 41=4660 70=5 79=1 (reserved, kept ours: 9 48 71 49 52 53)") == 1,
          "the relay logs the applied and the reserved ids like the Android driver");
    st->features.clear();
    const uint8_t onlyReserved[] = {9, 1, 0, 52, 0xff, 0xff};
    check(a->hostSettings(0x87, 2, onlyReserved, g_Now) && a->queuedSettings() == 0, "a write of reserved ids only is accepted but queues nothing");
    run(m);
    check(st->features.empty() && logCount("host settings 0x87: nothing to apply (reserved, kept ours: 9 52)") == 1, "nothing is written and the log says so");
    const uint8_t unreserved[] = {41, 1, 0};
    check(a->hostSettings(0x87, 1, unreserved, g_Now) && a->queuedSettings() == 1, "an unreserved id alone is applied");
    run(m);
    check(st->features.size() == 1 && st->features[0] == frame64("01870329" "0100") && logCount("host settings 0x87: 41=1") == 1, "a one-triple write has length 3 and no reserved note");
    // 0x8E: reset, then this link's own settings right behind it
    st->features.clear();
    check(a->hostSettings(0x8E, 0, nullptr, g_Now) && a->queuedSettings() == 2, "0x8E queues the reset and the link settings");
    run(m);
    check(st->features.size() == 2 && st->features[0] == frame64("018e00") && st->features[1] == usbSettings
          && logCount("Bluetooth LE slot 0: host settings 0x8e (load defaults), re-sending this link's setup") == 1,
          "0x8E goes out as `01 8E 00` followed by the link settings");
    check(!a->hostSettings(0x83, 0, nullptr, g_Now) && !a->hostSettings(0x87, 2, nullptr, g_Now) && !a->hostSettings(0x87, 9, triples, g_Now, 99)
          && a->queuedSettings() == 0, "other commands, a missing payload and a stale generation are rejected");
    uint8_t many[25 * 3];
    for (int i = 0; i < 25; i++) { many[3 * i] = (uint8_t)(100 + i); many[3 * i + 1] = (uint8_t)i; many[3 * i + 2] = 0; }
    st->features.clear(); a->hostSettings(0x87, 25, many, g_Now); run(m);
    check(st->features.size() == 1 && st->features[0][2] == 60 && st->features[0][3] == 100 && st->features[0][60] == 119 && st->features[0][63] == 0,
          "a write longer than the frame keeps the first 20 triples (61 payload bytes)");

    // Queue class: settings are never evicted by haptics; only a settings flood evicts settings
    st->features.clear(); st->writes.clear();
    a->hostSettings(0x87, 1, unreserved, g_Now);
    for (int i = 0; i < 100; i++) { auto p = pulse(1, 1); a->steamHaptic(p.data(), (int)p.size(), g_Now); }
    check(a->queuedOutputs() <= 32 && a->queuedSettings() == 1, "a haptic burst fills the bounded queue around the settings write without evicting it");
    run(m);
    check(st->features.size() == 1 && st->features[0] == frame64("018703290100") && st->countWrites(0x81) == 31, "the settings write is sent and 31 of the 100 pulses survive the burst");
    st->features.clear();
    for (int i = 0; i < 20; i++) { uint8_t t[3] = {41, (uint8_t)i, 0}; a->hostSettings(0x87, 1, t, g_Now); }
    check(a->queuedSettings() == 16, "a settings flood keeps the newest 16 writes (the Android reservation)");
    run(m);
    check(st->features.size() == 16 && st->features[0][4] == 4 && st->features[15][4] == 19, "a flood evicts the oldest settings first, in order");
    st->features.clear(); st->writes.clear();
    for (int i = 0; i < 16; i++) { uint8_t t[3] = {41, (uint8_t)i, 0}; a->hostSettings(0x87, 1, t, g_Now); }
    auto stopPulse = pulse(0, 0);
    a->steamHaptic(stopPulse.data(), (int)stopPulse.size(), g_Now);
    for (int i = 0; i < 100; i++) { auto p = pulse(1, 1); a->steamHaptic(p.data(), (int)p.size(), g_Now); }
    check(a->queuedOutputs() <= 32 && a->queuedSettings() == 16, "16 settings, a stop and a burst stay within the 32-entry queue");
    run(m);
    check(st->features.size() == 16 && st->wrote(stopPulse), "every settings write and the stop are sent; effects took the evictions");
    // A failed settings write is one strike and is not replayed
    st->features.clear(); st->failFeature = true;
    a->hostSettings(0x87, 1, unreserved, g_Now); run(m);
    check(a->connected() && a->queuedSettings() == 0 && logCount("Bluetooth LE: output failed on Bluetooth LE slot 0 (1 of 3)") == 1,
          "a refused settings write counts as one output failure and is dropped");
    st->failFeature = false;
    // Summary line
    for (int i = 0; i < 10; i++) input(*a, state(0x45));
    g_Now += 10000; run(m);
    check(has(lastLogLine("Bluetooth LE slot 0: 1 reports/s"), ", host settings ") && has(lastLogLine("Bluetooth LE slot 0: 1 reports/s"), ", adjusted 0"),
          "the BLE summary line adds the host settings and adjusted counts");

    // The host relay: through the registry for an announced BLE link; a USB link ignores it
    FakeTransport uio;
    uio.add("usb", 0x1302, 0, 0xFF00);
    Events ue;
    SteamControllerManager um(uio, cfg, 500, &ue);
    run(um);
    auto u = um.links()[0];
    input(*u, state(0x42)); run(um);
    {
        SteamControllerHost host;
        SteamControllerHost::setActiveGamepadMask(3);
        host.announce(0, a, a->generation());
        host.announce(1, u, u->generation());
        st->features.clear(); uio.state("usb")->features.clear();
        SteamControllerHost::clSteamSettings(0, 0x87, 3, triples + 18);
        SteamControllerHost::clSteamSettings(1, 0x87, 3, triples + 18);
        check(a->queuedSettings() == 1 && u->queuedSettings() == 0 && !u->hostSettings(0x87, 3, triples + 18, g_Now),
              "clSteamSettings reaches the announced BLE link; a USB link refuses host settings");
        SteamControllerHost::clSteamSettings(0, 0x8E, 0, nullptr);
        check(a->queuedSettings() == 3 && u->queuedSettings() == 0, "clSteamSettings relays 0x8E too");
        // The registry stamps the queue with the real clock: move the virtual clock past it to drain.
        g_Now = std::max<uint64_t>(g_Now, nowMs() + 1);
        run(m);
        bool relayed = false, reset = false;
        for (auto& f : st->features) { relayed |= f == frame64("018709293412460500" "4f0100"); reset |= f == frame64("018e00"); }
        check(relayed && reset && a->queuedSettings() == 0 && uio.state("usb")->features.empty() && u->queuedSettings() == 0,
              "the relayed 0x87 and 0x8E reach only the BLE controller");
        host.retire(a, 0, 2); host.retire(u, 1, 0);
    }

    // Output failures retire a BLE slot with the BLE wording; the stop sequence is the USB one
    st->writes.clear(); st->failWrite = true;
    for (int i = 0; i < 3; i++) { auto p = pulse(0, 1); a->steamHaptic(p.data(), (int)p.size(), g_Now); run(m); }
    check(!a->connected() && logCount("Steam Controller Bluetooth LE: output keeps failing on Bluetooth LE slot 0; retiring the slot") == 1,
          "three failed BLE output transfers retire the slot with the Bluetooth LE wording");
    st->failWrite = false; g_Now += 1000; input(*a, state(0x45)); run(m);
    check(a->connected() && a->generation() == 3, "the BLE slot reconnects after the backoff");
    g_Now += 3000; a->checkStale(g_Now);
    check(!a->connected() && logCount("Bluetooth LE slot 0: no state report for 3000 ms; controller gone") == 1, "a silent BLE link is dropped after 3 s like USB");
    input(*a, state(0x45)); run(m);
    st->writes.clear(); st->features.clear();
    m.stop(g_Now);
    check(st->writes.size() == 5 && st->writes[0] == motorStop() && st->writes[4] == std::vector<uint8_t>{0x82, 1, 0, 0}
          && st->features.size() == 1 && st->features[0] == usbStopSettings && !st->open,
          "the BLE stop sequence is the USB one: motor stop, both pad stops, lizard mode back on");
    um.stop(g_Now);
    // Ignore list and reserved ids
    std::string ignore = SDL_IGNORE_DEVICES;
    check(has(ignore, "0x28de/0x1303") && has(ignore, "0x28de/0x1302") && has(ignore, "0x28de/0x1304"), "the SDL ignore list names the cable, Bluetooth LE and puck PIDs");
    bool reservedSet = true;
    for (int id = 0; id < 256; id++) {
        bool expected = id == 9 || id == 48 || id == 71 || id == 49 || id == 52 || id == 53;
        reservedSet &= isHostSettingReserved(id) == expected;
    }
    check(reservedSet, "the reserved host setting ids are exactly {9, 48, 71, 49, 52, 53}");
}

// The three Bluetooth LE haptic knobs (ported from the Android BLE driver): off by default, and
// never applied to a USB or puck link in the same session.
static void testBleHapticKnobs()
{
    auto twoLinks = [](const SteamControllerConfig& c, FakeTransport& io, Events& e) {
        io.add("ble", 0x1303, -1, 0xFF00);
        io.add("usb", 0x1302, 0, 0xFF00);
        auto m = std::make_shared<SteamControllerManager>(io, c, 500, &e);
        run(*m);
        input(*byPath(*m, "ble"), state(0x45)); input(*byPath(*m, "usb"), state(0x42)); run(*m);
        io.state("ble")->writes.clear(); io.state("usb")->writes.clear();
        return m;
    };
    auto send = [](SteamControllerManager& m, const char* path, std::vector<uint8_t> r) {
        auto l = byPath(m, path); l->steamHaptic(r.data(), (int)r.size(), g_Now); run(m);
    };

    // Boost: the signed gain byte of 0x82 ticks and clicks
    {
        SteamControllerConfig c; c.hapticBoostDb = 6;
        FakeTransport io; Events e; auto m = twoLinks(c, io, e);
        auto bs = io.state("ble"), us = io.state("usb");
        send(*m, "ble", {0x82, 0, 2, 0xF2}); send(*m, "usb", {0x82, 0, 2, 0xF2});
        check(bs->writes.back() == std::vector<uint8_t>{0x82, 0, 2, 0xF8} && us->writes.back() == std::vector<uint8_t>{0x82, 0, 2, 0xF2},
              "+6 dB lifts Steam's -14 dB click to -8 dB on the BLE link and leaves the USB link's bytes alone");
        send(*m, "ble", {0x82, 1, 1, 0x00});
        check(bs->writes.back() == std::vector<uint8_t>{0x82, 1, 1, 0x06}, "a 0 dB tick becomes +6 dB");
        send(*m, "ble", {0x82, 1, 1, 0xFF});
        check(bs->writes.back() == std::vector<uint8_t>{0x82, 1, 1, 0x05}, "a -1 dB tick becomes +5 dB (signed arithmetic)");
        send(*m, "ble", {0x82, 0, 2, 0x04});
        check(bs->writes.back() == std::vector<uint8_t>{0x82, 0, 2, 0x06}, "the boost clamps at the firmware's +6 dB ceiling");
        send(*m, "ble", {0x82, 0, 0, 0x00});
        check(bs->writes.back() == std::vector<uint8_t>{0x82, 0, 0, 0x00}, "a 0x82 stop (command 0) is untouched");
        send(*m, "ble", {0x82, 0, 3, 0xF2});
        check(bs->writes.back() == std::vector<uint8_t>{0x82, 0, 3, 0xF2}, "other 0x82 commands are untouched");
        send(*m, "ble", pulse(0, 1));
        check(bs->writes.back() == pulse(0, 1), "the boost does not touch 0x81 pulses");
        check(has(lastLogLine("Bluetooth LE slot 0: haptics:"), "0x82 gain boost +6 dB (ceiling +6 dB), 0x81 pulse on-time x1.0, master gain setting not written"),
              "the connect line shows the boost");
        g_Now += 10000; run(*m);
        check(has(lastLogLine("Bluetooth LE slot 0: 0 reports/s"), ", adjusted 4") && has(lastLogLine("USB slot 0: 0 reports/s"), "other 0") && !has(lastLogLine("USB slot 0: 0 reports/s"), "adjusted"),
              "the summary counts the four rewritten commands; the USB line is unchanged");
        m->stop(g_Now);
    }
    {
        SteamControllerConfig c; c.hapticBoostDb = 20;
        FakeTransport io; Events e; auto m = twoLinks(c, io, e);
        send(*m, "ble", {0x82, 0, 2, 0xF2}); check(io.state("ble")->writes.back()[3] == 0x06, "+20 dB puts a -14 dB click at the ceiling");
        send(*m, "ble", {0x82, 1, 1, 0xE8}); check(io.state("ble")->writes.back()[3] == (uint8_t)(int8_t)-4, "+20 dB lifts a -24 dB tick to -4 dB");
        m->stop(g_Now);
    }
    {
        SteamControllerConfig c; c.hapticBoostDb = 99;   // clamped to 30
        FakeTransport io; Events e; auto m = twoLinks(c, io, e);
        check(byPath(*m, "ble")->config().hapticBoostDb == 30 && has(lastLogLine("Bluetooth LE slot 0: haptics:"), "boost +30 dB"), "an out-of-range boost is clamped to 30 dB");
        send(*m, "ble", {0x82, 1, 1, 0xE8}); check(io.state("ble")->writes.back()[3] == 0x06, "+30 dB on -24 dB clamps to the ceiling");
        m->stop(g_Now);
    }

    // Pulse scale: the on-time of 0x81 pulses
    {
        SteamControllerConfig c; c.hapticPulseScalePercent = 200;
        FakeTransport io; Events e; auto m = twoLinks(c, io, e);
        auto bs = io.state("ble"), us = io.state("usb");
        std::vector<uint8_t> slide{0x81, 0, 0x90, 0x01, 0x00, 0x00, 0x01, 0x00};   // 400 us on, repeat 1
        send(*m, "ble", slide); send(*m, "usb", slide);
        check(bs->writes.back() == std::vector<uint8_t>{0x81, 0, 0x20, 0x03, 0x00, 0x00, 0x01, 0x00} && us->writes.back() == slide,
              "200% doubles a 400 us slide pulse to 800 us (0x0320) on the BLE link only");
        send(*m, "ble", {0x81, 1, 0x00, 0x90, 0x10, 0x00, 0x05, 0x00});
        check(bs->writes.back() == std::vector<uint8_t>{0x81, 1, 0xFF, 0xFF, 0x10, 0x00, 0x05, 0x00}, "the scaled on-time is capped at 0xFFFF");
        send(*m, "ble", {0x81, 1, 0x00, 0x00, 0x10, 0x00, 0x05, 0x00});
        check(bs->writes.back() == std::vector<uint8_t>{0x81, 1, 0x00, 0x00, 0x10, 0x00, 0x05, 0x00}, "a zero on-time is untouched");
        std::vector<uint8_t> stop{0x81, 0, 0x90, 0x01, 0x00, 0x00, 0x00, 0x00};   // repeat 0: a stop
        send(*m, "ble", stop);
        check(bs->writes.back() == stop, "a zero-repeat stop is untouched");
        send(*m, "ble", {0x82, 0, 2, 0xF2});
        check(bs->writes.back() == std::vector<uint8_t>{0x82, 0, 2, 0xF2}, "the pulse scale does not touch 0x82 commands");
        check(has(lastLogLine("Bluetooth LE slot 0: haptics:"), "0x81 pulse on-time x2.0"), "the connect line shows the pulse scale");
        g_Now += 10000; run(*m);
        check(has(lastLogLine("Bluetooth LE slot 0: 0 reports/s"), ", adjusted 2"), "two pulses were rewritten");
        m->stop(g_Now);
    }
    {
        SteamControllerConfig c; c.hapticPulseScalePercent = 300;
        FakeTransport io; Events e; auto m = twoLinks(c, io, e);
        send(*m, "ble", {0x81, 0, 0x90, 0x01, 0x00, 0x00, 0x01, 0x00});
        check(io.state("ble")->writes.back()[2] == 0xB0 && io.state("ble")->writes.back()[3] == 0x04 && has(lastLogLine("Bluetooth LE slot 0: haptics:"), "x3.0"),
              "300% makes 400 us into 1200 us (0x04B0)");
        m->stop(g_Now);
    }
    {
        SteamControllerConfig c; c.hapticPulseScalePercent = 50;   // clamped to 100
        FakeTransport io; Events e; auto m = twoLinks(c, io, e);
        std::vector<uint8_t> slide{0x81, 0, 0x90, 0x01, 0x00, 0x00, 0x01, 0x00};
        send(*m, "ble", slide);
        check(io.state("ble")->writes.back() == slide && byPath(*m, "ble")->config().hapticPulseScalePercent == 100, "a scale under 100% is clamped to 100% (no change)");
        m->stop(g_Now);
    }

    // Master gain: setting 76 in the link's own settings, BLE only, cleared at stop
    {
        SteamControllerConfig c; c.hapticMasterGainDb = 12;
        FakeTransport io; Events e;
        io.add("ble", 0x1303, -1, 0xFF00);
        io.add("usb", 0x1302, 0, 0xFF00);
        SteamControllerManager m(io, c, 500, &e);
        run(m);
        auto bl = byPath(m, "ble"), ul = byPath(m, "usb");
        input(*bl, state(0x45)); input(*ul, state(0x42)); run(m);
        auto bs = io.state("ble"), us = io.state("usb");
        const std::vector<uint8_t> gainSettings = frame64("01870c0900003018004700004c0c00");
        check(bs->settingsWrites().size() == 1 && bs->settingsWrites()[0] == gainSettings, "the BLE link writes four triples: lizard 0, IMU 0x18, watchdog 0, master gain 76=12");
        check(us->settingsWrites().size() == 1 && us->settingsWrites()[0] == frame64("018709090000301800470000"), "the USB link in the same session keeps the three-triple frame");
        check(has(lastLogLine("Bluetooth LE slot 0: link settings"), "lizard mode 0, IMU mode 0x18, Steam watchdog 0, haptic master gain +12 dB -> accepted")
              && has(lastLogLine("Bluetooth LE slot 0: haptics:"), "master gain setting 76=+12 dB")
              && !has(lastLogLine("USB slot 0: link settings"), "master gain"),
              "the settings and haptics lines show the master gain for the BLE link only");
        g_Now += 1000; run(m);
        check(bs->settingsWrites().size() == 2 && bs->settingsWrites()[1] == gainSettings, "the keep-alive repeats the four-triple frame");
        bs->features.clear();
        bl->hostSettings(0x8E, 0, nullptr, g_Now); run(m);
        check(bs->features.size() == 2 && bs->features[1] == gainSettings, "the settings re-sent after a host 0x8E include the master gain");
        bs->features.clear(); us->features.clear();
        m.stop(g_Now);
        check(bs->features.size() == 1 && bs->features[0] == frame64("01870c0901003000004700004c0000") && us->features[0] == frame64("018709090100300000470000"),
              "the BLE stop frame clears the master gain (76=0); the USB stop frame is unchanged");
    }
    {
        SteamControllerConfig c; c.hapticMasterGainDb = 99;   // clamped to 24
        FakeTransport io; Events e; auto m = twoLinks(c, io, e);
        check(io.state("ble")->settingsWrites()[0][13] == 24 && byPath(*m, "ble")->config().hapticMasterGainDb == 24, "an out-of-range master gain is clamped to 24 dB");
        m->stop(g_Now);
    }
    // All three off: a BLE link's frames and reports are the USB ones (covered above); all three
    // on together on a USB link change nothing.
    {
        SteamControllerConfig c; c.hapticBoostDb = 20; c.hapticPulseScalePercent = 300; c.hapticMasterGainDb = 12;
        FakeTransport io; Events e; auto m = twoLinks(c, io, e);
        auto us = io.state("usb");
        send(*m, "usb", {0x82, 0, 2, 0xF2}); send(*m, "usb", {0x81, 0, 0x90, 0x01, 0x00, 0x00, 0x01, 0x00});
        check(us->writes[0] == std::vector<uint8_t>{0x82, 0, 2, 0xF2} && us->writes[1] == std::vector<uint8_t>{0x81, 0, 0x90, 0x01, 0x00, 0x00, 0x01, 0x00}
              && us->settingsWrites()[0] == frame64("018709090000301800470000") && byPath(*m, "usb")->capabilities() == SteamControllerCore(c, nullptr).capabilities(),
              "the knobs never touch a USB link's reports, settings or capabilities");
        m->stop(g_Now);
    }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setLogFunction(captureLog);
    try {
        testAttributes();
        testCore();
        testLink();
        testWired();
        testMotionRateBounds();
        testOpenRetry();
        testHostFeedbackGeneration();
        testBle();
        testBleHapticKnobs();
    }
    catch (const std::exception& ex) {
        printf("FAIL exception: %s\n", ex.what());
        g_Failed++;
    }
    printf("%d checks, %d failures\n", g_Checks, g_Failed);
    return g_Failed == 0 ? 0 : 1;
}
