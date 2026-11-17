#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <mutex>

// When each decode boundary finished on the GPU, timestamped as it happened.
//
// The pacing worker learns a frame's decode completion only by waiting on its
// fence, and it can wait only once it has presented the previous frame. With a
// playout buffer longer than one source interval the worker reaches every
// fast frame after it finished, and all it can then report is its own arrival
// time. That made the measurement depend on the buffer it sizes: on a
// recorded 4K 138 FPS stream (Radeon 780M, decode 3 to 8.5 ms a frame) the
// share of frames the worker still blocked on fell from 95% to 63% above
// 7.25 ms of buffer, the source-clock mapping rose 2 ms while the solved
// buffer fell 2 ms, and both swung back every 10 to 15 s.
//
// A watcher that does nothing else waits on the same fence for every boundary
// in turn, so the timestamp no longer depends on what the worker is doing.
namespace D3D11DecodeCompletion {

class Log {
public:
    void record(uint64_t boundary, uint64_t completedUs)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Entries[m_Next] = {boundary, completedUs};
        m_Next = (m_Next + 1) % m_Entries.size();
    }

    // Zero when the watcher did not see this boundary complete.
    uint64_t completedUs(uint64_t boundary) const
    {
        if (!boundary) return 0;
        std::lock_guard<std::mutex> lock(m_Lock);
        for (const auto& entry : m_Entries) {
            if (entry.boundary == boundary) return entry.completedUs;
        }
        return 0;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Entries = {};
        m_Next = 0;
    }

    // More than the pacing queue can hold between completion and collection.
    static constexpr size_t Capacity = 64;

private:
    struct Entry { uint64_t boundary = 0, completedUs = 0; };
    mutable std::mutex m_Lock;
    std::array<Entry, Capacity> m_Entries {};
    size_t m_Next = 0;
};

// Runs until wait() reports a stop, arming fails or the device is removed.
//   clock(): microseconds on the pacing clock
//   poll():  the fence's completed value
//   arm(v):  request a wake when the fence reaches v; false on failure
//   wait():  block for that wake; false to stop
//
// Boundaries that completed before the watcher started are never recorded:
// when they finished is not known. A boundary recorded here completed while
// this thread was waiting for it, so its time is the completion plus this
// thread's wake latency. Several boundaries found complete at one wake share
// that time; the later ones finished between the wake and the poll.
template<class Clock, class Poll, class Arm, class Wait>
void watch(Log& log, Clock&& clock, Poll&& poll, Arm&& arm, Wait&& wait)
{
    constexpr uint64_t removed = (std::numeric_limits<uint64_t>::max)();
    uint64_t completed = poll();
    if (completed == removed) return;
    uint64_t next = completed + 1;
    for (;;) {
        if (!arm(next)) return;
        uint64_t nowUs = 0;
        do {
            // A wake left over from an earlier boundary proves nothing; the
            // request for this one is still pending, so wait on it again.
            if (!wait()) return;
            nowUs = clock();
            completed = poll();
            if (completed == removed) return;
        } while (completed < next);
        // A long gap cannot leave more unrecorded boundaries than a reader
        // could still ask for.
        if (completed - next >= Log::Capacity) next = completed - (Log::Capacity - 1);
        for (uint64_t boundary = next; boundary <= completed; ++boundary) log.record(boundary, nowUs);
        next = completed + 1;
    }
}

// What the pacing worker reports for one frame. A frame it had to wait for
// completed during that wait, which a wake on the fence's own event bounds as
// tightly as the watcher does; the earlier of the two is used. A frame that
// had already finished is known only to the watcher.
inline uint64_t resolve(uint64_t watchedUs, bool workerWaitedOnEvent, uint64_t nowUs)
{
    if (watchedUs && watchedUs <= nowUs) return watchedUs;
    return workerWaitedOnEvent ? nowUs : 0;
}
}
