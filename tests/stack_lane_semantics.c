/* Lane-forwarded 16-byte stack loads against plain loads: whole CPU and memory. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
uint8_t *ac1_direct_arena;
static uint8_t arena_a[0x10000], arena_b[0x10000];
void plain_L_00401000(CPU *);
void forwarded_L_00401000(CPU *);
void (guest_execution_read)(uint32_t a, void *p, size_t n) { (void)a; (void)p; (void)n; abort(); }
void (guest_execution_write)(uint32_t a, const void *p, size_t n) { (void)a; (void)p; (void)n; abort(); }
static uint32_t random_state = 0x2468ace1u;
static uint32_t random32(void) {
    random_state ^= random_state << 13; random_state ^= random_state >> 17;
    return random_state ^= random_state << 5;
}
int main(void) {
    for (unsigned i = 0; i < 4096; ++i) {
        CPU seed, a, b;
        memset(&seed, 0, sizeof seed);
        seed.eax = 0x6000u + 4u * (random32() & 0x3ffu);   /* the heap store's target */
        seed.ecx = random32(); seed.edx = random32(); seed.ebx = random32();
        seed.esp = 0x8000u - 16u * (random32() & 31u); seed.ebp = random32(); seed.esi = random32(); seed.edi = random32();
        for (unsigned r = 0; r < 8; ++r) for (unsigned k = 0; k < 4; ++k) seed.xmm[r].u32[k] = random32();
        for (unsigned k = 0; k < sizeof arena_a; ++k) arena_a[k] = arena_b[k] = (uint8_t)random32();
        if (i & 1) {   /* the heap store hits a forwarded slot: forwarding must have stopped */
            seed.eax = seed.esp + 0x10u;
        }
        a = seed; b = seed;
        ac1_direct_arena = arena_a; plain_L_00401000(&a);
        ac1_direct_arena = arena_b; forwarded_L_00401000(&b);
        assert(!memcmp(&a, &b, sizeof a));
        assert(!memcmp(arena_a, arena_b, sizeof arena_a));
    }
    puts("4096 forwarded stack-lane CPU and memory cases passed");
    return 0;
}
