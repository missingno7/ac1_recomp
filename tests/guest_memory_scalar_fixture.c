#include "cpu.h"
#include "../src/runtime/ac1_virtual_query.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static int mapping_capacity_contract(void)
{
    enum { reservations = 8192 };
#ifdef _WIN32
    void *native[reservations];
    for (unsigned i = 0; i < reservations; ++i) {
        native[i] = VirtualAlloc(NULL, 4096u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(native[i] != NULL);
    }
    for (unsigned i = 0; i < reservations; ++i)
        CHECK(VirtualFree(native[i], 0, MEM_RELEASE));
#endif
    guest_address_space *space = guest_address_space_create_direct();
    CHECK(space);
    guest_address_space_select(space);
    uint32_t addresses[reservations];
    for (unsigned i = 0; i < reservations; ++i) {
        addresses[i] = guest_memory_find_gap(0x10000000u, 4096u, 65536u, 0x70000000u);
        if (!addresses[i]) fprintf(stderr, "reservation %u/%u failed\n", i + 1u, reservations);
        CHECK(addresses[i] != 0u);
        uint32_t *p = guest_memory_allocate(addresses[i], 4096u);
        CHECK(p && *p == 0u);
        *p = i + 1u;
    }
    for (unsigned i = 0; i < reservations; ++i) {
        CHECK(guest_memory_region_size(addresses[i]) == 4096u);
        CHECK(*(uint32_t *)guest_memory_try_ptr(addresses[i], 4u) == i + 1u);
    }
    for (unsigned i = 0; i < reservations; i += 2u)
        guest_memory_release(addresses[i]);
    CHECK(guest_memory_find_gap(0x10000000u, 4096u, 65536u, 0x70000000u) == addresses[0]);
    uint32_t *reused = guest_memory_allocate(addresses[0], 4096u);
    CHECK(reused && *reused == 0u);
    guest_memory_release(addresses[0]);
    for (unsigned i = 1; i < reservations; i += 2u)
        guest_memory_release(addresses[i]);
    guest_address_space_destroy(space);
    return 0;
}

int main(void)
{
    /* VirtualQuery is bounded to tracked allocations. An interior address
     * reports its page base and the remainder of that allocation; adjacent
     * allocations remain separate and gaps are rejected. */
    ac1_virtual_query_region query_region = {0};
    CHECK(ac1_virtual_query_region_for_allocation(
        0x11AD0000u, 0x11AD0000u, 0x80000u, &query_region));
    CHECK(query_region.base_address == 0x11AD0000u &&
          query_region.allocation_base == 0x11AD0000u &&
          query_region.region_size == 0x80000u);
    CHECK(ac1_virtual_query_region_for_allocation(
        0x11AE0123u, 0x11AD0000u, 0x80000u, &query_region));
    CHECK(query_region.base_address == 0x11AE0000u &&
          query_region.allocation_base == 0x11AD0000u &&
          query_region.region_size == 0x70000u);
    CHECK(ac1_virtual_query_region_for_allocation(
        0x11B4FFFFu, 0x11AD0000u, 0x80000u, &query_region));
    CHECK(query_region.base_address == 0x11B4F000u &&
          query_region.allocation_base == 0x11AD0000u &&
          query_region.region_size == 0x1000u);
    CHECK(ac1_virtual_query_region_for_allocation(
        0x11B50000u, 0x11B50000u, 0x10000u, &query_region));
    CHECK(query_region.base_address == 0x11B50000u &&
          query_region.allocation_base == 0x11B50000u &&
          query_region.region_size == 0x10000u);
    CHECK(!ac1_virtual_query_region_for_allocation(
        0x11C00000u, 0x11AD0000u, 0x80000u, &query_region));
    CHECK(!ac1_virtual_query_region_for_allocation(
        0x11B50000u, 0x11AD0000u, 0x80000u, &query_region));
#ifdef _WIN32
    void *native_allocation = VirtualAlloc(NULL, 0x80000u,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(native_allocation != NULL);
    MEMORY_BASIC_INFORMATION native_query;
    uint8_t *native_address = (uint8_t *)native_allocation + 0x10123u;
    CHECK(VirtualQuery(native_address, &native_query,
                       sizeof(native_query)) == sizeof(native_query));
    CHECK(ac1_virtual_query_region_for_allocation(
        (uint32_t)(uintptr_t)native_address,
        (uint32_t)(uintptr_t)native_allocation, 0x80000u, &query_region));
    CHECK(query_region.base_address ==
              (uint32_t)(uintptr_t)native_query.BaseAddress &&
          query_region.allocation_base ==
              (uint32_t)(uintptr_t)native_query.AllocationBase &&
          query_region.region_size == native_query.RegionSize &&
          native_query.State == MEM_COMMIT &&
          native_query.Protect == PAGE_READWRITE &&
          native_query.Type == MEM_PRIVATE);
    CHECK(VirtualFree(native_allocation, 0, MEM_RELEASE));
#endif

    guest_address_space *space = guest_address_space_create();
    CHECK(space);
    guest_address_space_select(space);

    /* Match the captured runtime boundary: 3 bytes in the first backing,
     * then 1 byte in an adjacent guest mapping with a separate host buffer. */
    uint8_t left[0x20000] = {0};
    uint8_t right[0x20000] = {0};
    guest_memory_add(left, 0x121F0000u, sizeof(left));
    guest_memory_add(right, 0x12210000u, sizeof(right));
    left[0x1FFFD] = 0x11;
    left[0x1FFFE] = 0x22;
    left[0x1FFFF] = 0x33;
    right[0] = 0x44;
    CHECK(rd32(0x1220FFFDu) == 0x44332211u);
    wr32(0x1220FFFDu, 0xAABBCCDDu);
    CHECK(left[0x1FFFD] == 0xDD && left[0x1FFFE] == 0xCC &&
          left[0x1FFFF] == 0xBB && right[0] == 0xAA);
    CHECK(guest_memory_try_ptr(0x1220FFFDu, 4u) == NULL);
    CHECK(guest_memory_try_ptr(0x1220FFFDu, 3u) != NULL);

    /* A 16-byte SSE operand can cross three independent guest mappings. */
    uint8_t segment_a[3] = {0, 1, 2};
    uint8_t segment_b[5] = {3, 4, 5, 6, 7};
    uint8_t segment_c[8] = {8, 9, 10, 11, 12, 13, 14, 15};
    guest_memory_reset();
    guest_memory_add(segment_a, 0x20000000u, sizeof(segment_a));
    guest_memory_add(segment_b, 0x20000003u, sizeof(segment_b));
    guest_memory_add(segment_c, 0x20000008u, sizeof(segment_c));
    XMM xmm = rdxm(0x20000000u);
    for (unsigned i = 0; i < sizeof(xmm.u8); ++i) CHECK(xmm.u8[i] == i);
    for (unsigned i = 0; i < sizeof(xmm.u8); ++i) xmm.u8[i] = (uint8_t)(0xF0u + i);
    wrxm(0x20000000u, xmm);
    CHECK(!memcmp(segment_a, xmm.u8, sizeof(segment_a)));
    CHECK(!memcmp(segment_b, xmm.u8 + sizeof(segment_a), sizeof(segment_b)));
    CHECK(!memcmp(segment_c, xmm.u8 + sizeof(segment_a) + sizeof(segment_b), sizeof(segment_c)));

    /* Raw checked copies cover a 10-byte ext80-sized operand over all maps. */
    uint8_t ext80[10] = {0};
    CHECK(guest_memory_read_scalar(0x20000002u, ext80, sizeof(ext80)));
    CHECK(!memcmp(ext80, xmm.u8 + 2, sizeof(ext80)));

    uint64_t eight = rd64(0x20000001u);
    uint8_t eight_bytes[8];
    memcpy(eight_bytes, &eight, sizeof(eight_bytes));
    CHECK(!memcmp(eight_bytes, xmm.u8 + 1, sizeof(eight_bytes)));
    const uint64_t eight_replacement = UINT64_C(0x0123456789ABCDEF);
    wr64(0x20000001u, eight_replacement);
    memcpy(eight_bytes, &eight_replacement, sizeof(eight_bytes));
    CHECK(segment_a[1] == eight_bytes[0] && segment_a[2] == eight_bytes[1]);
    CHECK(!memcmp(segment_b, eight_bytes + 2, sizeof(segment_b)));
    CHECK(segment_c[0] == eight_bytes[7]);

    /* One-region checked and CPU reads retain the normal fast path. */
    uint32_t ordinary = 0x13579BDFu;
    CHECK(guest_memory_try_ptr(0x20000008u, sizeof(ordinary)) == segment_c);
    CHECK(guest_memory_write_scalar(0x20000008u, &ordinary, sizeof(ordinary)));
    uint32_t ordinary_read = 0;
    CHECK(rd32(0x20000008u) == ordinary);
    CHECK(guest_memory_read_scalar(0x20000008u, &ordinary_read, sizeof(ordinary_read)));
    CHECK(ordinary_read == ordinary);

    /* Holes and uint32 wrap fail atomically for both directions. */
    uint8_t gap_left[2] = {0x11, 0x22};
    uint8_t gap_right[2] = {0x33, 0x44};
    guest_memory_reset();
    guest_memory_add(gap_left, 0x30000000u, sizeof(gap_left));
    guest_memory_add(gap_right, 0x30000003u, sizeof(gap_right));
    uint32_t sentinel = 0xAABBCCDDu;
    CHECK(!guest_memory_read_scalar(0x30000000u, &sentinel, sizeof(sentinel)));
    CHECK(sentinel == 0xAABBCCDDu);
    uint32_t replacement = 0xFFEEDDCCu;
    CHECK(!guest_memory_write_scalar(0x30000000u, &replacement, sizeof(replacement)));
    CHECK(gap_left[0] == 0x11 && gap_left[1] == 0x22 &&
          gap_right[0] == 0x33 && gap_right[1] == 0x44);
    uint8_t top[2] = {0x55, 0x66};
    uint8_t bottom[2] = {0x77, 0x88};
    guest_memory_add(top, 0xFFFFFFFEu, sizeof(top));
    guest_memory_add(bottom, 0u, sizeof(bottom));
    CHECK(!guest_memory_read_scalar(0xFFFFFFFEu, &sentinel, sizeof(sentinel)));
    CHECK(sentinel == 0xAABBCCDDu);
    CHECK(!guest_memory_write_scalar(0xFFFFFFFEu, &replacement, sizeof(replacement)));
    CHECK(top[0] == 0x55 && top[1] == 0x66 && bottom[0] == 0x77 && bottom[1] == 0x88);

    guest_address_space_destroy(space);
    CHECK(mapping_capacity_contract() == 0);
    puts("PASS: VirtualQuery/scalar spans and 8192 simultaneous independent reservations with release/reuse");
    return 0;
}
