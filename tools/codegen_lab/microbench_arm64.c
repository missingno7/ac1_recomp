/* Cost of the code shapes the lifter emits, measured in isolation.
 *
 * usage: microbench KERNEL ITERATIONS    (run under simpleperf stat; "list" names the kernels)
 * Each kernel repeats one shape ITERATIONS times. A "chain" kernel makes every
 * iteration depend on the previous one, so it measures latency on the critical
 * path; a "tput" kernel keeps iterations independent, so it measures what the
 * shape costs when the core can overlap it. "empty" is the loop alone.
 * Compiler barriers stand in for what makes the lifted code keep values in the
 * CPU struct: calls and escapes the compiler cannot see through. */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t gpr[8]; uint32_t eip; uint32_t flags[4]; uint32_t top; double st[8]; uint32x4_t xmm[8]; } cpu_t;
static cpu_t cpu_storage __attribute__((aligned(64)));
static cpu_t *volatile cpu_slot = &cpu_storage;
#define OPAQUE(x) __asm__ __volatile__("" : "+r"(x))
#define OPAQUE_V(x) __asm__ __volatile__("" : "+w"(x))
#define CLOBBER() __asm__ __volatile__("" ::: "memory")
#define NOINLINE __attribute__((noinline))

/* --- XMM: whole-vector and lane traffic through the struct --- */
NOINLINE static uint64_t xmm_reg_chain(cpu_t *c, uint64_t n) {          /* register-only reference */
    uint32x4_t v = c->xmm[0];
    for (uint64_t i = 0; i < n; ++i) { v = vsetq_lane_u32(vgetq_lane_u32(v, 1) + 1u, v, 0); v = vaddq_u32(v, v); OPAQUE_V(v); }
    c->xmm[0] = v; return 0;
}
NOINLINE static uint64_t xmm_v16_v16_chain(cpu_t *c, uint64_t n) {     /* 16B store, 16B load */
    uint32x4_t v = c->xmm[0];
    for (uint64_t i = 0; i < n; ++i) { v = vaddq_u32(v, v); c->xmm[0] = v; CLOBBER(); v = c->xmm[0]; }
    return 0;
}
NOINLINE static uint64_t xmm_v16_lane_v16_chain(cpu_t *c, uint64_t n) { /* 16B store, 4B lane store, 16B load */
    uint32x4_t v = c->xmm[0];
    for (uint64_t i = 0; i < n; ++i) {
        v = vaddq_u32(v, v); c->xmm[0] = v; CLOBBER();
        ((uint32_t *)&c->xmm[0])[0] = ((uint32_t *)&c->xmm[0])[1] + 1u; CLOBBER(); v = c->xmm[0];
    }
    return 0;
}
NOINLINE static uint64_t xmm_4x4_v16_chain(cpu_t *c, uint64_t n) {     /* four 4B scalar stores, 16B load */
    uint32_t *l = (uint32_t *)&c->xmm[0]; uint32_t s = l[0];
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t a = s + 1u, b = s ^ 5u, d = s + 7u, e = s << 1; OPAQUE(a); OPAQUE(b); OPAQUE(d); OPAQUE(e);
        l[0] = a; l[1] = b; l[2] = d; l[3] = e; CLOBBER();
        uint32x4_t v = c->xmm[0]; v = vaddq_u32(v, v); s = vgetq_lane_u32(v, 0) + vgetq_lane_u32(v, 3);
    }
    return 0;
}
NOINLINE static uint64_t xmm_v16_4_chain(cpu_t *c, uint64_t n) {       /* 16B store, 4B load of a lane */
    uint32x4_t v = c->xmm[0];
    for (uint64_t i = 0; i < n; ++i) {
        v = vaddq_u32(v, v); c->xmm[0] = v; CLOBBER();
        uint32_t x = ((uint32_t *)&c->xmm[0])[2]; v = vsetq_lane_u32(x, v, 3);
    }
    return 0;
}

