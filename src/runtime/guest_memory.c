#include "guest_memory.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#define ARENA_BYTES UINT64_C(0x100000000)

typedef struct {
    uint8_t *data;
    uint32_t base;
    uint64_t size;
} guest_memory_region_t;

struct guest_address_space {
    guest_memory_region_t regions[GUEST_MEMORY_MAX_REGIONS];
    size_t count;
    /* Derived lookup cache, owned by the shared process just like its maps.
     * Guest execution/mapping mutation is serialized by guest_scheduler.
     * Every hit still checks the complete requested span. */
    guest_memory_region_t lookup[256];
    uint8_t *arena;
    size_t page_size;
};

_Thread_local uint8_t *guest_execution_base;
static _Thread_local guest_address_space *selected_memory;
static _Thread_local guest_address_space *implicit_memory;
#define current_memory (*selected_memory)

guest_address_space *guest_address_space_create(void)
{
    return calloc(1, sizeof(guest_address_space));
}

guest_address_space *guest_address_space_create_direct(void)
{
    if (sizeof(void *) < 8) return NULL;
    guest_address_space *s = guest_address_space_create();
    if (!s) return NULL;
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info); s->page_size = info.dwPageSize;
    s->arena = VirtualAlloc(NULL, (SIZE_T)(ARENA_BYTES+s->page_size), MEM_RESERVE, PAGE_NOACCESS);
#else
    s->page_size = (size_t)sysconf(_SC_PAGESIZE);
    void *p = mmap(NULL, (size_t)(ARENA_BYTES+s->page_size), PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    s->arena = p == MAP_FAILED ? NULL : p;
#endif
    if (!s->arena) { free(s); return NULL; }
    return s;
}

void guest_address_space_destroy(guest_address_space *space)
{
    if (selected_memory == space) { selected_memory = NULL; guest_execution_base = NULL; }
    if (implicit_memory == space) implicit_memory = NULL;
    if (space && space->arena) {
#ifdef _WIN32
        VirtualFree(space->arena, 0, MEM_RELEASE);
#else
        munmap(space->arena, (size_t)(ARENA_BYTES+space->page_size));
#endif
    }
    free(space);
}

guest_address_space *guest_address_space_select(guest_address_space *space)
{
    guest_address_space *previous = selected_memory;
    selected_memory = space;
    guest_execution_base = space ? space->arena : NULL;
    return previous;
}

guest_address_space *guest_address_space_current(void) { return selected_memory; }

static void fail(const char *reason, uint32_t address, size_t width)
{
    fprintf(stderr, "guest memory %s: address=0x%08x width=%zu\n",
            reason, address, width);
    abort();
}

int guest_memory_is_direct(void) { return selected_memory && selected_memory->arena; }

void *guest_memory_allocate(uint32_t base, uint64_t size)
{
    if (!selected_memory) guest_memory_reset();
    if (!size || (uint64_t)base+size > ARENA_BYTES) return NULL;
    uint8_t *data;
    if (current_memory.arena) {
        size_t page = current_memory.page_size;
        uint64_t rounded = (size+page-1) & ~(uint64_t)(page-1);
        if (!base || base % page || (uint64_t)base+rounded > ARENA_BYTES) return NULL;
        for (size_t i=0; i<current_memory.count; ++i) {
            guest_memory_region_t *r = &current_memory.regions[i];
            uint64_t end = ((uint64_t)r->base+r->size+page-1) & ~(uint64_t)(page-1);
            if (base < end && r->base < (uint64_t)base+rounded) return NULL;
        }
        data = current_memory.arena+base;
#ifdef _WIN32
        if (!VirtualAlloc(data,(SIZE_T)rounded,MEM_COMMIT,PAGE_READWRITE)) return NULL;
#else
        if (mprotect(data,(size_t)rounded,PROT_READ|PROT_WRITE)) return NULL;
#endif
    } else {
        data = calloc(1,(size_t)size);
        if (!data) return NULL;
    }
    guest_memory_add(data,base,size);
    return data;
}

void guest_memory_release(uint32_t base)
{
    if (!selected_memory) return;
    for (size_t i=0; i<current_memory.count; ++i) {
        guest_memory_region_t r = current_memory.regions[i];
        if (r.base != base) continue;
        guest_memory_remove(base);
        if (!current_memory.arena) free(r.data);
        return;
    }
}

