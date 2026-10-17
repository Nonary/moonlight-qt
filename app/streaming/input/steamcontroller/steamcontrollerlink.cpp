// SPDX-License-Identifier: GPL-3.0-or-later
#include "steamcontrollerlink.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace SteamController;

namespace {
const uint64_t NEVER = UINT64_MAX;

std::vector<uint8_t> motorStop() { return std::vector<uint8_t>{OUT_RUMBLE, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }

std::string slotName(const SteamHidDeviceInfo& info)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "USB slot %d", info.interfaceNumber < 0 ? 0 : info.interfaceNumber);
    return buffer;
}
}

SteamControllerLink::SteamControllerLink(int id, const SteamHidDeviceInfo& info, std::unique_ptr<SteamHidDevice> device,
                                         const SteamControllerConfig& config, int rumbleHoldMs, Listener* listener)
    : m_Id(id),
      m_Info(info),
      m_Name(slotName(info)),
      m_Device(std::move(device)),
      m_Listener(listener),
      m_RumbleHoldMs(rumbleHoldMs < 0 ? 0 : rumbleHoldMs),
      m_Core(config, this)
{
}

SteamControllerLink::~SteamControllerLink() = default;

bool SteamControllerLink::connected() const { std::lock_guard<std::mutex> lock(m_Mutex); return m_Connected; }
int SteamControllerLink::generation() const { std::lock_guard<std::mutex> lock(m_Mutex); return m_Generation; }
SteamControllerAttributes SteamControllerLink::attributes() const { std::lock_guard<std::mutex> lock(m_Mutex); return m_Attributes; }
bool SteamControllerLink::hasBattery() const { std::lock_guard<std::mutex> lock(m_Mutex); return m_HasBattery; }
size_t SteamControllerLink::queuedOutputs() const { std::lock_guard<std::mutex> lock(m_Mutex); return m_Outputs.size(); }

// ----- sink (core -> listener), called under m_Mutex -----

void SteamControllerLink::steamState(int buttonFlags, uint8_t lt, uint8_t rt, short lsX, short lsY, short rsX, short rsY)
{
    m_Listener->linkState(*this, m_Generation, buttonFlags, lt, rt, lsX, lsY, rsX, rsY);
}

void SteamControllerLink::steamTouch(uint8_t touchpadIndex, uint8_t eventType, uint32_t pointerId, float x, float y, float pressure)
{
    m_Listener->linkTouch(*this, m_Generation, touchpadIndex, eventType, pointerId, x, y, pressure);
}

void SteamControllerLink::steamMotion(uint8_t motionType, float x, float y, float z)
{
    m_Listener->linkMotion(*this, m_Generation, motionType, x, y, z);
}

void SteamControllerLink::steamBattery(uint8_t state, uint8_t percent)
{
    m_HasBattery = true;
    m_BatteryState = state;
    m_BatteryPercent = percent;
    m_Listener->linkBattery(*this, m_Generation, state, percent);
}

// ----- reader thread -----

void SteamControllerLink::handleReport(const uint8_t* report, int size, uint64_t nowMs)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Stopped || size <= 0) {
        return;
    }
    int id = report[0];
    if ((id == IN_WIRELESS_EVENT_ALT || id == IN_WIRELESS_EVENT) && size >= 2) {
        if (report[1] == 1) disconnectLocked(nowMs);
        else if (report[1] == 2) connectLocked(nowMs);
        return;
    }
    int expected = id == IN_STATE_WIRED ? STATE_SIZE_WIRED :
                   (id == IN_STATE || id == IN_STATE_TIMESTAMPED) ? STATE_SIZE_WIRELESS : 0;
    if (expected != 0 && size >= expected) {
        connectLocked(nowMs); // The puck may already be paired when Moonlight claims it.
        if (m_Connected) {
            m_LastStateMs = nowMs;
            m_SummaryReports++;
            if (m_InputResetPending.exchange(false)) {
                m_Core.resetInputState();
            }
            m_Core.handleState(report, size, 1, id == IN_STATE_TIMESTAMPED, nowMs);
            m_SummaryLeftPad |= m_Core.leftPadTouched();
            m_SummaryRightPad |= m_Core.rightPadTouched();
        }
    }
    else if (id == IN_BATTERY && size >= BATTERY_SIZE && m_Connected) {
        m_Core.handleBattery(report, size, 1);
    }
    // 0x44 haptic acknowledgements and 0x7B radio diagnostics are not game input.
}