/* --- GPR calling protocol: values passed through the struct or in registers --- */
NOINLINE uint32_t callee_regs(uint32_t a, uint32_t b) { OPAQUE(a); return a + b; }
NOINLINE void callee_struct1(cpu_t *c) { uint32_t a = c->gpr[0]; OPAQUE(a); c->gpr[0] = a + c->gpr[1]; }
NOINLINE void callee_struct8(cpu_t *c) {
    uint32_t r[8]; for (int k = 0; k < 8; ++k) r[k] = c->gpr[k];
    OPAQUE(r[0]); r[0] += r[1];
    for (int k = 0; k < 8; ++k) c->gpr[k] = r[k];
}
NOINLINE void callee_empty(cpu_t *c) { (void)c; CLOBBER(); }
NOINLINE static uint64_t call_regs_chain(cpu_t *c, uint64_t n) {        /* argument and result in registers */
    uint32_t a = c->gpr[0], b = c->gpr[1];
    for (uint64_t i = 0; i < n; ++i) a = callee_regs(a, b);
    c->gpr[0] = a; return 0;
}
NOINLINE static uint64_t call_struct1_chain(cpu_t *c, uint64_t n) {     /* eax through the struct, both ways */
    uint32_t a = c->gpr[0];
    for (uint64_t i = 0; i < n; ++i) { c->gpr[0] = a; callee_struct1(c); a = c->gpr[0]; }
    return 0;
}
NOINLINE static uint64_t call_struct8_chain(cpu_t *c, uint64_t n) {     /* all eight GPRs synced around each call */
    uint32_t r[8]; for (int k = 0; k < 8; ++k) r[k] = c->gpr[k];
    for (uint64_t i = 0; i < n; ++i) {
        for (int k = 0; k < 8; ++k) c->gpr[k] = r[k];
        callee_struct8(c);
        for (int k = 0; k < 8; ++k) r[k] = c->gpr[k];
    }
    return 0;
}
NOINLINE static uint64_t call_sync8_offpath(cpu_t *c, uint64_t n) {     /* full sync, values off the loop chain */
    uint32_t acc = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t base = (uint32_t)i; OPAQUE(base);
        for (int k = 0; k < 8; ++k) c->gpr[k] = base + (uint32_t)k;
        callee_empty(c);
        uint32_t x = 0; for (int k = 0; k < 8; ++k) x ^= c->gpr[k];
        acc += x;
    }
    c->gpr[0] = acc; return 0;
}
NOINLINE static uint64_t call_store8_only(cpu_t *c, uint64_t n) {       /* stores before the call, nothing reloaded */
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t base = (uint32_t)i; OPAQUE(base);
        for (int k = 0; k < 8; ++k) c->gpr[k] = base + (uint32_t)k;
        callee_empty(c);
    }
    return 0;
}
NOINLINE static uint64_t call_live4_chain(cpu_t *c, uint64_t n) {       /* four values live across the call, through the struct */
    uint32_t r0 = c->gpr[3], r1 = c->gpr[5], r2 = c->gpr[6], r3 = c->gpr[7];
    for (uint64_t i = 0; i < n; ++i) {
        c->gpr[3] = r0; c->gpr[5] = r1; c->gpr[6] = r2; c->gpr[7] = r3;
        callee_empty(c);
        r0 = c->gpr[3] + 1u; r1 = c->gpr[5] ^ r0; r2 = c->gpr[6] + r1; r3 = c->gpr[7] + 1u;
    }
    c->gpr[0] = r0 + r1 + r2 + r3; return 0;
}
NOINLINE static uint64_t call_live4_regs(cpu_t *c, uint64_t n) {        /* the same values kept in callee-saved registers */
    uint32_t r0 = c->gpr[3], r1 = c->gpr[5], r2 = c->gpr[6], r3 = c->gpr[7];
    for (uint64_t i = 0; i < n; ++i) {
        callee_empty(c);
        r0 = r0 + 1u; r1 = r1 ^ r0; r2 = r2 + r1; r3 = r3 + 1u; OPAQUE(r0); OPAQUE(r1); OPAQUE(r2); OPAQUE(r3);
    }
    c->gpr[0] = r0 + r1 + r2 + r3; return 0;
}
NOINLINE static uint64_t call_empty_chain(cpu_t *c, uint64_t n) {       /* the call alone */
    for (uint64_t i = 0; i < n; ++i) callee_empty(c);
    return 0;
}

/* --- indirect transfer: guest address to host function --- */
typedef void (*host_fn)(cpu_t *);
static host_fn table[1u << 12];
NOINLINE static uint64_t indirect_table(cpu_t *c, uint64_t n) {         /* load the target, blr */
    uint32_t va = 0x00401000u;
    for (uint64_t i = 0; i < n; ++i) { OPAQUE(va); table[(va >> 4) & 0xFFFu](c); }
    return 0;
}
NOINLINE static uint64_t indirect_checked(cpu_t *c, uint64_t n) {       /* compare with the expected target first */
    uint32_t va = 0x00401000u;
    for (uint64_t i = 0; i < n; ++i) {
        OPAQUE(va); host_fn f = table[(va >> 4) & 0xFFFu];
        if (f == callee_empty) callee_empty(c); else f(c);
    }
    return 0;
}

/* --- x87: stack slots through the struct's TOP --- */
NOINLINE static uint64_t x87_top_mem_chain(cpu_t *c, uint64_t n) {      /* fld st(0); op; fstp: TOP and slots in memory */
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t t = c->top; double x = c->st[t & 7]; x = x * 1.0000001 + 1.0;
        t = (t + 1u) & 7u; c->st[t] = x; c->top = t; CLOBBER();
    }
    return 0;
}
NOINLINE static uint64_t x87_reg_chain(cpu_t *c, uint64_t n) {          /* the same value in a register */
    double x = c->st[0];
    for (uint64_t i = 0; i < n; ++i) { x = x * 1.0000001 + 1.0; OPAQUE_V(x); }
    c->st[0] = x; return 0;
}

