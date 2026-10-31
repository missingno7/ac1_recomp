#!/usr/bin/env python3
"""Prepare the small pinned runtime subset reused from the local Duke project."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / "config/ac1_dx9.json").read_text(encoding="utf-8"))
DUKE_ROOT = Path(CONFIG["pcrecomp_root"]).resolve().parents[1]
FINGERPRINT_HEADER = ROOT / "src/runtime/ac1_fingerprint.h"
SOURCE_FILES = {
    "src/runtime/guest_memory.c": ("src/runtime/guest_memory.c", "6beb27ea60b92faa8b197554cc650cc50b57bdc31729a11d1f3599bebc4bc22c"),
    "src/runtime/guest_memory.h": ("src/runtime/guest_memory.h", "b2074c584a8f7566a1de98a651e1cb4567920b22d95b66f130c8a655cb4104a0"),
    "src/runtime/pe32_loader.c": ("src/runtime/pe32_loader.c", "0cdd8787c31898f98f5bfffeb2b91ae6892711e5eae9a6b61f7a9d0222f69402"),
    "src/runtime/pe32_loader.h": ("src/runtime/pe32_loader.h", "612c669eaa50ceef205b5989b6db18489f59c30da22cf8a1dc9b61562b6c270e"),
    "src/runtime/guest_clock.c": ("src/runtime/guest_clock.c", "e30a6a9444fb98928fe925c130376a96b1d10776d89eedddd2c5e4439334cf51"),
    "src/runtime/guest_clock.h": ("src/runtime/guest_clock.h", "717ee58822b319d0db590fb1d550f40fd83cdea8c8b203269af82867e58a246a"),
    "src/runtime/pcrecomp_heap.c": ("src/runtime/pcrecomp_heap.c", "63054a3cfc491af7de2d69df17b628fb714df0b34bd34197091ea1e89d5b9f1b"),
    "src/runtime/pcrecomp_heap.h": ("src/runtime/pcrecomp_heap.h", "885795647e5b396a6d2010824c8266beea3b49b45d22cfa376578fad621bda00"),
    "src/runtime/pcrecomp_heap.inc": ("work/generated/pcrecomp_heap.inc", "8cac9f155a4b5b8ac7668419d54044b4cfb072ac75a80e0515208b86b54e47fc"),
    "src/runtime/LICENSE.pcrecomp.txt": (".deps/pcrecomp/LICENSE", "969c26f4e5da359fea84127dfecd3057729cfed75211fb324bdb42af6c087614"),
    "src/runtime/cpu.h": ("work/generated/cpu.h", "ded33fb92194b50c42d80f03ec9142f3517bb62400545ca57f00e903e3383dfe"),
}

# CPU copies made for host callbacks retain this pointer: dispatch accounting
# belongs to the guest thread, not to the snapshot of its architectural state.
CALL_STATE_DECL = r'''
typedef struct ac1_call_state {
    uint64_t count;
    /* Generated code calls directly while the host stack pointer is at or
     * above `stack_floor` and enters the dispatcher below it. The runtime puts
     * the floor near the end of the thread's host stack, or at UINTPTR_MAX so
     * that tracing sees every call; the dispatcher stops below `stack_limit`. */
    uintptr_t stack_floor, stack_limit;
    /* Target of the innermost dispatched (not direct) call, for diagnostics. */
    uint32_t last_target;
    /* Generated code counts returns and enters the runtime's checkpoint only
     * when `returns` reaches `return_threshold` (0: after every return). The
     * runtime adds them to `count`, the dispatch budget, when it resets them. */
    uint32_t returns, return_threshold;
} ac1_call_state;
'''
CALL_HELPERS = r'''
void ac1_scheduler_return_checkpoint(CPU *c);

/* The host stack pointer, without forcing a frame pointer. */
#if defined(__x86_64__)
#define AC1_HOST_SP() __extension__({ uintptr_t _sp; __asm__("movq %%rsp, %0" : "=r"(_sp)); _sp; })
#elif defined(__aarch64__)
#define AC1_HOST_SP() __extension__({ uintptr_t _sp; __asm__("mov %0, sp" : "=r"(_sp)); _sp; })
#else
#define AC1_HOST_SP() ((uintptr_t)__builtin_frame_address(0))
#endif

/* Unbound CPUs (tests, a thread's first transfer), tracing and a nearly
 * exhausted host stack take the dispatcher, which binds, traces or stops. A
 * direct call otherwise costs the stack comparison and the return count; the
 * runtime's checkpoints keep the dispatch budget. */
#if defined(AC1_NO_DIRECT_CALLS)
#define AC1_CALL(c, fn, target) dispatch((c), (target))
#else
#define AC1_CALL(c, fn, target) do { \
    CPU *_ac = (c); \
    ac1_call_state *_as = _ac->calls; \
    if (__builtin_expect(!_as || AC1_HOST_SP() < _as->stack_floor, 0)) { \
        dispatch(_ac, (target)); \
    } else { \
        fn(_ac); \
        _as = _ac->calls; \
        if (__builtin_expect(++_as->returns >= _as->return_threshold, 0)) \
            ac1_scheduler_return_checkpoint(_ac); \
    } \
} while (0)
#endif
'''

# Project-owned additions to the pinned CPU ABI. No donor input/hash is changed.
FP_STATE_FIELDS = r'''
    uint64_t mmx[8];
    uint16_t mmx_exp[8];
    uint16_t fpu_cw, fpu_tw, fpu_opcode, fpu_cs, fpu_ds;
    uint32_t fpu_ip, fpu_dp, mxcsr;
    uint8_t fp_initialized, mmx_active, x87_dirty, mmx_physical;
'''

FP_STATE_BASIC = r'''
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
'''

DIRECT_ACCESS_HELPERS = r'''/* Guest memory accessors are force-inlined: in very large generated
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
    memcpy(o, ac1_direct_arena + a, w);
}
AC1_ACCESSOR_INLINE void ac1_direct_write(uint32_t a, const void *i, size_t w)
{
    memcpy(ac1_direct_arena + a, i, w);
}
#define guest_execution_read(a, o, w) ac1_direct_read((a), (o), (w))
#define guest_execution_write(a, i, w) ac1_direct_write((a), (i), (w))
/* LOCK-prefixed read-modify-writes (31k sites) operate in place on the arena
 * with sequentially consistent builtins, matching x86 LOCK's full fence.
 * Unaligned operands keep the checked path and its diagnostic. */
AC1_ACCESSOR_INLINE uint32_t *ac1_direct_atomic_cell(uint32_t a)
{
    return !(a & 3u) ? (uint32_t *)(ac1_direct_arena + a) : NULL;
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

'''

FP_STATE_HELPERS = r'''
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
static inline uint64_t ac1_x87_integer(CPU *c, double value, unsigned bits, unsigned trunc_mode, uint32_t va) {
    /* bits <= 64, so the power of two is exact without an ldexp call. */
    double limit=(double)(UINT64_C(1)<<(bits-1));
    if((trunc_mode || ((c->fpu_cw>>10)&3u)==3u) && bits<=32u && value>-limit-1.0 && value<limit) {
        /* Chop (the game's CW=0x0E7F mode, and FISTTP): C conversion truncates toward
         * zero, exact for every finite in-range value; NaN fails both comparisons. */
        int64_t chopped=(int64_t)value;
        if((double)chopped!=value) {
            c->fpu_sw|=0x20u;
            if(!(c->fpu_cw&0x20u)) ac1_unsupported_instruction(va,"unmasked x87 precision exception");
        }
        return (uint64_t)chopped;
    }
    double result=ac1_fp_round(value,trunc_mode?3u:(c->fpu_cw>>10)&3u);
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
/* Experimental native x87 lowering (--x87 native): the same values and
 * status flags with masked exceptions, without unmasked-exception
 * diagnostics. Reads the control word and writes only the status word.
 * Chop (the game's CW=0x0E7F) stays inline; other modes are cold. */
static __attribute__((noinline, cold)) uint64_t ac1_x87_integer_native_slow(CPU *c, double value, unsigned bits, unsigned rc) {
    double limit=(double)(UINT64_C(1)<<(bits-1));
    double result=ac1_fp_round(value,rc);
    if(!(result>=-limit && result<limit)) { c->fpu_sw|=1u; return UINT64_C(1)<<(bits-1); }
    if(result!=value) c->fpu_sw|=0x20u;
    return (uint64_t)(int64_t)result;
}
static inline __attribute__((always_inline)) uint64_t ac1_x87_integer_native(CPU *c, double value, unsigned bits, unsigned trunc_mode) {
    double limit=(double)(UINT64_C(1)<<(bits-1));
    unsigned rc=trunc_mode?3u:(c->fpu_cw>>10)&3u;
    if(__builtin_expect(rc==3u && value>-limit-1.0 && value<limit, 1)) {
        int64_t chopped=(int64_t)value;
        if((double)chopped!=value) c->fpu_sw|=0x20u;
        return (uint64_t)chopped;
    }
    return ac1_x87_integer_native_slow(c,value,bits,rc);
}
'''


# MinGW's <math.h> classifies through its inline __fpclassify bit decoding; the
# compiler builtins give identical results (all IEEE classes, no exceptions) as a
# single compare. They sit on every x87 load (ac1_x87_tag) and conversion.
FP_CLASSIFY_BUILTINS = r'''
#if !defined(__cplusplus) && (defined(__GNUC__) || defined(__clang__))
#undef isfinite
#define isfinite(x) __builtin_isfinite(x)
#undef isnan
#define isnan(x) __builtin_isnan(x)
#undef isinf
#define isinf(x) __builtin_isinf(x)
#endif'''


XMM_VECTOR_TYPES = r'''/* AC1: vector views of the same 128 bits. Lifted code names XMM lanes and
 * whole registers only through these (x.vf32[0], x.vf32 * y.vf32, shuffles):
 * GCC and Clang keep an XMM local in a host vector register only while every
 * access to it is vector-typed, and one array-lane access sends it to the
 * stack. aligned(8) keeps the union's alignment, so CPU's layout is unchanged. */
typedef float    ac1_v4f32 __attribute__((vector_size(16), aligned(8)));
typedef double   ac1_v2f64 __attribute__((vector_size(16), aligned(8)));
typedef uint32_t ac1_v4u32 __attribute__((vector_size(16), aligned(8)));
typedef int32_t  ac1_v4i32 __attribute__((vector_size(16), aligned(8)));
typedef uint64_t ac1_v2u64 __attribute__((vector_size(16), aligned(8)));
typedef int64_t  ac1_v2i64 __attribute__((vector_size(16), aligned(8)));
typedef uint16_t ac1_v8u16 __attribute__((vector_size(16), aligned(8)));
typedef int16_t  ac1_v8i16 __attribute__((vector_size(16), aligned(8)));
typedef uint8_t  ac1_v16u8 __attribute__((vector_size(16), aligned(8)));
typedef int8_t   ac1_v16i8 __attribute__((vector_size(16), aligned(8)));
typedef float    ac1_v2f32 __attribute__((vector_size(8), aligned(8)));

'''

XMM_VECTOR_MEMBERS = '''    ac1_v4f32 vf32; ac1_v2f64 vf64; ac1_v4u32 vu32; ac1_v4i32 vi32; ac1_v2u64 vu64;
    ac1_v2i64 vi64; ac1_v8u16 vu16; ac1_v8i16 vi16; ac1_v16u8 vu8; ac1_v16i8 vi8;
'''

# Whole-register SQRTPS/PD and MINPS/MAXPS/MINPD/MAXPD for the vector lowering.
# min/max keep the instruction's "dst OP src ? dst : src" lane rule (sse_minf).
# Lane writes go through ac1_lane_*: GCC keeps a union local in a register
# only if its lanes are written as whole-vector values, never in place.
XMM_VECTOR_HELPERS = r'''
#define AC1_XMM_LANE_SETTER(member, vector, lane) \
    static inline vector ac1_lane_##member(vector v, unsigned i, lane x) { v[i] = x; return v; }
AC1_XMM_LANE_SETTER(vf32, ac1_v4f32, float)
AC1_XMM_LANE_SETTER(vf64, ac1_v2f64, double)
AC1_XMM_LANE_SETTER(vu32, ac1_v4u32, uint32_t)
AC1_XMM_LANE_SETTER(vi32, ac1_v4i32, int32_t)
AC1_XMM_LANE_SETTER(vu64, ac1_v2u64, uint64_t)
AC1_XMM_LANE_SETTER(vi64, ac1_v2i64, int64_t)
AC1_XMM_LANE_SETTER(vu16, ac1_v8u16, uint16_t)
AC1_XMM_LANE_SETTER(vi16, ac1_v8i16, int16_t)
AC1_XMM_LANE_SETTER(vu8, ac1_v16u8, uint8_t)
AC1_XMM_LANE_SETTER(vi8, ac1_v16i8, int8_t)
/* A 16-byte stack load assembled from lane values the lifter forwarded from
 * the stores just before it (tools/lift_ac1.py, StackLaneForwarding). */
static inline uint32_t ac1_f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline XMM ac1_xmm_from_u32(uint32_t l0, uint32_t l1, uint32_t l2, uint32_t l3) {
    XMM x; x.vu32 = (ac1_v4u32){ l0, l1, l2, l3 }; return x;
}
/* CVTPS2PD reads the low two floats; CVTPD2PS zeroes the upper two lanes. */
static inline ac1_v2f64 ac1_cvtps2pd(ac1_v4f32 s) { return (ac1_v2f64){ s[0], s[1] }; }
static inline ac1_v4f32 ac1_cvtpd2ps(ac1_v2f64 s) {
    return __builtin_shufflevector(__builtin_convertvector(s, ac1_v2f32), (ac1_v2f32){ 0.0f, 0.0f }, 0, 1, 2, 3);
}
#if defined(__clang__)
#if __has_builtin(__builtin_elementwise_sqrt)
#define AC1_ELEMENTWISE_SQRT 1
#endif
#endif
static inline ac1_v4f32 ac1_sqrt_v4f32(ac1_v4f32 v) {
#if defined(AC1_ELEMENTWISE_SQRT)
    return __builtin_elementwise_sqrt(v);
#elif defined(__SSE__) && !defined(__clang__)
    return __builtin_ia32_sqrtps(v);
#else
    return (ac1_v4f32){ sqrtf(v[0]), sqrtf(v[1]), sqrtf(v[2]), sqrtf(v[3]) };
#endif
}
static inline ac1_v2f64 ac1_sqrt_v2f64(ac1_v2f64 v) {
#if defined(AC1_ELEMENTWISE_SQRT)
    return __builtin_elementwise_sqrt(v);
#elif defined(__SSE2__) && !defined(__clang__)
    return __builtin_ia32_sqrtpd(v);
#else
    return (ac1_v2f64){ sqrt(v[0]), sqrt(v[1]) };
#endif
}
static inline ac1_v4f32 ac1_min_v4f32(ac1_v4f32 d, ac1_v4f32 s) {
    ac1_v4i32 pick = (ac1_v4i32)(d < s);
    return (ac1_v4f32)(((ac1_v4i32)d & pick) | ((ac1_v4i32)s & ~pick));
}
static inline ac1_v4f32 ac1_max_v4f32(ac1_v4f32 d, ac1_v4f32 s) {
    ac1_v4i32 pick = (ac1_v4i32)(d > s);
    return (ac1_v4f32)(((ac1_v4i32)d & pick) | ((ac1_v4i32)s & ~pick));
}
static inline ac1_v2f64 ac1_min_v2f64(ac1_v2f64 d, ac1_v2f64 s) {
    ac1_v2i64 pick = (ac1_v2i64)(d < s);
    return (ac1_v2f64)(((ac1_v2i64)d & pick) | ((ac1_v2i64)s & ~pick));
}
static inline ac1_v2f64 ac1_max_v2f64(ac1_v2f64 d, ac1_v2f64 s) {
    ac1_v2i64 pick = (ac1_v2i64)(d > s);
    return (ac1_v2f64)(((ac1_v2i64)d & pick) | ((ac1_v2i64)s & ~pick));
}
'''


def xmm_vector_views(text: str, source) -> str:
    """Vector members on XMM, vector movss/movsd loads, whole-register helpers."""
    replacements = (
        ('/* ---- SSE register file ----', XMM_VECTOR_TYPES + '/* ---- SSE register file ----'),
        ('    int64_t  i64[2];\n} XMM;', '    int64_t  i64[2];\n' + XMM_VECTOR_MEMBERS + '} XMM;'),
        ('static inline void sse_load_ss(XMM *d, float  v) { d->u64[0] = 0; d->u64[1] = 0; d->f32[0] = v; }',
         'static inline void sse_load_ss(XMM *d, float  v) { d->vf32 = (ac1_v4f32){ v, 0.0f, 0.0f, 0.0f }; }'),
        ('static inline void sse_load_sd(XMM *d, double v) { d->u64[0] = 0; d->u64[1] = 0; d->f64[0] = v; }',
         'static inline void sse_load_sd(XMM *d, double v) { d->vf64 = (ac1_v2f64){ v, 0.0 }; }'
         + XMM_VECTOR_HELPERS),
    )
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f"unexpected XMM declaration shape in {source}: {old[:60]!r}")
        text = text.replace(old, new)
    return text


def _replace_function(text: str, signature: str, body: str, source) -> str:
    """Replace one top-level C function (signature line through its closing brace)."""
    begin = text.find(signature)
    if begin < 0 or text.count(signature) != 1:
        raise SystemExit(f"unexpected guest_memory.c shape for {signature!r}: {source}")
    end = text.find("\n}\n", begin)
    if end < 0:
        raise SystemExit(f"unterminated function {signature!r}: {source}")
    return text[:begin] + body.rstrip("\n") + text[end + 2:]


def sorted_region_transform(text: str, source) -> str:
    """Keep guest regions sorted by base so lookups need no linear scans.

    Measured on the title workload: guest_memory_find_gap (quadratic) and the
    linear slow paths of guest_ptr/guest_memory_try_ptr over ~900 regions
    were the remaining guest-memory costs in runtime code. Regions never
    overlap, so a sorted array gives O(log n) lookups and a one-pass gap
    search; remove() already preserves order with memmove.
    """
    helper = r'''
/* AC1: regions are kept sorted by base (inserted in place); find the last
 * region whose base is <= address, or SIZE_MAX if none. */
static size_t ac1_region_floor(uint32_t address)
{
    size_t low = 0, high = current_memory.count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (current_memory.regions[mid].base <= address) low = mid + 1;
        else high = mid;
    }
    return low ? low - 1 : SIZE_MAX;
}

void ac1_guest_memory_dump_span(uint32_t address, size_t width)
{
    int wraps = width > UINT64_C(0x100000000) - address;
    uint64_t end = wraps ? UINT64_C(0x100000000) : (uint64_t)address + width;
    fprintf(stderr, "AC1 memory span: address=0x%08x width=%zu wraps=%d direct=%d regions=%zu\n",
            address, width, wraps, guest_memory_is_direct(),
            selected_memory ? current_memory.count : 0u);
    if (!selected_memory) return;
    size_t floor = ac1_region_floor(address);
    size_t first = floor == SIZE_MAX ? 0 : floor ? floor - 1 : 0;
    /* Include the neighbours so holes and independent backing are visible. */
    for (size_t i = first; i < current_memory.count; ++i) {
        const guest_memory_region_t *r = &current_memory.regions[i];
        fprintf(stderr, "AC1 memory region: index=%zu base=0x%08x end=0x%llx backing=%p\n",
                i, r->base, (unsigned long long)((uint64_t)r->base + r->size), (void *)r->data);
        if ((uint64_t)r->base >= end) break;
    }
}
'''
    marker = "void guest_memory_add(uint8_t *backing, uint32_t guest_base, uint64_t size)"
    if text.count(marker) != 1:
        raise SystemExit(f"unexpected guest_memory_add shape: {source}")
    text = text.replace(marker, helper.lstrip("\n") + "\n" + marker)
    text = _replace_function(text, marker, r'''void guest_memory_add(uint8_t *backing, uint32_t guest_base, uint64_t size)
{
    uint64_t end;
    if (!selected_memory) guest_memory_reset();
    if (!backing || !size)
        fail("invalid binding", guest_base, (size_t)size);
    if (current_memory.arena && backing != current_memory.arena+guest_base)
        fail("non-arena backing",guest_base,(size_t)size);
    if (size > UINT64_C(0x100000000))
        fail("binding too large", guest_base, (size_t)size);
    end = (uint64_t)guest_base + size;
    if (end > UINT64_C(0x100000000))
        fail("binding wraps", guest_base, (size_t)size);
    if (current_memory.count == GUEST_MEMORY_MAX_REGIONS)
        fail("too many regions", guest_base, (size_t)size);
    /* Sorted insert: only the neighbours can overlap. */
    size_t floor = ac1_region_floor(guest_base);
    size_t at = floor == SIZE_MAX ? 0 : floor + 1;
    if (floor != SIZE_MAX) {
        const guest_memory_region_t *before = &current_memory.regions[floor];
        if ((uint64_t)before->base + before->size > guest_base)
            fail("overlapping regions", guest_base, (size_t)size);
    }
    if (at < current_memory.count && current_memory.regions[at].base < end)
        fail("overlapping regions", guest_base, (size_t)size);
    memmove(&current_memory.regions[at + 1], &current_memory.regions[at],
            (current_memory.count - at) * sizeof(current_memory.regions[0]));
    current_memory.regions[at].data = backing;
    current_memory.regions[at].base = guest_base;
    current_memory.regions[at].size = size;
    ++current_memory.count;
}
''', source)
    text = _replace_function(text, "uint32_t guest_memory_find_gap(uint32_t start, uint32_t size,", r'''uint32_t guest_memory_find_gap(uint32_t start, uint32_t size,
                               uint32_t alignment, uint32_t limit)
{
    if (!selected_memory || !size || !alignment || (alignment & (alignment - 1)) ||
        current_memory.count == GUEST_MEMORY_MAX_REGIONS) return 0;
    uint64_t at = ((uint64_t)start + alignment - 1) & ~(uint64_t)(alignment - 1);
    if (!at) return 0;
    /* One pass over the sorted regions from the first one that can overlap. */
    size_t i = ac1_region_floor((uint32_t)(at > UINT32_MAX ? UINT32_MAX : at));
    for (i = i == SIZE_MAX ? 0 : i; i < current_memory.count; ++i) {
        const guest_memory_region_t *r = &current_memory.regions[i];
        uint64_t end = (uint64_t)r->base + r->size;
        if (end <= at) continue;
        if ((uint64_t)r->base >= at + size) break;
        at = (end + alignment - 1) & ~(uint64_t)(alignment - 1);
    }
    if (at + size > limit) return 0;
    return (uint32_t)at;
}
''', source)
    # Slow paths: binary search instead of scanning every region.
    old_try = '''    for (size_t i=0;i<current_memory.count;++i) {
        const guest_memory_region_t *r=&current_memory.regions[i];
        if (address>=r->base && end<=(uint64_t)r->base+r->size) {
            *cached=*r;
            AC1_PROFILE_SCAN(i + 1);
            return r->data+(address-r->base);
        }
    }
    AC1_PROFILE_SCAN(current_memory.count);
    return NULL;'''
    new_try = '''    size_t i = ac1_region_floor(address);
    if (i != SIZE_MAX) {
        const guest_memory_region_t *r=&current_memory.regions[i];
        if (end<=(uint64_t)r->base+r->size) {
            *cached=*r;
            AC1_PROFILE_SCAN(1);
            return r->data+(address-r->base);
        }
    }
    AC1_PROFILE_SCAN(1);
    return NULL;'''
    if text.count(old_try) != 1:
        raise SystemExit(f"unexpected guest_memory_try_ptr slow path: {source}")
    text = text.replace(old_try, new_try)
    old_ptr = '''    AC1_PROFILE_COUNT(slow);
    for (i = 0; i < current_memory.count; ++i) {
        const guest_memory_region_t *region = &current_memory.regions[i];'''
    new_ptr = '''    AC1_PROFILE_COUNT(slow);
    if (width) {
        /* Sorted regions: the floor region is the only candidate. */
        size_t floor = ac1_region_floor(address);
        if (floor != SIZE_MAX) {
            const guest_memory_region_t *region = &current_memory.regions[floor];
            if (end <= (uint64_t)region->base + region->size) {
                *cached = *region;
                AC1_PROFILE_SCAN(1);
                return region->data + (size_t)(address - region->base);
            }
        }
    }
    /* Zero width (adjacent-mapping preference) and misses keep the scan. */
    for (i = 0; i < current_memory.count; ++i) {
        const guest_memory_region_t *region = &current_memory.regions[i];'''
    if text.count(old_ptr) != 1:
        raise SystemExit(f"unexpected guest_ptr slow path: {source}")
    return text.replace(old_ptr, new_ptr)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


X87_INTEGER_INLINE = r'''
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
'''


def x87_inline_helpers(text: str) -> str:
    """Gate the observed out-of-line integer helper without changing its body."""
    signature='static inline uint64_t ac1_x87_integer('
    if text.count(signature)!=1:
        raise SystemExit('unexpected CPU x87 integer-conversion helper shape')
    return text.replace(signature,X87_INTEGER_INLINE+'\nAC1_X87_INTEGER_INLINE uint64_t ac1_x87_integer(',1)


def logical_flags_clear_af(text: str) -> str:
    """AND/OR/XOR/TEST leave AF undefined: clear it, as QEMU and Bochs do.

    The donor preserves AF, which lets an AF demand flow back through every
    logical operation into whole-program flag contracts.
    """
    match = re.search(r'static inline uint32_t flags_logicz\([^)]*\) \{[^{}]*\}', text)
    old = '    c->cf = 0; c->of = 0; c->sf = (r & s) != 0; c->zf = (r == 0);\n'
    if not match or match.group(0).count(old) != 1 or '->af' in match.group(0):
        raise SystemExit('unexpected pinned logical flag helper')
    helper = match.group(0).replace(old, old.replace('c->of = 0; ', 'c->of = 0; c->af = 0; '))
    return text.replace(match.group(0), helper, 1)


def masked_flag_helpers(text: str) -> str:
    """Clone pinned arithmetic helpers with constant, independently guarded lanes.

    Keep the original algorithms and unmasked entry points for runtime/Bink.
    Fail closed if donor signatures or lane assignments change.
    """
    bits = {name: 1 << i for i, name in enumerate(('cf', 'pf', 'af', 'zf', 'sf', 'of'))}
    helpers = []
    for name in ('flags_add', 'flags_sub', 'flags_logicz', 'flags_adc', 'flags_sbb',
                 'flags_incs', 'flags_decs'):
        match = re.search(r'static inline uint32_t ' + name + r'\(([^)]*)\) \{([^{}]*)\}', text)
        if not match:
            raise SystemExit(f'unexpected pinned flag helper: {name}')
        body = match.group(2)
        if name in ('flags_incs', 'flags_decs'):
            operation = 'flags_add' if name == 'flags_incs' else 'flags_sub'
            expected = f'\n    uint32_t keep = c->cf, r = {operation}(c, a, 1, sz); c->cf = keep; return r;\n'
            if body != expected:
                raise SystemExit(f'unexpected pinned preserving helper: {name}')
            body = f'\n    return {operation}_m(c, a, 1, sz, lanes & ~1u);\n'
        else:
            count = [0]
            def guard(assignment):
                count[0] += 1
                return f'if (lanes & {bits[assignment.group(1)]}u) {{ ' + assignment.group(0) + ' }'
            body = re.sub(r'c->(cf|pf|af|zf|sf|of)\s*=\s*[^;]+;', guard, body)
            if count[0] != 6:
                raise SystemExit(f'unexpected pinned flag lanes: {name}')
        helpers.append(f'AC1_ACCESSOR_INLINE uint32_t {name}_m({match.group(1)}, unsigned lanes) {{{body}}}')
    # Shifts: the same lanes for CF/ZF/SF/PF (a zero count still writes none).
    for name in ('op_shl', 'op_shr', 'op_sar', 'op_shld', 'op_shrd'):
        match = re.search(r'static inline uint32_t ' + name + r'\(([^)]*)\) \{([^{}]*)\}', text)
        if not match:
            raise SystemExit(f'unexpected pinned shift helper: {name}')
        count = [0]
        def guard(assignment):
            count[0] += 1
            return f'if (lanes & {bits[assignment.group(1)]}u) {{ ' + assignment.group(0) + ' }'
        body = re.sub(r'c->(cf|pf|af|zf|sf|of)\s*=\s*[^;]+;', guard, match.group(2))
        if count[0] != 4 or '->of' in body or '->af' in body:
            raise SystemExit(f'unexpected pinned shift flag lanes: {name}')
        helpers.append(f'AC1_ACCESSOR_INLINE uint32_t {name}_m({match.group(1)}, unsigned lanes) {{{body}}}')
    marker = '/* ---- x87 FPU ---- */'
    if text.count(marker) != 1:
        raise SystemExit('unexpected pinned masked-helper insertion marker')
    return text.replace(marker, '/* Constant-mask flag lanes; generated by prepare_runtime.py. */\n' +
                        '\n'.join(helpers) + '\n\n' + marker)


FLAG_LOCAL_HELPERS = ('flags_add_m', 'flags_sub_m', 'flags_logicz_m', 'flags_adc_m', 'flags_sbb_m',
                      'flags_incs_m', 'flags_decs_m', 'op_shl_m', 'op_shr_m', 'op_sar_m', 'op_shld_m',
                      'op_shrd_m', 'op_rol', 'op_ror', 'op_bsf', 'op_bsr', 'sse_compare',
                      'fcompare_eflags', 'do_sahf')


def flag_local_helpers(text: str) -> str:
    """Clones of the flag-writing helpers on a function-local flag struct.

    Generated code keeps the six arithmetic flags in an `ac1_flags` local and
    syncs the CPU struct only where whole-program flag contracts need it. Each
    clone keeps its helper's algorithm and touches nothing but those flags.
    """
    clones = ['/* Flag-local clones; generated by prepare_runtime.py. */',
              'typedef struct { uint32_t cf, zf, sf, of, pf, af; } ac1_flags;']
    for name in FLAG_LOCAL_HELPERS:
        match = re.search(r'(?m)^(?:static inline|AC1_ACCESSOR_INLINE) (\w+) ' + name +
                          r'\(CPU \*c, ([^)]*)\) \{((?:[^{}]|\{[^{}]*\})*)\}', text)
        if not match:
            raise SystemExit(f'unexpected flag-local helper source: {name}')
        body = re.sub(r'\b(flags_\w+_m)\(c,', r'\1_l(c,', match.group(3))
        fields = set(re.findall(r'\bc->(\w+)', body))
        calls = set(re.findall(r'\b(\w+)\(c\b', body))
        if not fields <= {'cf', 'zf', 'sf', 'of', 'pf', 'af'} or not all(c.endswith('_m_l') for c in calls):
            raise SystemExit(f'flag-local helper touches more than flags: {name}')
        clones.append(f'AC1_ACCESSOR_INLINE {match.group(1)} {name}_l(ac1_flags *c, {match.group(2)}) {{{body}}}')
    marker = '#endif /* PCRECOMP_CPU_H */'
    if text.count(marker) != 1:
        raise SystemExit('unexpected CPU header guard')
    return text.replace(marker, '\n'.join(clones) + '\n' + marker)


def helper_functions(text):
    """Find pinned inline bodies; mask comments/strings to preserve edit offsets."""
    masked=re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',lambda m:re.sub(r'[^\n]',' ',m[0]),text,flags=re.S)
    pattern=re.compile(r'(?m)^(static inline|AC1_ACCESSOR_INLINE|AC1_X87_INTEGER_INLINE)\s+([^\n{};]+?)\b(\w+)\s*\(([^{};]*?)\)\s*\{')
    functions={}
    for m in pattern.finditer(masked):
        depth=1;end=m.end()
        while depth:
            depth+=(masked[end]=='{')-(masked[end]=='}');end+=1
        functions[m[3]]={'name':m[3],'start':m.start(),'body':m.end(),'end':end,
            'args_start':m.start(4),'args_end':m.end(4),'name_start':m.start(3),'name_end':m.end(3),
            'code':masked[m.end():end-1]}
    return functions,masked

def local_memory_helpers(text):
    """Pass one captured arena value through every nested memory helper.

    The runtime reserves the arena before guest entry and releases it only
    after guest threads join, under the process run lock. Each invocation
    captures its own value, including a later ac1_run with a different arena.
    The local pointer's address never escapes; guest byte stores and opaque
    CPU observers cannot invalidate it. No fixed VA or const-function promise
    is required on Windows or Android. Unaligned atomics keep the original
    checked fallback, including its precise rejection diagnostics.

    The empty asm input observes only EIP before an access. It keeps the
    existing tracing store live for fault diagnostics after opaque fallback
    calls disappear, emits no instruction, and needs no GPR synchronization.
    """
    functions,masked=helper_functions(text)
    aliases={'guest_execution_read':'ac1_direct_read','guest_execution_write':'ac1_direct_write',
      'guest_atomic_xadd32':'ac1_direct_xadd32','guest_atomic_cmpxchg32':'ac1_direct_cmpxchg32','guest_atomic_xchg32':'ac1_direct_xchg32'}
    seeds={'ac1_direct_read','ac1_direct_write','ac1_direct_atomic_cell'}
    if not seeds<=functions.keys():
        raise ValueError('unexpected direct-memory CPU helper definitions')
    dependent=set(seeds)
    while True:
        current=set(dependent)
        for name,f in functions.items():
            called=set(re.findall(r'\b(\w+)\s*\(',f['code']))
            if {aliases.get(c,c) for c in called}&dependent:current.add(name)
        if current==dependent:break
        dependent=current
    prototypes=[];definitions=[]
    overrides={
      'ac1_direct_read':'memcpy(o, _ac1_base + a, w);',
      'ac1_direct_write':'memcpy(_ac1_base + a, i, w);',
      'ac1_direct_atomic_cell':'return !(a & 3u) ? (uint32_t *)(_ac1_base + a) : NULL;'}
    for name,f in functions.items():
        if name not in dependent:continue
        prefix=text[f['start']:f['name_start']]
        args=text[f['args_start']:f['args_end']].strip()
        args='uint8_t * const _ac1_base, const CPU * const _ac1_observer'+(', '+args if args and args!='void' else '')
        declaration=prefix+'ac1_base_'+name+'('+args+')'
        prototypes.append(declaration+';')
        body=text[f['body']:f['end']-1]
        if name in overrides:body='\n    __asm__ __volatile__("" : : "m"(_ac1_observer->eip));\n    '+overrides[name]+'\n'
        else:
            edits=[]
            for m in re.finditer(r'\b(\w+)\s*\(',f['code']):
                target=aliases.get(m[1],m[1])
                if target in dependent:edits.append((m.start(),m.end(),'ac1_base_'+target+'(_ac1_base, _ac1_observer, '))
            for a,b,replacement in reversed(edits):body=body[:a]+replacement+body[b:]
            if 'ac1_direct_arena' in body:raise SystemExit('unhandled arena read in '+name)
        definitions.append(declaration+' {\n'+body+'}')
    section='\n/* AC1_LOCAL_MEMORY_HELPERS */\n#if defined(AC1_DIRECT_MEMORY)\n'+'\n'.join(prototypes+definitions)+'\n#endif\n'
    guard='#endif /* PCRECOMP_CPU_H */'
    if text.count(guard)!=1:
        raise ValueError('unexpected CPU header closing guard for local memory helpers')
    text=text.replace(guard,section+guard,1)
    header='#pragma once\n#if defined(AC1_DIRECT_MEMORY)\n'
    header+='#define AC1_DECLARE_LOCAL_ARENA uint8_t * const _ac1_local_arena = ac1_direct_arena; (void)_ac1_local_arena\n'
    mapping={name:name for name in dependent}|{a:t for a,t in aliases.items() if t in dependent}
    for name,target in sorted(mapping.items()):
        header+='#undef '+name+'\n#define '+name+'(...) ac1_base_'+target+'(_ac1_local_arena, c, __VA_ARGS__)\n'
    header+='#else\n#define AC1_DECLARE_LOCAL_ARENA ((void)0)\n#endif\n'
    return text,header,sorted(dependent)


def main() -> None:
    provenance = {
        "duke_reference": str(DUKE_ROOT),
        "pcrecomp_root": CONFIG["pcrecomp_root"],
        "license": "pcrecomp-derived allocator files retain the upstream MIT license below src/runtime",
        "files": [],
    }
    for destination, (source_relative, expected_hash) in SOURCE_FILES.items():
        source = DUKE_ROOT / source_relative
        actual_hash = sha256(source)
        if actual_hash != expected_hash:
            raise SystemExit(
                f"pinned runtime input changed: {source} sha256={actual_hash}; "
                f"expected {expected_hash}"
            )
        output = ROOT / destination
        output.parent.mkdir(parents=True, exist_ok=True)
        content = source.read_bytes()
        if destination == "src/runtime/guest_memory.h":
            text = content.decode("utf-8").replace("\r\n", "\n")
            capacity = (
                "/* Original map/renderer allocation exceeds the initial 64-region probe cap. */\n"
                "#define GUEST_MEMORY_MAX_REGIONS 256u"
            )
            if text.count(capacity) != 1:
                raise SystemExit(f"unexpected guest-memory capacity definition: {source}")
            text = text.replace(
                capacity,
                "/* One record per 64 KiB reservation across the 4 GiB guest arena. */\n"
                "#define GUEST_MEMORY_MAX_REGIONS 65536u",
            )
            marker = "void guest_memory_release(uint32_t base);"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory header shape: {source}")
            text = text.replace(marker, marker + "\nuint64_t guest_memory_region_size(uint32_t base);")
            marker = "int guest_memory_read(uint32_t address, void *output, size_t width);"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory read declaration: {source}")
            text = text.replace(
                marker,
                marker + "\n"
                "/* Checked fixed-width CPU accesses may cross adjacent mappings. */\n"
                "int guest_memory_read_scalar(uint32_t address, void *output, size_t width);\n"
                "int guest_memory_write_scalar(uint32_t address, const void *input, size_t width);\n"
                "void guest_execution_read(uint32_t address, void *output, size_t width);\n"
                "void guest_execution_write(uint32_t address, const void *input, size_t width);",
            )
            content = text.replace("\r\n", "\n").encode("utf-8")
        if destination == "src/runtime/guest_clock.c":
            # The scheduler reads this clock every 64 guest returns: query the
            # boot-fixed frequency once, and convert the usual 10 MHz counter
            # with one multiply. Results are unchanged (floor(ticks*1e9/rate)).
            text = content.decode("utf-8").replace("\r\n", "\n")
            old = ("    LARGE_INTEGER ticks, frequency;\n"
                   "    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&ticks) || frequency.QuadPart <= 0) abort();\n"
                   "    uint64_t value = (uint64_t)ticks.QuadPart, rate = (uint64_t)frequency.QuadPart;\n"
                   "    return value / rate * 1000000000ull + value % rate * 1000000000ull / rate;\n")
            if text.count(old) != 1:
                raise SystemExit(f"unexpected guest clock: {source}")
            text = text.replace(old,
                   "    static volatile uint64_t cached_rate;\n"
                   "    LARGE_INTEGER ticks, frequency;\n"
                   "    uint64_t rate = cached_rate;\n"
                   "    if (!rate) {\n"
                   "        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) abort();\n"
                   "        cached_rate = rate = (uint64_t)frequency.QuadPart;\n"
                   "    }\n"
                   "    if (!QueryPerformanceCounter(&ticks)) abort();\n"
                   "    uint64_t value = (uint64_t)ticks.QuadPart;\n"
                   "    if (rate == 10000000ull) return value * 100ull;\n"
                   "    return value / rate * 1000000000ull + value % rate * 1000000000ull / rate;\n")
            content = text.encode("utf-8")
        if destination == "src/runtime/guest_memory.c":
            text = content.decode("utf-8").replace("\r\n", "\n")
            # Reserve the direct arena top-down: a bottom-up 4 GiB reservation
            # landed at 0x7FFF0000 and took the address space below 4 GiB that
            # native DirectSound needs (3D CreateSoundBuffer never completed).
            reserve = "MEM_RESERVE, PAGE_NOACCESS);"
            if text.count(reserve) != 1:
                raise SystemExit(f"unexpected direct-arena reservation in {source}")
            text = text.replace(reserve, "MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);")
            text = text.replace("#include <stdlib.h>", "#include <stdlib.h>\n#include <errno.h>")
            failure = "    if (!s->arena) { free(s); return NULL; }"
            if text.count(failure) != 1:
                raise SystemExit("unexpected direct-arena allocation failure shape")
            text = text.replace(failure, '''    if (!s->arena) {
#ifdef _WIN32
        unsigned long native_error = GetLastError();
        fprintf(stderr, "AC1 direct arena reservation failed: bytes=%llu guard=%zu VirtualAlloc error=%lu\\n",
                (unsigned long long)(ARENA_BYTES+s->page_size), s->page_size, native_error);
#else
        int native_error = errno;
        fprintf(stderr, "AC1 direct arena reservation failed: bytes=%llu guard=%zu mmap errno=%d (%s)\\n",
                (unsigned long long)(ARENA_BYTES+s->page_size), s->page_size, native_error, strerror(native_error));
#endif
        free(s); return NULL;
    }''')
            checkpoint = (
                "    if (current_memory.count==65) {\n"
                '        printf("guest-memory mapping checkpoint: count=65 base=0x%08x size=%llu\\n",\n'
                "               guest_base,(unsigned long long)size);\n"
                "        fflush(stdout);\n"
                "    }\n"
            )
            if text.count(checkpoint) != 1:
                raise SystemExit(f"unexpected guest-memory checkpoint shape: {source}")
            text = text.replace(checkpoint, "")
            marker = "void guest_memory_reset(void)"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory source shape: {source}")
            text = text.replace(
                marker,
                "uint64_t guest_memory_region_size(uint32_t base)\n"
                "{\n"
                "    if (!selected_memory) return 0;\n"
                "    for (size_t i = 0; i < current_memory.count; ++i)\n"
                "        if (current_memory.regions[i].base == base)\n"
                "            return current_memory.regions[i].size;\n"
                "    return 0;\n"
                "}\n\n"
                + marker,
            )
            marker = "void *guest_ptr(uint32_t address, size_t width)\n"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory pointer implementation: {source}")
            scalar_helpers = r'''/* CPU scalar accesses can cross adjacent guest mappings whose host backing
 * allocations are unrelated. Validate the full guest span first, then copy
 * it in per-region chunks. Raw guest_ptr intentionally remains one-region. */
static guest_memory_region_t *scalar_region_at(uint64_t address)
{
    if (!selected_memory || address >= UINT64_C(0x100000000)) return NULL;
    for (size_t i = 0; i < current_memory.count; ++i) {
        guest_memory_region_t *region = &current_memory.regions[i];
        uint64_t end = (uint64_t)region->base + region->size;
        if (address >= region->base && address < end) return region;
    }
    return NULL;
}

static int scalar_span_mapped(uint64_t begin, uint64_t end)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        if (!region) return 0;
        uint64_t region_end = (uint64_t)region->base + region->size;
        if (region_end <= at) return 0;
        at = region_end < end ? region_end : end;
    }
    return 1;
}

static void scalar_copy_from(uint64_t begin, uint64_t end, uint8_t *output)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        uint64_t region_end = (uint64_t)region->base + region->size;
        size_t chunk = (size_t)((region_end < end ? region_end : end) - at);
        memcpy(output, region->data + (size_t)(at - region->base), chunk);
        output += chunk;
        at += chunk;
    }
}

static void scalar_copy_to(uint64_t begin, uint64_t end, const uint8_t *input)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        uint64_t region_end = (uint64_t)region->base + region->size;
        size_t chunk = (size_t)((region_end < end ? region_end : end) - at);
        memcpy(region->data + (size_t)(at - region->base), input, chunk);
        input += chunk;
        at += chunk;
    }
}

int guest_memory_read_scalar(uint32_t address, void *output, size_t width)
{
    if (!output || !width || width > 16u) return 0;
    uint64_t end = (uint64_t)address + width;
    if (end > UINT64_C(0x100000000)) return 0;
    void *single = guest_memory_try_ptr(address, width);
    if (single) {
        memcpy(output, single, width);
        return 1;
    }
    if (!scalar_span_mapped(address, end)) return 0;
    scalar_copy_from(address, end, (uint8_t *)output);
    return 1;
}

int guest_memory_write_scalar(uint32_t address, const void *input, size_t width)
{
    if (!input || !width || width > 16u) return 0;
    uint64_t end = (uint64_t)address + width;
    if (end > UINT64_C(0x100000000)) return 0;
    void *single = guest_memory_try_ptr(address, width);
    if (single) {
        memcpy(single, input, width);
        return 1;
    }
    if (!scalar_span_mapped(address, end)) return 0;
    scalar_copy_to(address, end, (const uint8_t *)input);
    return 1;
}

void guest_execution_read(uint32_t address, void *output, size_t width)
{
#ifdef DUKEMP_DIRECT_MEMORY
    if (guest_execution_base) {
        memcpy(output, guest_execution_base + address, width);
        return;
    }
#endif
    if (!guest_memory_read_scalar(address, output, width))
        fail("unmapped scalar read", address, width);
}

void guest_execution_write(uint32_t address, const void *input, size_t width)
{
#ifdef DUKEMP_DIRECT_MEMORY
    if (guest_execution_base) {
        memcpy(guest_execution_base + address, input, width);
        return;
    }
#endif
    if (!guest_memory_write_scalar(address, input, width))
        fail("unmapped scalar write", address, width);
}

'''
            text = text.replace(marker, scalar_helpers + marker)
            # Profile only cache-miss scans, including the try_ptr path used by
            # fixed-width CPU operands. Preserve donor hashes and record the
            # transformed output hash in runtime-provenance.json as usual.
            text = text.replace('#include "guest_memory.h"', '''#include "guest_memory.h"
#include "ac1_memory_diagnostics.h"
#ifdef AC1_PROFILE_HOOKS
#include "ac1_profile.h"
#else
#define AC1_PROFILE_COUNT(field) ((void)0)
#define AC1_PROFILE_SCAN(n) ((void)0)
#endif''', 1)
            for name, following, loop, success, failure in (
                ("void *guest_ptr(", "uint32_t guest_atomic_xadd32(",
                 "    for (i = 0; i < current_memory.count; ++i) {",
                 "            return region->data + (size_t)offset;",
                 "    if (width == 0 && one_past)"),
                ("void *guest_memory_try_ptr(", "int guest_memory_read(",
                 "    for (size_t i=0;i<current_memory.count;++i) {",
                 "            return r->data+(address-r->base);",
                 "    return NULL;"),
            ):
                begin, end = text.index(name), text.index(following, text.index(name))
                body = text[begin:end]
                for needle in (loop, success, failure):
                    if body.count(needle) != 1:
                        raise SystemExit(f"unexpected profiling scan shape: {name}: {needle}")
                body = body.replace(loop, "    AC1_PROFILE_COUNT(slow);\n" + loop)
                body = body.replace(success, "            AC1_PROFILE_SCAN(i + 1);\n" + success)
                body = body.replace(failure, "    AC1_PROFILE_SCAN(current_memory.count);\n" + failure)
                text = text[:begin] + body + text[end:]
            text = sorted_region_transform(text, source)
            content = text.replace("\r\n", "\n").encode("utf-8")
        if destination == "src/runtime/cpu.h":
            text = content.decode("utf-8")
            include = "#include <stdint.h>"
            if text.count(include) != 1:
                raise SystemExit(f"unexpected CPU includes: {source}")
            text = text.replace(include, include + "\n#include <math.h>" + FP_CLASSIFY_BUILTINS)
            text = xmm_vector_views(text, source)
            marker = "} CPU;"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected pcrecomp CPU header shape: {source}")
            text = text.replace(
                marker,
                FP_STATE_FIELDS + "    ac1_call_state *calls;\n" + marker + "\n\n/* Project-owned native dispatcher, called by generated code. */\n"
                "void dispatch(CPU *cpu, uint32_t target);\n"
                "void dispatch_jmp(CPU *cpu, uint32_t target);\n"
                "void ac1_unsupported_instruction(uint32_t guest_va, const char *reason);\n" + CALL_HELPERS + FP_STATE_BASIC,
            )
            cpu_start = "typedef struct {\n    uint32_t eax"
            if text.count(cpu_start) != 1:
                raise SystemExit(f"unexpected pinned CPU declaration: {source}")
            text = text.replace(cpu_start, CALL_STATE_DECL + cpu_start)
            x87_helpers = {
                "static inline void   fpush(CPU *c, double v) { c->fpu_top = (c->fpu_top - 1) & 7; c->st[c->fpu_top] = v; }":
                    "static inline void fpush(CPU *c, double v) { if(!ac1_x87_check(c,c->eip)) return; "
                    "c->fpu_top=(c->fpu_top-1)&7; c->st[c->fpu_top]=v; "
                    "c->x87_dirty |= (uint8_t)(1u<<c->fpu_top); ac1_x87_set_tag(c,(unsigned)c->fpu_top,ac1_x87_tag(v)); }",
                "static inline double fpop(CPU *c) { double v = c->st[c->fpu_top]; c->fpu_top = (c->fpu_top + 1) & 7; return v; }":
                    "static inline double fpop(CPU *c) { if(!ac1_x87_check(c,c->eip)) return 0.0; "
                    "double v=c->st[c->fpu_top]; ac1_x87_set_tag(c,(unsigned)c->fpu_top,3); "
                    "c->fpu_top=(c->fpu_top+1)&7; return v; }",
                "static inline double *fst(CPU *c, int i) { return &c->st[(c->fpu_top + i) & 7]; }":
                    "static inline double *fst(CPU *c, int i) { ac1_fp_init(c); unsigned r=(unsigned)(c->fpu_top+i)&7u; "
                    "c->x87_dirty |= (uint8_t)(1u<<r); return &c->st[r]; }",
            }
            for old, new in x87_helpers.items():
                if text.count(old) != 1:
                    raise SystemExit(f"unexpected pinned CPU x87 helper: {old}")
                text = text.replace(old, new)
            todo = "#define RECOMP_TODO(va, text) abort()"
            if text.count(todo) != 1:
                raise SystemExit(f"unexpected pcrecomp RECOMP_TODO hook in {source}")
            text = text.replace(todo, "#define RECOMP_TODO(va, text) ac1_unsupported_instruction((va), (text))")
            access_replacements = {
                "memcpy(&v, guest_execution_ptr(a, 1), 1);": "guest_execution_read(a, &v, 1);",
                "memcpy(&v, guest_execution_ptr(a, 2), 2);": "guest_execution_read(a, &v, 2);",
                "memcpy(&v, guest_execution_ptr(a, 4), 4);": "guest_execution_read(a, &v, 4);",
                "memcpy(&v, guest_execution_ptr(a, 8), 8);": "guest_execution_read(a, &v, 8);",
                "memcpy(guest_execution_ptr(a, 1), &v, 1);": "guest_execution_write(a, &v, 1);",
                "memcpy(guest_execution_ptr(a, 2), &v, 2);": "guest_execution_write(a, &v, 2);",
                "memcpy(guest_execution_ptr(a, 4), &v, 4);": "guest_execution_write(a, &v, 4);",
                "memcpy(guest_execution_ptr(a, 8), &v, 8);": "guest_execution_write(a, &v, 8);",
                "memcpy(&f, guest_execution_ptr(a, 4), 4);": "guest_execution_read(a, &f, 4);",
                "memcpy(&d, guest_execution_ptr(a, 8), 8);": "guest_execution_read(a, &d, 8);",
                "memcpy(&i, guest_execution_ptr(a, 2), 2);": "guest_execution_read(a, &i, 2);",
                "memcpy(&i, guest_execution_ptr(a, 4), 4);": "guest_execution_read(a, &i, 4);",
                "memcpy(&i, guest_execution_ptr(a, 8), 8);": "guest_execution_read(a, &i, 8);",
                "memcpy(guest_execution_ptr(a, 4), &f, 4);": "guest_execution_write(a, &f, 4);",
                "memcpy(guest_execution_ptr(a, 4), &i, 4);": "guest_execution_write(a, &i, 4);",
                "memcpy(guest_execution_ptr(a, 2), &i, 2);": "guest_execution_write(a, &i, 2);",
                "memcpy(guest_execution_ptr(a, 8), &i, 8);": "guest_execution_write(a, &i, 8);",
                "memcpy(&x, guest_execution_ptr(a, 16), 16);": "guest_execution_read(a, &x, 16);",
                "memcpy(guest_execution_ptr(a, 16), &v, 16);": "guest_execution_write(a, &v, 16);",
                "memcpy(b, guest_execution_ptr(a, 10), 10);": "guest_execution_read(a, b, 10);",
                "memcpy(guest_execution_ptr(a, 10), b, 10);": "guest_execution_write(a, b, 10);",
            }
            for old, new in access_replacements.items():
                if text.count(old) == 0:
                    raise SystemExit(f"expected CPU guest-memory operand not found: {old}")
                text = text.replace(old, new)
            if "guest_execution_ptr(a," in text:
                raise SystemExit("CPU header still has an unconverted fixed-width memory operand")
            closing = "#endif /* PCRECOMP_CPU_H */"
            if text.count(closing) != 1 or not text.rstrip().endswith(closing):
                raise SystemExit("unexpected CPU header closing include guard")
            text = text.replace(closing, FP_STATE_HELPERS + "\n" + closing)
            gva = "#define GVA(abs) ((uint32_t)((abs) + g_image_delta))"
            if text.count(gva) != 1:
                raise SystemExit("unexpected CPU header GVA definition")
            # AC1's EXE has no relocation directory and both images load at
            # their preferred bases (checked at load), so absolute guest
            # addresses need no delta. Reading the _Thread_local delta cost an
            # __emutls_get_address call on every global access under MinGW.
            text = text.replace(gva, "#define GVA(abs) ((uint32_t)(abs))  /* AC1: image delta is always 0 */")
            access = "/* ---- memory access: checked uint32 guest VA to native pointer ---- */"
            if text.count(access) != 1:
                raise SystemExit("unexpected CPU header memory-access marker")
            text = text.replace(access, DIRECT_ACCESS_HELPERS + access)
            # The gameplay profile showed ac1_direct_read/write and push/pop as
            # out-of-line functions: in very large generated functions GCC's
            # inline-growth limits stop inlining, turning each guest load into a
            # call plus a variable-size memcpy. Force the tiny accessors inline.
            for name in ("rd8", "rd16", "rd32", "wr8", "wr16", "wr32", "rd64", "wr64",
                         "push32", "pop32"):
                hits = [line for line in text.split("\n")
                        if line.startswith("static inline ") and f" {name}(" in line.split("{")[0]]
                if len(hits) != 1:
                    raise SystemExit(f"unexpected CPU header accessor shape: {name}")
                text = text.replace(hits[0], hits[0].replace(
                    "static inline ", "AC1_ACCESSOR_INLINE ", 1))
            text = masked_flag_helpers(logical_flags_clear_af(text))
            text = x87_inline_helpers(text)
            text, aliases, base_helpers = local_memory_helpers(text)
            alias_path = output.with_name("ac1_local_memory.h")
            if not alias_path.exists() or alias_path.read_text(encoding="utf-8") != aliases:
                alias_path.write_text(aliases, encoding="utf-8")
            provenance["local_memory_helpers"] = base_helpers
            text = flag_local_helpers(text)
            content = text.replace("\r\n", "\n").encode("utf-8")
        if not output.exists() or output.read_bytes() != content:
            output.write_bytes(content)
        provenance["files"].append({
            "source": str(source),
            "destination": destination,
            "sha256": actual_hash,
            "prepared_sha256": hashlib.sha256(content).hexdigest(),
        })
    expected_game_hash = str(CONFIG["sha256"]).lower()
    if len(expected_game_hash) != 64 or any(c not in "0123456789abcdef" for c in expected_game_hash):
        raise SystemExit("config/ac1_dx9.json contains an invalid executable SHA-256")
    fingerprint_text = (
        "/* Generated from config/ac1_dx9.json; do not edit. */\n"
        f'#define AC1_EXPECTED_EXE_SHA256 "{expected_game_hash}"\n'
    )
    if not FINGERPRINT_HEADER.exists() or FINGERPRINT_HEADER.read_text(encoding="utf-8") != fingerprint_text:
        FINGERPRINT_HEADER.write_text(fingerprint_text, encoding="utf-8")
    provenance["expected_game_executable_sha256"] = expected_game_hash
    generated_dir = ROOT / "work"
    generated_dir.mkdir(exist_ok=True)
    provenance_path = generated_dir / "runtime-provenance.json"
    provenance_text = json.dumps(provenance, indent=2) + "\n"
    if not provenance_path.exists() or provenance_path.read_text(encoding="utf-8") != provenance_text:
        provenance_path.write_text(provenance_text, encoding="utf-8")
    print(f"prepared {len(SOURCE_FILES)} pinned runtime files from {DUKE_ROOT}")


if __name__ == "__main__":
    main()