void SteamControllerLink::checkStale(uint64_t nowMs)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Stopped && m_Connected && nowMs - m_LastStateMs >= (uint64_t)STALE_STATE_MS) {
        logf(LogInfo, "Steam Controller %s: no state report for %d ms; controller gone", m_Name.c_str(), STALE_STATE_MS);
        disconnectLocked(nowMs);
    }
}

void SteamControllerLink::connectLocked(uint64_t nowMs)
{
    if (m_Connected || m_Stopped || nowMs < m_RetryAtMs) {
        return;
    }
    m_Connected = true;
    m_Generation++;
    m_Attributes = SteamControllerAttributes(isPuck() ? LI_SCATTR_TRANSPORT_PUCK : LI_SCATTR_TRANSPORT_USB);
    m_HasAttributes = false;
    m_HasBattery = false;
    m_Core.resetInputState();
    m_LastStateMs = nowMs;
    m_ConfigurePending = true;
    m_KeepAliveDueMs = nowMs + KEEPALIVE_MS;
    m_SummaryDueMs = nowMs + SUMMARY_MS;
    logf(LogInfo, "Steam Controller %s: controller connected (%s, link generation %d)",
         m_Name.c_str(), isPuck() ? "puck" : "cable", m_Generation);
    m_Listener->linkConnected(*this, m_Generation);
    if (wake) wake();
}

void SteamControllerLink::disconnectLocked(uint64_t nowMs)
{
    (void)nowMs;
    m_Outputs.clear();
    m_DrainBusy = false;
    m_ConfigureState = 0;
    m_ConfigurePending = false;
    m_AttributesQueried = false;
    m_HasRepeatingRumble = false;
    m_OutputFailures = 0;
    m_HasStrikedReport = false;
    m_RetryDelayMs = OUTPUT_RETRY_MS;
    // Release any touches the host still thinks are down, under the generation it knows.
    if (m_Connected) m_Core.releaseTouches();
    m_HasAttributes = false;
    m_Attributes = SteamControllerAttributes(isPuck() ? LI_SCATTR_TRANSPORT_PUCK : LI_SCATTR_TRANSPORT_USB);
    m_HasBattery = false;
    m_Core.resetInputState();
    int retired = m_Generation;
    bool wasConnected = m_Connected;
    m_Connected = false;
    m_Generation++;
    if (wasConnected) {
        logf(LogInfo, "Steam Controller %s: controller disconnected (link generation %d)", m_Name.c_str(), retired);
        m_Listener->linkDisconnected(*this, retired);
    }
}

// ----- output budget -----

int SteamControllerLink::stopKey(const std::vector<uint8_t>& report)
{
    int id = report[0];
    if (id == OUT_RUMBLE && report[4] == 0 && report[5] == 0 && report[7] == 0 && report[8] == 0) return OUT_RUMBLE;
    if ((id == OUT_PULSE && ((report[6] == 0 && report[7] == 0) || (report[2] == 0 && report[3] == 0))) ||
        (id == OUT_HAPTIC_COMMAND && report[2] == 0)) {
        return (id << 8) | report[1];
    }
    return -1;
}

