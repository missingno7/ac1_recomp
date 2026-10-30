/*
 * cpu.h - runtime for the CPU-struct x86-32 recompilation model
 *         (paired with tools/lift/lift32_cpu.py).
 *
 * Lifted x86 functions operate on an explicit CPU state struct, so several
 * machine states can be live at once - which is what a hybrid build needs when
 * real library code calls back into lifted code while an outer lifted call is
 * still on the stack. See runtime/hybrid/.
 *
 * Guest registers and memory addresses stay 32-bit. This generated adapter
 * translates memory accesses through guest_execution_ptr(), so the host can use 64-bit
 * pointers without treating guest addresses as native addresses.
 *
 * Flags are computed eagerly by helpers after arithmetic/logic ops; Jcc reads
 * the stored bits. This is verbose but mechanical and easy to verify against
 * the hardware. (A lazy-flags optimization can come later.)
 */
#ifndef PCRECOMP_CPU_H
#define PCRECOMP_CPU_H

#include <stdint.h>
#include <math.h>
#include "guest_memory.h"
#include "guest_clock.h"
#include <string.h>
#include <stdlib.h>
#if defined(_MSC_VER)
#include <intrin.h>   /* __readfsdword/__writefsdword: TIB-relative (fs:) access for SEH */
#if defined(__clang__) && defined(_M_IX86)
/* clang-cl has __readfsdword but not __writefsdword on x86: the call links as
 * an undefined ___writefsdword. MSVC has both. */
static __inline void pcrecomp_writefsdword(unsigned long off, unsigned long v)
{
    __asm__ volatile("movl %1, %%fs:(%0)" : : "r"(off), "r"(v) : "memory");
}
#define __writefsdword pcrecomp_writefsdword
#endif
#endif

/* ---- SSE register file ----
 *
 * One storage, several views - which is how the silicon works, and why this is
 * a union rather than eight float[4]. `movaps` moves a bit pattern, `pxor`
 * works on integers and `addss` works on floats, all through the same 128
 * bits; a float-only model corrupts the first two the moment a game stores a
 * mask in an XMM register, which every SSE-era game does.
 *
 * Scalar single precision is `float` here and NOT `double` like the x87 stack
 * below it. That difference is deliberate. An x87 register really is wider
 * than the values it holds, so widening it costs nothing - but SSE single
 * genuinely IS 32-bit, and computing `addss` in double rounds once where the
 * hardware rounds twice. The results differ in the low bits, and in a game
 * that feeds them to a coordinate transform the difference is visible. */
typedef union {
    float    f32[4];
    double   f64[2];
    uint32_t u32[4];
    uint64_t u64[2];
    int32_t  i32[4];
    /* The word lanes pextrw and pinsrw address. Expressing those two through
     * u32 and a shift works and reads like arithmetic; a lane array says what
     * the instruction says. */
    uint16_t u16[8];
    int16_t  i16[8];
    /* The byte lanes pslldq and psrldq shift through. Those two move the whole
     * register by a byte count, not each lane by a bit count, so they are the
     * one SSE shift that cannot be written in terms of any wider lane. */
    uint8_t  u8[16];
    int8_t   i8[16];
    int64_t  i64[2];
} XMM;


typedef struct ac1_call_state {
    uint64_t count;
    uint32_t depth, last_target;
} ac1_call_state;
typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip;
    /* flags as discrete bits (0/1) */
    uint32_t cf, zf, sf, of, pf, af, df, eflags_misc;
    uint64_t tsc;
    /* x87: register-stack of doubles. st0 == st[fpu_top]; push pre-decrements. */
    double   st[8];
    int      fpu_top;
    /* SSE: xmm0..xmm7. A 32-bit target has no xmm8-15. */
    XMM      xmm[8];
    uint32_t fpu_sw;   /* status word: only C0/C1/C2/C3 condition bits modelled */
    /* Segment registers: stored, but NOT used in address computation. The
     * memory model below is flat - a register holds a real 32-bit address - so
     * `mov ds, ax` records the selector and does nothing else. That is right
     * for code that only saves and restores them, which is the common case even
     * in segmented 32-bit code, and wrong for code that switches DS to reach
     * another segment's data. The latter needs a selector -> base mapping the
     * runtime does not have; such code should be caught rather than lifted
     * silently. */
    uint16_t cs, ds, es, fs, gs, ss;
    /* Linear base of the TIB that `fs:` points at. Used only by the portable
     * fs: path (FS_* macros below): the host points this at a simulated TIB
     * per thread, and fs:[off] reads ordinary memory at fs_base+off. Left 0 and
     * unused on the MSVC default path, which reads the real segment. */
    uint32_t fs_base;

    uint64_t mmx[8];
    uint16_t mmx_exp[8];
    uint16_t fpu_cw, fpu_tw, fpu_opcode, fpu_cs, fpu_ds;
    uint32_t fpu_ip, fpu_dp, mxcsr;
    uint8_t fp_initialized, mmx_active, x87_dirty, mmx_physical;
    ac1_call_state *calls;
} CPU;

/* Project-owned native dispatcher, called by generated code. */
void dispatch(CPU *cpu, uint32_t target);
void dispatch_jmp(CPU *cpu, uint32_t target);
void ac1_unsupported_instruction(uint32_t guest_va, const char *reason);

extern uint64_t ac1_dispatch_limit;
extern int ac1_direct_slow_mode;
void ac1_dispatch_guard_failed(CPU *c, uint32_t target);
void ac1_scheduler_return_checkpoint(CPU *c);

/* Tracing takes the original dispatcher, including its callsite-specific
 * diagnostics. Unbound test CPUs also retain that path. The runtime binds
 * calls on its first dispatch, without reading emulated TLS on later calls. */
#if defined(AC1_NO_DIRECT_CALLS)
#define AC1_CALL(c, fn, target) dispatch((c), (target))
#else
#define AC1_CALL(c, fn, target) do { \
    CPU *_ac = (c); \
    if (ac1_direct_slow_mode || !_ac->calls) { \
        dispatch(_ac, (target)); \
    } else { \
        ac1_call_state *_as = _ac->calls; \
        ++_as->count; \
        if ((ac1_dispatch_limit && _as->count > ac1_dispatch_limit) || _as->depth >= 4096u) \
            ac1_dispatch_guard_failed(_ac, (target)); \
        uint32_t _ap = _as->last_target; \
        _as->last_target = (target); \
        ++_as->depth; \
        fn(_ac); \
        --_as->depth; \
        _as->last_target = _ap; \
        ac1_scheduler_return_checkpoint(_ac); \
    } \
} while (0)
#endif

static inline void ac1_fp_init(CPU *c) {
    if (!c->fp_initialized) {
        c->fpu_cw = 0x037fu; c->fpu_tw = 0xffffu; c->mxcsr = 0x1f80u;
        c->fp_initialized = 1;
    }
}
static inline unsigned ac1_x87_tag(double v) {
    /* Every finite nonzero binary64 value is normal in the ext80 range. */
    return v == 0.0 ? 1u : !isfinite(v) ? 2u : 0u;
}
static inline int ac1_x87_check(CPU *c, uint32_t va) {
    ac1_fp_init(c);
    if (!c->mmx_active) return 1;
    ac1_unsupported_instruction(va, "x87 data operation while MMX active (missing EMMS)");
    return 0;
}
static inline void ac1_x87_set_tag(CPU *c, unsigned reg, unsigned tag) {
    c->fpu_tw = (uint16_t)((c->fpu_tw & ~(3u << (reg * 2))) | (tag << (reg * 2)));
}


