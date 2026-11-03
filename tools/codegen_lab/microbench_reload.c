/* Reloading a callee-saved register after a guest call: plain load or value speculation.
 *
 * usage: microbench_reload plain|speculative ITERATIONS
 * The lifted calling protocol: the caller spills esi to the CPU struct, the
 * callee loads it, pushes it to the guest stack, pops it back and stores it at
 * return, and the caller reloads it and chases a pointer through it (as
 * `mov esi, [esi+8]` would), so every iteration's address depends on the
 * reload. "speculative" still loads c->esi but keeps using the caller's own
 * value unless the two differ (a predicted branch), which is exact. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; } cpu_t;
static uint8_t arena[1 << 20];
#define NOINLINE __attribute__((noinline))
#define AC1_KEEP_IF_SAME(local, loaded) do { uint32_t _v = (loaded); \
    if (__builtin_expect(_v != (local), 0)) { __asm__ __volatile__("" : "+r"(_v)); (local) = _v; } \
    else __asm__("" : "+r"(local)); } while (0)
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, arena + a, 4); return v; }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(arena + a, &v, 4); }

NOINLINE void callee(cpu_t *c) {          /* push esi; mov esi, ecx; mov eax, [esi+4]; pop esi; ret */
    uint32_t esp = c->esp, esi = c->esi, ecx = c->ecx;
    esp -= 4; wr32(esp, esi);
    esi = ecx; uint32_t eax = rd32(esi + 4);
    esi = rd32(esp); esp += 4; esp += 4;                     /* pop esi; ret */
    c->eax = eax; c->esi = esi; c->esp = esp;
}

NOINLINE uint32_t run(cpu_t *c, uint64_t n, int speculative) {
    uint32_t esi = c->esi, acc = 0;
    for (uint64_t i = 0; i < n; ++i) {
        c->esi = esi; c->esp -= 4; wr32(c->esp, 0x00401234u);   /* spill, push the return address */
        callee(c);
        if (speculative) AC1_KEEP_IF_SAME(esi, c->esi); else esi = c->esi;
        acc += c->eax;
        esi = rd32(esi + 8);                                      /* mov esi, [esi + 8] */
    }
    return acc + esi;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s plain|speculative ITERATIONS\n", argv[0]); return 2; }
    int speculative = !strcmp(argv[1], "speculative");
    uint64_t n = strtoull(argv[2], NULL, 10);
    /* A ring of 64 objects: [esi + 8] is the next object. */
    for (uint32_t k = 0; k < 64; ++k) { uint32_t a = 0x1000u + 64u * k; wr32(a + 8, 0x1000u + 64u * ((k + 17) % 64)); wr32(a + 4, k); }
    cpu_t c = { .ecx = 0x1000u, .esp = 0x80000u, .esi = 0x1000u };
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t r = run(&c, n, speculative);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    printf("%s %.3f ns/iteration (check %u)\n", argv[1], ns / n, r);
    return 0;
}
