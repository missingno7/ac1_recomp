/* Vector SSE lowering against the donor's lane lowering on the full CPU state.
 * C lets a compiler commute + and *, so when BOTH inputs of a lane are NaN
 * neither lowering fixes which NaN payload wins (x86 picks the destination's).
 * Such lanes of commutative float ops may differ; every other bit must match. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>

typedef struct { const char *name; void (*ref)(CPU *); void (*vec)(CPU *); unsigned nan_lane_bits, d, s; } sse_case;
extern const sse_case sse_cases[];
extern const unsigned sse_case_count;

static uint64_t state = 0x9E3779B97F4A7C15ull;
static uint32_t random32(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return (uint32_t)(state >> 11); }

static const uint32_t f32_special[] = {
    0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u, 0x7F800001u, 0xFFA00005u,
    0x00000001u, 0x807FFFFFu, 0x3F800000u, 0xBFC00000u, 0x4B000000u, 0x7F7FFFFFu, 0xCF000000u, 0x4F000000u,
};
static const uint64_t f64_special[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
    0x7FF8000000000000ull, 0xFFF8000000000123ull, 0x7FF0000000000001ull, 0x0000000000000001ull,
    0x3FF0000000000000ull, 0xC1E0000000000000ull, 0x41DFFFFFFFC00000ull, 0x47EFFFFFE0000000ull,
    0x36A0000000000000ull, 0x3810000000000000ull, 0xBFF8000000000000ull, 0x7FEFFFFFFFFFFFFFull,
};
static const uint32_t int_special[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x0000FFFFu, 0x00008000u, 0x7F7F8080u};

static void fill_xmm(XMM *x)
{
    unsigned mode = random32() % 6;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t r = random32();
        switch (mode) {
        case 0: x->u32[i] = f32_special[r % 16]; break;
        case 1: { float f = (float)((int32_t)r % 20000) / 64.0f; memcpy(&x->u32[i], &f, 4); break; }
        case 2: if (i < 2) x->u64[i] = f64_special[r % 16]; break;
        case 3: if (i < 2) { double d = (double)((int32_t)r) / 1024.0; memcpy(&x->u64[i], &d, 8); } break;
        case 4: x->u32[i] = (r & 1) ? int_special[(r >> 1) % 8] : random32(); break;
        default: x->u32[i] = i == 0 ? r % 80u : i == 1 ? (r & 0x10u ? 0u : r) : random32(); break;  /* shift counts */
        }
    }
}

static int is_nan_bits(const XMM *x, unsigned lane, unsigned bits)
{
    if (bits == 32) return (x->u32[lane] & 0x7FFFFFFFu) > 0x7F800000u;
    return (x->u64[lane] & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
}
/* Only the destination differs, and only in lanes where both inputs and both results are NaN. */
static int nan_payload_only(const CPU *start, const CPU *a, const CPU *b, const sse_case *k)
{
    CPU x, y;
    memcpy(&x, a, sizeof x); memcpy(&y, b, sizeof y);
    memset(&x.xmm[k->d], 0, 16); memset(&y.xmm[k->d], 0, 16);
    if (memcmp(&x, &y, sizeof x)) return 0;
    unsigned bits = k->nan_lane_bits, lanes = 128 / bits;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        int same = bits == 32 ? a->xmm[k->d].u32[lane] == b->xmm[k->d].u32[lane]
                              : a->xmm[k->d].u64[lane] == b->xmm[k->d].u64[lane];
        if (same) continue;
        if (!is_nan_bits(&a->xmm[k->d], lane, bits) || !is_nan_bits(&b->xmm[k->d], lane, bits) ||
            !is_nan_bits(&start->xmm[k->d], lane, bits) || !is_nan_bits(&start->xmm[k->s], lane, bits))
            return 0;
    }
    return 1;
}

int main(void)
{
    unsigned failures = 0, tolerated = 0;
    for (unsigned k = 0; k < sse_case_count; ++k) {
        for (unsigned n = 0; n < 4096; ++n) {
            CPU start, a, b;
            memset(&start, 0, sizeof start);
            for (unsigned r = 0; r < 8; ++r) fill_xmm(&start.xmm[r]);
            start.eax = random32() % 3 ? random32() : int_special[random32() % 8];
            start.ebx = random32(); start.ecx = random32(); start.esi = random32() % 70u;
            start.cf = random32() & 1; start.zf = random32() & 1; start.pf = random32() & 1;
            start.fp_initialized = 1; start.mxcsr = 0x1F80u; start.fpu_cw = 0x037Fu;
            memcpy(&a, &start, sizeof a); memcpy(&b, &start, sizeof b);
            sse_cases[k].ref(&a);
            sse_cases[k].vec(&b);
            if (memcmp(&a, &b, sizeof a)) {
                if (sse_cases[k].nan_lane_bits && nan_payload_only(&start, &a, &b, &sse_cases[k])) { ++tolerated; continue; }
                if (failures++ < 12) {
                    printf("MISMATCH %s state %u\n", sse_cases[k].name, n);
                    for (unsigned r = 0; r < 8; ++r)
                        if (memcmp(&a.xmm[r], &b.xmm[r], 16))
                            printf("  xmm%u ref %08x %08x %08x %08x vec %08x %08x %08x %08x\n", r,
                                   a.xmm[r].u32[0], a.xmm[r].u32[1], a.xmm[r].u32[2], a.xmm[r].u32[3],
                                   b.xmm[r].u32[0], b.xmm[r].u32[1], b.xmm[r].u32[2], b.xmm[r].u32[3]);
                }
                break;
            }
        }
    }
    if (failures) { printf("%u of %u SSE cases differ\n", failures, sse_case_count); return 1; }
    printf("%u SSE vector-lowering cases x 4096 states match the lane lowering\n", sse_case_count);
    printf("%u states differed only in NaN payloads of lanes with two NaN inputs\n", tolerated);
    return 0;
}
