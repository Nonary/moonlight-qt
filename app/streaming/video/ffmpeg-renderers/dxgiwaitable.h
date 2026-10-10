#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

// Admission is queue capacity, never scanout/blanking evidence. Only the render
// worker acquires/consumes a permit; interruption is safe on the UI/owner thread.
namespace DxgiWaitable {
constexpr unsigned MaximumFrameLatency = 2;
inline bool compositionRequested(bool vrr, const char* overrideValue)
{
    return vrr && overrideValue && std::strcmp(overrideValue, "1") == 0;
}
constexpr bool requested(bool vrr, bool vsync, bool testOnly, bool composition)
{
    return vrr && vsync && !testOnly && !composition;
}
enum class Status { Admitted, Interrupted, Timeout, Failed };
enum class Wake { Signalled, Timeout, Failed };
class Admission {
public:
    uint64_t epoch() const { return m_Epoch.load(); }
    bool interrupted(uint64_t expected) const
    {
        return m_Stopping.load() || epoch() != expected;
    }
    void interrupt(bool stopping)
    {
        if (stopping) m_Stopping.store(true);
        m_Epoch.fetch_add(1);
    }
    void presented() { m_Held = false; }

    template<class Clock, class Wait>
    Status acquire(uint64_t expected, Clock&& clock, Wait&& wait)
    {
        const auto start = clock();
        // Both wall time and iteration bounds are needed if a clock stalls.
        for (unsigned attempt = 0; attempt < 50; ++attempt) {
            if (interrupted(expected)) return Status::Interrupted;
            if (m_Held) return Status::Admitted;
            const auto now = clock();
            if (now < start || now - start >= 50000) return Status::Timeout;
            const auto wake = wait(1); // Never hold presentation/decode locks here.
            if (wake == Wake::Failed) return Status::Failed;
            if (wake == Wake::Signalled) {
                // Retain an acquired permit if this attempt is cancelled before
                // Present. Waiting again could strand an auto-reset signal.
                m_Held = true;
                return interrupted(expected) ? Status::Interrupted : Status::Admitted;
            }
        }
        return Status::Timeout;
    }
private:
    std::atomic<uint64_t> m_Epoch{0};
    std::atomic<bool> m_Stopping{false};
    bool m_Held = false;
};
}
