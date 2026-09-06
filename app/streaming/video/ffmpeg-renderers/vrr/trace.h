#pragma once

#include "timing.h"
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace Vrr {
enum class Event : uint64_t {
    Config = 1, Arrival, Plan, Prepared, Render, Wake, Submit, Feedback,
    Drop, Reset, Stop, CheckpointBegin, CheckpointData, CheckpointEnd, PrepareStages, Build, OverlayWork, Recovery, Reserve
};
enum class Drop : uint64_t { Capacity, Stale, PrepareFailed, PresentFailed, Shutdown };

struct Record {
    uint64_t serial = 0;
    Event event = Event::Config;
    uint64_t id = 0;
    Ns at = 0;
    std::array<int64_t, 12> data{};
};

// A flight recorder, not a streaming logger. Fixed allocation; overwrite old
// records; never open/write/flush a file on the render or decoder thread.
// Snapshots are explicit little-endian integers (no ABI padding or pointers).
class Trace {
public:
    static constexpr size_t Capacity = 32768;
    Trace();
    void setBuildHash(const std::array<int64_t, 4>& hash) { m_BuildHash = hash; }
    void add(Event event, uint64_t id, Ns at, std::initializer_list<int64_t> data = {});
    void checkpoint(const Controller& controller, Ns at);
    bool save(const std::string& path) const; // Call only after joining producers.
    static bool read(const std::string& path, std::vector<Record>& records);
private:
    void append(Record record);
    mutable std::mutex m_Lock;
    std::vector<Record> m_Records;
    uint64_t m_Serial = 0;
    std::array<int64_t, 4> m_BuildHash{};
};
}
