#ifndef AC1_VIRTUAL_QUERY_H
#define AC1_VIRTUAL_QUERY_H

#include <stdint.h>

typedef struct {
    uint32_t base_address;
    uint32_t allocation_base;
    uint32_t region_size;
} ac1_virtual_query_region;

/* Resolve one committed guest VirtualAlloc reservation. Each reservation is
 * kept separate even when an adjacent allocation has identical attributes. */
static inline int ac1_virtual_query_region_for_allocation(
    uint32_t address, uint32_t allocation_base, uint64_t allocation_size,
    ac1_virtual_query_region *result)
{
    if (!result || !allocation_base || !allocation_size) return 0;

    const uint64_t allocation_end = (uint64_t)allocation_base + allocation_size;
    const uint32_t page_base = address & ~UINT32_C(0xFFF);
    if (allocation_end > UINT64_C(0x100000000) || address < allocation_base ||
        (uint64_t)address >= allocation_end || page_base < allocation_base ||
        (uint64_t)page_base >= allocation_end)
        return 0;

    const uint64_t region_size = allocation_end - page_base;
    if (!region_size || region_size > UINT32_MAX) return 0;

    result->base_address = page_base;
    result->allocation_base = allocation_base;
    result->region_size = (uint32_t)region_size;
    return 1;
}

#endif