/* ---- partial register access (preserve unaffected bits, like x86) ---- */
#define R8L(r)        ((uint8_t)(r))
#define R8H(r)        ((uint8_t)((r) >> 8))
#define R16(r)        ((uint16_t)(r))
#define SET8L(r, v)   ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SET8H(r, v)   ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))
#define SET16(r, v)   ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))

/* Guest memory accessors are force-inlined: in very large generated
 * functions GCC otherwise emits calls (and variable-size memcpy). The CMake
 * option AC1_FORCE_INLINE_ACCESSORS=OFF defines AC1_NO_FORCE_INLINE for A/B. */
#ifdef AC1_NO_FORCE_INLINE
#define AC1_ACCESSOR_INLINE static inline
#else
#define AC1_ACCESSOR_INLINE static inline __attribute__((always_inline))
#endif
/* AC1 direct memory: one process-wide 4 GiB arena maps guest VA a at
 * ac1_direct_arena + a, so lifted fixed-width accesses become plain loads and
 * stores. A plain global (not _Thread_local, which MinGW lowers to emutls
 * calls) is safe because every AC1 guest thread shares one address space.
 * Unmapped arena pages fault instead of producing the checked diagnostic.
 * Without an arena the parenthesized calls reach the checked implementation. */
#ifdef AC1_DIRECT_MEMORY
#include <string.h>
extern uint8_t *ac1_direct_arena;
AC1_ACCESSOR_INLINE void ac1_direct_read(uint32_t a, void *o, size_t w)
{
    if (__builtin_expect(ac1_direct_arena != NULL, 1)) memcpy(o, ac1_direct_arena + a, w);
    else (guest_execution_read)(a, o, w);
}
AC1_ACCESSOR_INLINE void ac1_direct_write(uint32_t a, const void *i, size_t w)
{
    if (__builtin_expect(ac1_direct_arena != NULL, 1)) memcpy(ac1_direct_arena + a, i, w);
    else (guest_execution_write)(a, i, w);
}
#define guest_execution_read(a, o, w) ac1_direct_read((a), (o), (w))
#define guest_execution_write(a, i, w) ac1_direct_write((a), (i), (w))
/* LOCK-prefixed read-modify-writes (31k sites) operate in place on the arena
 * with sequentially consistent builtins, matching x86 LOCK's full fence.
 * Unaligned operands keep the checked path and its diagnostic. */
AC1_ACCESSOR_INLINE uint32_t *ac1_direct_atomic_cell(uint32_t a)
{
    return (ac1_direct_arena && !(a & 3u)) ? (uint32_t *)(ac1_direct_arena + a) : NULL;
}
AC1_ACCESSOR_INLINE uint32_t ac1_direct_xadd32(uint32_t a, uint32_t v)
{
    uint32_t *cell = ac1_direct_atomic_cell(a);
    return cell ? __atomic_fetch_add(cell, v, __ATOMIC_SEQ_CST) : (guest_atomic_xadd32)(a, v);
}
AC1_ACCESSOR_INLINE uint32_t ac1_direct_cmpxchg32(uint32_t a, uint32_t compare, uint32_t value)
{
    uint32_t *cell = ac1_direct_atomic_cell(a);
    if (!cell) return (guest_atomic_cmpxchg32)(a, compare, value);
    __atomic_compare_exchange_n(cell, &compare, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return compare;  /* the previous value, as CMPXCHG leaves it in EAX */
}
AC1_ACCESSOR_INLINE uint32_t ac1_direct_xchg32(uint32_t a, uint32_t v)
{
    uint32_t *cell = ac1_direct_atomic_cell(a);
    return cell ? __atomic_exchange_n(cell, v, __ATOMIC_SEQ_CST) : (guest_atomic_xchg32)(a, v);
}
#define guest_atomic_xadd32(a, v) ac1_direct_xadd32((a), (v))
#define guest_atomic_cmpxchg32(a, c, v) ac1_direct_cmpxchg32((a), (c), (v))
#define guest_atomic_xchg32(a, v) ac1_direct_xchg32((a), (v))
#endif

/* ---- memory access: checked uint32 guest VA to native pointer ---- */
AC1_ACCESSOR_INLINE uint8_t rd8(uint32_t a) { uint8_t v; guest_execution_read(a, &v, 1); return v; }
AC1_ACCESSOR_INLINE uint16_t rd16(uint32_t a) { uint16_t v; guest_execution_read(a, &v, 2); return v; }
AC1_ACCESSOR_INLINE uint32_t rd32(uint32_t a) { uint32_t v; guest_execution_read(a, &v, 4); return v; }
AC1_ACCESSOR_INLINE void wr8(uint32_t a, uint8_t v) { guest_execution_write(a, &v, 1); }
AC1_ACCESSOR_INLINE void wr16(uint32_t a, uint16_t v) { guest_execution_write(a, &v, 2); }
AC1_ACCESSOR_INLINE void wr32(uint32_t a, uint32_t v) { guest_execution_write(a, &v, 4); }

/* ---- fs:-relative access (TIB / SEH chain) ----
 *
 * The lifter emits these for every `fs:` memory operand (SEH prologues store
 * the exception-registration record through fs:[0]). Two implementations, one
 * chosen at compile time so a single lift builds for either host:
 *
 *   MSVC default        the real segment, via the __readfsN/__writefsN
 *                       intrinsics. Correct when the host is a 1:1 Win32
 *                       process whose own TIB the guest shares.
 *   portable / non-MSVC read ordinary memory at c->fs_base + off. The host
 *                       gives each guest thread a simulated TIB and points
 *                       c->fs_base at it. This is what a non-Windows (Linux)
 *                       host needs; it also works on Windows if the host sets
 *                       c->fs_base to NtCurrentTeb() and defines
 *                       RECOMP_FS_SIMULATED.
 */
#if defined(_MSC_VER) && !defined(RECOMP_FS_SIMULATED)
#define FS_RD8(c, off)      __readfsbyte(off)
#define FS_RD16(c, off)     __readfsword(off)
#define FS_RD32(c, off)     __readfsdword(off)
#define FS_WR8(c, off, v)   __writefsbyte((off), (unsigned char)(v))
#define FS_WR16(c, off, v)  __writefsword((off), (unsigned short)(v))
#define FS_WR32(c, off, v)  __writefsdword((off), (unsigned long)(v))
#else
#define FS_RD8(c, off)      rd8 ((c)->fs_base + (off))
#define FS_RD16(c, off)     rd16((c)->fs_base + (off))
#define FS_RD32(c, off)     rd32((c)->fs_base + (off))
#define FS_WR8(c, off, v)   wr8 ((c)->fs_base + (off), (uint8_t )(v))
#define FS_WR16(c, off, v)  wr16((c)->fs_base + (off), (uint16_t)(v))
#define FS_WR32(c, off, v)  wr32((c)->fs_base + (off), (uint32_t)(v))
#endif
/* movlps/movhps and friends move a lane as bits, not as a number: a double
 * round-trip would be exact for every value except the signalling NaNs a
 * mask-producing compare leaves behind. memcpy, because the address need
 * not be aligned. */
AC1_ACCESSOR_INLINE uint64_t rd64(uint32_t a) { uint64_t v; guest_execution_read(a, &v, 8); return v; }
AC1_ACCESSOR_INLINE void wr64(uint32_t a, uint64_t v) { guest_execution_write(a, &v, 8); }

/* ---- EFLAGS pack/unpack (modelled bits only) ---- */
/* `lock`-prefixed read-modify-write.
 *
 * A recompiled game with a worker pool needs these to be real: the guest uses
 * `lock xadd` for every reference count and every queue index, and a
 * non-atomic one is a use-after-free an hour into a session rather than a
 * crash you can find. Nothing else in this header cares about other threads;
 * these do.
 *
 * The x86 semantics are "return the old value" for xadd, and for cmpxchg
 * "return what was there, and the caller compares" - the ZF the instruction
 * sets follows from that. */
static inline uint32_t atomic_xadd32(uint32_t a, uint32_t v) { return guest_atomic_xadd32(a, v); }
static inline uint32_t atomic_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) { return guest_atomic_cmpxchg32(a, cmp, nv); }
static inline uint32_t atomic_xchg32(uint32_t a, uint32_t v) { return guest_atomic_xchg32(a, v); }

