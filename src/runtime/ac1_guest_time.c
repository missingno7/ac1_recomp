/* Guest-visible time.
 *
 * By default the guest reads the real monotonic clock. AC1_FIXED_FRAME_HZ=N
 * selects a deterministic benchmark clock (a timedemo): guest time starts at a
 * fixed epoch and advances by exactly 1/N s per successful Present, so every
 * run simulates the same time-step sequence and time-derived seeds repeat.
 * Within a frame the clock stands still. A thread that keeps polling it, as a
 * time-based spin wait does, is released after SPIN_READS reads in one frame:
 * each further read then advances the clock by 1 us (reported once).
 * Host waits, the scheduler and frame statistics stay on real time. */
#include "ac1_guest_time.h"
#include "guest_clock.h"
#include <stdio.h>
#include <stdlib.h>

#define EPOCH_NS UINT64_C(3600000000000)
#define SPIN_READS (UINT64_C(1) << 20)

static int fixed_state = -1;           /* -1 unparsed, 0 real time, 1 fixed */
static uint64_t fixed_hz;
static uint64_t presents, reads_this_frame, spin_ns;
static int spin_reported;

int ac1_guest_time_fixed(void)
{
    int state = __atomic_load_n(&fixed_state, __ATOMIC_ACQUIRE);
    if (state >= 0) return state;
    const char *value = getenv("AC1_FIXED_FRAME_HZ");
    char *end = NULL;
    unsigned long long hz = value && *value ? strtoull(value, &end, 10) : 0;
    if (value && *value && (*end || hz == 0 || hz > 1000)) {
        fprintf(stderr, "AC1 fixed frame clock: invalid AC1_FIXED_FRAME_HZ=%s (1..1000)\n", value);
        abort();
    }
    fixed_hz = hz;
    if (hz) fprintf(stderr, "AC1 fixed frame clock: %llu Hz guest time per Present\n", hz);
    state = hz != 0;
    __atomic_store_n(&fixed_state, state, __ATOMIC_RELEASE);
    return state;
}

uint64_t ac1_guest_time_ns(void)
{
    if (!ac1_guest_time_fixed()) return guest_monotonic_ns();
    if (__atomic_add_fetch(&reads_this_frame, 1, __ATOMIC_RELAXED) > SPIN_READS) {
        __atomic_add_fetch(&spin_ns, 1000, __ATOMIC_RELAXED);
        if (!__atomic_exchange_n(&spin_reported, 1, __ATOMIC_RELAXED))
            fprintf(stderr, "AC1 fixed frame clock: over %llu reads in one frame; "
                    "advancing 1 us per read (time spin wait)\n",
                    (unsigned long long)SPIN_READS);
    }
    const uint64_t frames = __atomic_load_n(&presents, __ATOMIC_RELAXED);
    return EPOCH_NS + frames * UINT64_C(1000000000) / fixed_hz +
           __atomic_load_n(&spin_ns, __ATOMIC_RELAXED);
}

void ac1_guest_time_present(void)
{
    if (!ac1_guest_time_fixed()) return;
    __atomic_add_fetch(&presents, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&reads_this_frame, 0, __ATOMIC_RELAXED);
}