uint64_t guest_memory_region_size(uint32_t base)
{
    if (!selected_memory) return 0;
    for (size_t i = 0; i < current_memory.count; ++i)
        if (current_memory.regions[i].base == base)
            return current_memory.regions[i].size;
    return 0;
}

void guest_memory_reset(void)
{
    if (!selected_memory) {
        if (!implicit_memory) implicit_memory = guest_address_space_create();
        if (!implicit_memory) fail("allocation failed", 0, 0);
        selected_memory = implicit_memory;
    }
    if (current_memory.arena) {
        while (current_memory.count) guest_memory_release(current_memory.regions[0].base);
    } else memset(&current_memory, 0, sizeof(current_memory));
}

void guest_memory_add(uint8_t *backing, uint32_t guest_base, uint64_t size)
{
    uint64_t end, existing_end;
    size_t i;
    if (!selected_memory) guest_memory_reset();
    if (!backing || !size)
        fail("invalid binding", guest_base, (size_t)size);
    if (current_memory.arena && backing != current_memory.arena+guest_base)
        fail("non-arena backing",guest_base,(size_t)size);
    if (size > UINT64_C(0x100000000))
        fail("binding too large", guest_base, (size_t)size);
    end = (uint64_t)guest_base + size;
    if (end > UINT64_C(0x100000000))
        fail("binding wraps", guest_base, (size_t)size);
    if (current_memory.count == GUEST_MEMORY_MAX_REGIONS)
        fail("too many regions", guest_base, (size_t)size);
    for (i = 0; i < current_memory.count; ++i) {
        const guest_memory_region_t *region = &current_memory.regions[i];
        existing_end = (uint64_t)region->base + region->size;
        if ((uint64_t)guest_base < existing_end && (uint64_t)region->base < end)
            fail("overlapping regions", guest_base, (size_t)size);
    }
    current_memory.regions[current_memory.count].data = backing;
    current_memory.regions[current_memory.count].base = guest_base;
    current_memory.regions[current_memory.count].size = size;
    ++current_memory.count;
}

void guest_memory_bind(uint8_t *data, uint32_t guest_base, uint64_t size)
{
    guest_memory_reset();
    guest_memory_add(data, guest_base, size);
}

int guest_memory_remove(uint32_t guest_base)
{
    if (!selected_memory) return 0;
    for (size_t i = 0; i < current_memory.count; ++i) {
        if (current_memory.regions[i].base != guest_base) continue;
        if (current_memory.arena) {
            guest_memory_region_t *r = &current_memory.regions[i];
            size_t page = current_memory.page_size;
            size_t size = (size_t)((r->size+page-1) & ~(uint64_t)(page-1));
#ifdef _WIN32
            if (!VirtualFree(r->data,size,MEM_DECOMMIT)) fail("decommit failed",guest_base,size);
#else
            if (mprotect(r->data,size,PROT_NONE) || madvise(r->data,size,MADV_DONTNEED))
                fail("decommit failed",guest_base,size);
#endif
        }
        memmove(&current_memory.regions[i], &current_memory.regions[i + 1],
                (--current_memory.count - i) * sizeof(current_memory.regions[0]));
        memset(current_memory.lookup, 0, sizeof(current_memory.lookup));
        return 1;
    }
    return 0;
}

uint32_t guest_memory_find_gap(uint32_t start, uint32_t size,
                               uint32_t alignment, uint32_t limit)
{
    if (!selected_memory || !size || !alignment || (alignment & (alignment - 1)) ||
        current_memory.count == GUEST_MEMORY_MAX_REGIONS) return 0;
    uint64_t at = ((uint64_t)start + alignment - 1) & ~(uint64_t)(alignment - 1);
    for (;;) {
        if (!at || at + size > limit) return 0;
        size_t i;
        for (i = 0; i < current_memory.count; ++i) {
            const guest_memory_region_t *r = &current_memory.regions[i];
            uint64_t end = (uint64_t)r->base + r->size;
            if (at < end && (uint64_t)r->base < at + size) {
                at = (end + alignment - 1) & ~(uint64_t)(alignment - 1);
                break;
            }
        }
        if (i == current_memory.count) return (uint32_t)at;
    }
}

void guest_memory_unbind(void)
{
    if (selected_memory && selected_memory == implicit_memory)
        guest_address_space_destroy(implicit_memory);
    selected_memory = NULL;
    guest_execution_base = NULL;
}

void *guest_memory_try_ptr(uint32_t address, size_t width)
{
    if (!selected_memory || !width || width>UINT32_MAX ||
        (uint64_t)address+width>UINT64_C(0x100000000)) return NULL;
    /* Native replacements validate several spans per call. Share the existing
     * process-owned cache with guest_ptr rather than scanning every mapping.
     * Full-span checks and remove/reset invalidation remain identical. */
    uint64_t end=(uint64_t)address+width;
    unsigned slot=((address>>12)^(address>>20))&255u;
    guest_memory_region_t *cached=&current_memory.lookup[slot];
    if(address>=cached->base && end<=(uint64_t)cached->base+cached->size)
        return cached->data+(address-cached->base);
    for (size_t i=0;i<current_memory.count;++i) {
        const guest_memory_region_t *r=&current_memory.regions[i];
        if (address>=r->base && end<=(uint64_t)r->base+r->size) {
            *cached=*r;
            return r->data+(address-r->base);
        }
    }
    return NULL;
}
int guest_memory_read(uint32_t address, void *output, size_t width)
{
    void *source=guest_memory_try_ptr(address,width);
    if (!source || !output) return 0;
    memcpy(output,source,width);return 1;
}

/* CPU scalar accesses can cross adjacent guest mappings whose host backing
 * allocations are unrelated. Validate the full guest span first, then copy
 * it in per-region chunks. Raw guest_ptr intentionally remains one-region. */
static guest_memory_region_t *scalar_region_at(uint64_t address)
{
    if (!selected_memory || address >= UINT64_C(0x100000000)) return NULL;
    for (size_t i = 0; i < current_memory.count; ++i) {
        guest_memory_region_t *region = &current_memory.regions[i];
        uint64_t end = (uint64_t)region->base + region->size;
        if (address >= region->base && address < end) return region;
    }
    return NULL;
}

static int scalar_span_mapped(uint64_t begin, uint64_t end)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        if (!region) return 0;
        uint64_t region_end = (uint64_t)region->base + region->size;
        if (region_end <= at) return 0;
        at = region_end < end ? region_end : end;
    }
    return 1;
}

static void scalar_copy_from(uint64_t begin, uint64_t end, uint8_t *output)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        uint64_t region_end = (uint64_t)region->base + region->size;
        size_t chunk = (size_t)((region_end < end ? region_end : end) - at);
        memcpy(output, region->data + (size_t)(at - region->base), chunk);
        output += chunk;
        at += chunk;
    }
}

static void scalar_copy_to(uint64_t begin, uint64_t end, const uint8_t *input)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        uint64_t region_end = (uint64_t)region->base + region->size;
        size_t chunk = (size_t)((region_end < end ? region_end : end) - at);
        memcpy(region->data + (size_t)(at - region->base), input, chunk);
        input += chunk;
        at += chunk;
    }
}

int guest_memory_read_scalar(uint32_t address, void *output, size_t width)
{
    if (!output || !width || width > 16u) return 0;
    uint64_t end = (uint64_t)address + width;
    if (end > UINT64_C(0x100000000)) return 0;
    void *single = guest_memory_try_ptr(address, width);
    if (single) {
        memcpy(output, single, width);
        return 1;
    }
    if (!scalar_span_mapped(address, end)) return 0;
    scalar_copy_from(address, end, (uint8_t *)output);
    return 1;
}

int guest_memory_write_scalar(uint32_t address, const void *input, size_t width)
{
    if (!input || !width || width > 16u) return 0;
    uint64_t end = (uint64_t)address + width;
    if (end > UINT64_C(0x100000000)) return 0;
    void *single = guest_memory_try_ptr(address, width);
    if (single) {
        memcpy(single, input, width);
        return 1;
    }
    if (!scalar_span_mapped(address, end)) return 0;
    scalar_copy_to(address, end, (const uint8_t *)input);
    return 1;
}

void guest_execution_read(uint32_t address, void *output, size_t width)
{
#ifdef DUKEMP_DIRECT_MEMORY
    if (guest_execution_base) {
        memcpy(output, guest_execution_base + address, width);
        return;
    }
#endif
    if (!guest_memory_read_scalar(address, output, width))
        fail("unmapped scalar read", address, width);
}

void guest_execution_write(uint32_t address, const void *input, size_t width)
{
#ifdef DUKEMP_DIRECT_MEMORY
    if (guest_execution_base) {
        memcpy(guest_execution_base + address, input, width);
        return;
    }
#endif
    if (!guest_memory_write_scalar(address, input, width))
        fail("unmapped scalar write", address, width);
}

void *guest_ptr(uint32_t address, size_t width)
{
    uint64_t end, region_end, offset;
    void *one_past = NULL;
    size_t i;
    if (!selected_memory || !current_memory.count)
        fail("unbound", address, width);
    if (width > UINT32_MAX)
        fail("width overflow", address, width);
    end = (uint64_t)address + (uint64_t)width;
    if (end > UINT64_C(0x100000000))
        fail("address wraps", address, width);
    unsigned slot = ((address >> 12) ^ (address >> 20)) & 255u;
    guest_memory_region_t *cached = &current_memory.lookup[slot];
    /* Zero-width boundary resolution must retain the original preference
     * for an adjacent mapping over a preceding mapping's one-past pointer. */
    if (width && address >= cached->base && end <= (uint64_t)cached->base + cached->size)
        return cached->data + (size_t)(address - cached->base);
    for (i = 0; i < current_memory.count; ++i) {
        const guest_memory_region_t *region = &current_memory.regions[i];
        region_end = (uint64_t)region->base + region->size;
        if ((uint64_t)address < region->base || (uint64_t)address > region_end)
            continue;
        offset = (uint64_t)address - region->base;
        if (end <= region_end) {
            if (width == 0 && (uint64_t)address == region_end) {
                one_past = region->data + (size_t)region->size;
                continue;
            }
            if (width) *cached = *region;
            return region->data + (size_t)offset;
        }
    }
    if (width == 0 && one_past)
        return one_past;
    fail("outside mapped range", address, width);
    return NULL;
}

uint32_t guest_atomic_xadd32(uint32_t address, uint32_t value)
{
    _Atomic uint32_t *cell;
    void *ptr = guest_ptr(address, 4);
    if (((uintptr_t)ptr % _Alignof(_Atomic uint32_t)) != 0)
        fail("unaligned atomic", address, 4);
    cell = (_Atomic uint32_t *)ptr;
    return atomic_fetch_add_explicit(cell, value, memory_order_seq_cst);
}

const char *guest_cstring(uint32_t address, size_t limit)
{
    const char *start = guest_ptr(address, 1);
    for (size_t i = 0; i < current_memory.count; ++i) {
        const guest_memory_region_t *r = &current_memory.regions[i];
        if (address < r->base || (uint64_t)address >= (uint64_t)r->base + r->size) continue;
        uint64_t available = (uint64_t)r->base + r->size - address;
        size_t scan = available < limit ? (size_t)available : limit;
        if (memchr(start, 0, scan)) return start;
        fail("unterminated string", address, scan);
    }
    fail("outside mapped range", address, 1);
    return NULL;
}

size_t guest_utf16_length(uint32_t address)
{
    const uint8_t *start = guest_ptr(address, 2);
    for (size_t i = 0; i < current_memory.count; ++i) {
        const guest_memory_region_t *r = &current_memory.regions[i];
        if (address < r->base || (uint64_t)address >= (uint64_t)r->base + r->size) continue;
        size_t available = (size_t)((uint64_t)r->base + r->size - address);
        for (size_t k = 0; k < available / 2; ++k) {
            uint16_t word; memcpy(&word, start + k*2, 2);
            if (!word) return k;
        }
        fail("unterminated UTF-16 string", address, available);
    }
    fail("outside mapped range", address, 2);
    return 0;
}

uint32_t guest_atomic_cmpxchg32(uint32_t address, uint32_t compare,
                                uint32_t value)
{
    _Atomic uint32_t *cell;
    void *ptr = guest_ptr(address, 4);
    if (((uintptr_t)ptr % _Alignof(_Atomic uint32_t)) != 0)
        fail("unaligned atomic", address, 4);
    cell = (_Atomic uint32_t *)ptr;
    atomic_compare_exchange_strong_explicit(cell, &compare, value,
                                            memory_order_seq_cst,
                                            memory_order_seq_cst);
    return compare;
}

uint32_t guest_atomic_xchg32(uint32_t address, uint32_t value)
{
    _Atomic uint32_t *cell;
    void *ptr = guest_ptr(address, 4);
    if (((uintptr_t)ptr % _Alignof(_Atomic uint32_t)) != 0)
        fail("unaligned atomic", address, 4);
    cell = (_Atomic uint32_t *)ptr;
    return atomic_exchange_explicit(cell, value, memory_order_seq_cst);
}
