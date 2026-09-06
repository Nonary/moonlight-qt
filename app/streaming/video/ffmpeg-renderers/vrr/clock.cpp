#include "timing.h"

#include <chrono>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <time.h>
#endif

namespace Vrr {
Ns now()
{
#ifdef _WIN32
    static const auto frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart / frequency * Second + counter.QuadPart % frequency * Second / frequency;
#else
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return Ns(t.tv_sec) * Second + t.tv_nsec;
#endif
}

bool waitUntil(Ns deadline, const std::atomic<bool>& stopping, Ns yieldTail)
{
#ifdef _WIN32
    // One high resolution timer per rendering thread. No global timer-period change.
    struct Timer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        ~Timer() { if (handle) CloseHandle(handle); }
    };
    thread_local Timer timer;
#endif
    while (!stopping.load(std::memory_order_relaxed)) {
        const Ns at = now();
        if (at >= deadline) return true;
        // Short cancellable sleeps; only the final 50 us uses scheduler yielding.
        // No millisecond truncation or unconditional multi-ms busy spin.
        const Ns duration = std::min(deadline - at - yieldTail, 2 * Millisecond);
        if (duration <= 0) { std::this_thread::yield(); continue; }
#ifdef _WIN32
        if (timer.handle) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<Ns>(1, duration / 100);
            if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer.handle, 5);
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(duration));
#elif defined(__linux__)
        const Ns target = at + duration;
        timespec t{time_t(target / Second), long(target % Second)};
        // Absolute monotonic deadlines survive EINTR without accumulating drift.
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr);
#else
        std::this_thread::sleep_for(std::chrono::nanoseconds(duration));
#endif
    }
    return false;
}
}