void SteamControllerLink::writeFailedLocked(int generation, uint64_t nowMs)
{
    if (!m_Stopped && m_Connected && m_Generation == generation) {
        if (++m_OutputFailures < OUTPUT_FAILURES_BEFORE_DISCONNECT) {
            logf(LogWarn, "Steam Controller USB: output failed on interface %d (%d of %d)",
                 m_Info.interfaceNumber, m_OutputFailures, OUTPUT_FAILURES_BEFORE_DISCONNECT);
            return;
        }
        logf(LogWarn, "Steam Controller USB: output keeps failing on interface %d; retiring the slot", m_Info.interfaceNumber);
        m_RetryAtMs = nowMs + KEEPALIVE_MS;
        disconnectLocked(nowMs);
    }
}

void SteamControllerLink::writeSucceededLocked()
{
    m_OutputFailures = 0;
}

bool SteamControllerLink::steamHaptic(const uint8_t* report, int length, uint64_t nowMs, int expectedGeneration)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Stopped || !m_Connected || (expectedGeneration != 0 && expectedGeneration != m_Generation) ||
            report == nullptr || length <= 0 || length != outputReportSize(report[0])) {
        return false;
    }
    int id = report[0];
    // Invalid side values must not consume the finite stop-priority queue.
    if ((id == OUT_PULSE || id == OUT_HAPTIC_COMMAND) && report[1] > 2) {
        return false;
    }
    std::vector<uint8_t> copy(report, report + length);
    if (id == OUT_RUMBLE) {
        m_HasRepeatingRumble = stopKey(copy) < 0;
        m_RepeatingRumble = copy;
        m_RumbleDeadlineMs = m_RumbleHoldMs == 0 ? NEVER : nowMs + m_RumbleHoldMs;
        m_RumbleTickDueMs = nowMs + RUMBLE_TICK_MS;
        m_SummaryRumble++;
    }
    else if (id == OUT_PULSE) m_SummaryPulse[report[1]]++;
    else if (id == OUT_HAPTIC_COMMAND) m_SummaryCommand[report[1]]++;
    else m_SummaryOther++;
    enqueueLocked(std::move(copy), nowMs);
    if (wake) wake();
    return true;
}

bool SteamControllerLink::rumble(uint16_t lowFreqMotor, uint16_t highFreqMotor, uint64_t nowMs, int expectedGeneration)
{
    uint8_t report[10] = {OUT_RUMBLE, 0, 0, 0, (uint8_t)lowFreqMotor, (uint8_t)(lowFreqMotor >> 8),
                          0, (uint8_t)highFreqMotor, (uint8_t)(highFreqMotor >> 8), 0};
    return steamHaptic(report, sizeof(report), nowMs, expectedGeneration);
}

bool SteamControllerLink::setMotionRate(uint8_t motionType, uint16_t reportRateHz, int expectedGeneration)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Stopped || !m_Connected || (expectedGeneration != 0 && expectedGeneration != m_Generation)) {
        return false;
    }
    m_Core.setMotionRate(motionType, reportRateHz);
    return true;
}

void SteamControllerLink::replayForArrival(int generation)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Stopped || !m_Connected || m_Generation != generation) {
        return;
    }
    if (m_HasAttributes) m_Listener->linkAttributes(*this, generation, m_Attributes);
    if (m_HasBattery) m_Listener->linkBattery(*this, generation, m_BatteryState, m_BatteryPercent);
}

void SteamControllerLink::enqueueLocked(std::vector<uint8_t> report, uint64_t nowMs)
{
    int stop = stopKey(report);
    // Latest rumble wins; duplicate stop packets need only one queue entry.
    uint8_t id = report[0];
    m_Outputs.erase(std::remove_if(m_Outputs.begin(), m_Outputs.end(), [&](const Output& old) {
        return (id == OUT_RUMBLE && old.data[0] == OUT_RUMBLE) || (stop >= 0 && stopKey(old.data) == stop);
    }), m_Outputs.end());
    if ((int)m_Outputs.size() >= MAX_OUTPUTS) {
        auto victim = std::find_if(m_Outputs.begin(), m_Outputs.end(), [](const Output& o) { return stopKey(o.data) < 0; });
        // Stops are never evicted by effects. Each stop key is coalesced above.
        if (victim == m_Outputs.end()) return;
        m_Outputs.erase(victim);
    }
    Output out;
    out.data = std::move(report);
    out.seq = ++m_OutputSeq;
    m_Outputs.push_back(std::move(out));
    if (!m_DrainScheduled) {
        m_DrainScheduled = true;
        m_DrainDueMs = nowMs;
    }
}

