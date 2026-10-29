#!/usr/bin/env python3
"""Prepare the small pinned runtime subset reused from the local Duke project."""
from __future__ import annotations

import hashlib
import json
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
'''


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


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
                "/* AC1 startup exceeds the donor's 256-region bookkeeping capacity. */\n"
                "#define GUEST_MEMORY_MAX_REGIONS 4096u",
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
        if destination == "src/runtime/guest_memory.c":
            text = content.decode("utf-8").replace("\r\n", "\n")
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
            content = text.replace("\r\n", "\n").encode("utf-8")
        if destination == "src/runtime/cpu.h":
            text = content.decode("utf-8")
            include = "#include <stdint.h>"
            if text.count(include) != 1:
                raise SystemExit(f"unexpected CPU includes: {source}")
            text = text.replace(include, include + "\n#include <math.h>")
            marker = "} CPU;"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected pcrecomp CPU header shape: {source}")
            text = text.replace(
                marker,
                FP_STATE_FIELDS + marker + "\n\n/* Project-owned native dispatcher, called by generated code. */\n"
                "void dispatch(CPU *cpu, uint32_t target);\n"
                "void dispatch_jmp(CPU *cpu, uint32_t target);\n"
                "void ac1_unsupported_instruction(uint32_t guest_va, const char *reason);\n" + FP_STATE_BASIC,
            )
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