static inline uint32_t eflags_pack(CPU *c) {
    return (c->eflags_misc & ~0xCD5u) | 2u | (c->cf) | (c->pf << 2) | (c->af << 4) |
           (c->zf << 6) | (c->sf << 7) | (c->df << 10) | (c->of << 11);
}
static inline void eflags_unpack(CPU *c, uint32_t v) {
    c->cf = v & 1; c->pf = (v >> 2) & 1; c->af = (v >> 4) & 1;
    c->zf = (v >> 6) & 1; c->sf = (v >> 7) & 1; c->of = (v >> 11) & 1; c->df = (v >> 10) & 1; c->eflags_misc = (v & ~0xCD5u) | 2u;
}

/* ---- stack ----
 *
 * ESP is an address in a flat build, so the base is zero and these compile to
 * exactly what they did before. A segmented build defines STACK_BASE to its
 * SS base instead, because there ESP is an OFFSET: 16-bit code reaching its
 * arguments does `mov bp, sp` and then `[bp+6]`, and truncating a host address
 * to 16 bits produces a pointer to nothing. */
#ifndef STACK_BASE
#define STACK_BASE(c) 0u
#endif
AC1_ACCESSOR_INLINE void push32(CPU *c, uint32_t v) { c->esp -= 4; wr32(STACK_BASE(c) + c->esp, v); }
AC1_ACCESSOR_INLINE uint32_t pop32(CPU *c) { uint32_t v = rd32(STACK_BASE(c) + c->esp); c->esp += 4; return v; }

/* An instruction the lifter could not express. Plain abort() by default, so a
 * project that includes only cpu.h is unchanged; a runtime that can say
 * something useful - which guest address, which mnemonic, how it got there -
 * defines RECOMP_TODO before including this. A bare abort() in two million
 * lines becomes __fastfail in a release build, which no handler sees. */
#ifndef RECOMP_TODO
#define RECOMP_TODO(va, text) ac1_unsupported_instruction((va), (text))
#endif

/* ---- absolute image references: abs VA at preferred base -> live address ---- */
extern _Thread_local uint32_t g_image_delta;   /* live_base - PE ImageBase (0 if loaded where it wanted) */
#define GVA(abs) ((uint32_t)(abs))  /* AC1: image delta is always 0 */

/* ---- flag helpers ---- */
static inline uint32_t parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (~v) & 1;
}

/* logical ops (AND/OR/XOR/TEST): CF=OF=0 */
static inline void flags_logic32(CPU *c, uint32_t r) {
    c->cf = 0; c->of = 0; c->sf = r >> 31; c->zf = (r == 0); c->pf = parity8((uint8_t)r);
}
static inline void flags_logic8(CPU *c, uint8_t r) {
    c->cf = 0; c->of = 0; c->sf = (r >> 7) & 1; c->zf = (r == 0); c->pf = parity8(r);
}
/* cmp/sub (a - b) */
static inline uint32_t flags_sub32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->cf = (a < b);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = (((a ^ b) & (a ^ r)) >> 31) & 1;
    c->af = ((a ^ b ^ r) >> 4) & 1;
    c->pf = parity8((uint8_t)r);
    return r;
}
/* add (a + b) */
static inline uint32_t flags_add32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->cf = (r < a);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    c->af = ((a ^ b ^ r) >> 4) & 1;
    c->pf = parity8((uint8_t)r);
    return r;
}
/* inc/dec: like add/sub by 1 but CF preserved */
static inline uint32_t flags_inc32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_add32(c, a, 1); c->cf = keepcf; return r;
}
static inline uint32_t flags_dec32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_sub32(c, a, 1); c->cf = keepcf; return r;
}

