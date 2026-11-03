/* x86 round trip: original leaves vs their lifted C recompiled for 32-bit x86
 * (tools/codegen_lab.py, docs/codegen-lab.md).
 *
 *   roundtrip IMAGE check N     gate: outputs and return values of original
 *                               and lifted code from the same inputs
 *   roundtrip IMAGE time N REPS reference cycles per call (QueryThreadCycleTime),
 *                               every repetition printed
 *
 * One arena holds the image at arena+VA, the guest stack and the test buffers.
 * Lifted code uses it as its direct-memory base, as the runtime does; original
 * code runs from its copy at arena+VA with absolute data operands patched to
 * arena+VA (lab_patches.h); its branches are relative and internal. Windows
 * exposes no retired-instruction counter to user mode, so only cycles. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "lab_patches.h"

uint8_t *ac1_direct_arena;
#undef guest_execution_read
#undef guest_execution_write
void guest_execution_read(uint32_t a, void *o, size_t n) { memcpy(o, ac1_direct_arena + a, n); }
void guest_execution_write(uint32_t a, const void *v, size_t n) { memcpy(ac1_direct_arena + a, v, n); }
void ac1_unsupported_instruction(uint32_t a, const char *s) { fprintf(stderr, "%08x %s\n", a, s); exit(2); }

void lifted_00428540(CPU *); void lifted_00653400(CPU *); void lifted_01176DE0(CPU *);
typedef void (__attribute__((thiscall)) *quat_to_matrix)(const float *q, float *out);
typedef float *(*matrix_product)(float *out, const float *a, const float *b);
typedef int (*plane_box)(const float *plane, const float *low, const float *high);

enum { IMAGE_VA = 0x400000u, STACK_VA = 0x1C00000u, DATA_VA = 0x1C80000u, ARENA_BYTES = 0x1D00000u };
static CPU cpu;
static uint32_t gva(const void *p) { return (uint32_t)((const uint8_t *)p - ac1_direct_arena); }
static unsigned seed = 0x1234567u;
static float rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return ((int)(seed % 20001) - 10000) / 10000.0f; }

static void args(uint32_t a1, uint32_t a2, uint32_t a3) {
    cpu.esp = STACK_VA + 0x8000u;
    uint32_t *sp = (uint32_t *)(ac1_direct_arena + cpu.esp);
    sp[0] = 0xFFFFF000u; sp[1] = a1; sp[2] = a2; sp[3] = a3;
}
static void l428(const float *q, float *out) { cpu.ecx = gva(q); args(gva(out), 0, 0); lifted_00428540(&cpu); }
static void l653(float *out, const float *a, const float *b) { args(gva(out), gva(a), gva(b)); lifted_00653400(&cpu); }
static int l117(const float *p, const float *lo, const float *hi) { args(gva(p), gva(lo), gva(hi)); lifted_01176DE0(&cpu); return (int)cpu.eax; }
__attribute__((noinline)) static void empty_lifted(CPU *c) { __asm__ volatile("" :: "r"(c) : "memory"); }
static void l_empty(void) { cpu.ecx = 0; args(0, 0, 0); empty_lifted(&cpu); }
__attribute__((noinline, thiscall)) static void o_empty(const float *q, float *out) { __asm__ volatile("" :: "r"(q), "r"(out) : "memory"); }
static ULONG64 cycles(void) { ULONG64 t; QueryThreadCycleTime(GetCurrentThread(), &t); return t; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: IMAGE check N | IMAGE time N REPS\n"); return 2; }
    ac1_direct_arena = VirtualAlloc(NULL, ARENA_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    FILE *f = fopen(argv[1], "rb");
    if (!ac1_direct_arena || !f) { fprintf(stderr, "arena or image unavailable\n"); return 2; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (IMAGE_VA + (uint32_t)size > STACK_VA || fread(ac1_direct_arena + IMAGE_VA, 1, size, f) != (size_t)size) return 2;
    fclose(f);
    for (unsigned k = 0; k < sizeof disp_offsets / sizeof disp_offsets[0]; ++k)
        *(uint32_t *)(ac1_direct_arena + IMAGE_VA + disp_offsets[k]) += (uint32_t)(uintptr_t)ac1_direct_arena;
    SetThreadAffinityMask(GetCurrentThread(), 1u << 4);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    quat_to_matrix o428 = (quat_to_matrix)(ac1_direct_arena + leaf_start[0]);
    matrix_product o653 = (matrix_product)(ac1_direct_arena + leaf_start[1]);
    plane_box o117 = (plane_box)(ac1_direct_arena + leaf_start[2]);
    float *data = (float *)(ac1_direct_arena + DATA_VA);
    float *q = data, *a = data + 64, *b = data + 128, *x = data + 256, *y = data + 320;
    unsigned n = (unsigned)atoi(argv[3]);

    if (!strcmp(argv[2], "check")) {
        unsigned bad[3] = {0};
        for (unsigned i = 0; i < n; ++i) {
            for (int k = 0; k < 64; ++k) { a[k] = rnd(); b[k] = rnd(); }
            for (int k = 0; k < 4; ++k) q[k] = rnd();
            memset(x, 0x55, 64); memset(y, 0x55, 64); o428(q, x); l428(q, y); bad[0] += memcmp(x, y, 64) != 0;
            memset(x, 0x55, 64); memset(y, 0x55, 64); o653(x, a, b); l653(y, a, b); bad[1] += memcmp(x, y, 64) != 0;
            bad[2] += o117(a, b, b + 16) != l117(a, b, b + 16);
        }
        printf("check 00428540 %u/%u\ncheck 00653400 %u/%u\ncheck 01176DE0 %u/%u\n", n - bad[0], n, n - bad[1], n, n - bad[2], n);
        return bad[0] || bad[1] || bad[2];
    }
    if (!strcmp(argv[2], "time")) {
        unsigned reps = (unsigned)atoi(argv[4]);
        for (int k = 0; k < 64; ++k) { a[k] = rnd(); b[k] = rnd(); }
        for (int k = 0; k < 4; ++k) q[k] = rnd();
        volatile int sink = 0;
        for (unsigned r = 0; r < reps; ++r) {
            ULONG64 t;
#define TIME(label, expr) t = cycles(); for (unsigned i = 0; i < n; ++i) { expr; } printf("time %s %.3f\n", label, (double)(cycles() - t) / n)
            TIME("empty", l_empty());
            TIME("original_empty", o_empty(q, x));
            TIME("00428540", l428(q, x));   TIME("original_00428540", o428(q, x));
            TIME("00653400", l653(x, a, b)); TIME("original_00653400", o653(x, a, b));
            TIME("01176DE0", sink += l117(a, b, b + 16)); TIME("original_01176DE0", sink += o117(a, b, b + 16));
        }
        return 0;
    }
    return 2;
}