bool SteamControllerLink::writeOutput(const std::vector<uint8_t>& report)
{
    // hidapi pads to the collection's output report length on Windows and reports that length;
    // hidraw writes exactly these bytes.
    return m_Device && m_Device->write(report.data(), (int)report.size()) >= (int)report.size();
}

// ----- link setup and attributes -----

bool SteamControllerLink::configure(bool active)
{
    // HID feature report 1: SET_SETTINGS_VALUES (0x87), three (id, u16) triples, framed as Steam
    // frames it on the wire (SET_REPORT, 64 bytes starting 01 87). Lizard mode off and the raw
    // IMU on while active; lizard mode back on and the IMU off at stop. The firmware's Steam
    // watchdog (71) stays disabled: armed, it reverts the settings when it never sees Steam's
    // heartbeat, which turned the cable-connected unit's IMU off between keep-alives (fork.119).
    uint8_t feature[FEATURE_REPORT_SIZE];
    memset(feature, 0, sizeof(feature));
    feature[0] = FEATURE_REPORT_ID;
    feature[1] = CMD_SET_SETTINGS_VALUES;
    feature[2] = 9;
    feature[3] = SETTING_LIZARD_MODE;
    feature[4] = active ? 0 : 1;
    feature[6] = SETTING_IMU_MODE;
    feature[7] = (active && m_Core.config().motion) ? IMU_MODE_RAW : 0;
    feature[9] = SETTING_STEAM_WATCHDOG_ENABLE;
    feature[10] = 0;
    return m_Device && m_Device->sendFeatureReport(feature, sizeof(feature)) >= (int)sizeof(feature);
}

void SteamControllerLink::logLinkSettingsLocked(bool ok)
{
    m_ConfigureState = ok ? 1 : 2;
    logf(LogInfo, "Steam Controller %s: link settings (feature report 1, interface %d): lizard mode 0, IMU mode 0x%x, Steam watchdog 0 -> %s",
         m_Name.c_str(), m_Info.interfaceNumber, m_Core.config().motion ? IMU_MODE_RAW : 0, ok ? "accepted" : "write failed");
}

bool SteamControllerLink::featureQuery(uint8_t type, uint8_t length, const uint8_t* payload, int payloadLength, std::vector<uint8_t>& reply)
{
    uint8_t command[FEATURE_REPORT_SIZE];
    memset(command, 0, sizeof(command));
    command[0] = FEATURE_REPORT_ID;
    command[1] = type;
    command[2] = length;
    if (payloadLength > 0) memcpy(command + 3, payload, payloadLength);
    if (!m_Device || m_Device->sendFeatureReport(command, sizeof(command)) < (int)sizeof(command)) {
        logf(LogWarn, "Steam Controller %s: attribute query 0x%02x not accepted", m_Name.c_str(), type);
        return false;
    }
    // One more byte than the report: the Windows HID stack wants the full report length.
    uint8_t buffer[FEATURE_REPORT_SIZE + 1];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = FEATURE_REPORT_ID;
    int read = m_Device->getFeatureReport(buffer, sizeof(buffer));
    if (read < 3 || buffer[0] != FEATURE_REPORT_ID || buffer[1] != type) {
        if (read < 0) logf(LogWarn, "Steam Controller %s: no reply to attribute query 0x%02x", m_Name.c_str(), type);
        else logf(LogWarn, "Steam Controller %s: no reply to attribute query 0x%02x (%d bytes)", m_Name.c_str(), type, read);
        return false;
    }
    int payloadSize = std::min<int>(buffer[2], read - 3);
    reply.assign(buffer + 3, buffer + 3 + payloadSize);
    return true;
}