/* ---- width-generic flag helpers (sz in {1,2,4}); used by generated code ---- */
static inline uint32_t mask_sz(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static inline uint32_t sign_sz(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }

static inline uint32_t flags_sub(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a - b) & m;
    c->cf = (a < b); c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = (((a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t flags_add(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a + b) & m;
    c->cf = (r < a); c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t flags_logicz(CPU *c, uint32_t r, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); r &= m;
    c->cf = 0; c->of = 0; c->sf = (r & s) != 0; c->zf = (r == 0);
    c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t flags_incs(CPU *c, uint32_t a, int sz) {
    uint32_t keep = c->cf, r = flags_add(c, a, 1, sz); c->cf = keep; return r;
}
static inline uint32_t flags_decs(CPU *c, uint32_t a, int sz) {
    uint32_t keep = c->cf, r = flags_sub(c, a, 1, sz); c->cf = keep; return r;
}
static inline uint32_t flags_adc(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), cin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a + b + cin; uint32_t r = (uint32_t)full & m;
    c->cf = (full >> (sz * 8)) & 1; c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t flags_sbb(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), bin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a - b - bin; uint32_t r = (uint32_t)full & m;
    c->cf = (full >> (sz * 8)) & 1; c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = (((a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}

static inline uint32_t op_shl(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); cnt &= 31; if (!cnt) return v & m;
    c->cf = (v >> (sz * 8 - cnt)) & 1; uint32_t r = (v << cnt) & m;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t op_shr(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (cnt - 1)) & 1; uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t op_sar(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    uint32_t ext = (v & s) ? (m << (sz * 8 - cnt)) & m : 0;
    c->cf = (v >> (cnt - 1)) & 1; uint32_t r = ((v >> cnt) | ext) & m;
    c->zf = (r == 0); c->sf = (r & s) != 0; c->pf = parity8((uint8_t)r); return r;
}

/* ---- rotates. Unlike the shifts these leave ZF/SF/PF alone, which is not an
   omission: x86 does not touch them here, and code that rotates then branches
   on the zero flag is reading the flag the PREVIOUS instruction set. ---- */
static inline uint32_t op_rol(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; cnt %= (uint32_t)w; if (!cnt) return v;
    uint32_t r = ((v << cnt) | (v >> (w - cnt))) & m;
    c->cf = r & 1;
    c->of = (((r >> (w - 1)) & 1) ^ (c->cf & 1));
    return r;
}
static inline uint32_t op_ror(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; cnt %= (uint32_t)w; if (!cnt) return v;
    uint32_t r = ((v >> cnt) | (v << (w - cnt))) & m;
    c->cf = (r >> (w - 1)) & 1;
    c->of = (((r >> (w - 1)) & 1) ^ ((r >> (w - 2)) & 1));
    return r;
}

/* ---- byte order ----
 *
 * bswap reverses the four bytes of a 32-bit register and touches no flags.
 * It is how a compiler writes htonl/ntohl inline, so it turns up in any code
 * that opens a socket, next to a `ror bx, 8` doing the 16-bit half.
 *
 * The 16-bit form is architecturally undefined - Intel documents it as such
 * and real parts leave the register zeroed - so a compiler never emits it and
 * this deliberately does not pretend to implement it. */
static inline uint32_t op_bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u)
         | ((v << 8) & 0x00FF0000u) | (v << 24);
}

/* ---- double-precision shifts: shift `d`, feeding in bits from `s` ---- */
static inline uint32_t op_shld(CPU *c, uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;      /* count > width is undefined */
    uint32_t r = ((d << cnt) | (s >> (w - cnt))) & m;
    c->cf = (d >> (w - cnt)) & 1;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r);
    return r;
}
/* BSF/BSR: the index of the lowest (or highest) set bit. The flag is the
 * whole interface - ZF says whether there WAS one - and when the source is
 * zero the destination is architecturally undefined, so it is left alone
 * rather than given a value that code might come to depend on. */
static inline uint32_t op_bsf(CPU *c, uint32_t d, uint32_t s, int sz) {
    uint32_t i, n = (uint32_t)sz * 8;
    c->zf = (s == 0);
    if (s == 0) return d;
    for (i = 0; i < n; i++) if (s & (1u << i)) return i;
    return d;
}

static inline uint32_t op_bsr(CPU *c, uint32_t d, uint32_t s, int sz) {
    uint32_t i, n = (uint32_t)sz * 8;
    c->zf = (s == 0);
    if (s == 0) return d;
    for (i = n; i-- > 0; ) if (s & (1u << i)) return i;
    return d;
}

/* BT/BTS/BTR/BTC: the selected bit into CF, then the register form's own
 * change to it. The count is taken modulo the operand width - which is what
 * makes `bt eax, 33` test bit 1 and not read past the register. Only the
 * register form is expressed here; the memory form of these addresses a bit
 * string that can run past the operand, and no lifted site uses it. */
static inline uint32_t op_bittest(CPU *c, uint32_t d, uint32_t bit, int sz, int op) {
    uint32_t mask = 1u << (bit & ((uint32_t)sz * 8 - 1));
    c->cf = (d & mask) ? 1 : 0;
    switch (op) {
        case 1: return d | mask;      /* bts */
        case 2: return d & ~mask;     /* btr */
        case 3: return d ^ mask;      /* btc */
        default: return d;            /* bt  */
    }
}

/* RCL/RCR rotate through CF, so the rotated quantity is one bit wider than
 * the operand and the count is taken modulo that wider width. Writing this as
 * a bit at a time is slower than the closed form and is the version that is
 * obviously right, which for an instruction this rare is the better trade. */
static inline uint32_t op_rcl(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8, i;
    uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    cnt %= (n + 1);
    if (cnt == 0) return d;        /* a zero count changes no flag at all */
    for (i = 0; i < cnt; i++) {
        uint32_t top = (d >> (n - 1)) & 1u;
        d = ((d << 1) | c->cf) & mask;
        c->cf = top;
    }
    /* OF is defined only for a count of one; hardware leaves something there
     * for the rest and nothing may read it. */
    if (cnt == 1) c->of = (uint32_t)(((d >> (n - 1)) & 1u) ^ c->cf);
    return d;
}

static inline uint32_t op_rcr(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8, i;
    uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    cnt %= (n + 1);
    if (cnt == 0) return d;
    /* RCR sets OF from the two top bits of the RESULT, and before the rotate
     * rather than after - but only the count-of-one case is defined, and for
     * that one the two are the same thing. */
    if (cnt == 1) c->of = (uint32_t)((((d >> 1) | (c->cf << (n - 1))) >> (n - 1)) & 1u)
                        ^ (uint32_t)((((d >> 1) | (c->cf << (n - 1))) >> (n - 2)) & 1u);
    for (i = 0; i < cnt; i++) {
        uint32_t bot = d & 1u;
        d = ((d >> 1) | (c->cf << (n - 1))) & mask;
        c->cf = bot;
    }
    return d;
}

static inline uint32_t op_shrd(CPU *c, uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;
    uint32_t r = ((d >> cnt) | (s << (w - cnt))) & m;
    c->cf = (d >> (cnt - 1)) & 1;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r);
    return r;
}

/* ---- shifts (set flags like x86; count masked to 5 bits) ---- */
static inline uint32_t shr32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (cnt - 1)) & 1;
    uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = r >> 31; c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t shl32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (32 - cnt)) & 1;
    uint32_t r = v << cnt;
    c->zf = (r == 0); c->sf = r >> 31; c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint8_t shr8(CPU *c, uint8_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    if (cnt <= 8) c->cf = (v >> (cnt - 1)) & 1;
    uint8_t r = (cnt < 8) ? (uint8_t)(v >> cnt) : 0;
    c->zf = (r == 0); c->sf = (r >> 7) & 1; c->pf = parity8(r);
    return r;
}

/* Constant-mask flag lanes; generated by prepare_runtime.py. */
AC1_ACCESSOR_INLINE uint32_t flags_add_m(CPU *c, uint32_t a, uint32_t b, int sz, unsigned lanes) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a + b) & m;
    if (lanes & 1u) { c->cf = (r < a); } if (lanes & 8u) { c->zf = (r == 0); } if (lanes & 16u) { c->sf = (r & s) != 0; }
    if (lanes & 32u) { c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; } if (lanes & 4u) { c->af = ((a ^ b ^ r) & 0x10) != 0; }
    if (lanes & 2u) { c->pf = parity8((uint8_t)r); } return r;
}
AC1_ACCESSOR_INLINE uint32_t flags_sub_m(CPU *c, uint32_t a, uint32_t b, int sz, unsigned lanes) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a - b) & m;
    if (lanes & 1u) { c->cf = (a < b); } if (lanes & 8u) { c->zf = (r == 0); } if (lanes & 16u) { c->sf = (r & s) != 0; }
    if (lanes & 32u) { c->of = (((a ^ b) & (a ^ r)) & s) != 0; } if (lanes & 4u) { c->af = ((a ^ b ^ r) & 0x10) != 0; }
    if (lanes & 2u) { c->pf = parity8((uint8_t)r); } return r;
}
AC1_ACCESSOR_INLINE uint32_t flags_logicz_m(CPU *c, uint32_t r, int sz, unsigned lanes) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); r &= m;
    if (lanes & 1u) { c->cf = 0; } if (lanes & 32u) { c->of = 0; } if (lanes & 16u) { c->sf = (r & s) != 0; } if (lanes & 8u) { c->zf = (r == 0); }
    if (lanes & 2u) { c->pf = parity8((uint8_t)r); } return r;
}
AC1_ACCESSOR_INLINE uint32_t flags_adc_m(CPU *c, uint32_t a, uint32_t b, int sz, unsigned lanes) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), cin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a + b + cin; uint32_t r = (uint32_t)full & m;
    if (lanes & 1u) { c->cf = (full >> (sz * 8)) & 1; } if (lanes & 8u) { c->zf = (r == 0); } if (lanes & 16u) { c->sf = (r & s) != 0; }
    if (lanes & 32u) { c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; } if (lanes & 4u) { c->af = ((a ^ b ^ r) & 0x10) != 0; }
    if (lanes & 2u) { c->pf = parity8((uint8_t)r); } return r;
}
AC1_ACCESSOR_INLINE uint32_t flags_sbb_m(CPU *c, uint32_t a, uint32_t b, int sz, unsigned lanes) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), bin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a - b - bin; uint32_t r = (uint32_t)full & m;
    if (lanes & 1u) { c->cf = (full >> (sz * 8)) & 1; } if (lanes & 8u) { c->zf = (r == 0); } if (lanes & 16u) { c->sf = (r & s) != 0; }
    if (lanes & 32u) { c->of = (((a ^ b) & (a ^ r)) & s) != 0; } if (lanes & 4u) { c->af = ((a ^ b ^ r) & 0x10) != 0; }
    if (lanes & 2u) { c->pf = parity8((uint8_t)r); } return r;
}
AC1_ACCESSOR_INLINE uint32_t flags_incs_m(CPU *c, uint32_t a, int sz, unsigned lanes) {
    return flags_add_m(c, a, 1, sz, lanes & ~1u);
}
AC1_ACCESSOR_INLINE uint32_t flags_decs_m(CPU *c, uint32_t a, int sz, unsigned lanes) {
    return flags_sub_m(c, a, 1, sz, lanes & ~1u);
}

