#ifndef AC1_LIFTED_LOOKUP_CACHE_H
#define AC1_LIFTED_LOOKUP_CACHE_H

#include <stddef.h>
#include <stdint.h>

/* Include after initial_dispatch.h, which defines ac1_lifted_fn. */

/* A separate direct-map cache for the immutable main AC1 function table.
 * Guest dispatch is serialized by the runtime scheduler baton, so these
 * derived slots need no atomics. Verify the full VA on every hit; a collision
 * always falls back to the original sorted-table binary search. */
#define AC1_LIFTED_LOOKUP_CACHE_SLOTS 1024u

typedef struct {
    uint32_t guest_va;
    ac1_lifted_fn function;
} ac1_lifted_lookup_cache_slot;

typedef ac1_lifted_fn (*ac1_lifted_lookup_fallback)(uint32_t guest_va);

static inline ac1_lifted_fn ac1_lifted_lookup_cached(
    uint32_t guest_va, int enabled,
    ac1_lifted_lookup_cache_slot cache[AC1_LIFTED_LOOKUP_CACHE_SLOTS],
    ac1_lifted_lookup_fallback fallback)
{
    if (enabled) {
        const unsigned slot =
            (guest_va * UINT32_C(0x9e3779b1)) >> 22;
        const ac1_lifted_lookup_cache_slot *entry = &cache[slot];
        if (entry->function && entry->guest_va == guest_va)
            return entry->function;
    }

    ac1_lifted_fn function = fallback(guest_va);
    if (enabled && function) {
        const unsigned slot =
            (guest_va * UINT32_C(0x9e3779b1)) >> 22;
        cache[slot].guest_va = guest_va;
        cache[slot].function = function;
    }
    return function;
}

#endif