void SteamControllerLink::readAttributes(int generation)
{
    // Once per connect, after a successful settings write: the real unit's firmware attributes
    // for the host (GET_ATTRIBUTES_VALUES, GET_DEVICE_INFO sub 0, the unit and board serials),
    // each asked the way Steam asks: SET_REPORT of the command on feature report 1, then
    // GET_REPORT of the same report for the reply. A failed query leaves its fields out of the
    // valid mask and does not count against the output budget.
    SteamControllerAttributes attributes(isPuck() ? LI_SCATTR_TRANSPORT_PUCK : LI_SCATTR_TRANSPORT_USB);
    std::vector<uint8_t> reply;
    static const uint8_t subZero = 0, tagUnit = 1, tagBoard = 0;
    if (featureQuery(CMD_GET_ATTRIBUTES_VALUES, 0, nullptr, 0, reply)) attributes.parseAttributeValues(reply.data(), (int)reply.size());
    if (featureQuery(CMD_GET_DEVICE_INFO, 1, &subZero, 1, reply)) attributes.parseDeviceInfo(reply.data(), (int)reply.size());
    if (featureQuery(CMD_GET_STRING_ATTRIBUTE, STRING_ATTRIBUTE_REQUEST_LENGTH, &tagUnit, 1, reply)) attributes.parseStringAttribute(reply.data(), (int)reply.size());
    if (featureQuery(CMD_GET_STRING_ATTRIBUTE, STRING_ATTRIBUTE_REQUEST_LENGTH, &tagBoard, 1, reply)) attributes.parseStringAttribute(reply.data(), (int)reply.size());

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Stopped || !m_Connected || m_Generation != generation) {
        return;   // a reading from a retired link is dropped
    }
    const char* transport = isPuck() ? "puck" : "usb";
    logf(LogInfo, "Steam Controller %s (%s): %s", transport, m_Name.c_str(), attributes.describe().c_str());
    if (attributes.isEmpty()) {
        return;
    }
    logf(LogWarn, "Steam Controller %s (%s): %s", transport, m_Name.c_str(), FIRMWARE_UPDATE_WARNING);
    m_Attributes = attributes;
    m_HasAttributes = true;
    m_Listener->linkAttributes(*this, generation, attributes);
}

// ----- service thread -----