/* ---- x87 FPU ---- */
#include <math.h>
static inline void fpush(CPU *c, double v) { if(!ac1_x87_check(c,c->eip)) return; c->fpu_top=(c->fpu_top-1)&7; c->st[c->fpu_top]=v; c->x87_dirty |= (uint8_t)(1u<<c->fpu_top); ac1_x87_set_tag(c,(unsigned)c->fpu_top,ac1_x87_tag(v)); }
static inline double fpop(CPU *c) { if(!ac1_x87_check(c,c->eip)) return 0.0; double v=c->st[c->fpu_top]; ac1_x87_set_tag(c,(unsigned)c->fpu_top,3); c->fpu_top=(c->fpu_top+1)&7; return v; }
static inline double *fst(CPU *c, int i) { ac1_fp_init(c); unsigned r=(unsigned)(c->fpu_top+i)&7u; c->x87_dirty |= (uint8_t)(1u<<r); return &c->st[r]; }

/* float/double/int memory operands (memcpy avoids alignment/aliasing issues) */
static inline double rdf32(uint32_t a) { float  f; guest_execution_read(a, &f, 4); return (double)f; }
static inline double rdf64(uint32_t a) { double d; guest_execution_read(a, &d, 8); return d; }
static inline double rdi16(uint32_t a) { int16_t i; guest_execution_read(a, &i, 2); return (double)i; }
static inline double rdi32(uint32_t a) { int32_t i; guest_execution_read(a, &i, 4); return (double)i; }
static inline double rdi64(uint32_t a) { int64_t i; guest_execution_read(a, &i, 8); return (double)i; }
static inline void   wrf32(uint32_t a, double v) { float  f = (float)v; guest_execution_write(a, &f, 4); }
static inline void   wrf64(uint32_t a, double v) { guest_execution_write(a, &v, 8); }
static inline void   wri32(uint32_t a, double v) { int32_t i = (int32_t)nearbyint(v); guest_execution_write(a, &i, 4); }
static inline void   wri16(uint32_t a, double v) { int16_t i = (int16_t)nearbyint(v); guest_execution_write(a, &i, 2); }
static inline void   wri64(uint32_t a, double v) { int64_t i = (int64_t)nearbyint(v); guest_execution_write(a, &i, 8); }


/* fcom/fcomp: set C3/C2/C0 (st0 vs v). Cleared C1. */
static inline void fcompare(CPU *c, double a, double b) {
    uint32_t sw = c->fpu_sw & ~0x4700u;
    if      (a > b)  { /* 000 */ }
    else if (a < b)  sw |= 0x0100u;          /* C0 */
    else if (a == b) sw |= 0x4000u;          /* C3 */
    else             sw |= 0x4700u;          /* unordered: C3|C2|C0 */
    c->fpu_sw = sw;
}
/* fcomi/fucomi: the same comparison, into EFLAGS instead of the status word.
 * The P6 forms exist so a compiler can branch on a float compare without
 * going through `fnstsw ax; test ah`. Note the mapping is not the integer
 * one: "unordered" is ZF=PF=CF=1, which is why `jp` after an fcomi is the
 * NaN test, and OF/SF/AF are cleared. */
static inline void fcompare_eflags(CPU *c, double a, double b) {
    c->of = c->sf = c->af = 0;
    if      (a > b)  { c->zf = 0; c->pf = 0; c->cf = 0; }
    else if (a < b)  { c->zf = 0; c->pf = 0; c->cf = 1; }
    else if (a == b) { c->zf = 1; c->pf = 0; c->cf = 0; }
    else             { c->zf = 1; c->pf = 1; c->cf = 1; }
}
/* sahf: load AH into CF,PF,AF,ZF,SF */
static inline void do_sahf(CPU *c, uint8_t ah) {
    c->cf = ah & 1; c->pf = (ah >> 2) & 1; c->af = (ah >> 4) & 1;
    c->zf = (ah >> 6) & 1; c->sf = (ah >> 7) & 1;
}

/* ---- SSE ----
 *
 * Enough of SSE/SSE2 for a Pentium 4-era game: scalar float and double, the
 * packed moves and bitwise ops, and the compares. Packed arithmetic beyond
 * that is not here yet - it lifts to a TODO rather than silently wrong code.
 */

/* Memory operands. Separate from the x87 rdf32/rdf64 above because those
 * widen to double for the 80-bit stack, and these must not. memcpy because an
 * SSE memory operand is frequently unaligned (movups exists for a reason). */
static inline float  rdss(uint32_t a) { float  f; guest_execution_read(a, &f, 4); return f; }
static inline double rdsd(uint32_t a) { double d; guest_execution_read(a, &d, 8); return d; }
static inline XMM    rdxm(uint32_t a) { XMM x;    guest_execution_read(a, &x, 16); return x; }
static inline void   wrss(uint32_t a, float  v) { guest_execution_write(a, &v, 4); }
static inline void   wrsd(uint32_t a, double v) { guest_execution_write(a, &v, 8); }
static inline void   wrxm(uint32_t a, XMM    v) { guest_execution_write(a, &v, 16); }

/* MOVSS/MOVSD loading from memory zero the rest of the register; the
 * register-to-register forms leave the high lanes alone. Getting that
 * backwards is invisible until something reads the high lanes, which is why
 * it is a named helper and not an open-coded assignment. */
static inline void sse_load_ss(XMM *d, float  v) { d->u64[0] = 0; d->u64[1] = 0; d->f32[0] = v; }
static inline void sse_load_sd(XMM *d, double v) { d->u64[0] = 0; d->u64[1] = 0; d->f64[0] = v; }

/* UCOMISS/COMISS/UCOMISD/COMISD: the comparison lands in ZF/PF/CF, and clears
 * OF/SF/AF. An unordered compare - either operand NaN - sets all three, which
 * is exactly what makes `ucomiss; jp` a NaN test and `ucomiss; jbe` mean
 * "below or equal, or unordered". Note the flags are the UNSIGNED set: the
 * compiler emits `ja`/`jbe` after a float compare, never `jg`/`jle`. */
static inline void sse_compare(CPU *c, double a, double b) {
    c->of = 0; c->sf = 0; c->af = 0;
    if (a != a || b != b) { c->zf = 1; c->pf = 1; c->cf = 1; }   /* unordered */
    else                  { c->zf = (a == b); c->pf = 0; c->cf = (a < b); }
}

/* MINSS/MAXSS are not fmin/fmax. The manual defines them as
 *     if (dst OP src) then dst else src
 * and that "else src" is load-bearing in two cases fmin gets the other way
 * round: when the operands are equal - which includes +0.0 against -0.0, so
 * min(+0,-0) is -0 but min(-0,+0) is +0 - and when either is NaN, where the
 * comparison is false and the SECOND operand wins. Written in that exact
 * shape, C's own "any compare with NaN is false" rule delivers both for free.
 * Writing it the other way round, `(src < dst) ? src : dst`, looks identical
 * and is wrong on both. */
