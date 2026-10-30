/* push/pop on the local ESP against the CPU-struct helpers: CPU state and stack memory. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
uint8_t *ac1_direct_arena;
static uint8_t arena_a[0x10000], arena_b[0x10000];
void plain_L_00401000(CPU *);
void local_L_00401000(CPU *);
void (guest_execution_read)(uint32_t a, void *p, size_t n) { (void)a; (void)p; (void)n; abort(); }
void (guest_execution_write)(uint32_t a, const void *p, size_t n) { (void)a; (void)p; (void)n; abort(); }
static unsigned calls;
void opaque(CPU *c) { ++calls; c->eax ^= c->esp; c->edx += rd32(c->esp); }
static uint32_t random_state = 0x13579bdfu;
static uint32_t random32(void) {
    random_state ^= random_state << 13; random_state ^= random_state >> 17;
    return random_state ^= random_state << 5;
}
int main(void) {
    for (unsigned i = 0; i < 4096; ++i) {
        CPU seed, a, b;
        memset(&seed, 0, sizeof seed);
        seed.eax = random32(); seed.ecx = random32(); seed.edx = random32(); seed.ebx = random32();
        seed.esp = 0x8000u - 4u * (random32() & 63u); seed.ebp = random32(); seed.esi = random32(); seed.edi = random32();
        for (unsigned k = 0; k < sizeof arena_a; ++k) arena_a[k] = arena_b[k] = (uint8_t)random32();
        a = seed; b = seed;
        ac1_direct_arena = arena_a; calls = 0; plain_L_00401000(&a);
        unsigned plain_calls = calls;
        ac1_direct_arena = arena_b; calls = 0; local_L_00401000(&b);
        assert(calls == plain_calls);
        assert(!memcmp(&a, &b, sizeof a));
        assert(!memcmp(arena_a, arena_b, sizeof arena_a));
    }
    puts("4096 local push/pop CPU and stack memory cases passed");
    return 0;
}
