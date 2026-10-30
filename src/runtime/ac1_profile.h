#ifndef AC1_PROFILE_H
#define AC1_PROFILE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* No instrumentation is inserted into generated functions or CPU helpers.
 * Hot hooks test one process-constant flag before touching TLS. */
extern int ac1_profile_enabled, ac1_profile_frames_enabled;
typedef enum {
    AC1_HANDOFF_RETURN, AC1_HANDOFF_IMPORT, AC1_HANDOFF_SLEEP,
    AC1_HANDOFF_YIELD, AC1_HANDOFF_WAIT, AC1_HANDOFF_CS,
    AC1_HANDOFF_SYNC, AC1_HANDOFF_IO, AC1_HANDOFF_START,
    AC1_HANDOFF_EXIT, AC1_HANDOFF_CAUSES
} ac1_handoff_cause;
typedef struct {
    uint64_t dispatches, misses, slow, scanned, handoffs, wait_ticks, wait_start;
    uint64_t handoff_causes[AC1_HANDOFF_CAUSES];
    uint64_t transfer_count, transfer_ns, transfer_max_ns;
    uint64_t io_wait_completions;
} ac1_profile_counts;
#ifdef _WIN32
#ifdef __cplusplus
extern thread_local ac1_profile_counts *ac1_profile_current;
#else
extern _Thread_local ac1_profile_counts *ac1_profile_current;
#endif
#define ac1_profile_local (*ac1_profile_current)
#define AC1_PROFILE_COUNT(field) do { if (ac1_profile_enabled && ac1_profile_current) ++ac1_profile_local.field; } while (0)
#define AC1_PROFILE_SCAN(n) do { if (ac1_profile_enabled && ac1_profile_current) ac1_profile_local.scanned += (n); } while (0)
#else
#define AC1_PROFILE_COUNT(field) ((void)0)
#define AC1_PROFILE_SCAN(n) ((void)0)
#endif
void ac1_profile_init(void);
void ac1_profile_shutdown(void);
void ac1_profile_guest(unsigned slot, uint32_t guest_id, const uint32_t *eip,
                       const unsigned *owner);
void ac1_profile_guest_done(void);
uint64_t ac1_profile_wait_begin(void);
void ac1_profile_wait_end(uint64_t start);
void ac1_profile_handoff(ac1_handoff_cause cause);
void ac1_profile_transfer(uint64_t nanoseconds);
void ac1_profile_present(void);
/* Only explicitly count-instrumented generated functions call this. Normal
 * generated code has no call or enable test. Never use that build for timing. */
void ac1_profile_entry(uint32_t target);
#ifdef __cplusplus
}
#endif
#endif