uint64_t SteamControllerLink::service(uint64_t nowMs)
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    if (m_Stopped) {
        return NEVER;
    }
    uint64_t next = NEVER;
    if (!m_Connected) {
        return next;
    }
    const int generation = m_Generation;

    // Link setup and keep-alive: the settings write at connect and then every second.
    if (!m_ConfigureBusy && (m_ConfigurePending || nowMs >= m_KeepAliveDueMs)) {
        m_ConfigurePending = false;
        m_ConfigureBusy = true;
        lock.unlock();
        bool ok = configure(true);
        lock.lock();
        m_ConfigureBusy = false;
        if (m_Stopped) return NEVER;
        if (m_Connected && m_Generation == generation) {
            m_KeepAliveDueMs = nowMs + KEEPALIVE_MS;
            if (m_ConfigureState != (ok ? 1 : 2)) logLinkSettingsLocked(ok);
            if (ok) {
                writeSucceededLocked();
                // Initial setup can fail without retiring the slot: read the attributes after the
                // first successful write, once per connected generation.
                if (!m_AttributesQueried) {
                    m_AttributesQueried = true;
                    lock.unlock();
                    readAttributes(generation);
                    lock.lock();
                    if (m_Stopped) return NEVER;
                }
            }
            else {
                writeFailedLocked(generation, nowMs);
            }
        }
        if (!m_Connected || m_Generation != generation) {
            return next;
        }
    }
    next = std::min(next, m_KeepAliveDueMs);

    // Rumble hold: a non-stop 0x80 is re-sent every 40 ms until the deadline, then the motors stop.
    if (m_HasRepeatingRumble && nowMs >= m_RumbleTickDueMs) {
        if (nowMs >= m_RumbleDeadlineMs) {
            m_HasRepeatingRumble = false;
            enqueueLocked(motorStop(), nowMs);
        }
        else {
            // A fresh copy each tick: enqueue() drops any queued 0x80, including the one a slow
            // transfer is still writing, and that transfer's completion then removes its own copy.
            enqueueLocked(m_RepeatingRumble, nowMs);
            m_RumbleTickDueMs = nowMs + RUMBLE_TICK_MS;
        }
    }
    if (m_HasRepeatingRumble) next = std::min(next, m_RumbleTickDueMs);

    // One transfer per dispatch gives other puck slots a turn.
    if (m_DrainScheduled && !m_DrainBusy && nowMs >= m_DrainDueMs) {
        if (m_Outputs.empty()) {
            m_DrainScheduled = false;
        }
        else {
            std::vector<uint8_t> report = m_Outputs.front().data;
            const uint64_t seq = m_Outputs.front().seq;
            m_DrainBusy = true;
            lock.unlock();
            bool success = writeOutput(report);
            lock.lock();
            m_DrainBusy = false;
            if (m_Stopped) return NEVER;
            // A disconnect may have queued work for a new controller while this transfer was in
            // flight. Its queue and failure budget belong to it.
            if (m_Generation == generation) {
                if (success) {
                    writeSucceededLocked();
                    m_HasStrikedReport = false;
                    m_RetryDelayMs = OUTPUT_RETRY_MS;
                }
                else if (!m_HasStrikedReport || m_StrikedSeq != seq) {
                    // A retained stop that keeps failing is one strike, not one per retry.
                    m_HasStrikedReport = true;
                    m_StrikedSeq = seq;
                    writeFailedLocked(generation, nowMs);
                }
                // Retain an idempotent motor stop until it succeeds or the slot retires. Keeping it
                // in the bounded queue also lets newer 0x80 rumble replace an obsolete stop. Failed
                // effects are not replayed.
                if (success || stopKey(report) < 0) {
                    auto it = std::find_if(m_Outputs.begin(), m_Outputs.end(), [&](const Output& o) { return o.seq == seq; });
                    if (it != m_Outputs.end()) m_Outputs.erase(it);
                }
                if (!m_Stopped && m_Connected && m_Generation == generation && !m_Outputs.empty()) {
                    // Back off failures (40, 80, ... 1000 ms) so retries cannot spin or starve
                    // other slots; the keep-alive stays the liveness authority.
                    m_DrainDueMs = success ? nowMs : nowMs + m_RetryDelayMs;
                    if (!success) m_RetryDelayMs = std::min(m_RetryDelayMs * 2, KEEPALIVE_MS);
                }
                else {
                    m_DrainScheduled = false;
                }
            }
        }
    }
    if (m_Connected && m_DrainScheduled && !m_Outputs.empty()) next = std::min(next, m_DrainDueMs);

    next = std::min(next, logSummaryLocked(nowMs));
    return next;
}

uint64_t SteamControllerLink::logSummaryLocked(uint64_t nowMs)
{
    if (nowMs < m_SummaryDueMs) {
        return m_SummaryDueMs;
    }
    unsigned haptics = m_SummaryRumble + m_SummaryOther;
    for (int i = 0; i < 3; i++) haptics += m_SummaryPulse[i] + m_SummaryCommand[i];
    if (m_SummaryReports != 0 || haptics != 0) {
        logf(LogInfo, "Steam Controller %s: %u reports/s, pads touched L %s R %s, host haptics: rumble %u, pulse L/R/both %u/%u/%u, command L/R/both %u/%u/%u, other %u",
             m_Name.c_str(), (unsigned)(m_SummaryReports * 1000 / SUMMARY_MS),
             m_SummaryLeftPad ? "yes" : "no", m_SummaryRightPad ? "yes" : "no",
             m_SummaryRumble, m_SummaryPulse[0], m_SummaryPulse[1], m_SummaryPulse[2],
             m_SummaryCommand[0], m_SummaryCommand[1], m_SummaryCommand[2], m_SummaryOther);
    }
    m_SummaryReports = m_SummaryRumble = m_SummaryOther = 0;
    for (int i = 0; i < 3; i++) m_SummaryPulse[i] = m_SummaryCommand[i] = 0;
    m_SummaryLeftPad = m_SummaryRightPad = false;
    m_SummaryDueMs = nowMs + SUMMARY_MS;
    return m_SummaryDueMs;
}

