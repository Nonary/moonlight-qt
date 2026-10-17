// SPDX-License-Identifier: GPL-3.0-or-later
// Steam Controller (2026) HID transport layer: one SteamControllerLink per open vendor interface
// (the cable-connected unit's interface 0, or one of the wireless puck's interfaces 2..5), the
// SteamControllerManager that enumerates and owns them, and the SteamHidTransport they talk
// through. Port of the Android client's SteamControllerUsb: link setup through feature report 1,
// keep-alives, the firmware attribute queries, the bounded output queue with stop priority,
// rumble hold and the stop sequence. Everything is driven by explicit timestamps (handleReport,
// service, stop) so the SDL runner threads and the unit tests share one implementation.
#pragma once

#include "steamcontrollerattributes.h"
#include "steamcontrollercore.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SteamHidDeviceInfo {
    std::string path;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    int interfaceNumber = -1;
    uint16_t usagePage = 0;
    uint16_t usage = 0;
};

// One open HID handle. read() returns bytes read (buf[0] = report id), 0 on timeout, < 0 on error;
// the others return bytes transferred or < 0.
class SteamHidDevice {
public:
    virtual ~SteamHidDevice() = default;
    virtual int read(uint8_t* buffer, int length, int timeoutMs) = 0;
    virtual int write(const uint8_t* data, int length) = 0;
    virtual int sendFeatureReport(const uint8_t* data, int length) = 0;
    virtual int getFeatureReport(uint8_t* buffer, int length) = 0;
};

class SteamHidTransport {
public:
    virtual ~SteamHidTransport() = default;
    virtual std::vector<SteamHidDeviceInfo> enumerate() = 0;
    virtual std::unique_ptr<SteamHidDevice> open(const SteamHidDeviceInfo& info) = 0;
};

class SteamControllerLink : public SteamControllerSink {
public:
    static constexpr int IO_TIMEOUT_MS = 250;
    static constexpr int READ_TIMEOUT_MS = 250;
    static constexpr int KEEPALIVE_MS = 1000;
    static constexpr int STALE_STATE_MS = 3000;
    static constexpr int MAX_OUTPUTS = 32;
    static constexpr int OUTPUT_RETRY_MS = 40;
    static constexpr int OUTPUT_FAILURES_BEFORE_DISCONNECT = 3;
    static constexpr int RUMBLE_TICK_MS = 40;
    static constexpr int SUMMARY_MS = 10000;