static inline float  sse_minf(float  d, float  s) { return (d < s) ? d : s; }
static inline float  sse_maxf(float  d, float  s) { return (d > s) ? d : s; }
static inline double sse_mind(double d, double s) { return (d < s) ? d : s; }
static inline double sse_maxd(double d, double s) { return (d > s) ? d : s; }

/* CVTTSS2SI and friends yield the "integer indefinite" value when the source
 * does not fit in 32 bits, where the equivalent C cast is undefined behaviour
 * and may trap. One comparison buys a defined answer that matches hardware. */
static inline int32_t sse_cvtt_i32(double v) {
    return (v >= -2147483648.0 && v < 2147483648.0) ? (int32_t)v : (int32_t)0x80000000;
}

/* ---- BT / BTS / BTR / BTC ----
 *
 * With a memory destination and a register bit index, the index is NOT masked
 * to the operand size: it selects a dword away from the base address, which is
 * how a bitmap of arbitrary size gets addressed. Masking it the way the
 * register form does would quietly read and write the wrong word - the kind of
 * wrong that shows up as unrelated corruption much later.
 *
 * op: 0 test, 1 set, 2 reset, 3 complement. CF receives the bit as it was. */
static inline uint32_t bit_string_op(CPU *c, uint32_t addr, int32_t idx, int op)
{
    uint32_t a = addr + 4u * (uint32_t)(idx >> 5);
    unsigned b = (unsigned)(idx & 31);
    uint32_t w = rd32(a);
    c->cf = (w >> b) & 1u;
    if      (op == 1) w |=  (1u << b);
    else if (op == 2) w &= ~(1u << b);
    else if (op == 3) w ^=  (1u << b);
    if (op) wr32(a, w);
    return c->cf;
}

/* The register form, where the index really is taken modulo the width. */
static inline uint32_t bit_reg_op(CPU *c, uint32_t *r, uint32_t idx, int op)
{
    unsigned b = idx & 31u;
    c->cf = (*r >> b) & 1u;
    if      (op == 1) *r |=  (1u << b);
    else if (op == 2) *r &= ~(1u << b);
    else if (op == 3) *r ^=  (1u << b);
    return c->cf;
}

/* ---- 80-bit extended precision ----
 *
 * The x87 stack here is modelled as doubles, which is right for almost
 * everything - but `fld tbyte` and `fstp tbyte` name the hardware's own
 * 80-bit format directly, and a compiler emits them to spill a register
 * without losing precision. Reading one as anything else returns nonsense.
 *
 * The format is a sign bit, a 15-bit exponent biased by 16383, and a 64-bit
 * mantissa whose integer bit is EXPLICIT - unlike float and double, where it
 * is implied. Converting to double loses the extra bits, which is the same
 * loss the rest of this model already accepts.
 */
static inline double rdf80(uint32_t a)
{
    unsigned char b[10];
    guest_execution_read(a, b, 10);

    uint64_t mant;
    uint16_t se;
    memcpy(&mant, b, 8);
    memcpy(&se, b + 8, 2);

    int sign = (se >> 15) & 1;
    int exp  = se & 0x7FFF;

    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) {
        if (mant << 1) return (double)NAN;
        return sign ? -(double)INFINITY : (double)INFINITY;
    }
    double m = (double)mant * 5.4210108624275222e-20;   /* 2^-64 */
    return ldexp(sign ? -m : m, exp - 16383 + 1);
}

static inline void wrf80(uint32_t a, double v)
{
    unsigned char b[10];
    uint64_t mant = 0;
    uint16_t se = 0;

    memset(b, 0, sizeof b);
    if (v != v) {                                   /* NaN */
        se = 0x7FFF; mant = 0xC000000000000000ull;
    } else if (v == (double)INFINITY || v == -(double)INFINITY) {
        se = 0x7FFF; mant = 0x8000000000000000ull;
        if (v < 0) se |= 0x8000;
    } else if (v != 0.0) {
        int e;
        double m = frexp(signbit(v) ? -v : v, &e);   /* m in [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64);
        se = (uint16_t)((e - 1 + 16383) & 0x7FFF);
        if (signbit(v)) se |= 0x8000;
    } else if (signbit(v)) {
        se = 0x8000;                                 /* -0.0: mantissa 0, sign set */
    }
    memcpy(b, &mant, 8);
    memcpy(b + 8, &se, 2);
    guest_execution_write(a, b, 10);
}

/* FPREM: the remainder of st0/st1 with the quotient truncated toward zero,
 * which is what fmod computes. The hardware may reduce only partially and set
 * C2 to say "call me again"; computing the whole thing at once means C2 is
 * always clear, and the loop the compiler wrote around it runs once. */
static inline double fprem_op(CPU *c, double a, double b)
{
    c->fpu_sw &= ~0x0400u;              /* C2 = 0: reduction complete */
    return fmod(a, b);
}
/* The packed integer ops that saturate rather than wrap. PADDSW and friends
 * clamp to the lane's own range instead of carrying into the next lane, and
 * the arithmetic has to be done a width up for the clamp to see the overflow
 * at all - which is the whole reason these are functions and not expressions
 * in the generated code. */
static inline int8_t   sse_sat_i8 (int32_t v) { return v >  127 ?  127 : v < -128 ? -128 : (int8_t)v; }
static inline int16_t  sse_sat_i16(int32_t v) { return v >  32767 ?  32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint8_t  sse_sat_u8 (int32_t v) { return v >  255 ?  255 : v < 0 ? 0 : (uint8_t)v; }
static inline uint16_t sse_sat_u16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }


/* MIT recomp-kit CPU profile; see LICENSE.recomp-kit. */
static inline void guest_cpuid(CPU *c) {
    switch (c->eax) {
    case 0:
        c->eax = 1;
        memcpy(&c->ebx, "Genu", 4);
        memcpy(&c->edx, "ineI", 4);
        memcpy(&c->ecx, "ntel", 4);
        break;
    case 1:
        c->eax = 0x00000633; // family 6, model 3, stepping 3
        c->ebx = 0;
        c->ecx = 0;
        c->edx = 0x00008011; // FPU | TSC | CMOV, nothing else
        break;
    default:
        c->eax = c->ebx = c->ecx = c->edx = 0;
        break;
    }
}
static inline void guest_rdtsc(CPU *c) {
    uint64_t from_ms = (guest_monotonic_ns() / 1000000ull) * 1000000ull;
    if (from_ms > c->tsc)
        c->tsc = from_ms;
    c->tsc += 1000;
    c->eax = (uint32_t)c->tsc;
    c->edx = (uint32_t)(c->tsc >> 32);
}

/* Portable MMX integer lanes. Unsigned extraction/assembly avoids signed
 * overflow, signed left shifts and host vector/rounding-mode dependencies. */
