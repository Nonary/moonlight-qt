#include "highressleep.h"

#include <SDL.h>
#include <Limelight.h>

#ifdef Q_OS_WIN32
#include <Windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

void HighResSleep::sleepUntilUs(uint64_t targetUs)
{
    uint64_t nowUs = LiGetMicroseconds();
    if (targetUs <= nowUs) {
        return;
    }

#ifdef Q_OS_WIN32
    // A high-resolution waitable timer gets us within ~0.5ms of the target
    // without burning CPU, unlike Sleep()/SDL_Delay() which are limited by
    // the (typically fired-down) system timer resolution.
    static thread_local HANDLE waitableTimer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (waitableTimer == nullptr) {
        SDL_Delay(0);
        return;
    }

    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -((LONGLONG)(targetUs - nowUs) * 10);
    if (SetWaitableTimer(waitableTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(waitableTimer, INFINITE);
    }
    else {
        SDL_Delay(0);
    }
#elif defined(Q_OS_LINUX)
    // clock_nanosleep() against an absolute deadline: hrtimer-backed, so
    // wake latency is scheduler-bound (~50-200us) rather than tick-bound,
    // and TIMER_ABSTIME means an EINTR restart re-targets the SAME deadline
    // instead of accumulating signal-interruption drift.
    //
    // LiGetMicroseconds() runs on CLOCK_MONOTONIC_RAW, which clock_nanosleep
    // cannot take, so re-anchor the remaining duration onto CLOCK_MONOTONIC
    // here. The clocks' relative rate drift (NTP slew, <=500ppm) is under a
    // microsecond across the few-milliseconds waits used in pacing.
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    uint64_t remainingNs = (targetUs - nowUs) * 1000;
    deadline.tv_sec += (time_t)(remainingNs / 1000000000ULL);
    deadline.tv_nsec += (long)(remainingNs % 1000000000ULL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR);
#else
    // Generic POSIX (macOS et al): relative nanosleep(), resuming with the
    // kernel-updated remainder on signal interruption.
    uint64_t remainingNs = (targetUs - nowUs) * 1000;
    struct timespec remaining;
    remaining.tv_sec = (time_t)(remainingNs / 1000000000ULL);
    remaining.tv_nsec = (long)(remainingNs % 1000000000ULL);

    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR);
#endif
}

void HighResSleep::sleepUs(uint64_t durationUs)
{
    sleepUntilUs(LiGetMicroseconds() + durationUs);
}
