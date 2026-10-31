/* One real monotonic clock for the guest's elapsed-time measurements. */
#include "guest_clock.h"
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
uint64_t guest_monotonic_ns(void) {
    static volatile uint64_t cached_rate;
    LARGE_INTEGER ticks, frequency;
    uint64_t rate = cached_rate;
    if (!rate) {
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) abort();
        cached_rate = rate = (uint64_t)frequency.QuadPart;
    }
    if (!QueryPerformanceCounter(&ticks)) abort();
    uint64_t value = (uint64_t)ticks.QuadPart;
    if (rate == 10000000ull) return value * 100ull;
    return value / rate * 1000000000ull + value % rate * 1000000000ull / rate;
}
#else
#include <time.h>
uint64_t guest_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif
