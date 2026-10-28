/* Sparse-memory/lifetime adapter for pcrecomp's based heap. The generated
 * allocator bodies are unchanged; only their global state is per heap.
 * See docs/third-party.md for the pinned MIT source and extraction recipe. */
#include "pcrecomp_heap.h"
#include "guest_memory.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct heap_chunk {
    struct heap_chunk *next;
    uint8_t *data;
    uint32_t base, size;
} heap_chunk;
typedef struct heap_block {
    struct heap_block *next;
    uint32_t address, size, kind;
} heap_block;
/* Host-side ownership index only. Donor headers, free lists and allocation
 * order stay unchanged. Bulk model destruction used to scan every live block
 * twice per free, producing quadratic shutdown time. */
#define BLOCK_BUCKET_BITS 12u
#define BLOCK_BUCKETS (1u<<BLOCK_BUCKET_BITS)
struct pcrecomp_heap {
    guest_address_space *space;
    uint32_t free_list[64], carve, carve_end;
    heap_chunk *chunks;
    heap_block *blocks[BLOCK_BUCKETS];
    atomic_flag lock;
};
static _Thread_local pcrecomp_heap *active_heap;
static void require_owner(pcrecomp_heap *heap) {
    if (!heap || heap->space != guest_address_space_current()) {
        fprintf(stderr, "heap used outside its owning guest process address space\n");
        abort();
    }
}
static void heap_lock(atomic_flag *lock) {
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {}
}
static void heap_unlock(atomic_flag *lock) {
    atomic_flag_clear_explicit(lock, memory_order_release);
}
static void hle_mem_init(void) {}
static uint8_t *host(uint32_t va) { return guest_ptr(va, 1); }
static uint32_t hle_va(const void *pointer) {
    uintptr_t p = (uintptr_t)pointer;
    for (heap_chunk *c = active_heap->chunks; c; c = c->next) {
        uintptr_t start = (uintptr_t)c->data;
        if (p >= start && p - start < c->size) return c->base + (uint32_t)(p - start);
    }
    abort(); /* An allocator-internal pointer escaped its backing reservation. */
}
static uint32_t vm_reserve_locked(uint32_t va, uint32_t size) {
    uint64_t rounded = ((uint64_t)size + 0xffffu) & ~UINT64_C(0xffff);
    if (va || !rounded || rounded > 0x40000000u) return 0;
    uint32_t base = guest_memory_find_gap(0x30000000u, (uint32_t)rounded,
                                         0x10000u, 0x70000000u);
    if (!base) return 0;
    heap_chunk *c = calloc(1, sizeof(*c));
    if (!c) return 0;
    c->data = guest_memory_allocate(base, rounded);
    if (!c->data) { free(c); return 0; }
    c->base = base; c->size = (uint32_t)rounded;
    c->next = active_heap->chunks; active_heap->chunks = c;
    return base;
}
static void vm_release_locked(uint32_t va) {
    heap_chunk **link = &active_heap->chunks;
    while (*link && (*link)->base != va) link = &(*link)->next;
    if (!*link) abort();
    heap_chunk *c = *link;
    guest_memory_release(c->base);
    *link = c->next; free(c);
}
static int hle_heap_ok(const void *u);
static uint32_t hle_heap_size(const void *u);
static void *hle_heap_alloc(uint32_t n, int zero);
static void hle_heap_free(void *u);
static void *hle_heap_realloc(void *u, uint32_t n, int zero);
#define g_free (active_heap->free_list)
#define g_carve (active_heap->carve)
#define g_carve_end (active_heap->carve_end)
#define g_mem_lock (active_heap->lock)
#define pthread_mutex_lock heap_lock
#define pthread_mutex_unlock heap_unlock
#include "pcrecomp_heap.inc"
#undef pthread_mutex_lock
#undef pthread_mutex_unlock

/* Live ownership is checked before calling donor helpers, whose magic check
 * alone cannot safely validate an arbitrary/unmapped guest pointer. Expected
 * header metadata also prevents guest corruption from indexing a free list. */