    // Host-side events. Called with the link's mutex held (see steamcontrollerhost.cpp for the
    // lock order); implementations must not call back into the link except through the
    // lock-free requestInputReset().
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void linkConnected(SteamControllerLink& link, int generation) = 0;
        virtual void linkDisconnected(SteamControllerLink& link, int generation) = 0;
        virtual void linkState(SteamControllerLink& link, int generation, int buttonFlags, uint8_t lt, uint8_t rt,
                               short lsX, short lsY, short rsX, short rsY) = 0;
        virtual void linkTouch(SteamControllerLink& link, int generation, uint8_t touchpadIndex, uint8_t eventType,
                               uint32_t pointerId, float x, float y, float pressure) = 0;
        virtual void linkMotion(SteamControllerLink& link, int generation, uint8_t motionType, float x, float y, float z) = 0;
        virtual void linkBattery(SteamControllerLink& link, int generation, uint8_t state, uint8_t percent) = 0;
        virtual void linkAttributes(SteamControllerLink& link, int generation, const SteamControllerAttributes& attributes) = 0;
    };

    SteamControllerLink(int id, const SteamHidDeviceInfo& info, std::unique_ptr<SteamHidDevice> device,
                        const SteamControllerConfig& config, int rumbleHoldMs, Listener* listener);
    ~SteamControllerLink() override;

    int id() const { return m_Id; }
    const SteamHidDeviceInfo& info() const { return m_Info; }
    uint16_t productId() const { return m_Info.productId; }
    bool isPuck() const { return m_Info.productId == SteamController::PRODUCT_PUCK; }
    // Puck slot 0..3 (interface - 2) or 0 for the cable.
    int slot() const { return isPuck() ? m_Info.interfaceNumber - 2 : 0; }
    // "USB slot N" as the Android client names it, N = HID interface number.
    const std::string& name() const { return m_Name; }
    const SteamControllerConfig& config() const { return m_Core.config(); }
    uint16_t capabilities() const { return m_Core.capabilities(); }

    // Reader thread: blocking read on the device (no lock; the device lives until stop()).
    int readReport(uint8_t* buffer, int length, int timeoutMs) { return m_Device ? m_Device->read(buffer, length, timeoutMs) : -1; }
    // One input report; stale-state check; read failure bookkeeping.
    void handleReport(const uint8_t* report, int size, uint64_t nowMs);
    void checkStale(uint64_t nowMs);
    void noteReadError() { m_ReadErrors++; }
    int readErrors() const { return m_ReadErrors; }

    // Service thread: link setup, keep-alive, attribute reads, output drain, rumble hold, summary.
    // Returns the next time (ms) it wants to run, or UINT64_MAX when idle.
    uint64_t service(uint64_t nowMs);

    // Host side (any thread). Haptic reports carry their id first; invalid ones are dropped.
    void steamHaptic(const uint8_t* report, int length, uint64_t nowMs);
    void rumble(uint16_t lowFreqMotor, uint16_t highFreqMotor, uint64_t nowMs);
    void setMotionRate(uint8_t motionType, uint16_t reportRateHz);
    // The next report resends its state and pad DOWNs (after an announcement). Lock-free.
    void requestInputReset() { m_InputResetPending = true; }
    // Re-delivers the cached attributes and battery through the listener (after an announcement).
    void replayForArrival(int generation);

    bool connected() const;
    int generation() const;
    SteamControllerAttributes attributes() const;
    bool hasBattery() const;
    size_t queuedOutputs() const;

    // Stop sequence (stream end): motors and pad engines off, lizard mode back on, link retired.
    // The reader and service threads must have stopped first.
    void stop(uint64_t nowMs);
    // A read error said the device is gone: the manager drops the link without the restore writes.
    bool stopped() const { return m_Stopped; }
    void markLost() { m_Lost = true; }
    bool lost() const { return m_Lost; }
    // The reader thread must exit (the link is about to be stopped and closed).
    void requestClose() { m_Closing = true; }
    bool closing() const { return m_Closing; }

    // Wakes the service thread after a host-side enqueue (set by the runner).
    std::function<void()> wake;

