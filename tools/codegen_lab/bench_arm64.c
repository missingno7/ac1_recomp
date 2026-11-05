/* Isolated ARM64 benchmark of lifted leaves (tools/codegen_lab.py, docs/codegen-lab.md).
 *
 *   time N REPS    nanoseconds per call, every repetition printed
 *   run LEAF N     N calls of one leaf (or "empty") for performance counters */
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned char memory[0x2000000] __attribute__((aligned(64)));
uint8_t *ac1_direct_arena = memory;
#undef guest_execution_read
#undef guest_execution_write
void guest_execution_read(uint32_t a, void *o, size_t n) { memcpy(o, memory + a, n); }
void guest_execution_write(uint32_t a, const void *v, size_t n) { memcpy(memory + a, v, n); }
void ac1_unsupported_instruction(uint32_t a, const char *s) { fprintf(stderr, "%08x %s\n", a, s); exit(2); }

void lifted_00428540(CPU *); void lifted_00653400(CPU *); void lifted_01176DE0(CPU *);

enum { ESP0 = 0x10000u, QUAT = 0x2A000u, MAT = 0x2B000u, A = 0x21000u, B = 0x22000u, OUT = 0x23000u, PLANE = 0x24000u };
static unsigned seed = 0x584735u;
static unsigned rng(void) { seed ^= seed << 13; seed ^= seed >> 17; return seed ^= seed << 5; }
static float rf(float scale) { return ((int)(rng() % 20001) - 10000) / scale; }

/* Per-call setups: argument slots and, with fresh != 0, new random inputs. */
static void s428(CPU *c, unsigned i, int fresh) {
    c->esp = ESP0; c->ecx = QUAT + 16u * (i & 63u); wr32(c->esp + 4, MAT + 64u * (i & 63u));
    if (fresh) for (unsigned j = 0; j < 4; ++j) wrss(c->ecx + 4 * j, rf(i % 3 ? 10000.0f : 16.0f));
}
static void s653(CPU *c, unsigned i, int fresh) {
    unsigned k = i & 15u; c->esp = ESP0;
    wr32(c->esp + 4, OUT + 64u * k); wr32(c->esp + 8, A + 64u * k); wr32(c->esp + 12, B + 64u * k);
    if (fresh) for (unsigned j = 0; j < 16; ++j) { wrss(A + 64u * k + 4 * j, rf(128.0f)); wrss(B + 64u * k + 4 * j, rf(128.0f)); }
}
static void s117(CPU *c, unsigned i, int fresh) {
    unsigned k = i & 63u, p = PLANE + 48u * k; c->esp = ESP0;
    wr32(c->esp + 4, p); wr32(c->esp + 8, p + 16); wr32(c->esp + 12, p + 32);
    if (fresh) for (unsigned j = 0; j < 12; ++j) wrss(p + 4 * j, rf(32.0f));
}
__attribute__((noinline)) static void empty_body(CPU *c) { __asm__ volatile("" :: "r"(c) : "memory"); }
static void s_empty(CPU *c, unsigned i, int fresh) { (void)fresh; c->esp = ESP0; c->ecx = QUAT + 16u * (i & 63u); }

typedef struct {
    const char *name; void (*lifted)(CPU *);
    void (*setup)(CPU *, unsigned, int); uint32_t out_base, out_span;
} leaf;
static const leaf leaves[] = {
    {"00428540", lifted_00428540, s428, MAT, 64u * 64u},
    {"00653400", lifted_00653400, s653, OUT, 64u * 16u},
    {"01176DE0", lifted_01176DE0, s117, PLANE, 48u * 64u},
};
enum { LEAVES = sizeof leaves / sizeof leaves[0] };
static const leaf empty = {"empty", empty_body, s_empty, 0, 0};

static void constants(void) {
    wr32(0x1680f90, 0x3f800000); wr32(0x1680f98, 0); wr32(0x1680f9c, 0x3ff00000);
    wr32(0x1682fa0, 0); wr32(0x1682fa4, 0x40000000);
    for (unsigned j = 0; j < 4; ++j) wr32(0x01A44C90 + 4 * j, 0);
    for (unsigned i = 0; i < 64; ++i) for (unsigned k = 0; k < LEAVES; ++k) { CPU c; memset(&c, 0, sizeof c); leaves[k].setup(&c, i, 1); }
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double time_leaf(const leaf *l, unsigned n) {
    CPU c; memset(&c, 0, sizeof c);
    void (*f)(CPU *) = l->lifted;
    double t0 = now();
    for (unsigned i = 0; i < n; ++i) { l->setup(&c, i, 0); f(&c); }
    return (now() - t0) / n * 1e9;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: time N REPS | run LEAF N\n"); return 2; }
    constants();
    if (!strcmp(argv[1], "time")) {
        unsigned n = (unsigned)atoi(argv[2]), reps = (unsigned)atoi(argv[3]);
        for (unsigned r = 0; r < reps; ++r) {
            printf("time empty %.3f\n", time_leaf(&empty, n));
            for (unsigned k = 0; k < LEAVES; ++k)
                printf("time %s %.3f\n", leaves[k].name, time_leaf(&leaves[k], n));
        }
        return 0;
    }
    if (!strcmp(argv[1], "run")) {
        const leaf *l = &empty;
        for (unsigned k = 0; k < LEAVES; ++k) if (!strcmp(argv[2], leaves[k].name)) l = &leaves[k];
        if (l == &empty && strcmp(argv[2], "empty")) { fprintf(stderr, "unknown leaf %s\n", argv[2]); return 2; }
        volatile double sink = time_leaf(l, (unsigned)atoi(argv[3])); (void)sink;
        return 0;
    }
    return 2;
}
