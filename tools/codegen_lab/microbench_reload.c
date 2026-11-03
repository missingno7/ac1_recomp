/* Reloading a callee-saved register after a guest call: plain load or value speculation.
 *
 * usage: microbench_reload plain|speculative|pinned|pinned_speculative ITERATIONS
 * The lifted calling protocol: the caller spills esi to the CPU struct, the
 * callee loads it, pushes it to the guest stack, pops it back and stores it at
 * return, and the caller reloads it and chases a pointer through it (as
 * `mov esi, [esi+8]` would), so every iteration's address depends on the
 * reload. "speculative" still loads c->esi but keeps using the caller's own
 * value unless the two differ (a predicted branch), which is exact.
 * "pinned" keeps esi in host register x26 across guest calls instead of the
 * struct (as Rosetta 2 keeps x86 registers in fixed ARM registers): the file is
 * built with -ffixed-x26 so compiled code never allocates it, and the struct
 * hops become register moves; the callee's own push/pop of esi stays in guest
 * memory. "pinned_speculative" also compares instead of using the moved value;
 * "pinned_all" pins esp (x27) too, so neither register crosses the call in memory;
 * "speculative_all" keeps both in memory but speculates esi and the callee's
 * known esp (the pushed return address popped) after the call.
 * An "espfixed_" prefix stores a constant esp each iteration, which cuts the
 * esp chain through the struct (caller push, callee ret) so only esi's shows. */
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

#if defined(__aarch64__)
#define PIN_GET(v) __asm__ __volatile__("mov %w0, w26" : "=r"(v))
#define PIN_SET(v) __asm__ __volatile__("mov w26, %w0" : : "r"(v))
#else
static uint32_t pinned_esi;
#define PIN_GET(v) __asm__ __volatile__("" : "=r"(v) : "0"(pinned_esi))
#define PIN_SET(v) do { pinned_esi = (v); __asm__ __volatile__("" ::: "memory"); } while (0)
#endif

NOINLINE void callee_pinned(cpu_t *c) {   /* the same callee, esi in the pinned register */
    uint32_t esp = c->esp, esi, ecx = c->ecx;
    PIN_GET(esi);
    esp -= 4; wr32(esp, esi);
    esi = ecx; uint32_t eax = rd32(esi + 4);
    esi = rd32(esp); esp += 4; esp += 4;
    c->eax = eax; c->esp = esp;
    PIN_SET(esi);
}

static int esp_fixed;
#if defined(__aarch64__)
#define PIN_GET_ESP(v) __asm__ __volatile__("mov %w0, w27" : "=r"(v))
#define PIN_SET_ESP(v) __asm__ __volatile__("mov w27, %w0" : : "r"(v))
#else
static uint32_t pinned_esp;
#define PIN_GET_ESP(v) __asm__ __volatile__("" : "=r"(v) : "0"(pinned_esp))
#define PIN_SET_ESP(v) do { pinned_esp = (v); __asm__ __volatile__("" ::: "memory"); } while (0)
#endif

NOINLINE void callee_pinned_all(cpu_t *c) {   /* esi and esp both in pinned registers */
    uint32_t esp, esi, ecx = c->ecx;
    PIN_GET_ESP(esp); PIN_GET(esi);
    esp -= 4; wr32(esp, esi);
    esi = ecx; uint32_t eax = rd32(esi + 4);
    esi = rd32(esp); esp += 4; esp += 4;
    c->eax = eax;
    PIN_SET(esi); PIN_SET_ESP(esp);
}

NOINLINE uint32_t run_pinned_all(cpu_t *c, uint64_t n) {
    uint32_t esi = c->esi, esp = c->esp, acc = 0;
    for (uint64_t i = 0; i < n; ++i) {
        esp -= 4; wr32(esp, 0x00401234u);
        PIN_SET(esi); PIN_SET_ESP(esp);
        callee_pinned_all(c);
        PIN_GET(esi); PIN_GET_ESP(esp);
        acc += c->eax;
        esi = rd32(esi + 8);
    }
    c->esp = esp;
    return acc + esi;
}

NOINLINE uint32_t run_speculative_all(cpu_t *c, uint64_t n) {
    uint32_t esi = c->esi, esp = c->esp, acc = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t expected = esp;                                  /* the callee pops what the call pushed */
        esp -= 4; wr32(esp, 0x00401234u);
        c->esp = esp; c->esi = esi;
        callee(c);
        AC1_KEEP_IF_SAME(esi, c->esi);
        AC1_KEEP_IF_SAME(expected, c->esp); esp = expected;
        acc += c->eax;
        esi = rd32(esi + 8);
    }
    return acc + esi;
}

NOINLINE uint32_t run(cpu_t *c, uint64_t n, int mode) {
    if (mode == 4) return run_pinned_all(c, n);
    if (mode == 5) return run_speculative_all(c, n);
    uint32_t esi = c->esi, acc = 0;
    for (uint64_t i = 0; i < n; ++i) {
        if (esp_fixed) c->esp = 0x80000u;                         /* isolate the esi chain */
        c->esp -= 4; wr32(c->esp, 0x00401234u);                  /* push the return address */
        if (mode >= 2) {
            PIN_SET(esi);
            callee_pinned(c);
            uint32_t v; PIN_GET(v);
            if (mode == 3) AC1_KEEP_IF_SAME(esi, v); else esi = v;
        } else {
            c->esi = esi;                                         /* spill */
            callee(c);
            if (mode == 1) AC1_KEEP_IF_SAME(esi, c->esi); else esi = c->esi;
        }
        acc += c->eax;
        esi = rd32(esi + 8);                                      /* mov esi, [esi + 8] */
    }
    return acc + esi;
}

int main(int argc, char **argv) {
    static const char *modes[] = {"plain", "speculative", "pinned", "pinned_speculative", "pinned_all", "speculative_all"};
    int mode = -1;
    const char *name = argv[1];
    if (argc == 3 && !strncmp(name, "espfixed_", 9)) { esp_fixed = 1; name += 9; }
    for (int k = 0; argc == 3 && k < 6; ++k) if (!strcmp(name, modes[k])) mode = k;
    if (mode < 0) { fprintf(stderr, "usage: %s plain|speculative|pinned|pinned_speculative ITERATIONS\n", argv[0]); return 2; }
    uint64_t n = strtoull(argv[2], NULL, 10);
    /* A ring of 64 objects: [esi + 8] is the next object. */
    for (uint32_t k = 0; k < 64; ++k) { uint32_t a = 0x1000u + 64u * k; wr32(a + 8, 0x1000u + 64u * ((k + 17) % 64)); wr32(a + 4, k); }
    cpu_t c = { .ecx = 0x1000u, .esp = 0x80000u, .esi = 0x1000u };
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t r = run(&c, n, mode);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    printf("%s %.3f ns/iteration (check %u)\n", argv[1], ns / n, r);
    return 0;
}