private:
    // SteamControllerSink (called by the core under m_Mutex)
    void steamState(int buttonFlags, uint8_t lt, uint8_t rt, short lsX, short lsY, short rsX, short rsY) override;
    void steamTouch(uint8_t touchpadIndex, uint8_t eventType, uint32_t pointerId, float x, float y, float pressure) override;
    void steamMotion(uint8_t motionType, float x, float y, float z) override;
    void steamBattery(uint8_t state, uint8_t percent) override;

    struct Output { std::vector<uint8_t> data; uint64_t seq = 0; };

    void connectLocked(uint64_t nowMs);
    void disconnectLocked(uint64_t nowMs);
    void writeFailedLocked(int generation, uint64_t nowMs);
    void writeSucceededLocked();
    void enqueueLocked(std::vector<uint8_t> report, uint64_t nowMs);
    void logLinkSettingsLocked(bool ok);
    bool configure(bool active);
    bool featureQuery(uint8_t type, uint8_t length, const uint8_t* payload, int payloadLength, std::vector<uint8_t>& reply);
    void readAttributes(int generation);
    bool writeOutput(const std::vector<uint8_t>& report);
    static int stopKey(const std::vector<uint8_t>& report);
    uint64_t logSummaryLocked(uint64_t nowMs);

    const int m_Id;
    const SteamHidDeviceInfo m_Info;
    const std::string m_Name;
    std::unique_ptr<SteamHidDevice> m_Device;
    Listener* const m_Listener;
    const int m_RumbleHoldMs;

    mutable std::mutex m_Mutex;
    SteamControllerCore m_Core;
    bool m_Connected = false;
    int m_Generation = 0;
    uint64_t m_LastStateMs = 0, m_RetryAtMs = 0;
    std::atomic<bool> m_InputResetPending{false};
    std::atomic<bool> m_Stopped{false};
    std::atomic<bool> m_Lost{false};
    std::atomic<bool> m_Closing{false};
    std::atomic<int> m_ReadErrors{0};

    // Setup / keep-alive / attributes
    bool m_ConfigurePending = false;     // first settings write after connect
    bool m_ConfigureBusy = false;        // a settings write is in flight (unlocked I/O)
    uint64_t m_KeepAliveDueMs = 0;
    int m_ConfigureState = 0;            // 0 not logged yet, 1 accepted, 2 failed
    bool m_AttributesQueried = false;    // one query sequence per generation
    SteamControllerAttributes m_Attributes;
    bool m_HasAttributes = false;
    bool m_HasBattery = false;
    uint8_t m_BatteryState = 0, m_BatteryPercent = 0;

    // Output budget
    std::deque<Output> m_Outputs;
    uint64_t m_OutputSeq = 0;
    uint64_t m_DrainDueMs = 0;
    bool m_DrainScheduled = false;
    bool m_DrainBusy = false;
    int m_OutputFailures = 0;
    bool m_HasStrikedReport = false;
    uint64_t m_StrikedSeq = 0;
    int m_RetryDelayMs = OUTPUT_RETRY_MS;

    // Rumble hold
    bool m_HasRepeatingRumble = false;
    std::vector<uint8_t> m_RepeatingRumble;
    uint64_t m_RumbleDeadlineMs = 0, m_RumbleTickDueMs = 0;

    // Summary (10 s)
    uint64_t m_SummaryDueMs = 0;
    unsigned m_SummaryReports = 0, m_SummaryRumble = 0;
    unsigned m_SummaryPulse[3] = {0, 0, 0}, m_SummaryCommand[3] = {0, 0, 0}, m_SummaryOther = 0;
    bool m_SummaryLeftPad = false, m_SummaryRightPad = false;
};

// Enumerates the Steam Controller interfaces, opens the vendor collection of each and owns the
// links. rescan()/service() are called by the runner's service thread (or the test).
class SteamControllerManager {
public:
    static constexpr int RESCAN_MS = 2000;

    SteamControllerManager(SteamHidTransport& transport, const SteamControllerConfig& config, int rumbleHoldMs,
                           SteamControllerLink::Listener* listener);

    // Called when a link was opened (start a reader) and right before one is dropped (stop it).
    std::function<void(const std::shared_ptr<SteamControllerLink>&)> onLinkOpened;
    std::function<void(const std::shared_ptr<SteamControllerLink>&)> onLinkClosing;

    // Enumerate now; open new vendor interfaces; drop links whose path vanished or whose reader
    // reported the device gone.
    void rescan(uint64_t nowMs);
    void requestRescan() { m_RescanRequested = true; }
    // Services every link and rescans when due. Returns the next time it wants to run.
    uint64_t service(uint64_t nowMs);
    // Stop sequence on every link, then drop them all.
    void stop(uint64_t nowMs);

    std::vector<std::shared_ptr<SteamControllerLink>> links() const;
    std::shared_ptr<SteamControllerLink> find(int id) const;

    static bool isCandidate(const SteamHidDeviceInfo& info);

private:
    void dropLink(const std::shared_ptr<SteamControllerLink>& link, uint64_t nowMs);

    SteamHidTransport& m_Transport;
    const SteamControllerConfig m_Config;
    const int m_RumbleHoldMs;
    SteamControllerLink::Listener* const m_Listener;
    mutable std::mutex m_Mutex;
    std::vector<std::shared_ptr<SteamControllerLink>> m_Links;
    std::vector<std::string> m_RejectedPaths;   // probed and refused (not the vendor collection)
    static std::atomic<int> s_NextId;           // process-wide: ids never repeat across sessions
    uint64_t m_RescanDueMs = 0;
    std::atomic<bool> m_RescanRequested{true};
    bool m_Stopped = false;
};