static inline uint64_t ac1_mm_mask(unsigned bits) { return UINT64_MAX >> (64u - bits); }
static inline int64_t ac1_mm_signed(uint64_t v, unsigned bits) {
    uint64_t mask = ac1_mm_mask(bits); v &= mask;
    return v & (UINT64_C(1) << (bits-1)) ? -1 - (int64_t)(mask-v) : (int64_t)v;
}
static inline uint64_t ac1_mmx_lanes(uint64_t a, uint64_t b, unsigned bits, unsigned op) {
    uint64_t out = 0, mask = ac1_mm_mask(bits);
    for (unsigned i = 0; i < 64u/bits; ++i) {
        uint64_t u = (a >> (i*bits)) & mask, v = (b >> (i*bits)) & mask, r = 0;
        int64_t x = ac1_mm_signed(u,bits), y = ac1_mm_signed(v,bits), s = 0;
        switch (op) {
        case 0: r=u+v; break; case 1: r=u-v; break;
        case 2: case 3: {
            int64_t hi = (INT64_C(1) << (bits-1))-1, lo = -hi-1;
            s = op==2 ? x+y : x-y; if (s>hi) s=hi; if (s<lo) s=lo; r=(uint64_t)s; break;
        }
        case 4: r=u+v; if (r>mask) r=mask; break;
        case 5: r=u<v ? 0 : u-v; break;
        case 6: r=u==v ? mask:0; break; case 7: r=x>y ? mask:0; break;
        case 8: r=(u+v+1)>>1; break;
        case 9: r=u<v?u:v; break; case 10:r=u>v?u:v; break;
        case 11:r=(uint64_t)(x<y?x:y); break; case 12:r=(uint64_t)(x>y?x:y); break;
        case 13:r=u*v; break; case 14:r=((uint64_t)(x*y))>>16; break;
        case 15:r=(u*v)>>16; break;
        }
        out |= (r & mask) << (i*bits);
    }
    return out;
}
static inline uint64_t ac1_mmx_unpack(uint64_t a, uint64_t b, unsigned bits, unsigned high) {
    uint64_t out=0, mask=ac1_mm_mask(bits); unsigned half=32u/bits;
    for (unsigned i=0;i<half;++i) {
        unsigned from=(i+high*half)*bits;
        out |= ((a>>from)&mask) << (2*i*bits);
        out |= ((b>>from)&mask) << ((2*i+1)*bits);
    }
    return out;
}
static inline uint64_t ac1_mmx_pack(uint64_t a, uint64_t b, unsigned bits, unsigned unsign) {
    uint64_t out=0; unsigned n=64u/bits, width=bits/2;
    int64_t hi=unsign ? (INT64_C(1)<<width)-1 : (INT64_C(1)<<(width-1))-1;
    int64_t lo=unsign ? 0 : -hi-1;
    for (unsigned i=0;i<2*n;++i) {
        int64_t v=ac1_mm_signed((i<n?a:b)>>((i%n)*bits),bits);
        if (v>hi) v=hi;
        if (v<lo) v=lo;
        out |= ((uint64_t)v & ac1_mm_mask(width)) << (i*width);
    }
    return out;
}
static inline uint64_t ac1_mmx_shift(uint64_t a, uint64_t count, unsigned bits, unsigned kind) {
    uint64_t out=0, mask=ac1_mm_mask(bits);
    for (unsigned i=0;i<64u/bits;++i) {
        uint64_t v=(a>>(i*bits))&mask, r=0;
        unsigned negative=(unsigned)(v>>(bits-1));
        if (count>=bits) r=kind==2 && negative ? mask:0;
        else if (kind==0) r=(v << (unsigned)count)&mask;
        else {
            r=v >> (unsigned)count;
            if (kind==2 && negative) r |= mask ^ (mask >> (unsigned)count);
        }
        out |= r << (i*bits);
    }
    return out;
}
static inline uint64_t ac1_mmx_madd(uint64_t a, uint64_t b) {
    uint64_t out=0;
    for(unsigned i=0;i<2;++i) {
        int64_t sum=ac1_mm_signed(a>>(i*32),16)*ac1_mm_signed(b>>(i*32),16) +
                    ac1_mm_signed(a>>(i*32+16),16)*ac1_mm_signed(b>>(i*32+16),16);
        out |= (uint64_t)(uint32_t)sum << (i*32);
    }
    return out;
}
static inline uint64_t ac1_mmx_shuffle(uint64_t a, unsigned imm) {
    uint64_t out=0;
    for(unsigned i=0;i<4;++i) out |= ((a>>(16*((imm>>(i*2))&3)))&0xffffu) << (i*16);
    return out;
}
static inline uint64_t ac1_mmx_sad(uint64_t a, uint64_t b) {
    unsigned sum=0;
    for(unsigned i=0;i<8;++i) {
        int x=(int)((a>>(i*8))&255u), y=(int)((b>>(i*8))&255u);
        sum+=(unsigned)(x>y?x-y:y-x);
    }
    return sum;
}
static inline uint32_t ac1_mmx_mask(uint64_t a) {
    uint32_t out=0; for(unsigned i=0;i<8;++i) out|=(uint32_t)((a>>(i*8+7))&1u)<<i;
    return out;
}
static inline uint64_t ac1_fp_mantissa(double v) {
    if (isnan(v)) return UINT64_C(0xc000000000000000);
    if (isinf(v)) return UINT64_C(0x8000000000000000);
    if (v==0.0) return 0;
    int exponent;
    return (uint64_t)ldexp(frexp(fabs(v), &exponent),64);
}
static inline void ac1_mmx_enter(CPU *c) {
    ac1_fp_init(c);
    if (!c->mmx_active) {
        for(unsigned i=0;i<8;++i) if(c->x87_dirty & (1u<<i)) {
            double v=c->st[i]; int exponent=0;
            c->mmx[i]=ac1_fp_mantissa(v);
            if(isfinite(v) && v!=0.0) (void)frexp(fabs(v),&exponent);
            c->mmx_exp[i]=(uint16_t)((signbit(v)?0x8000u:0u) |
                (!isfinite(v)?0x7fffu:v==0.0?0u:(unsigned)(exponent-1+16383)));
            c->mmx_physical |= (uint8_t)(1u<<i);
        }
        c->x87_dirty=0;
    }
    c->fpu_top=0; c->fpu_tw=0; c->mmx_active=1;
}
static inline uint64_t ac1_mmx_read(CPU *c, unsigned reg) { ac1_mmx_enter(c); return c->mmx[reg]; }
static inline void ac1_mmx_write(CPU *c, unsigned reg, uint64_t value) {
    ac1_mmx_enter(c); c->mmx[reg]=value; c->mmx_exp[reg]=0xffffu;
    c->mmx_physical |= (uint8_t)(1u<<reg);
}
static inline void ac1_mmx_emms(CPU *c) { ac1_fp_init(c); c->fpu_tw=0xffffu; c->mmx_active=0; }
static inline double ac1_fp_round(double x, unsigned rc) {
    if (!isfinite(x) || x==0.0) return x;
    if(rc==1) return floor(x);
    if(rc==2) return ceil(x);
    if(rc==3) return trunc(x);
    double lo=floor(x), f=x-lo;
    double rounded=f>0.5 || (f==0.5 && fmod(lo,2.0)!=0.0) ? lo+1.0 : lo;
    return rounded==0.0 ? copysign(0.0,x) : rounded;
}
static inline void ac1_simd_exception(CPU *c, unsigned flag, uint32_t va) {
    c->mxcsr |= flag;
    if (!(c->mxcsr & (flag<<7))) ac1_unsupported_instruction(va,"unmasked SIMD exception in MMX conversion");
}
static inline uint64_t ac1_mmx_from_ps(CPU *c, uint64_t source, unsigned truncate_mode, uint32_t va) {
    uint64_t out=0; ac1_fp_init(c);
    for(unsigned i=0;i<2;++i) {
        uint32_t bits=(uint32_t)(source>>(i*32)), result; float f;
        if ((c->mxcsr & 0x40u) && !(bits&0x7f800000u)) bits &= 0x80000000u;
        memcpy(&f,&bits,4);
        double r=ac1_fp_round((double)f,truncate_mode?3u:(c->mxcsr>>13)&3u);
        if (!isfinite(r) || r < -2147483648.0 || r >= 2147483648.0) {
            result=0x80000000u; ac1_simd_exception(c,1u,va);
        } else {
            result=(uint32_t)(int32_t)r;
            if(r!=(double)f) ac1_simd_exception(c,0x20u,va);
        }
        out|=(uint64_t)result<<(i*32);
    }
    return out;
}
static inline uint32_t ac1_mmx_to_ps_lane(CPU *c, uint32_t bits, uint32_t va) {
    unsigned sign=bits>>31, exponent=0;
    uint32_t magnitude=sign?0u-bits:bits;
    if(!magnitude) return 0;
    for(uint32_t n=magnitude;n>1;n>>=1) ++exponent;
    uint32_t mantissa;
    if(exponent<=23) mantissa=magnitude<<(23-exponent);
    else {
        unsigned shift=exponent-23, rc=(c->mxcsr>>13)&3u;
        uint32_t lost=magnitude&((1u<<shift)-1), half=1u<<(shift-1);
        mantissa=magnitude>>shift;
        if((rc==0 && (lost>half || (lost==half && (mantissa&1)))) ||
           (lost && ((rc==1 && sign) || (rc==2 && !sign)))) ++mantissa;
        if(mantissa==0x1000000u) { mantissa>>=1; ++exponent; }
        if(lost) ac1_simd_exception(c,0x20u,va);
    }
    return (sign<<31)|((exponent+127)<<23)|(mantissa&0x7fffffu);
}
static inline uint64_t ac1_mmx_to_ps(CPU *c, uint64_t source, uint32_t va) {
    ac1_fp_init(c);
    uint32_t lo=ac1_mmx_to_ps_lane(c,(uint32_t)source,va);
    uint32_t hi=ac1_mmx_to_ps_lane(c,(uint32_t)(source>>32),va);
    return (uint64_t)lo|((uint64_t)hi<<32);
}
static inline uint16_t ac1_x87_status(CPU *c) {
    return (uint16_t)((c->fpu_sw&~0x3800u)|((unsigned)c->fpu_top<<11));
}
static inline void ac1_x87_reset(CPU *c) {
    ac1_fp_init(c); c->fpu_cw=0x037fu; c->fpu_sw=0; c->fpu_tw=0xffffu;
    c->fpu_top=0; c->mmx_active=0;
    c->fpu_ip=c->fpu_dp=0; c->fpu_cs=c->fpu_ds=c->fpu_opcode=0;
}
static inline uint16_t ac1_x87_tags(CPU *c) {
    uint16_t tags=0;
    for(unsigned i=0;i<8;++i) {
        unsigned t=(c->fpu_tw>>(i*2))&3u;
        if(t!=3) {
            if((c->mmx_physical & (1u<<i)) && !(c->x87_dirty & (1u<<i))) {
                unsigned exp=c->mmx_exp[i]&0x7fffu; uint64_t value=c->mmx[i];
                t=!exp && !value ? 1u : exp==0x7fffu || !exp || !(value>>63) ? 2u : 0u;
            } else t=ac1_x87_tag(c->st[i]);
        }
        tags |= (uint16_t)(t<<(i*2));
    }
    return tags;
}
static inline void ac1_x87_store_env(CPU *c, uint32_t a) {
    ac1_fp_init(c);
    wr32(a,0xffff0000u|c->fpu_cw); wr32(a+4,0xffff0000u|ac1_x87_status(c));
    wr32(a+8,0xffff0000u|ac1_x87_tags(c)); wr32(a+12,c->fpu_ip);
    wr32(a+16,(uint32_t)c->fpu_cs|((uint32_t)(c->fpu_opcode&0x7ffu)<<16));
    wr32(a+20,c->fpu_dp); wr32(a+24,0xffff0000u|c->fpu_ds);
    c->fpu_cw |= 0x3fu; /* FNSTENV masks exceptions after storing the old CW. */
}
static inline void ac1_x87_load_env(CPU *c, uint32_t a) {
    ac1_fp_init(c);
    c->fpu_cw=rd16(a); c->fpu_sw=rd16(a+4); c->fpu_tw=rd16(a+8);
    c->fpu_top=(int)((c->fpu_sw>>11)&7u);
    c->fpu_ip=rd32(a+12); c->fpu_cs=rd16(a+16); c->fpu_opcode=rd16(a+18)&0x7ffu;
    c->fpu_dp=rd32(a+20); c->fpu_ds=rd16(a+24);
    if(c->fpu_tw==0xffffu) c->mmx_active=0;
}
static inline void ac1_x87_save(CPU *c, uint32_t a) {
    ac1_x87_store_env(c,a);
    for(unsigned i=0;i<8;++i) {
        unsigned reg=((unsigned)c->fpu_top+i)&7u;
        if((c->mmx_physical & (1u<<reg)) && !(c->x87_dirty & (1u<<reg))) {
            wr64(a+28+i*10,c->mmx[reg]); wr16(a+36+i*10,c->mmx_exp[reg]);
        }
        else wrf80(a+28+i*10,c->st[reg]);
    }
    ac1_x87_reset(c);
}
static inline void ac1_x87_restore(CPU *c, uint32_t a) {
    ac1_x87_load_env(c,a);
    unsigned mmx_image=c->fpu_top==0, mmx_payload=0;
    for(unsigned i=0;i<8;++i) {
        unsigned reg=((unsigned)c->fpu_top+i)&7u;
        c->mmx[reg]=rd64(a+28+i*10); c->st[reg]=rdf80(a+28+i*10);
        c->mmx_exp[reg]=rd16(a+36+i*10);
        if(((c->fpu_tw>>(reg*2))&3u)==3u) mmx_image=0;
        if(c->mmx_exp[reg]==0xffffu) mmx_payload=1;
    }
    c->mmx_active=(uint8_t)(mmx_image && mmx_payload); c->x87_dirty=0; c->mmx_physical=0xffu;
}

