/* Harness for abi_chain.c: differential check of every protocol against the struct protocol, and timing.
 *
 * usage: abi_main check CASES          every variant on CASES random states (registers and memory)
 *        abi_main time VARIANT CALLS    VARIANT's chain, CALLS inner calls (run under simpleperf stat)
 * Variants are listed in VARIANTS (the build compiles abi_chain.c once per entry). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t r[8]; uint32_t eip; } cpu_t;
enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
uint8_t guest_memory[1 << 20];

#define VARIANTS(X) X(p0_struct) X(p1_esp) X(p1_esp_esi) X(p1_all) X(p1_broken) X(p2_esp) X(p2_esp_esi) X(p2_four_args) X(p2_four_saved) X(p2_all)
#define DECLARE(v) void v##_enter_A(cpu_t *c);
VARIANTS(DECLARE)
static const struct { const char *name; void (*enter)(cpu_t *); } variants[] = {
#define ENTRY(v) { #v, v##_enter_A },
    VARIANTS(ENTRY)
};
enum { NV = sizeof variants / sizeof *variants };

static uint32_t seed = 0x2545F491u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void setup(cpu_t *c, uint32_t s, uint32_t calls) {
    seed = s;
    memset(guest_memory, 0, 0x20000);
    for (uint32_t i = 0x1000; i < 0x2000; i += 4) { uint32_t v = rnd(); memcpy(guest_memory + i, &v, 4); }
    uint32_t object = 0x1000u + (rnd() & 0x3F0u), tag = 0xC0DEu;
    memcpy(guest_memory + object, &tag, 4);                  /* [esi]: the "vtable" slot dispatch resolves */
    for (int i = 0; i < 8; ++i) c->r[i] = rnd();
    c->r[ECX] = object; c->r[EDX] = calls; c->r[ESP] = 0x10000u - 4u * (rnd() & 0xF); c->eip = 0;
    uint32_t ret = 0x400000u; c->r[ESP] -= 4; memcpy(guest_memory + c->r[ESP], &ret, 4);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "check")) {
        uint32_t cases = (uint32_t)strtoul(argv[2], NULL, 10);
        static uint8_t expected[0x20000];
        int failed = 0;
        for (int v = 1; v < NV; ++v) {
            uint32_t bad = 0;
            for (uint32_t k = 0; k < cases; ++k) {
                cpu_t a, b;
                setup(&a, 0x1234567u + k, 1 + k % 9); variants[0].enter(&a); memcpy(expected, guest_memory, sizeof expected);
                setup(&b, 0x1234567u + k, 1 + k % 9); variants[v].enter(&b);
                if (memcmp(a.r, b.r, sizeof a.r) || memcmp(expected, guest_memory, sizeof expected)) ++bad;
            }
            printf("%-12s %s (%u of %u cases differ)\n", variants[v].name, bad ? "DIFFERS" : "exact", bad, cases);
            failed |= bad && strcmp(variants[v].name, "p1_broken");
        }
        return failed;
    }
    if (argc == 4 && !strcmp(argv[1], "time")) {
        for (int v = 0; v < NV; ++v) {
            if (strcmp(variants[v].name, argv[2])) continue;
            uint64_t calls = strtoull(argv[3], NULL, 10), done = 0; uint32_t acc = 0;
            cpu_t start; setup(&start, 7u, 256);
            while (done < calls) {
                cpu_t c = start; variants[v].enter(&c); acc += c.r[EBX]; done += 256;
            }
            printf("%s %llu calls (check %u)\n", argv[2], (unsigned long long)done, acc);
            return 0;
        }
    }
    fprintf(stderr, "usage: %s check CASES | time VARIANT CALLS\n", argv[0]); return 2;
}