// ----- stop -----

void SteamControllerLink::stop(uint64_t nowMs)
{
    bool restore;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopped) return;
        restore = m_Connected && !m_Lost;
        disconnectLocked(nowMs);
        m_Stopped = true;
    }
    if (restore) {
        // Stop internal rumble and both pad pulse/command engines, then lizard mode back on so
        // the controller works as keyboard/mouse on the desktop and in the app's menus.
        writeOutput(motorStop());
        for (uint8_t side = 0; side < 2; side++) {
            writeOutput(std::vector<uint8_t>{OUT_PULSE, side, 0, 0, 0, 0, 0, 0});
            writeOutput(std::vector<uint8_t>{OUT_HAPTIC_COMMAND, side, 0, 0});
        }
        bool ok = configure(false);
        logf(LogInfo, "Steam Controller %s: stop sequence sent (lizard mode 1, IMU mode 0 -> %s)", m_Name.c_str(), ok ? "accepted" : "write failed");
    }
    m_Device.reset();
}

// ===== manager =====

std::atomic<int> SteamControllerManager::s_NextId{1};

SteamControllerManager::SteamControllerManager(SteamHidTransport& transport, const SteamControllerConfig& config,
                                               int rumbleHoldMs, SteamControllerLink::Listener* listener)
    : m_Transport(transport), m_Config(config), m_RumbleHoldMs(rumbleHoldMs), m_Listener(listener)
{
}

bool SteamControllerManager::isCandidate(const SteamHidDeviceInfo& info)
{
    if (info.vendorId != VENDOR_VALVE) return false;
    if (info.productId == PRODUCT_WIRED) return info.interfaceNumber <= 0;   // -1 when the platform cannot tell
    if (info.productId == PRODUCT_PUCK) return info.interfaceNumber >= 2 && info.interfaceNumber <= 5;
    return false;
}

std::vector<std::shared_ptr<SteamControllerLink>> SteamControllerManager::links() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Links;
}

std::shared_ptr<SteamControllerLink> SteamControllerManager::find(int id) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& link : m_Links) {
        if (link->id() == id) return link;
    }
    return nullptr;
}

void SteamControllerManager::dropLink(const std::shared_ptr<SteamControllerLink>& link, uint64_t nowMs)
{
    if (onLinkClosing) onLinkClosing(link);
    link->stop(nowMs);
}

