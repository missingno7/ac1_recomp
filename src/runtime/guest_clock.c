/* One real monotonic clock for the guest's elapsed-time measurements. */
#include "guest_clock.h"
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
uint64_t guest_monotonic_ns(void) {
    LARGE_INTEGER ticks, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&ticks) || frequency.QuadPart <= 0) abort();
    uint64_t value = (uint64_t)ticks.QuadPart, rate = (uint64_t)frequency.QuadPart;
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