/* --- flags: materialized into the struct or kept in a register --- */
NOINLINE static uint64_t flags_mem_chain(cpu_t *c, uint64_t n) {        /* sub, store result for ZF, reload, branch */
    uint32_t a = c->gpr[0], hits = 0;
    for (uint64_t i = 0; i < n; ++i) {
        a = a * 3u + 1u; c->flags[0] = a; c->flags[1] = a >> 31; CLOBBER();
        if (c->flags[0] == 0) ++hits;
    }
    c->gpr[0] = a + hits; return 0;
}
NOINLINE static uint64_t flags_reg_chain(cpu_t *c, uint64_t n) {
    uint32_t a = c->gpr[0], hits = 0;
    for (uint64_t i = 0; i < n; ++i) { a = a * 3u + 1u; OPAQUE(a); if (a == 0) ++hits; }
    c->gpr[0] = a + hits; return 0;
}

/* --- partial registers: a byte or word written, the dword read back --- */
NOINLINE static uint64_t gpr_word_chain(cpu_t *c, uint64_t n) {         /* 32-bit store, 32-bit load */
    for (uint64_t i = 0; i < n; ++i) { uint32_t a = c->gpr[0]; c->gpr[0] = a + 1u; CLOBBER(); }
    return 0;
}
NOINLINE static uint64_t gpr_byte_chain(cpu_t *c, uint64_t n) {         /* mov al: 8-bit store, 32-bit load */
    for (uint64_t i = 0; i < n; ++i) { uint32_t a = c->gpr[0]; *(volatile uint8_t *)&c->gpr[0] = (uint8_t)(a + 1u); CLOBBER(); }
    return 0;
}
NOINLINE static uint64_t gpr_byte_merge_chain(cpu_t *c, uint64_t n) {   /* the same merge done in a register */
    for (uint64_t i = 0; i < n; ++i) { uint32_t a = c->gpr[0]; c->gpr[0] = (a & ~0xFFu) | ((a + 1u) & 0xFFu); CLOBBER(); }
    return 0;
}
NOINLINE static uint64_t pair_2x8_v16_chain(cpu_t *c, uint64_t n) {     /* two 8B stores, one 16B load */
    uint64_t *l = (uint64_t *)&c->xmm[1];
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t a = l[0] + 1u, b = l[1] ^ 3u; OPAQUE(a); OPAQUE(b); l[0] = a; l[1] = b; CLOBBER();
        uint32x4_t v = c->xmm[1]; OPAQUE_V(v); (void)v;
    }
    return 0;
}

NOINLINE static uint64_t empty(cpu_t *c, uint64_t n) { for (uint64_t i = 0; i < n; ++i) OPAQUE(i); (void)c; return 0; }

static const struct { const char *name; uint64_t (*fn)(cpu_t *, uint64_t); } kernels[] = {
#define K(x) { #x, x }
    K(empty), K(xmm_reg_chain), K(xmm_v16_v16_chain), K(xmm_v16_lane_v16_chain), K(xmm_4x4_v16_chain), K(xmm_v16_4_chain),
    K(call_empty_chain), K(call_regs_chain), K(call_struct1_chain), K(call_struct8_chain), K(call_sync8_offpath), K(call_store8_only), K(call_live4_chain), K(call_live4_regs),
    K(indirect_table), K(indirect_checked), K(x87_reg_chain), K(x87_top_mem_chain), K(flags_reg_chain), K(flags_mem_chain),
    K(gpr_word_chain), K(gpr_byte_chain), K(gpr_byte_merge_chain), K(pair_2x8_v16_chain),
#undef K
};

int main(int argc, char **argv) {
    for (unsigned i = 0; i < sizeof table / sizeof *table; ++i) table[i] = callee_empty;
    cpu_t *c = cpu_slot; memset(c, 0, sizeof *c); c->gpr[0] = 7; c->gpr[1] = 3; c->st[1] = 1.5;
    if (argc == 2 && !strcmp(argv[1], "list")) {
        for (unsigned i = 0; i < sizeof kernels / sizeof *kernels; ++i) puts(kernels[i].name);
        return 0;
    }
    if (argc != 3) { fprintf(stderr, "usage: %s KERNEL ITERATIONS | list\n", argv[0]); return 2; }
    uint64_t n = strtoull(argv[2], NULL, 10);
    for (unsigned i = 0; i < sizeof kernels / sizeof *kernels; ++i)
        if (!strcmp(argv[1], kernels[i].name)) { kernels[i].fn(c, n); return (int)(c->gpr[0] & 0); }
    fprintf(stderr, "unknown kernel %s\n", argv[1]); return 2;
}
