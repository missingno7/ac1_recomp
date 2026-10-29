#include "cpu.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        return 1; \
    } \
} while (0)

int main(void)
{
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
    puts("PASS: fixed-width CPU copies span only fully mapped adjacent guest regions");
    return 0;
}