static unsigned block_bucket(uint32_t address) {
    return ((address>>4)*UINT32_C(2654435761))>>(32u-BLOCK_BUCKET_BITS);
}
/* Return the owning link as well as validating the header. Removal then uses
 * this same lookup; an unregistered pointer never reaches guest memory. */
static heap_block **find_block_link(pcrecomp_heap *heap, uint32_t address) {
    require_owner(heap);
    for (heap_block **link=&heap->blocks[block_bucket(address)];*link;link=&(*link)->next) {
        heap_block *b=*link;
        if (b->address != address) continue;
        const uint32_t *h = (const uint32_t *)guest_ptr(address - HDR, HDR);
        if (h[0] != b->size || h[1] != MAGIC || h[2] != b->kind || h[3]) return NULL;
        return link;
    }
    return NULL;
}
pcrecomp_heap *pcrecomp_heap_create(uint32_t initial_size) {
    pcrecomp_heap *heap = calloc(1, sizeof(*heap));
    if (!heap) return NULL;
    heap->space = guest_address_space_current();
    atomic_flag_clear(&heap->lock);
    pcrecomp_heap *saved = active_heap; active_heap = heap;
    uint32_t base = vm_reserve_locked(0, initial_size ? initial_size : 0x10000u);
    if (base) {
        heap->carve = base;
        heap->carve_end = base + heap->chunks->size;
    }
    active_heap = saved;
    if (!base) { free(heap); return NULL; }
    return heap;
}
void pcrecomp_heap_destroy(pcrecomp_heap *heap) {
    if (!heap) return;
    require_owner(heap);
    pcrecomp_heap *saved = active_heap; active_heap = heap;
    while (heap->chunks) vm_release_locked(heap->chunks->base);
    for(unsigned i=0;i<BLOCK_BUCKETS;++i)while(heap->blocks[i]) {
        heap_block *b=heap->blocks[i];heap->blocks[i]=b->next;free(b);
    }
    active_heap = saved; free(heap);
}
uint32_t pcrecomp_heap_alloc(pcrecomp_heap *heap, uint32_t size, int zero) {
    require_owner(heap);
    heap_block *b = calloc(1, sizeof(*b));
    if (!b) return 0;
    pcrecomp_heap *saved = active_heap; active_heap = heap;
    void *p = hle_heap_alloc(size, zero);
    uint32_t address = p ? hle_va(p) : 0;
    if (p) {
        b->address = address; b->size = size;
        b->kind = ((uint32_t *)p)[-2];
        unsigned bucket=block_bucket(address);
        b->next=heap->blocks[bucket];heap->blocks[bucket]=b;
    } else free(b);
    active_heap = saved; return address;
}
uint32_t pcrecomp_heap_size(pcrecomp_heap *heap, uint32_t address) {
    heap_block **link=find_block_link(heap,address);
    return link ? hle_heap_size(guest_ptr(address, 1)) : UINT32_MAX;
}
int pcrecomp_heap_free(pcrecomp_heap *heap, uint32_t address) {
    heap_block **link=find_block_link(heap,address);
    if (!link) return 0;
    heap_block *b=*link;
    pcrecomp_heap *saved = active_heap; active_heap = heap;
    hle_heap_free(guest_ptr(address, 1));
    *link = b->next; free(b);
    active_heap = saved; return 1;
}
uint32_t pcrecomp_heap_realloc(pcrecomp_heap *heap, uint32_t address,
                              uint32_t size, int zero, int in_place_only) {
    heap_block **link=find_block_link(heap,address);
    if (!link) return 0;
    heap_block *b=*link;
    pcrecomp_heap *saved = active_heap; active_heap = heap;
    void *p = guest_ptr(address, 1);
    /* Use the donor's current capacity (large-block shrink may lower it). */
    if (in_place_only && size > capacity((uint32_t *)p - 4)) {
        active_heap = saved; return 0;
    }
    p = hle_heap_realloc(p, size, zero);
    if (p) {
        *link=b->next;
        b->address = hle_va(p); b->size = size;
        b->kind = ((uint32_t *)p)[-2];
        unsigned bucket=block_bucket(b->address);
        b->next=heap->blocks[bucket];heap->blocks[bucket]=b;
    }
    active_heap = saved; return p ? b->address : 0;
}