void SteamControllerManager::rescan(uint64_t nowMs)
{
    std::vector<SteamHidDeviceInfo> infos = m_Transport.enumerate();
    std::vector<std::shared_ptr<SteamControllerLink>> dropped, opened;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopped) return;
        m_RescanRequested = false;
        m_RescanDueMs = nowMs + RESCAN_MS;

        for (auto it = m_Links.begin(); it != m_Links.end();) {
            const auto& link = *it;
            bool present = std::any_of(infos.begin(), infos.end(), [&](const SteamHidDeviceInfo& i) { return i.path == link->info().path; });
            if (link->lost() || !present) {
                logf(LogInfo, "Steam Controller %s: device gone (%s)", link->name().c_str(), link->lost() ? "read failed" : "no longer enumerated");
                dropped.push_back(link);
                it = m_Links.erase(it);
            }
            else {
                ++it;
            }
        }
        // Paths that are no longer enumerated may be probed again if they come back.
        m_RejectedPaths.erase(std::remove_if(m_RejectedPaths.begin(), m_RejectedPaths.end(), [&](const std::string& path) {
            return std::none_of(infos.begin(), infos.end(), [&](const SteamHidDeviceInfo& i) { return i.path == path; });
        }), m_RejectedPaths.end());

        for (const SteamHidDeviceInfo& info : infos) {
            if (!isCandidate(info)) continue;
            // A path dropped in this pass (its reads failed) is retried at the next rescan, not now.
            bool known = std::any_of(m_Links.begin(), m_Links.end(), [&](const std::shared_ptr<SteamControllerLink>& l) { return l->info().path == info.path; })
                      || std::any_of(dropped.begin(), dropped.end(), [&](const std::shared_ptr<SteamControllerLink>& l) { return l->info().path == info.path; })
                      || std::find(m_RejectedPaths.begin(), m_RejectedPaths.end(), info.path) != m_RejectedPaths.end();
            if (known) continue;
            // Windows and macOS list one path per top-level collection: the vendor collection is the
            // one on usage page 0xFF00 (the real unit has three; the vendor one is the third). Where
            // the usage is missing or describes the interface's first collection (Linux hidraw), the
            // path that answers a feature report 1 read is the one.
            std::unique_ptr<SteamHidDevice> device = m_Transport.open(info);
            if (!device) {
                logf(LogWarn, "Steam Controller: cannot open %s (product 0x%04x, interface %d); check the hidraw permissions (udev rule) on Linux",
                     info.path.c_str(), info.productId, info.interfaceNumber);
                // Open can fail temporarily while another application releases
                // the device or permissions settle. Retry at the next bounded
                // rescan; only a failed collection probe is cached below.
                continue;
            }
            const char* how = "usage page 0xff00";
            if (info.usagePage != VENDOR_USAGE_PAGE) {
                uint8_t probe[FEATURE_REPORT_SIZE + 1];
                memset(probe, 0, sizeof(probe));
                probe[0] = FEATURE_REPORT_ID;
                if (device->getFeatureReport(probe, sizeof(probe)) < 1) {
                    m_RejectedPaths.push_back(info.path);
                    continue;
                }
                how = "accepted a feature report 1 read";
            }
            SteamHidDeviceInfo opened_info = info;
            if (opened_info.interfaceNumber < 0) opened_info.interfaceNumber = 0;
            auto link = std::make_shared<SteamControllerLink>(s_NextId++, opened_info, std::move(device), m_Config, m_RumbleHoldMs, m_Listener);
            logf(LogInfo, "Steam Controller: opened %s (product 0x%04x, interface %d, usage page 0x%04x; %s) as %s",
                 info.path.c_str(), info.productId, info.interfaceNumber, info.usagePage, how, link->name().c_str());
            m_Links.push_back(link);
            opened.push_back(link);
        }
    }
    for (const auto& link : dropped) dropLink(link, nowMs);
    for (const auto& link : opened) if (onLinkOpened) onLinkOpened(link);
}

uint64_t SteamControllerManager::service(uint64_t nowMs)
{
    bool lost = false;
    uint64_t rescanDue;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopped) return NEVER;
        rescanDue = m_RescanDueMs;
        for (const auto& link : m_Links) lost |= link->lost();
    }
    if (m_RescanRequested || lost || nowMs >= rescanDue) {
        rescan(nowMs);
    }
    uint64_t next;
    std::vector<std::shared_ptr<SteamControllerLink>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopped) return NEVER;
        next = m_RescanDueMs;
        snapshot = m_Links;
    }
    for (const auto& link : snapshot) {
        next = std::min(next, link->service(nowMs));
    }
    return next;
}

void SteamControllerManager::stop(uint64_t nowMs)
{
    std::vector<std::shared_ptr<SteamControllerLink>> links;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopped) return;
        m_Stopped = true;
        links.swap(m_Links);
    }
    for (const auto& link : links) dropLink(link, nowMs);
}
