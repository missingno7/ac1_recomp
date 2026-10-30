/* Guest-visible time: GetTickCount, timeGetTime, QueryPerformanceCounter and
 * GetSystemTimeAsFileTime. Host scheduling, waits and statistics keep the real
 * monotonic clock (guest_monotonic_ns). */
#ifndef AC1_GUEST_TIME_H
#define AC1_GUEST_TIME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Nonzero when AC1_FIXED_FRAME_HZ selects the deterministic frame clock. */
int ac1_guest_time_fixed(void);
/* Monotonic guest time in nanoseconds (real time unless fixed). */
uint64_t ac1_guest_time_ns(void);
/* Advance a fixed clock by one frame period; called on every successful Present. */
void ac1_guest_time_present(void);
#ifdef __cplusplus
}
#endif
#endif