/* Independent A/B gate: keep the integer-conversion algorithm unchanged. */
#if defined(AC1_FORCE_INLINE_X87)
#if defined(__GNUC__) || defined(__clang__)
#define AC1_X87_INTEGER_INLINE static inline __attribute__((always_inline))
#else
#error AC1_FORCE_INLINE_X87 requires always_inline compiler support
#endif
#else
#define AC1_X87_INTEGER_INLINE static inline
#endif

AC1_X87_INTEGER_INLINE uint64_t ac1_x87_integer(CPU *c, double value, unsigned bits, unsigned trunc_mode, uint32_t va) {
    double result=ac1_fp_round(value,trunc_mode?3u:(c->fpu_cw>>10)&3u);
    double limit=ldexp(1.0,(int)bits-1);
    if(!isfinite(result) || result < -limit || result >= limit) {
        c->fpu_sw|=1u;
        if(!(c->fpu_cw&1u)) ac1_unsupported_instruction(va,"unmasked x87 integer-conversion exception");
        return UINT64_C(1)<<(bits-1);
    }
    if(result!=value) {
        c->fpu_sw|=0x20u;
        if(!(c->fpu_cw&0x20u)) ac1_unsupported_instruction(va,"unmasked x87 precision exception");
    }
    return (uint64_t)(int64_t)result;
}

#endif /* PCRECOMP_CPU_H */
