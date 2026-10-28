#ifndef DUKEMP_GUEST_MEMORY_H
#define DUKEMP_GUEST_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Guest memory uses sparse, bounded mappings. Backing storage belongs to caller. */
/* Original map/renderer allocation exceeds the initial 64-region probe cap. */
#define GUEST_MEMORY_MAX_REGIONS 256u

typedef struct guest_address_space guest_address_space;
/* A process owns the mappings; TLS selects that process, never copies its
 * registry. Backing storage and synchronization remain the owner's duty.
 * Map changes and backing destruction require quiescent guest execution.
 * No guest threads are enabled by these APIs. */
guest_address_space *guest_address_space_create(void);
guest_address_space *guest_address_space_create_direct(void);
/* Allocations are zeroed, registered, and released by exact guest base.
 * Direct mode requires host-page-aligned bases and exclusive rounded pages. */
void *guest_memory_allocate(uint32_t base, uint64_t size);
void guest_memory_release(uint32_t base);
uint64_t guest_memory_region_size(uint32_t base);
int guest_memory_is_direct(void);
extern _Thread_local uint8_t *guest_execution_base;
void guest_address_space_destroy(guest_address_space *space);
guest_address_space *guest_address_space_select(guest_address_space *space);
guest_address_space *guest_address_space_current(void);

/* Start an empty address space, then add non-overlapping uint32 guest VA ranges. */
void guest_memory_reset(void);
void guest_memory_add(uint8_t *backing, uint32_t guest_base, uint64_t size);
/* Exact reservation removal and aligned VA placement; zero means no space. */
int guest_memory_remove(uint32_t guest_base);
uint32_t guest_memory_find_gap(uint32_t start, uint32_t size,
                               uint32_t alignment, uint32_t limit);

/* Legacy isolated probes lazily own a space until unbind. bind/reset replace
 * mappings in the selected space; unbind releases only an implicit space. */
void guest_memory_bind(uint8_t *data, uint32_t guest_base, uint64_t size);
void guest_memory_unbind(void);
void *guest_ptr(uint32_t address, size_t width);
/* Optional observers: bounded read under the serialized guest owner; no abort. */
void *guest_memory_try_ptr(uint32_t address, size_t width);
int guest_memory_read(uint32_t address, void *output, size_t width);
static inline void *guest_execution_ptr(uint32_t address, size_t width) {
#ifdef DUKEMP_DIRECT_MEMORY
    /* Only lifted fixed-width accesses use this path. The extra inaccessible
     * page after the arena catches accesses crossing the uint32 boundary. */
    if (guest_execution_base) return guest_execution_base + address;
#endif
    return guest_ptr(address, width);
}
/* NUL must occur within limit bytes of one mapped region. */
const char *guest_cstring(uint32_t address, size_t limit);
size_t guest_utf16_length(uint32_t address);

uint32_t guest_atomic_xadd32(uint32_t address, uint32_t value);
uint32_t guest_atomic_cmpxchg32(uint32_t address, uint32_t compare,
                                uint32_t value);
uint32_t guest_atomic_xchg32(uint32_t address, uint32_t value);

#endif
