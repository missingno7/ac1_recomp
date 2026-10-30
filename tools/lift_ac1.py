#!/usr/bin/env python3
"""Discover and mechanically lift a bounded AC1 DX9 entry-point call cone.

The original executable is read in place and never normalized or modified.
Catalogs, generated C, and provenance are written below work/lift_initial/.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import importlib.metadata
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# The user's existing projects are read-only references, including caches.
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATH = ROOT / "config/ac1_dx9.json"
DUKE_DEFAULT = Path("D:/Prog/dukemp_recomp/.deps/pcrecomp")
PYTHON_DEFAULT = Path("D:/Prog/dukemp_recomp/.venv/Scripts/python.exe")
OUT_DEFAULT = ROOT / "work/lift_initial"
_TEST_RUNTIME = None
AC1_ARRAY_CTOR_HELPER = 0x00F9FA43
AC1_SCHEMA_CALLBACK_HELPER = 0x00438EF0
AC1_READER_CALLBACK_HELPER = 0x009312F0
AC1_ARG2_CALLBACK_HELPERS = {
    AC1_SCHEMA_CALLBACK_HELPER: "schema_callback",
    AC1_READER_CALLBACK_HELPER: "reader_callback",
}


def prepare_test_runtime() -> Path:
    """Build pinned test headers privately, without changing tracked runtime copies."""
    global _TEST_RUNTIME
    if _TEST_RUNTIME is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("ac1_test_prepare_runtime", ROOT / "tools/prepare_runtime.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.ROOT = ROOT / "work/test_runtime"
        module.FINGERPRINT_HEADER = module.ROOT / "src/runtime/ac1_fingerprint.h"
        module.main()
        _TEST_RUNTIME = module.ROOT / "src/runtime"
    return _TEST_RUNTIME

# Retain the original Havok interval proofs as regression checks. Emission
# now uses CPU-owned MMX registers for every function in AC1 and Bink.
AC1_MMX_FUNCTION_COUNTS = {
    0x01023410: 30,
    0x01023520: 29,
}
AC1_MMX_MNEMONICS = {"movq", "pcmpgtw", "paddw", "psubw", "pand"}

FP_ADAPTER_HELPERS = r"""
/* The legacy SSE reciprocal instructions have implementation-defined low
 * result bits but specified special values and a maximum relative error of
 * 1.5*2^-12. Correctly-rounded scalar C results are tighter approximations;
 * handle the instruction's denormal and NaN rules explicitly first. */
static inline uint32_t ac1_rcpps_lane(uint32_t bits)
{
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t exp = bits & 0x7f800000u;
    const uint32_t frac = bits & 0x007fffffu;
    if (exp == 0x7f800000u) {
        if (frac) return bits | 0x00400000u; /* quiet an SNaN, preserve QNaN */
        return sign;                         /* reciprocal of signed infinity */
    }
    if (exp == 0) {
        if (!frac) return sign | 0x7f800000u; /* signed zero */
        return sign | 0x7f800000u;            /* denormal input is signed zero */
    }
    {
        float x, y;
        uint32_t result;
        memcpy(&x, &bits, sizeof x);
        y = 1.0f / x;
        memcpy(&result, &y, sizeof result);
        /* RCPPS flushes tiny (subnormal) reciprocal results to signed zero. */
        if ((result & 0x7f800000u) == 0) return sign;
        return result;
    }
}

static inline uint32_t ac1_rsqrtss_lane(uint32_t bits)
{
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t mag = bits & 0x7fffffffu;
    const uint32_t exp = bits & 0x7f800000u;
    const uint32_t frac = bits & 0x007fffffu;
    if (exp == 0x7f800000u && frac) return bits | 0x00400000u;
    if (mag == 0 || (exp == 0 && frac))
        return sign | 0x7f800000u;             /* signed zero/denormal */
    if (exp == 0x7f800000u) {
        if (!sign) return 0;                   /* +infinity */
        return 0xffc00000u;                    /* negative infinity: indefinite */
    }
    if (sign) return 0xffc00000u;              /* negative finite: indefinite */
    {
        float x, y;
        uint32_t result;
        memcpy(&x, &bits, sizeof x);
        y = 1.0f / sqrtf(x);
        memcpy(&result, &y, sizeof result);
        return result;
    }
}
""".strip()


def rsqrtps_lanes_code(destination: int, source_expression: str) -> str:
    """Emit packed RSQRTPS by applying the established scalar lane helper."""
    return (
        f"{{ XMM _s = {source_expression}; "
        f"c->xmm[{destination}].u32[0] = ac1_rsqrtss_lane(_s.u32[0]); "
        f"c->xmm[{destination}].u32[1] = ac1_rsqrtss_lane(_s.u32[1]); "
        f"c->xmm[{destination}].u32[2] = ac1_rsqrtss_lane(_s.u32[2]); "
        f"c->xmm[{destination}].u32[3] = ac1_rsqrtss_lane(_s.u32[3]); }}"
    )


def pmaxsw_lanes_code(destination: int, source_expression: str) -> str:
    """Emit PMAXSW over eight signed 16-bit lanes without changing CPU flags."""
    return (
        f"{{ XMM _s = {source_expression}; XMM _d = c->xmm[{destination}]; "
        f"for (unsigned _i = 0; _i < 8; ++_i) "
        f"c->xmm[{destination}].i16[_i] = _d.i16[_i] > _s.i16[_i] "
        f"? _d.i16[_i] : _s.i16[_i]; }}"
    )


def ac1_sse_width_translation(lifter, insn) -> list[str] | None:
    """Lower two proven 64-bit SIMD memory forms without 16-byte overreads."""
    from capstone.x86 import X86_OP_MEM

    ops = insn.operands
    if len(ops) < 2:
        return None
    destination, source = ops[0], ops[1]
    if insn.mnemonic == "movq":
        if lifter._is_xmm(destination):
            dst = lifter._xi(destination)
            if lifter._is_xmm(source):
                bits = f"c->xmm[{lifter._xi(source)}].u64[0]"
            elif source.type == X86_OP_MEM and source.size == 8:
                bits = f"rd64({lifter.addr_expr(insn, source)})"
            else:
                return None
            # Snapshot the source before writing either lane; this also gives
            # the self-move form its required zeroed upper qword.
            return [f"{{ uint64_t _bits = {bits}; c->xmm[{dst}].u64[0] = _bits; "
                    f"c->xmm[{dst}].u64[1] = 0; }}"]
        if (destination.type == X86_OP_MEM and destination.size == 8 and
                lifter._is_xmm(source)):
            return [f"wr64({lifter.addr_expr(insn, destination)}, "
                    f"c->xmm[{lifter._xi(source)}].u64[0]);"]
        return None

    if insn.mnemonic == "cvtdq2pd" and lifter._is_xmm(destination):
        dst = lifter._xi(destination)
        if lifter._is_xmm(source):
            bits = f"c->xmm[{lifter._xi(source)}].u64[0]"
        elif source.type == X86_OP_MEM and source.size == 8:
            bits = f"rd64({lifter.addr_expr(insn, source)})"
        else:
            return None
        # The architectural source is exactly two signed dwords. Decode their
        # two's-complement values explicitly, then convert exactly to binary64.
        return [
            f"{{ uint64_t _bits = {bits}; "
            "int64_t _lo = (int64_t)(uint32_t)_bits; "
            "int64_t _hi = (int64_t)(uint32_t)(_bits >> 32); "
            "if (_lo >= INT64_C(0x80000000)) _lo -= INT64_C(0x100000000); "
            "if (_hi >= INT64_C(0x80000000)) _hi -= INT64_C(0x100000000); "
            f"c->xmm[{dst}].f64[0] = (double)_lo; "
            f"c->xmm[{dst}].f64[1] = (double)_hi; }}"
        ]
    return None


def ac1_mmx_translation(lifter, insn) -> list[str] | None:
    """CPU-owned MMX state, including the SSE-era MMX operand forms."""
    from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_OP_IMM, X86_REG_FS, X86_REG_GS
    m, ops = insn.mnemonic, insn.operands

    def mm(op):
        name = insn.reg_name(op.reg) if op.type == X86_OP_REG else ""
        return int(name[2]) if len(name) == 3 and name[:2] == "mm" and name[2] in "01234567" else None

    if m in ("emms", "femms"):
        return ["ac1_mmx_emms(c);"]
    if not any(mm(op) is not None for op in ops):
        return None
    for op in ops:
        if op.type == X86_OP_MEM and op.mem.segment in (X86_REG_FS, X86_REG_GS):
            raise NotImplementedError(f"segmented MMX operand at 0x{insn.address:08X}: {insn.op_str}")

    def src(op, width=8):
        reg = mm(op)
        if reg is not None:
            return f"ac1_mmx_read(c, {reg}u)"
        if op.type == X86_OP_IMM:
            return f"{op.imm & 255}u"
        if op.type == X86_OP_MEM:
            return f"rd{width*8}({lifter.addr_expr(insn, op)})"
        name = insn.reg_name(op.reg)
        if name.startswith("xmm"):
            return f"c->xmm[{int(name[3:])}].u64[0]"
        return lifter.src(insn, op)

    def write(value):
        reg = mm(ops[0])
        if reg is not None:
            return [f"ac1_mmx_write(c, {reg}u, {value});"]
        if ops[0].type == X86_OP_MEM:
            width = 4 if m == "movd" else 8
            return [f"wr{width*8}({lifter.addr_expr(insn, ops[0])}, ({'uint32_t' if width==4 else 'uint64_t'})({value}));"]
        name = insn.reg_name(ops[0].reg)
        if name.startswith("xmm"):
            return [f"c->xmm[{int(name[3:])}].u64[0] = {value};",
                    f"c->xmm[{int(name[3:])}].u64[1] = 0;"]
        return [lifter.dst_write(insn, ops[0], f"(uint32_t)({value})")]

    prefix = ["ac1_mmx_enter(c);"]
    if m in ("movq", "movd", "movntq", "movq2dq", "movdq2q"):
        value = src(ops[1], 4 if m == "movd" else 8)
        if m == "movd":
            value = f"(uint32_t)({value})"
        return prefix + write(value)
    if m in ("cvtps2pi", "cvttps2pi"):
        return prefix + write(f"ac1_mmx_from_ps(c, {src(ops[1])}, {int(m=='cvttps2pi')}u, 0x{insn.address:08X}u)")
    if m == "cvtpi2ps":
        name = insn.reg_name(ops[0].reg)
        return prefix + [f"c->xmm[{int(name[3:])}].u64[0] = ac1_mmx_to_ps(c, {src(ops[1])}, 0x{insn.address:08X}u);"]
    if m == "pshufw":
        return prefix + write(f"ac1_mmx_shuffle({src(ops[1])}, {ops[2].imm & 255}u)")
    if m == "pinsrw":
        lane = ops[2].imm & 3
        return prefix + write(f"({src(ops[0])} & ~(UINT64_C(0xffff) << {lane*16})) | ((uint64_t)(({src(ops[1],2)}) & 0xffffu) << {lane*16})")
    if m == "pextrw":
        return prefix + write(f"({src(ops[1])} >> {(ops[2].imm & 3)*16}) & 0xffffu")
    if m == "pmovmskb":
        return prefix + write(f"ac1_mmx_mask({src(ops[1])})")
    if m == "maskmovq":
        return prefix + [f"{{ uint64_t _v={src(ops[0])}, _mask={src(ops[1])}; "
                         "for(unsigned _i=0;_i<8;++_i) if((_mask>>(_i*8+7))&1u) "
                         "wr8(c->edi+_i,(uint8_t)(_v>>(_i*8))); }"]
    a, b = src(ops[0]), src(ops[1])
    logic = {"pand": f"({a}) & ({b})", "pandn": f"~({a}) & ({b})",
             "por": f"({a}) | ({b})", "pxor": f"({a}) ^ ({b})"}
    if m in logic:
        return prefix + write(logic[m])
    if m in ("packsswb", "packssdw", "packuswb"):
        return prefix + write(f"ac1_mmx_pack({a}, {b}, {32 if m=='packssdw' else 16}u, {int(m=='packuswb')}u)")
    if m.startswith(("punpckl", "punpckh")) and m[-2:] in ("bw", "wd", "dq"):
        return prefix + write(f"ac1_mmx_unpack({a}, {b}, {dict(bw=8,wd=16,dq=32)[m[-2:]]}u, {int(m.startswith('punpckh'))}u)")
    if m.startswith(("psll", "psrl", "psra")) and m[-1] in "wdq":
        return prefix + write(f"ac1_mmx_shift({a}, {b}, {dict(w=16,d=32,q=64)[m[-1]]}u, {dict(psll=0,psrl=1,psra=2)[m[:4]]}u)")
    if m == "pmaddwd":
        return prefix + write(f"ac1_mmx_madd({a}, {b})")
    if m == "pmuludq":
        return prefix + write(f"(uint64_t)(uint32_t)({a}) * (uint32_t)({b})")
    if m == "psadbw":
        return prefix + write(f"ac1_mmx_sad({a}, {b})")
    lane_ops = {"padd": 0, "psub": 1, "padds": 2, "psubs": 3,
                "paddus": 4, "psubus": 5, "pcmpeq": 6, "pcmpgt": 7,
                "pavg": 8, "pminu": 9, "pmaxu": 10, "pmins": 11, "pmaxs": 12}
    special = {"pmullw": (16,13), "pmulhw": (16,14), "pmulhuw": (16,15)}
    if m in special:
        bits, operation = special[m]
    elif m[-1] in "bwdq" and m[:-1] in lane_ops:
        bits, operation = dict(b=8,w=16,d=32,q=64)[m[-1]], lane_ops[m[:-1]]
    else:
        raise NotImplementedError(f"unimplemented MMX at 0x{insn.address:08X}: {m} {insn.op_str}")
    return prefix + write(f"ac1_mmx_lanes({a}, {b}, {bits}u, {operation}u)")


def ac1_state_translation(lifter, insn) -> list[str] | None:
    """State-management instructions shared by AC1 and Bink's exact emitter."""
    from capstone.x86 import X86_OP_MEM
    m, ops = insn.mnemonic, insn.operands
    if m in ("pushal", "pushad"):
        return ["{ uint32_t _esp=c->esp; push32(c,c->eax); push32(c,c->ecx); push32(c,c->edx); "
                "push32(c,c->ebx); push32(c,_esp); push32(c,c->ebp); push32(c,c->esi); push32(c,c->edi); }"]
    if m in ("popal", "popad"):
        return ["c->edi=pop32(c); c->esi=pop32(c); c->ebp=pop32(c); (void)pop32(c); "
                "c->ebx=pop32(c); c->edx=pop32(c); c->ecx=pop32(c); c->eax=pop32(c);"]
    if m in ("fninit", "finit"):
        return ["ac1_x87_reset(c);"]
    if m in ("fnstenv", "fstenv", "fldenv", "fnsave", "fsave", "frstor"):
        if 0x66 in insn.prefix:
            return [f'ac1_unsupported_instruction(0x{insn.address:08X}u,"16-bit x87 environment layout"); return;']
        helper = {"fnstenv":"store_env", "fstenv":"store_env", "fldenv":"load_env",
                  "fnsave":"save", "fsave":"save", "frstor":"restore"}[m]
        return [f"ac1_x87_{helper}(c, {lifter.addr_expr(insn,ops[0])});"]
    if m in ("fnstcw", "fstcw", "fldcw", "stmxcsr", "ldmxcsr", "fnstsw", "fstsw"):
        prefix = ["ac1_fp_init(c);"]
        if m in ("fnstcw", "fstcw"):
            return prefix + [lifter.dst_write(insn,ops[0],"c->fpu_cw")]
        if m == "fldcw":
            return prefix + [f"c->fpu_cw=(uint16_t)({lifter.src(insn,ops[0])});"]
        if m in ("fnstsw", "fstsw"):
            return prefix + [lifter.dst_write(insn,ops[0],"ac1_x87_status(c)")]
        if m == "stmxcsr":
            return prefix + [lifter.dst_write(insn,ops[0],"c->mxcsr")]
        return prefix + [f"{{ uint32_t _mx={lifter.src(insn,ops[0])}; "
                         f'if(_mx&0xffff0000u) {{ ac1_unsupported_instruction(0x{insn.address:08X}u,"reserved MXCSR bits"); return; }} '
                         "c->mxcsr=_mx; }"]
    if m in ("fnclex", "fclex"):
        return ["ac1_fp_init(c); c->fpu_sw &= ~0x80ffu;"]
    if m in ("fincstp", "fdecstp"):
        return [f"ac1_fp_init(c); c->fpu_top=(c->fpu_top+{1 if m=='fincstp' else 7})&7; c->fpu_sw &= ~0x200u;"]
    if m in ("ffree", "ffreep"):
        name = insn.reg_name(ops[0].reg)
        index = lifter._st_idx(ops[0])
        return [f"ac1_fp_init(c); ac1_x87_set_tag(c,(unsigned)(c->fpu_top+{index})&7u,3);" +
                (" c->fpu_top=(c->fpu_top+1)&7;" if m=="ffreep" else "")]
    if m in ("fist", "fistp", "fisttp") and ops and ops[0].type == X86_OP_MEM:
        bits = ops[0].size*8
        return [f"{{ uint64_t _i=ac1_x87_integer(c,*fst(c,0),{bits}u,{int(m=='fisttp')}u,0x{insn.address:08X}u); "
                f"wr{bits}({lifter.addr_expr(insn,ops[0])},(uint{bits}_t)_i); " +
                ("fpop(c);" if m != "fist" else "") + " }"]
    if m == "frndint":
        return ["*fst(c,0)=ac1_fp_round(*fst(c,0),(c->fpu_cw>>10)&3u);"]
    return None


def validate_ac1_mmx_interval(function_address: int, spec: dict, decoded: dict) -> None:
    """Pin the two closed MMX intervals and prove every CFG path initializes reads."""
    expected_count = AC1_MMX_FUNCTION_COUNTS.get(function_address)
    if expected_count is None:
        return
    insns = [decoded[va] for va in sorted(decoded)]
    mmx_count = sum(insn.mnemonic in AC1_MMX_MNEMONICS or insn.mnemonic == "emms"
                    for insn in insns)
    if mmx_count != expected_count:
        raise RuntimeError(
            f"AC1 MMX interval at 0x{function_address:08X} changed: "
            f"expected {expected_count} instructions, found {mmx_count}")
    first = next(i for i, insn in enumerate(insns)
                 if insn.mnemonic in AC1_MMX_MNEMONICS)
    endings = [i for i, insn in enumerate(insns) if insn.mnemonic == "emms"]
    if len(endings) != 1:
        raise RuntimeError(f"AC1 MMX interval at 0x{function_address:08X} needs one EMMS")
    last = endings[0]
    if last <= first:
        raise RuntimeError(f"AC1 MMX interval at 0x{function_address:08X} ends before it starts")
    from capstone.x86 import X86_OP_MEM, X86_OP_REG

    def mm_register(insn, operand) -> int | None:
        if operand.type != X86_OP_REG or operand.size != 8:
            return None
        name = insn.reg_name(operand.reg)
        if len(name) == 3 and name.startswith("mm") and name[2] in "01234567":
            return int(name[2])
        return None

    for insn in insns[first:last + 1]:
        if insn.mnemonic.startswith("f") or insn.mnemonic in {"call", "lcall"}:
            raise RuntimeError(
                f"AC1 MMX/x87 mixed interval at 0x{function_address:08X}: "
                f"{insn.mnemonic} at 0x{insn.address:08X}")
    blocks = {block["start"]: block for block in spec.get("blocks", [])}
    if function_address not in blocks:
        raise RuntimeError(f"AC1 MMX function entry 0x{function_address:08X} has no CFG block")
    reachable = set()
    queue = collections.deque([function_address])
    while queue:
        address = queue.popleft()
        if address in reachable:
            continue
        reachable.add(address)
        block = blocks[address]
        for successor in block["successors"]:
            if successor not in blocks:
                raise RuntimeError(
                    f"AC1 MMX CFG successor 0x{successor:08X} has no block")
            queue.append(successor)

    def mmx_reads_writes(insn) -> tuple[int, int]:
        """Return (read mask, write mask) for exactly the supported MMX forms."""
        destination, source = insn.operands
        dst = mm_register(insn, destination)
        src = mm_register(insn, source)
        if insn.mnemonic == "movq":
            if dst is not None:
                if src is not None:
                    return 1 << src, 1 << dst
                if source.type == X86_OP_MEM and source.size == 8:
                    return 0, 1 << dst
            elif (destination.type == X86_OP_MEM and destination.size == 8 and
                  src is not None):
                return 1 << src, 0
            raise RuntimeError(f"unexpected MMX MOVQ form at 0x{insn.address:08X}")
        if insn.mnemonic in {"pcmpgtw", "paddw", "psubw", "pand"} and dst is not None:
            if src is not None:
                return (1 << dst) | (1 << src), 1 << dst
            if source.type == X86_OP_MEM and source.size == 8:
                return 1 << dst, 1 << dst
        raise RuntimeError(f"unexpected MMX instruction form at 0x{insn.address:08X}")

    def block_transfer(start: int, mask: int, *, check_reads: bool) -> int:
        for va, _size in blocks[start]["instructions"]:
            insn = decoded[va]
            if insn.mnemonic == "emms":
                continue
            if insn.mnemonic not in AC1_MMX_MNEMONICS:
                continue
            reads, writes = mmx_reads_writes(insn)
            if check_reads and (mask & reads) != reads:
                missing = reads & ~mask
                register = (missing & -missing).bit_length() - 1
                raise RuntimeError(
                    f"AC1 MMX mm{register} is not initialized on every path before "
                    f"0x{insn.address:08X}")
            mask |= writes
        return mask

    all_registers = 0xff
    in_masks = {address: all_registers for address in reachable}
    out_masks = {address: all_registers for address in reachable}
    in_masks[function_address] = 0
    changed = True
    while changed:
        changed = False
        for address in sorted(reachable):
            if address == function_address:
                incoming = 0
            else:
                predecessors = [source for source in reachable
                                if address in blocks[source]["successors"]]
                if not predecessors:
                    incoming = 0
                else:
                    incoming = all_registers
                    for predecessor in predecessors:
                        incoming &= out_masks[predecessor]
            outgoing = block_transfer(address, incoming, check_reads=False)
            if in_masks[address] != incoming or out_masks[address] != outgoing:
                in_masks[address] = incoming
                out_masks[address] = outgoing
                changed = True
    for address in sorted(reachable):
        block_transfer(address, in_masks[address], check_reads=True)

    # Track both possibilities at joins, so every function exit must execute
    # EMMS, including the zero-count early-exit edge.
    pending = collections.deque([(function_address, False)])
    visited = set()
    while pending:
        address, emms_seen = pending.popleft()
        state = (address, emms_seen)
        if state in visited:
            continue
        visited.add(state)
        block = blocks[address]
        state_after = emms_seen
        for va, _size in block["instructions"]:
            insn = decoded[va]
            if insn.mnemonic == "emms":
                state_after = True
            elif state_after and insn.mnemonic in AC1_MMX_MNEMONICS:
                raise RuntimeError(
                    f"AC1 MMX instruction occurs after EMMS at 0x{insn.address:08X}")
        if block["is_exit"]:
            if not state_after:
                raise RuntimeError(
                    f"AC1 MMX function exit at 0x{block['end']:08X} bypasses EMMS")
            continue
        for successor in block["successors"]:
            pending.append((successor, state_after))


def ac1_int3_translation(insn) -> list[str]:
    """Keep a reachable guest breakpoint as a precise runtime trap."""
    return [
        f'ac1_unsupported_instruction(0x{insn.address:08X}u, "guest INT3 breakpoint");'
    ]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_text_if_changed(path: Path, text: str) -> bool:
    """Preserve output mtimes unless the generated content changed."""
    data = text.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def write_report_if_inputs_unchanged(path: Path, text: str,
                                     inputs: dict[str, tuple[Path, str]]) -> None:
    """Refuse to publish a success report if any generation input changed."""
    changed = []
    for name, (input_path, expected_hash) in inputs.items():
        try:
            actual_hash = sha256(input_path)
        except OSError as exc:
            changed.append(f"{name} ({input_path}: {exc})")
            continue
        if actual_hash != expected_hash:
            changed.append(f"{name} ({input_path})")
    if changed:
        raise RuntimeError("generation inputs changed during lift: " + ", ".join(changed))
    write_text_if_changed(path, text)


def generation_inputs_unchanged(inputs: dict[str, tuple[Path, str]]) -> bool:
    """Return whether the exact inputs captured before generation still match."""
    for input_path, expected_hash in inputs.values():
        try:
            if sha256(input_path) != expected_hash:
                return False
        except OSError:
            return False
    return True


def invalidate_success_report(path: Path) -> None:
    """Remove a prior success marker before any outputs for a new run change."""
    path.unlink(missing_ok=True)


def run_input_fingerprint_selftest() -> None:
    """Prove stable inputs publish a report and a changed input cannot."""
    with tempfile.TemporaryDirectory(prefix="lift_input_guard_") as temp_name:
        temp = Path(temp_name)
        config_path = temp / "config.json"
        seed_path = temp / "seeds.json"
        config_path.write_bytes(b'{"depth": 2}')
        seed_path.write_bytes(b'[4198400]')
        inputs = {
            "config": (config_path, sha256_bytes(config_path.read_bytes())),
            "seed": (seed_path, sha256_bytes(seed_path.read_bytes())),
        }
        success_report = temp / "success.json"
        write_report_if_inputs_unchanged(success_report, '{"ok":true}\n', inputs)
        assert json.loads(success_report.read_text(encoding="utf-8"))["ok"] is True

        seed_path.write_bytes(b'[4198401]')
        blocked_report = temp / "blocked.json"
        blocked_report.write_text('{"ok":true}\n', encoding="utf-8")
        invalidate_success_report(blocked_report)
        assert not blocked_report.exists()
        # A failed verification after invalidation must not leave the previous
        # success marker available to a build that inspects the output folder.
        try:
            write_report_if_inputs_unchanged(blocked_report, '{"ok":true}\n', inputs)
        except RuntimeError as exc:
            assert "seed" in str(exc)
        else:
            raise AssertionError("input-change guard accepted a modified seed file")
        assert not blocked_report.exists()


def entry_guard_lines(function_start: int, ordered_addresses: list[int]) -> list[str]:
    """Return the branch needed when a backward edge sorts before entry."""
    if not ordered_addresses or function_start not in ordered_addresses:
        raise ValueError("recovered instruction set must contain the function entry")
    if ordered_addresses[0] == function_start:
        return []
    return [f"    goto L_{function_start:08X};"]


def direct_tail_thunk_target(function) -> int | None:
    """Return the exact target of a one-instruction direct unconditional thunk."""
    blocks = list(function.blocks.values())
    if len(blocks) != 1 or len(blocks[0].instructions) != 1:
        return None
    insn = blocks[0].instructions[0]
    if insn.mnemonic != "jmp" or len(insn.operands) != 1:
        return None
    from capstone.x86 import X86_OP_IMM
    if insn.operands[0].type != X86_OP_IMM:
        return None
    return insn.operands[0].imm & 0xFFFFFFFF


def terminal_external_direct_jump_targets(function) -> list[int]:
    """Return external targets of terminal direct JMPs in recovered CFG blocks.

    This catches real tail transfers in adapter functions that do work before
    their final jump, while excluding local CFG edges and targets inside any
    instruction span already decoded for the same function.
    """
    from capstone.x86 import X86_OP_IMM

    blocks = list(function.blocks.values())
    local_starts = {block.start for block in blocks}
    instruction_spans = [
        (insn.address, insn.address + insn.size)
        for block in blocks for insn in block.instructions
    ]
    targets = set()
    for block in blocks:
        if not block.instructions:
            continue
        insn = block.instructions[-1]
        if (insn.mnemonic != "jmp" or len(insn.operands) != 1 or
                insn.operands[0].type != X86_OP_IMM):
            continue
        target = insn.operands[0].imm & 0xFFFFFFFF
        if target in local_starts:
            continue
        if any(start <= target < end for start, end in instruction_spans):
            continue
        targets.add(target)
    return sorted(targets)


def fallthrough_target(mnemonic: str, next_pc: int, next_decoded: int | None) -> int | None:
    """Return the real next guest VA when sorted C would cross a code gap."""
    terminal = {"jmp", "ret", "retn", "retf", "iret", "iretd", "hlt", "int3"}
    if mnemonic.split()[-1] in terminal:
        return None
    return None if next_decoded == next_pc else next_pc


def function_tu_bucket(address: int, bucket_count: int) -> int:
    """Map a guest VA to a stable generated-TU bucket, independent of catalog size."""
    if bucket_count < 1:
        raise ValueError("translation unit bucket count must be positive")
    mixed = (((address & 0xFFFFFFFF) >> 2) * 2654435761) & 0xFFFFFFFF
    mixed ^= mixed >> 16
    return mixed % bucket_count


def partition_functions_by_bucket(functions: list[dict], bucket_count: int) -> dict[int, list[dict]]:
    """Partition functions by stable address hash while preserving address order."""
    if bucket_count < 1:
        raise ValueError("translation unit bucket count must be positive")
    buckets = {index: [] for index in range(bucket_count)}
    seen = set()
    for function in functions:
        address = function["address"]
        if address in seen:
            raise ValueError(f"duplicate function address 0x{address:08X}")
        seen.add(address)
        buckets[function_tu_bucket(address, bucket_count)].append(function)
    for bucket in buckets.values():
        bucket.sort(key=lambda function: function["address"])
    return buckets


def run_selftest() -> None:
    # A backward branch can make a lower-address block precede the actual
    # function entry in the address-sorted output. Execution must jump over it.
    assert entry_guard_lines(0x401008, [0x401000, 0x401008]) == [
        "    goto L_00401008;"
    ]
    assert entry_guard_lines(0x401000, [0x401000, 0x401008]) == []
    assert fallthrough_target("mov", 0x401101, 0x401101) is None
    assert fallthrough_target("mov", 0x401101, 0x401108) == 0x401101
    assert fallthrough_target("je", 0x401106, 0x401110) == 0x401106
    assert fallthrough_target("jmp", 0x401108, 0x401110) is None
    # Adding one new low-address root changes only its fixed hash bucket. The
    # bucket count is explicit and does not depend on the catalog size.
    partition_input = [{"address": address} for address in
                       (0x401000, 0x401018, 0x401034, 0x402008, 0x405100)]
    partition_before = partition_functions_by_bucket(partition_input, 100)
    inserted = {"address": 0x400FF0}
    partition_after = partition_functions_by_bucket(partition_input + [inserted], 100)
    assert sorted(item["address"] for items in partition_before.values() for item in items) == \
        sorted(item["address"] for item in partition_input)
    assert sorted(item["address"] for items in partition_after.values() for item in items) == \
        sorted([item["address"] for item in partition_input] + [inserted["address"]])
    changed_buckets = [index for index in partition_before
                       if partition_before[index] != partition_after[index]]
    assert changed_buckets == [function_tu_bucket(inserted["address"], 100)]
    # FXAM's condition-code nibble for the modeled nonempty binary64 x87 value:
    # NaN, infinity, zero, and every other finite nonzero value. Binary64
    # subnormals load as normal values in the wider x87 format. This CPU model
    # does not track empty x87 tags, so no empty-stack result is claimed here.
    def fxam_cc(value: float) -> int:
        import math
        if math.isnan(value):
            cc = 0x0100
        elif math.isinf(value):
            cc = 0x0500
        elif value == 0.0:
            cc = 0x4000
        else:
            cc = 0x0400
        return cc | (0x0200 if math.copysign(1.0, value) < 0 else 0)
    def fxam_status(sw: int, value: float) -> int:
        return (sw & ~0x4700) | fxam_cc(value)
    import math
    assert fxam_cc(0.0) == 0x4000
    assert fxam_cc(-0.0) == 0x4200
    assert fxam_cc(1.0) == 0x0400
    assert fxam_cc(-1.0) == 0x0600
    assert fxam_cc(math.inf) == 0x0500
    assert fxam_cc(-math.inf) == 0x0700
    assert fxam_cc(math.nan) == 0x0100
    assert fxam_cc(math.ldexp(1.0, -1074)) == 0x0400
    seed_sw = 0xA55F
    assert fxam_status(seed_sw, -0.0) == ((seed_sw & ~0x4700) | 0x4200)
    # RSQRTPS uses a source snapshot so register aliasing is safe, and all
    # four independent lanes go through the same edge-aware scalar helper.
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from capstone.x86 import X86_OP_MEM, X86_OP_REG
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    class MMXFixtureLifter:
        @staticmethod
        def addr_expr(insn, operand):
            return "0x12345678u"

    mmx_cases = [
        ("0f6f442408", "ac1_mmx_write", "rd64(0x12345678u)"),
        ("0f7f442408", "wr64(0x12345678u,", "ac1_mmx_read"),
        ("0f6fc1", "ac1_mmx_write", "ac1_mmx_read"),
        ("0f65c1", "ac1_mmx_lanes", "ac1_mmx_read"),
        ("0ffdc1", "ac1_mmx_lanes", "ac1_mmx_read"),
        ("0ff9c1", "ac1_mmx_lanes", "ac1_mmx_read"),
        ("0fdbc1", "ac1_mmx_write", "ac1_mmx_read"),
        ("0f77", "ac1_mmx_emms", ""),
    ]
    for raw, required, also_required in mmx_cases:
        fixture = next(md.disasm(bytes.fromhex(raw), 0x00401100))
        emitted = ac1_mmx_translation(MMXFixtureLifter(), fixture)
        text = " ".join(emitted or [])
        assert emitted and required in text and also_required in text, (raw, fixture.mnemonic, text)
    assert "rd64(0x12345678u)" in " ".join(
        ac1_mmx_translation(MMXFixtureLifter(), next(md.disasm(bytes.fromhex("0f6f442408"), 0x401100))))
    assert "wr64(0x12345678u," in " ".join(
        ac1_mmx_translation(MMXFixtureLifter(), next(md.disasm(bytes.fromhex("0f7f442408"), 0x401100))))
    # Pin that the bounded local-state validator accepts a zero-count path that
    # bypasses the loop but still reaches EMMS, and rejects a join that reads
    # MM1 on a path that skipped its MOVQ definition.
    mmx_fixture_start = 0x00401100
    valid_mmx_bytes = bytes.fromhex("0f6f44240885c974060f6f080f65c10f77c3")
    valid_insns = list(md.disasm(valid_mmx_bytes, mmx_fixture_start))
    valid_decoded = {insn.address: insn for insn in valid_insns}
    assert [insn.mnemonic for insn in valid_insns] == [
        "movq", "test", "je", "movq", "pcmpgtw", "emms", "ret"
    ]
    valid_spec = {"blocks": [
        {"start": 0x00401100, "end": 0x00401109, "successors": [0x00401109, 0x0040110F],
         "is_exit": False, "instructions": [[i.address, i.size] for i in valid_insns[:3]]},
        {"start": 0x00401109, "end": 0x0040110F, "successors": [0x0040110F],
         "is_exit": False, "instructions": [[i.address, i.size] for i in valid_insns[3:5]]},
        {"start": 0x0040110F, "end": 0x00401112, "successors": [],
         "is_exit": True, "instructions": [[i.address, i.size] for i in valid_insns[5:]]},
    ]}
    previous_count = AC1_MMX_FUNCTION_COUNTS.get(mmx_fixture_start)
    AC1_MMX_FUNCTION_COUNTS[mmx_fixture_start] = 4
    try:
        validate_ac1_mmx_interval(mmx_fixture_start, valid_spec, valid_decoded)
        invalid_mmx_bytes = bytes.fromhex("0f6f44240885c974030f6f080f65c10f77c3")
        invalid_insns = list(md.disasm(invalid_mmx_bytes, mmx_fixture_start))
        invalid_decoded = {insn.address: insn for insn in invalid_insns}
        invalid_spec = {"blocks": [
            {"start": 0x00401100, "end": 0x00401109, "successors": [0x00401109, 0x0040110C],
             "is_exit": False, "instructions": [[i.address, i.size] for i in invalid_insns[:3]]},
            {"start": 0x00401109, "end": 0x0040110C, "successors": [0x0040110C],
             "is_exit": False, "instructions": [[invalid_insns[3].address, invalid_insns[3].size]]},
            {"start": 0x0040110C, "end": 0x00401112, "successors": [],
             "is_exit": True, "instructions": [[i.address, i.size] for i in invalid_insns[4:]]},
        ]}
        try:
            validate_ac1_mmx_interval(mmx_fixture_start, invalid_spec, invalid_decoded)
        except RuntimeError as exc:
            assert "not initialized on every path" in str(exc)
        else:
            raise AssertionError("MMX validator accepted an uninitialized register at a CFG join")
    finally:
        if previous_count is None:
            del AC1_MMX_FUNCTION_COUNTS[mmx_fixture_start]
        else:
            AC1_MMX_FUNCTION_COUNTS[mmx_fixture_start] = previous_count
    int3_fixture = next(md.disasm(b"\xCC", 0x00401234))
    int3_code = ac1_int3_translation(int3_fixture)
    assert int3_fixture.mnemonic == "int3" and int3_code == [
        'ac1_unsupported_instruction(0x00401234u, "guest INT3 breakpoint");'
    ]
    int3_compiler = shutil.which("gcc")
    if int3_compiler:
        with tempfile.TemporaryDirectory(prefix="int3_selftest_", dir=ROOT / "work") as temp_name:
            temp_dir = Path(temp_name)
            source = temp_dir / "int3_trap.c"
            executable = temp_dir / "int3_trap.exe"
            source.write_text(
                "#include <setjmp.h>\n#include <stdint.h>\n#include <string.h>\n"
                "static jmp_buf trap_env; static uint32_t trap_va; "
                "static const char *trap_reason; static int fell_through;\n"
                "void ac1_unsupported_instruction(uint32_t va, const char *reason) { "
                "trap_va = va; trap_reason = reason; longjmp(trap_env, 1); }\n"
                "static void run_int3(void) { " + int3_code[0] +
                " fell_through = 1; }\n"
                "int main(void) { if (setjmp(trap_env) == 0) { run_int3(); return 2; } "
                "return fell_through || trap_va != 0x00401234u || "
                "strcmp(trap_reason, \"guest INT3 breakpoint\") != 0; }\n",
                encoding="utf-8")
            build = subprocess.run(
                [int3_compiler, "-std=c11", "-O0", str(source), "-o", str(executable)],
                text=True, capture_output=True)
            if build.returncode:
                raise AssertionError("INT3 trap self-test compile failed:\n" +
                                     build.stdout + build.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            if result.returncode:
                raise AssertionError("INT3 trap did not stop synthetic execution:\n" +
                                     result.stdout + result.stderr)
    else:
        print("INT3 executed-trap self-test skipped (gcc not found)")
    rsqrt_reg = next(md.disasm(bytes.fromhex("0f52c1"), 0x401000))
    rsqrt_mem = next(md.disasm(bytes.fromhex("0f52542410"), 0x401000))
    assert rsqrt_reg.mnemonic == rsqrt_mem.mnemonic == "rsqrtps"
    assert rsqrt_reg.operands[0].type == X86_OP_REG and rsqrt_reg.operands[1].type == X86_OP_REG
    assert rsqrt_mem.operands[0].type == X86_OP_REG and rsqrt_mem.operands[1].type == X86_OP_MEM
    reg_code = rsqrtps_lanes_code(0, "c->xmm[1]")
    mem_code = rsqrtps_lanes_code(2, "rdxm(0x1234u)")
    for lane in range(4):
        assert f"c->xmm[0].u32[{lane}] = ac1_rsqrtss_lane(_s.u32[{lane}])" in reg_code
        assert f"c->xmm[2].u32[{lane}] = ac1_rsqrtss_lane(_s.u32[{lane}])" in mem_code
    assert "XMM _s = c->xmm[1]" in reg_code and "XMM _s = rdxm(0x1234u)" in mem_code
    pmax_reg = next(md.disasm(bytes.fromhex("660feec1"), 0x401100))
    pmax_mem = next(md.disasm(bytes.fromhex("660fee542410"), 0x401100))
    assert pmax_reg.mnemonic == pmax_mem.mnemonic == "pmaxsw"
    assert pmax_reg.operands[0].type == X86_OP_REG and pmax_reg.operands[1].type == X86_OP_REG
    assert pmax_mem.operands[0].type == X86_OP_REG and pmax_mem.operands[1].type == X86_OP_MEM
    pmax_reg_code = pmaxsw_lanes_code(0, "c->xmm[1]")
    pmax_mem_code = pmaxsw_lanes_code(2, "rdxm(0x1234u)")
    pmax_alias_code = pmaxsw_lanes_code(3, "c->xmm[3]")
    assert "XMM _s = c->xmm[1]; XMM _d = c->xmm[0]" in pmax_reg_code
    assert "XMM _s = rdxm(0x1234u); XMM _d = c->xmm[2]" in pmax_mem_code
    assert "XMM _s = c->xmm[3]; XMM _d = c->xmm[3]" in pmax_alias_code
    assert "_d.i16[_i] > _s.i16[_i]" in pmax_reg_code
    compiler = shutil.which("gcc")
    if compiler:
        with tempfile.TemporaryDirectory(prefix="pmaxsw_selftest_", dir=ROOT / "work") as temp_name:
            temp_dir = Path(temp_name)
            source = temp_dir / "pmaxsw_semantics.c"
            executable = temp_dir / "pmaxsw_semantics.exe"
            source.write_text(
                "#include <stdint.h>\n#include <string.h>\n#include <emmintrin.h>\n"
                "typedef union { int16_t i16[8]; uint16_t u16[8]; uint32_t u32[4]; } XMM;\n"
                "typedef struct { uint32_t gpr[8]; uint32_t flags[8]; XMM xmm[8]; } CPU;\n"
                "static XMM guest_vector;\n"
                "static XMM rdxm(uint32_t address) { (void)address; return guest_vector; }\n"
                "static void run_reg(CPU *c) { " + pmax_reg_code + " }\n"
                "static void run_mem(CPU *c) { " + pmax_mem_code + " }\n"
                "static void run_alias(CPU *c) { " + pmax_alias_code + " }\n"
                + r"""
static void native_max(const int16_t a[8], const int16_t b[8], int16_t out[8])
{
    __m128i va, vb, vo;
    memcpy(&va, a, 16); memcpy(&vb, b, 16);
    vo = _mm_max_epi16(va, vb);
    memcpy(out, &vo, 16);
}
static int unchanged_except_xmm(CPU *after, const CPU *before, unsigned dst)
{
    if (memcmp(after->gpr, before->gpr, sizeof after->gpr) ||
        memcmp(after->flags, before->flags, sizeof after->flags)) return 0;
    for (unsigned i = 0; i < 8; ++i)
        if (i != dst && memcmp(&after->xmm[i], &before->xmm[i], sizeof(XMM))) return 0;
    return 1;
}
static int check(const int16_t a[8], const int16_t b[8])
{
    CPU reg = {0}, mem = {0}, alias = {0};
    CPU reg_before, mem_before, alias_before;
    int16_t expected[8];
    native_max(a, b, expected);
    for (unsigned i = 0; i < 8; ++i) {
        reg.xmm[0].i16[i] = a[i]; reg.xmm[1].i16[i] = b[i];
        mem.xmm[2].i16[i] = a[i]; guest_vector.i16[i] = b[i];
        alias.xmm[3].i16[i] = a[i];
    }
    for (unsigned i = 0; i < 8; ++i) {
        reg.gpr[i] = mem.gpr[i] = alias.gpr[i] = 0x12340000u + i;
        reg.flags[i] = mem.flags[i] = alias.flags[i] = 0xA5A50000u + i;
    }
    reg_before = reg; mem_before = mem; alias_before = alias;
    run_reg(&reg); run_mem(&mem); run_alias(&alias);
    if (!unchanged_except_xmm(&reg, &reg_before, 0) ||
        !unchanged_except_xmm(&mem, &mem_before, 2) ||
        !unchanged_except_xmm(&alias, &alias_before, 3)) return 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (reg.xmm[0].i16[i] != expected[i] || mem.xmm[2].i16[i] != expected[i] ||
            alias.xmm[3].i16[i] != a[i]) return 0;
    }
    return 1;
}
int main(void)
{
    static const int16_t a[8] = {INT16_MIN, INT16_MAX, -1, 0, -32767, 32766, 1, -2};
    static const int16_t b[8] = {INT16_MAX, INT16_MIN, 0, -1, 32767, -32768, -1, 1};
    static const int16_t c[8] = {-1, 1, INT16_MIN, INT16_MAX, 0, -20000, 20000, -32768};
    return !(check(a, b) && check(b, c) && check(c, a));
}
""", encoding="utf-8")
            build = subprocess.run(
                [compiler, "-std=c11", "-O0", "-msse2", str(source), "-o", str(executable)],
                text=True, capture_output=True)
            if build.returncode:
                raise AssertionError("PMAXSW native SSE2 self-test compile failed:\n" +
                                     build.stdout + build.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            if result.returncode:
                raise AssertionError("PMAXSW/native SSE2 comparison failed:\n" +
                                     result.stdout + result.stderr)
    else:
        print("PMAXSW native SSE2 self-test skipped (gcc not found)")
    from types import SimpleNamespace
    def thunk_fixture(address: int, code: bytes):
        instructions = list(md.disasm(code, address))
        return SimpleNamespace(blocks={address: SimpleNamespace(instructions=instructions)})
    def rel_jmp(address: int, target: int) -> bytes:
        return b"\xE9" + ((target - (address + 5)) & 0xFFFFFFFF).to_bytes(4, "little")
    thunk_a, thunk_b, final_target = 0x00401000, 0x00402000, 0x00403000
    assert direct_tail_thunk_target(thunk_fixture(thunk_a, rel_jmp(thunk_a, thunk_b))) == thunk_b
    assert direct_tail_thunk_target(thunk_fixture(thunk_b, rel_jmp(thunk_b, final_target))) == final_target
    assert direct_tail_thunk_target(thunk_fixture(0x00404000, bytes.fromhex("ffe0"))) is None
    assert direct_tail_thunk_target(thunk_fixture(0x00405000, bytes.fromhex("ff248578563412"))) is None
    assert direct_tail_thunk_target(thunk_fixture(0x00406000, bytes.fromhex("0f8400000000"))) is None
    assert direct_tail_thunk_target(thunk_fixture(
        0x00407000, bytes.fromhex("50") + rel_jmp(0x00407001, 0x00408000))) is None
    # The same depth+1 and queued-set policy used by discovery must visit a
    # chain of two direct thunks exactly once.
    thunk_graph = {
        thunk_a: thunk_fixture(thunk_a, rel_jmp(thunk_a, thunk_b)),
        thunk_b: thunk_fixture(thunk_b, rel_jmp(thunk_b, final_target)),
        final_target: thunk_fixture(final_target, bytes.fromhex("c3")),
    }
    chain = collections.deque([(thunk_a, 0)])
    chain_queued = {thunk_a}
    chain_seen = []
    while chain:
        current, depth = chain.popleft()
        chain_seen.append((current, depth))
        target = direct_tail_thunk_target(thunk_graph[current])
        if target is not None and target not in chain_queued:
            chain_queued.add(target)
            chain.append((target, depth + 1))
    assert chain_seen == [(thunk_a, 0), (thunk_b, 1), (final_target, 2)]
    compiler = shutil.which("gcc")
    if compiler:
        with tempfile.TemporaryDirectory(prefix="rsqrtps_selftest_", dir=ROOT / "work") as temp_name:
            temp_dir = Path(temp_name)
            source = temp_dir / "rsqrtps_semantics.c"
            executable = temp_dir / "rsqrtps_semantics.exe"
            source.write_text(
                "#include <stdint.h>\n#include <string.h>\n#include <math.h>\n"
                "typedef union { float f32[4]; uint32_t u32[4]; } XMM;\n"
                "typedef struct { XMM xmm[8]; } CPU;\n"
                "static XMM guest_vector;\n"
                "static XMM rdxm(uint32_t address) { (void)address; return guest_vector; }\n"
                + FP_ADAPTER_HELPERS + "\n"
                + "static void run_reg(CPU *c) { " + reg_code + " }\n"
                + "static void run_mem(CPU *c) { " + mem_code + " }\n"
                + r"""
static int check(const uint32_t input[4], const uint32_t expected[4])
{
    CPU reg = {0}, mem = {0};
    for (unsigned i = 0; i < 4; ++i) {
        reg.xmm[1].u32[i] = input[i];
        mem.xmm[1].u32[i] = input[i];
    }
    guest_vector = mem.xmm[1];
    run_reg(&reg);
    run_mem(&mem);
    for (unsigned i = 0; i < 4; ++i)
        if (reg.xmm[0].u32[i] != expected[i] || mem.xmm[2].u32[i] != expected[i])
            return 0;
    return 1;
}
int main(void)
{
    static const uint32_t edges[4] = {0x00000000u, 0x80000000u, 0x00000001u, 0x7f812345u};
    static const uint32_t edge_results[4] = {0x7f800000u, 0xff800000u, 0x7f800000u, 0x7fc12345u};
    static const uint32_t finite[4] = {0x3f800000u, 0x40800000u, 0x3e800000u, 0x7f800000u};
    static const uint32_t finite_results[4] = {0x3f800000u, 0x3f000000u, 0x40000000u, 0x00000000u};
    static const uint32_t invalid[4] = {0xbf800000u, 0xff800000u, 0x7f800000u, 0x7fc12345u};
    static const uint32_t invalid_results[4] = {0xffc00000u, 0xffc00000u, 0x00000000u, 0x7fc12345u};
    return !(check(edges, edge_results) && check(finite, finite_results) &&
             check(invalid, invalid_results));
}
""", encoding="utf-8")
            build = subprocess.run(
                [compiler, "-std=c11", "-O0", str(source), "-lm", "-o", str(executable)],
                text=True, capture_output=True)
            if build.returncode:
                raise AssertionError("RSQRTPS self-test compile failed:\n" + build.stdout + build.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            if result.returncode:
                raise AssertionError("RSQRTPS lane/edge self-test failed:\n" + result.stdout + result.stderr)
    else:
        print("RSQRTPS semantic self-test skipped (gcc not found)")
    # XLATB indexes EBX with unsigned AL, replaces only AL, and leaves flags.
    eax, ebx, flags = 0x123456FE, 0xFFFF0000, 0x8D7
    table = bytes((i * 7 + 3) & 0xFF for i in range(256))
    al = table[((ebx & 0xFFFFFFFF) + (eax & 0xFF)) & 0xFF]
    after = (eax & 0xFFFFFF00) | al
    assert after == 0x123456F5 and flags == 0x8D7
    # Capstone exposes the implicit accumulator as a register operand on
    # STOS/LODS/SCAS. The AC1 string override must accept these while leaving
    # SSE MOVSD/CMPSD (XMM operands) to the donor translator.
    string_fixtures = ("f3aa", "f3ab", "f3ac", "f3ad", "f2ae", "f2af",
                       "f3a4", "f3a5", "f3a6", "f3a7")
    for encoding in string_fixtures:
        insn = next(md.disasm(bytes.fromhex(encoding), 0x401000))
        translated = translate_directional_string_instruction(insn)
        assert translated and "c->df ?" in translated[0], (encoding, insn)
    for encoding in ("f20f10c1", "f20fc2c100"):
        insn = next(md.disasm(bytes.fromhex(encoding), 0x401000))
        assert translate_directional_string_instruction(insn) is None, (encoding, insn)
    # The callback scanner resolves arg4/arg5 at pre-call ESP+12/+16. Check
    # that aliasing stores, explicit ESP changes, and implicit stack ops do
    # not get mistaken for the five-argument push sequence.
    def f9fa43_fixture(middle: bytes = b"", tail: bytes = b""):
        start = 0x00401000
        code = bytearray()
        for value in (0x00420000, 0x00410000):  # cleanup, constructor
            code += b"\x68" + value.to_bytes(4, "little")
        code += middle
        for value in (1, 4):  # count, stride
            code += b"\x68" + value.to_bytes(4, "little")
        code += b"\x51"  # base = ECX
        code += tail
        call_at = start + len(code)
        rel = (0x00F9FA43 - (call_at + 5)) & 0xFFFFFFFF
        code += b"\xE8" + rel.to_bytes(4, "little")
        instructions = list(md.disasm(bytes(code), start))
        return SimpleNamespace(blocks={start: SimpleNamespace(instructions=instructions)})
    safe_edges = ac1_array_constructor_call_edges(f9fa43_fixture())
    assert safe_edges == [{"callsite": 0x00401015,
                           "constructor": 0x00410000, "cleanup": 0x00420000}], safe_edges
    reject_fixtures = (
        f9fa43_fixture(middle=bytes.fromhex("890424")),  # overwrites ctor after its push
        f9fa43_fixture(tail=bytes.fromhex("8944240c")),  # call-time arg4 overwrite
        f9fa43_fixture(tail=bytes.fromhex("89440c0c")),  # indexed ESP alias
        f9fa43_fixture(tail=bytes.fromhex("83ec04")),    # explicit SUB ESP
        f9fa43_fixture(tail=bytes.fromhex("9c")),        # implicit PUSHFD
        f9fa43_fixture(tail=bytes.fromhex("94")),        # XCHG EAX,ESP
        f9fa43_fixture(tail=bytes.fromhex("89450c")),    # EBP-based alias
    )
    assert all(not ac1_array_constructor_call_edges(fixture) for fixture in reject_fixtures)
    def schema_fixture(callback: int | None = 0x00438860, tail: bytes = b"",
                       helper: int = AC1_SCHEMA_CALLBACK_HELPER):
        start = 0x00402000
        code = bytearray(b"\x52\x51")  # arg4=EDX, arg3=ECX
        if callback is None:
            code += b"\x56"             # dynamic arg2 must not be discovered
        else:
            code += b"\x68" + callback.to_bytes(4, "little")
        code += b"\x50"                 # arg1=EAX
        code += tail
        call_at = start + len(code)
        rel = (helper - (call_at + 5)) & 0xFFFFFFFF
        code += b"\xE8" + rel.to_bytes(4, "little")
        instructions = list(md.disasm(bytes(code), start))
        return SimpleNamespace(blocks={start: SimpleNamespace(instructions=instructions)})
    def schema_middle_fixture(middle: bytes, helper: int = AC1_SCHEMA_CALLBACK_HELPER):
        start = 0x00403000
        code = bytearray(b"\x52\x51\x68\x60\x88\x43\x00")  # arg4, arg3, literal arg2
        code += middle
        code += b"\x50"  # arg1=EAX
        call_at = start + len(code)
        rel = (helper - (call_at + 5)) & 0xFFFFFFFF
        code += b"\xE8" + rel.to_bytes(4, "little")
        instructions = list(md.disasm(bytes(code), start))
        return SimpleNamespace(blocks={start: SimpleNamespace(instructions=instructions)})
    schema_edges = ac1_schema_callback_call_edges(schema_fixture())
    assert schema_edges == [{"callsite": 0x00402008, "callback": 0x00438860}], schema_edges
    for helper in AC1_ARG2_CALLBACK_HELPERS:
        assert ac1_four_dword_arg2_callback_edges(
            schema_fixture(helper=helper), helper) == schema_edges
    # At call-time ESP+4..+7 is arg2. Exact adjacent DWORD writes are safe;
    # the same [ESP] write between arg2 and arg1 pushes aliases arg2.
    assert ac1_schema_callback_call_edges(schema_fixture(tail=bytes.fromhex("89542400")))
    assert ac1_schema_callback_call_edges(schema_fixture(tail=bytes.fromhex("89542408")))
    schema_rejects = (
        schema_fixture(callback=None),                         # nonconstant callback
        schema_fixture(tail=bytes.fromhex("89542404")),       # call-time arg2 slot
        schema_fixture(tail=bytes.fromhex("6689542406")),     # partial overlap of arg2
        schema_fixture(tail=bytes.fromhex("89441408")),       # indexed ESP alias
        schema_fixture(tail=bytes.fromhex("83ec04")),         # explicit ESP change
        schema_fixture(tail=bytes.fromhex("9c")),             # implicit PUSHFD
        schema_fixture(tail=bytes.fromhex("e800000000")),     # intervening CALL
        schema_fixture(tail=bytes.fromhex("8945fc")),         # unknown EBP alias
        schema_middle_fixture(bytes.fromhex("891424")),       # [ESP] aliases arg2 after arg1 push
        schema_fixture(tail=bytes.fromhex("66686000")),       # operand-size-override PUSH
    )
    assert all(not ac1_schema_callback_call_edges(fixture) for fixture in schema_rejects)
    reader_rejects = (
        schema_fixture(callback=None, helper=AC1_READER_CALLBACK_HELPER),
        schema_fixture(tail=bytes.fromhex("89542404"), helper=AC1_READER_CALLBACK_HELPER),
        schema_fixture(tail=bytes.fromhex("6689542406"), helper=AC1_READER_CALLBACK_HELPER),
        schema_fixture(tail=bytes.fromhex("89441408"), helper=AC1_READER_CALLBACK_HELPER),
        schema_fixture(tail=bytes.fromhex("83ec04"), helper=AC1_READER_CALLBACK_HELPER),
        schema_fixture(tail=bytes.fromhex("9c"), helper=AC1_READER_CALLBACK_HELPER),
        schema_fixture(tail=bytes.fromhex("e800000000"), helper=AC1_READER_CALLBACK_HELPER),
        schema_fixture(tail=bytes.fromhex("8945fc"), helper=AC1_READER_CALLBACK_HELPER),
        schema_middle_fixture(bytes.fromhex("891424"), AC1_READER_CALLBACK_HELPER),
        schema_fixture(tail=bytes.fromhex("66686000"), helper=AC1_READER_CALLBACK_HELPER),
    )
    assert all(not ac1_reader_callback_call_edges(fixture) for fixture in reader_rejects)
    run_input_fingerprint_selftest()
    print("lift generation-input fingerprint self-test: unchanged inputs publish; changed inputs reject")
    run_instruction_span_index_selftest()
    print("instruction-span index self-test: indexed overlap matches linear scan")
    print("lift control-flow and instruction-adapter self-test: ok")


def run_ac1_switch_selftest(binary: Path, pcrecomp: Path) -> None:
    """Exercise AC1's real code/data jump tables with the pinned PE base."""
    sys.path[:0] = [str(pcrecomp / "tools/pe"), str(pcrecomp / "tools/disasm")]
    sys.path.insert(0, str(pcrecomp / "tools/lift"))
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    from lift32_cpu import Lifter as BaseLifter
    import pefile

    info = analyze_pe(str(binary))
    code = binary.read_bytes()
    dis = Disassembler(code, info.image_base, info.sections)
    install_ac1_jump_table_bounds(dis)
    thunk_iat = build_iat_map(info)
    known_thunk = dis.disassemble_function(0x004C2B30, thunk_iat)
    if not known_thunk or not known_thunk.blocks:
        raise AssertionError("AC1 known direct thunk did not disassemble")
    assert direct_tail_thunk_target(known_thunk) == 0x005DA560
    assert validate_callback_code_entry(0x005DA560, dis, info, thunk_iat, {})
    allocator = dis.disassemble_function(0x007AD880, thunk_iat)
    if not allocator or not allocator.blocks:
        raise AssertionError("AC1 allocator INT3 regression function did not disassemble")
    allocator_int3 = next(
        (ins for block in allocator.blocks.values() for ins in block.instructions
         if ins.address == 0x007AD9FB), None)
    if allocator_int3 is None or allocator_int3.mnemonic != "int3" or bytes(allocator_int3.bytes) != b"\xCC":
        raise AssertionError("AC1 allocator assertion site is no longer the expected INT3")
    assert ac1_int3_translation(allocator_int3) == [
        'ac1_unsupported_instruction(0x007AD9FBu, "guest INT3 breakpoint");'
    ]
    rdata = next(section for section in info.sections if section.name == ".rdata")
    data_target = info.image_base + rdata.virtual_address
    assert not validate_callback_code_entry(data_target, dis, info, thunk_iat, {})
    assert not validate_callback_code_entry(0xFFFFFFFF, dis, info, thunk_iat, {})
    from types import SimpleNamespace
    synthetic_thunk_start = 0x00401000
    rel = (data_target - (synthetic_thunk_start + 5)) & 0xFFFFFFFF
    data_jump = next(dis.md.disasm(b"\xE9" + rel.to_bytes(4, "little"), synthetic_thunk_start))
    data_thunk = SimpleNamespace(blocks={
        synthetic_thunk_start: SimpleNamespace(instructions=[data_jump])
    })
    assert direct_tail_thunk_target(data_thunk) == data_target
    assert not validate_callback_code_entry(data_target, dis, info, thunk_iat, {})
    function = dis.disassemble_function(0x0079F910, build_iat_map(info))
    if function is None or not function.blocks:
        raise AssertionError("AC1 jump-table regression function did not disassemble")
    spec = {
        "address": 0x0079F910,
        "size": function.size,
        "blocks": [{
            "start": block.start,
            "end": block.end,
            "successors": sorted(set(block.successors)),
            "is_exit": block.is_exit,
            "instructions": [[ins.address, ins.size] for ins in block.instructions],
        } for block in function.blocks.values()],
    }
    pe = pefile.PE(str(binary), fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    image_size = pe.OPTIONAL_HEADER.SizeOfImage

    def read_va(va: int, count: int) -> bytes:
        return pe.get_data((va - image_base) & 0xFFFFFFFF, count)

    try:
        lifter = BaseLifter(str(binary), image_size, read_va)
        # Confirm the regression: the upstream constructor's historical base
        # does not contain AC1's switch table.
        assert not lifter.in_image(0x0079FD2C)
        lifter.image_lo = image_base
        lifter.image_hi = image_base + image_size
        assert lifter.in_image(0x0079FD2C)
        first_table = lifter.resolve_jumptable(0x0079FD2C)
        second_table = lifter.resolve_jumptable(0x0079FD68)
        assert 0x0079F9F5 in first_table
        assert 0x0079FA04 in first_table
        assert 0x0079FC38 in second_table and 0x0079FD18 in second_table
        body = lift_exact_function(lifter, spec, read_va, X86_OP_IMM, X86_OP_MEM)
        assert "if (_jt == GVA(0x0079F9F5)) goto L_0079F9F5;" in body
        assert "if (_jt == GVA(0x0079FA04)) goto L_0079FA04;" in body
        assert "if (_jt == GVA(0x0079FC38)) goto L_0079FC38;" in body
        assert "L_0079F9F5:" in body and "L_0079FA04:" in body
        assert "L_0079FC38:" in body
    finally:
        pe.close()

    # This switch has four dword targets followed immediately by an
    # eleven-byte index remap. Without the guard/remap bound, the first four
    # map bytes form the bogus in-image target 0x01010300.
    remapped = dis.disassemble_function(0x0099CBF0, build_iat_map(info))
    real_targets = {0x0099C9D3, 0x0099CA83, 0x0099CA35, 0x0099CA9A}
    if remapped is None or not remapped.blocks:
        raise AssertionError("AC1 remapped-switch regression function did not disassemble")
    assert 0x01010300 not in remapped.blocks
    switch_insn = next(
        ins for block in remapped.blocks.values() for ins in block.instructions
        if ins.address == 0x0099C9CC)
    inferred = infer_remapped_switch_limit(
        dis.md, dis.read_bytes, switch_insn, X86_OP_REG, X86_OP_MEM, X86_OP_IMM)
    assert inferred == (4, 11), inferred
    assert set(dis.jump_table_targets(switch_insn)) == real_targets
    assert real_targets <= remapped.jump_targets
    # A second switch in this function reads the byte map with EAX, writes
    # ECX, then dispatches through [ECX*4]. Keep this distinct-register form
    # covered while retaining the same guarded bound checks.
    remap2_insn = next(
        ins for block in remapped.blocks.values() for ins in block.instructions
        if ins.address == 0x0099CC04)
    remap2_bound = infer_remapped_switch_limit(
        dis.md, dis.read_bytes, remap2_insn, X86_OP_REG, X86_OP_MEM, X86_OP_IMM)
    remap2_targets = set(dis.jump_table_targets(remap2_insn))
    assert remap2_bound == (17, 30), remap2_bound
    assert len(remap2_targets) == 17
    assert remap2_targets <= remapped.jump_targets
    remapped_spec = {
        "address": 0x0099CBF0,
        "size": remapped.size,
        "blocks": [{
            "start": block.start,
            "end": block.end,
            "successors": sorted(set(block.successors)),
            "is_exit": block.is_exit,
            "instructions": [[ins.address, ins.size] for ins in block.instructions],
        } for block in remapped.blocks.values()],
    }
    pe = pefile.PE(str(binary), fast_load=True)
    try:
        remap_lifter = BaseLifter(str(binary), image_size,
                                  lambda va, count: pe.get_data((va - image_base) & 0xFFFFFFFF, count))
        remap_lifter.image_lo = image_base
        remap_lifter.image_hi = image_base + image_size
        register_switch = dis.disassemble_function(0x010D730C, thunk_iat)
        sites = {site["load"]: site for site in register_switch.register_switches}
        assert sites[0x010D731C]["count"] == 15
        assert len(set(sites[0x010D731C]["targets"])) == 15
        assert 0x010D74C6 in register_switch.blocks
        register_spec = {
            "address": register_switch.address, "size": register_switch.size,
            "register_switches": register_switch.register_switches,
            "blocks": [{"start": b.start, "end": b.end,
                        "instructions": [[ins.address, ins.size] for ins in b.instructions]}
                       for b in register_switch.blocks.values()],
        }
        register_body = lift_exact_function(remap_lifter, register_spec, remap_lifter.read_va,
                                            X86_OP_IMM, X86_OP_MEM)
        for target in sites[0x010D731C]["targets"]:
            assert f"if (_jt == GVA(0x{target:08X})) goto L_{target:08X};" in register_body
        assert "uint32_t _jt = c->edx;" in register_body
        assert (0x01732544, 0x01732544 + 15 * 4) in dis.ac1_switch_table_ranges
        print("AC1 register-switch self-test: 15 internal cases, RET 8, and indexed-table exclusion")
        remapped_body = lift_exact_function(
            remap_lifter, remapped_spec,
            lambda va, count: pe.get_data((va - image_base) & 0xFFFFFFFF, count),
            X86_OP_IMM, X86_OP_MEM)
        for target in sorted(real_targets):
            assert f"_jt == GVA(0x{target:08X})" in remapped_body
        for target in sorted(remap2_targets):
            assert f"_jt == GVA(0x{target:08X})" in remapped_body
        assert "0x01010300" not in remapped_body

        # GoSellMerchandise has a direct unsigned guard `cmp eax,3; ja`
        # before its four-entry table. The next dword is an executable-section
        # address inside a CALL displacement, so code-address-only scanning
        # must stop at the proven bound rather than decode it as a switch arm.
        merchandise = dis.disassemble_function(0x00EAD7A0, build_iat_map(info))
        if merchandise is None or not merchandise.blocks:
            raise AssertionError("AC1 GoSellMerchandise switch did not disassemble")
        merchandise_jump = next(
            ins for block in merchandise.blocks.values() for ins in block.instructions
            if ins.address == 0x00EAD7B6)
        merchandise_targets = {0x00EAD7BD, 0x00EAD7C5}
        assert ac1_pinned_switch_limit(merchandise_jump) == 4
        assert set(dis.jump_table_targets(merchandise_jump)) == merchandise_targets
        assert merchandise_targets <= merchandise.blocks.keys()
        assert 0x00C70424 not in merchandise.blocks
        # This false CFG entry lies inside the CALL immediate in C70420. A
        # CMP/JA-looking byte suffix must never authorize a generic bound.
        false_operand_jump = SimpleNamespace(
            address=0x00C7042F, mnemonic="jmp",
            operands=[SimpleNamespace(type=X86_OP_MEM, mem=SimpleNamespace(
                base=0, index=merchandise_jump.operands[0].mem.index,
                scale=4, disp=0x00EAD830))],
        )
        assert ac1_pinned_switch_limit(false_operand_jump) is None
        merchandise_spec = {
            "address": 0x00EAD7A0,
            "size": merchandise.size,
            "blocks": [{
                "start": block.start,
                "end": block.end,
                "successors": sorted(set(block.successors)),
                "is_exit": block.is_exit,
                "instructions": [[ins.address, ins.size] for ins in block.instructions],
            } for block in merchandise.blocks.values()],
        }
        merchandise_body = lift_exact_function(
            remap_lifter, merchandise_spec,
            lambda va, count: pe.get_data((va - image_base) & 0xFFFFFFFF, count),
            X86_OP_IMM, X86_OP_MEM)
        for target in sorted(merchandise_targets):
            assert f"if (_jt == GVA(0x{target:08X})) goto L_{target:08X};" in merchandise_body
            assert f"L_{target:08X}:" in merchandise_body
        assert "0x00C70424" not in merchandise_body
        assert "aad 0xcb" not in merchandise_body
        print("AC1 guarded-switch regression: GoSellMerchandise table bounded to four proven arms")

        # This scalar memmove switch has an invalid dword in slot 0 because
        # the nominal table base overlaps the previous JMP+NOP. The guarded
        # EAX mask selects slots 1..3, all three of which are local CFG blocks.
        memmove = dis.disassemble_function(0x00F9FB80, build_iat_map(info))
        if memmove is None or not memmove.blocks:
            raise AssertionError("AC1 memmove switch regression did not disassemble")
        memmove_jump = next(
            ins for block in memmove.blocks.values() for ins in block.instructions
            if ins.address == 0x00F9FBF5)
        memmove_targets = {0x00F9FC18, 0x00F9FC44, 0x00F9FC68}
        assert set(dis.jump_table_targets(memmove_jump)) == memmove_targets
        assert memmove_targets <= memmove.blocks.keys()
        memmove_negative_jump = next(
            ins for block in memmove.blocks.values() for ins in block.instructions
            if ins.address == 0x00F9FBFC)
        memmove_negative_targets = {0x00F9FD04, 0x00F9FD0C,
                                    0x00F9FD18, 0x00F9FD2C}
        assert memmove_negative_targets <= memmove.blocks.keys()
        # This second table really is negatively indexed, but only its local
        # arms are recovered from other switches. Do not expand the CFG here.
        biased_jump = next(
            ins for block in memmove.blocks.values() for ins in block.instructions
            if ins.address == 0x00F9FD6E)
        biased_targets = {0x00F9FE44, 0x00F9FE4C, 0x00F9FE54,
                          0x00F9FE5C, 0x00F9FE64, 0x00F9FE6C, 0x00F9FE74}
        assert biased_targets <= memmove.blocks.keys()
        assert infer_negated_switch_bound(
            dis.md, dis.read_bytes, biased_jump,
            X86_OP_REG, X86_OP_MEM, X86_OP_IMM) == 8
        assert set(dis.jump_table_targets(biased_jump)) == biased_targets | {0x00F9FE87}
        assert 0x00498D00 not in memmove.blocks
        assert dis.jump_table_targets(memmove_negative_jump) == []
        memmove_spec = {
            "address": 0x00F9FB80,
            "size": memmove.size,
            "blocks": [{
                "start": block.start,
                "end": block.end,
                "successors": sorted(set(block.successors)),
                "is_exit": block.is_exit,
                "instructions": [[ins.address, ins.size] for ins in block.instructions],
            } for block in memmove.blocks.values()],
        }
        memmove_body = lift_exact_function(
            remap_lifter, memmove_spec,
            lambda va, count: pe.get_data((va - image_base) & 0xFFFFFFFF, count),
            X86_OP_IMM, X86_OP_MEM)
        for target in sorted(memmove_targets):
            assert f"if (_jt == GVA(0x{target:08X})) goto L_{target:08X};" in memmove_body
            assert f"L_{target:08X}:" in memmove_body
        for target in sorted(memmove_negative_targets):
            assert f"if (_jt == GVA(0x{target:08X})) goto L_{target:08X};" in memmove_body
            assert f"L_{target:08X}:" in memmove_body
        assert "dispatch_jmp(c, rd32((c->eax*4 + GVA(0x00F9FC08))))" not in memmove_body
        assert "dispatch_jmp(c, rd32((c->ecx*4 + GVA(0x00F9FD04))))" not in memmove_body
        assert "0x00498D00" not in memmove_body and "insb" not in memmove_body.lower()

        # Compile and execute the real lifted function against native
        # memmove, using the original table/code page and portable guest
        # memory. This catches control-flow mistakes that label assertions
        # alone cannot detect.
        compiler = shutil.which("gcc")
        if compiler:
            work_dir = ROOT / "work"
            work_dir.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="memmove_selftest_", dir=work_dir) as temp_name:
                temp_dir = Path(temp_name)
                generated_c = temp_dir / "memmove_lifted.c"
                table_page = temp_dir / "original_page.bin"
                executable = temp_dir / "memmove_semantics.exe"
                generated_c.write_text(
                    '#include "cpu.h"\n' + FP_ADAPTER_HELPERS +
                    "\n#if defined(AC1_TRACE_EIP)\n"
                    "#define AC1_SET_GUEST_EIP(cpu, va) ((cpu)->eip = (va))\n"
                    "#else\n#define AC1_SET_GUEST_EIP(cpu, va) ((void)0)\n#endif\n\n" +
                    memmove_body + "\n", encoding="utf-8")
                original_page = read_va(0x00F9F000, 4096)
                if len(original_page) != 4096:
                    raise AssertionError("could not read original memmove switch page")
                table_page.write_bytes(original_page)
                command = [
                    compiler, "-std=c11", "-O0", "-DAC1_TRACE_EIP",
                    "-I", str(prepare_test_runtime()),
                    str(ROOT / "tests/memmove_semantics.c"), str(generated_c),
                    str(ROOT / "src/runtime/guest_memory.c"),
                    str(ROOT / "src/runtime/guest_clock.c"), "-lm", "-o", str(executable),
                ]
                build = subprocess.run(command, text=True, capture_output=True)
                if build.returncode:
                    raise AssertionError("memmove semantic self-test compile failed:\n" +
                                         build.stdout + build.stderr)
                result = subprocess.run([str(executable), str(table_page)],
                                        text=True, capture_output=True)
                if result.returncode:
                    raise AssertionError("memmove semantic self-test failed:\n" +
                                         result.stdout + result.stderr)
                print(result.stdout.strip())
        else:
            print("memmove semantic self-test skipped (gcc not found)")
        selftest_iat = build_iat_map(info)
        run_ac1_array_callback_selftest(dis, info, selftest_iat)
        run_ac1_schema_callback_selftest(dis, info, selftest_iat)
        run_ac1_reader_callback_selftest(dis, info, selftest_iat)
        run_ac1_sse_width_selftest(
            remap_lifter, dis.md,
            lambda va, count: pe.get_data((va - image_base) & 0xFFFFFFFF, count))
    finally:
        pe.close()
    print("AC1 actual-base jump-table self-test: local, remapped, and negative-index switches recovered")


def run_terminal_external_direct_jump_selftest(binary: Path, pcrecomp: Path) -> None:
    """Check bounded discovery of real multi-instruction AC1 tail transfers."""
    sys.path[:0] = [str(pcrecomp / "tools/pe"), str(pcrecomp / "tools/disasm")]
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler
    from types import SimpleNamespace

    info = analyze_pe(str(binary))
    dis = Disassembler(binary.read_bytes(), info.image_base, info.sections)
    install_ac1_jump_table_bounds(dis)
    iat = build_iat_map(info)
    adapter = dis.disassemble_function(0x004016D1, iat)
    if adapter is None or not adapter.blocks:
        raise AssertionError("AC1 startup adapter did not disassemble")
    assert direct_tail_thunk_target(adapter) is None
    assert terminal_external_direct_jump_targets(adapter) == [0x009D9600]
    adapter_spec = {
        "address": 0x004016D1,
        "blocks": [{"instructions": [[ins.address, ins.size]
                                      for ins in block.instructions]}
                   for block in adapter.blocks.values()],
    }
    spans = InstructionSpanIndex()
    spans.add_function(adapter_spec)
    assert validate_callback_code_entry(0x009D9600, dis, info, iat, spans)
    # The first jump in this function is internal and must not be promoted.
    assert 0x00403290 not in terminal_external_direct_jump_targets(adapter)

    # Existing exact one-instruction thunks still produce their same target.
    thunk = dis.disassemble_function(0x004C2B30, iat)
    assert thunk is not None
    assert terminal_external_direct_jump_targets(thunk) == [0x005DA560]

    indirect = next(dis.md.disasm(b"\xFF\xE0", 0x1000))
    conditional = next(dis.md.disasm(b"\x74\x05", 0x2000))
    indirect_fn = SimpleNamespace(blocks={0x1000: SimpleNamespace(start=0x1000, instructions=[indirect])})
    conditional_fn = SimpleNamespace(blocks={0x2000: SimpleNamespace(start=0x2000, instructions=[conditional])})
    assert terminal_external_direct_jump_targets(indirect_fn) == []
    assert terminal_external_direct_jump_targets(conditional_fn) == []

    interior = next(dis.md.disasm(b"\xE9\xFC\xFF\xFF\xFF", 0x3000))
    interior_fn = SimpleNamespace(blocks={0x3000: SimpleNamespace(start=0x3000, instructions=[interior])})
    assert terminal_external_direct_jump_targets(interior_fn) == []
    nonexec = next(dis.md.disasm(b"\xE9\xFA\xBE\xAD\xDE", 0x4000))
    nonexec_fn = SimpleNamespace(blocks={0x4000: SimpleNamespace(start=0x4000, instructions=[nonexec])})
    target = terminal_external_direct_jump_targets(nonexec_fn)[0]
    assert not dis.is_code_address(target)
    assert not validate_callback_code_entry(target, dis, info, iat, spans)
    print("AC1 external tail-jump self-test: startup adapter followed; local, indirect, conditional, interior, and non-code cases rejected")


def run_ac1_sse_width_selftest(lifter, md, read_va) -> None:
    """Execute narrow MOVQ/CVTDQ2PD adapters at an exact eight-byte boundary."""
    def decode(hex_bytes: str, address: int):
        decoded = list(md.disasm(bytes.fromhex(hex_bytes), address))
        if len(decoded) != 1:
            raise AssertionError(f"SSE width test bytes failed to decode: {hex_bytes}")
        return decoded[0]

    movq_load = next(iter(md.disasm(read_va(0x00974270, 8), 0x00974270)), None)
    if (not movq_load or movq_load.mnemonic != "movq" or
            movq_load.op_str != "xmm0, qword ptr [eax + 0x18]"):
        raise AssertionError("original AC1 MOVQ width fault site changed")
    instructions = {
        "movq_load": movq_load,
        "movq_store": decode("660FD64818", 0x00404000),
        "movq_reg": decode("F30F7EC1", 0x00404010),
        "movq_alias": decode("F30F7EC0", 0x00404020),
        "cvtdq2pd_load": decode("F30FE64018", 0x00404030),
        "cvtdq2pd_alias": decode("F30FE6C0", 0x00404040),
    }
    lowered = {name: ac1_sse_width_translation(lifter, insn)
               for name, insn in instructions.items()}
    if any(lines is None for lines in lowered.values()):
        raise AssertionError("narrow SSE adapter did not handle every tested XMM form")
    if "rd64(" not in lowered["movq_load"][0] or "rdxm(" in lowered["movq_load"][0]:
        raise AssertionError("MOVQ m64 load must read exactly one qword")
    if "wr64(" not in lowered["movq_store"][0] or "wrsd(" in lowered["movq_store"][0]:
        raise AssertionError("MOVQ m64 store must write the source qword bitwise")
    if "rd64(" not in lowered["cvtdq2pd_load"][0] or "rdxm(" in lowered["cvtdq2pd_load"][0]:
        raise AssertionError("CVTDQ2PD m64 load must read exactly one qword")

    compiler = shutil.which("gcc")
    if not compiler:
        raise AssertionError("GCC is required for the exact-width SSE adapter self-test")
    work_dir = ROOT / "work"
    work_dir.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sse_width_selftest_", dir=work_dir) as temp_name:
        temp_dir = Path(temp_name)
        source_path = temp_dir / "sse_width_semantics.c"
        executable = temp_dir / "sse_width_semantics.exe"

        def wrapper(name: str) -> str:
            body = "\n".join(lowered[name])
            return f"static void {name}(CPU *c) {{ {body} }}\n"

        source = r'''#include "cpu.h"
#include <stdio.h>
#include <string.h>

'''
        source += "\n".join(wrapper(name) for name in instructions)
        source += r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "SSE width check failed at line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void capture(CPU *c, uint32_t gpr[8], uint32_t flg[8]) {
    gpr[0]=c->eax; gpr[1]=c->ecx; gpr[2]=c->edx; gpr[3]=c->ebx;
    gpr[4]=c->esp; gpr[5]=c->ebp; gpr[6]=c->esi; gpr[7]=c->edi;
    flg[0]=c->cf; flg[1]=c->pf; flg[2]=c->af; flg[3]=c->zf;
    flg[4]=c->sf; flg[5]=c->of; flg[6]=c->df; flg[7]=c->eflags_misc;
}
static int unchanged(CPU *c, uint32_t gpr[8], uint32_t flg[8]) {
    return c->eax==gpr[0] && c->ecx==gpr[1] && c->edx==gpr[2] && c->ebx==gpr[3] &&
           c->esp==gpr[4] && c->ebp==gpr[5] && c->esi==gpr[6] && c->edi==gpr[7] &&
           c->cf==flg[0] && c->pf==flg[1] && c->af==flg[2] && c->zf==flg[3] &&
           c->sf==flg[4] && c->of==flg[5] && c->df==flg[6] && c->eflags_misc==flg[7];
}
int main(void) {
    enum { BASE=0x1087F000u, PAGE=4096u, EA=0x1087FFF8u };
    uint8_t memory[PAGE]={0};
    uint32_t gpr[8], flg[8];
    const uint64_t patterns[]={0, UINT64_C(0x8000000000000000),
        UINT64_C(0x7ff0000000000001), UINT64_C(0xfff8000012345678),
        UINT64_C(0x1122334455667788)};
    CPU c={0};
    guest_memory_bind(memory, BASE, sizeof memory);
    CHECK(guest_memory_try_ptr(EA, 8u)!=NULL);
    CHECK(guest_memory_try_ptr(EA, 16u)==NULL);
    c.eax=EA-0x18u; c.ecx=0x11223344u; c.edx=0x55667788u; c.ebx=0x99aabbccu;
    c.esp=0x7feff000u; c.ebp=0x12345678u; c.esi=0x87654321u; c.edi=0xa5a5a5a5u;
    c.cf=1; c.pf=0; c.af=1; c.zf=0; c.sf=1; c.of=1; c.df=1; c.eflags_misc=0x202u;
    for (unsigned i=0;i<sizeof patterns/sizeof patterns[0];++i) {
        memcpy(memory+PAGE-8,&patterns[i],8);
        c.xmm[0].u64[1]=UINT64_C(0xdeadbeefcafebabe);
        capture(&c,gpr,flg); movq_load(&c);
        CHECK(c.xmm[0].u64[0]==patterns[i]); CHECK(c.xmm[0].u64[1]==0);
        CHECK(unchanged(&c,gpr,flg));

        c.xmm[1].u64[0]=patterns[i]; c.xmm[1].u64[1]=UINT64_C(0x7ff0000000000001);
        memset(memory+PAGE-8,0,8); capture(&c,gpr,flg); movq_store(&c);
        uint64_t stored=0; memcpy(&stored,memory+PAGE-8,8);
        CHECK(stored==patterns[i]); CHECK(unchanged(&c,gpr,flg));
    }
    c.xmm[1].u64[0]=UINT64_C(0x0123456789abcdef);
    c.xmm[1].u64[1]=UINT64_C(0xfedcba9876543210);
    capture(&c,gpr,flg); movq_reg(&c);
    CHECK(c.xmm[0].u64[0]==UINT64_C(0x0123456789abcdef)); CHECK(c.xmm[0].u64[1]==0);
    CHECK(c.xmm[1].u64[1]==UINT64_C(0xfedcba9876543210)); CHECK(unchanged(&c,gpr,flg));
    c.xmm[0].u64[0]=UINT64_C(0x7ff0000000000001);
    c.xmm[0].u64[1]=UINT64_C(0x0123456789abcdef);
    capture(&c,gpr,flg); movq_alias(&c);
    CHECK(c.xmm[0].u64[0]==UINT64_C(0x7ff0000000000001)); CHECK(c.xmm[0].u64[1]==0);
    CHECK(unchanged(&c,gpr,flg));

    { int32_t values[2]={INT32_MIN,INT32_MAX}; memcpy(memory+PAGE-8,values,8); }
    capture(&c,gpr,flg); cvtdq2pd_load(&c);
    CHECK(c.xmm[0].f64[0]==-2147483648.0); CHECK(c.xmm[0].f64[1]==2147483647.0);
    CHECK(unchanged(&c,gpr,flg));
    c.xmm[0].u32[0]=UINT32_C(0x80000000); c.xmm[0].u32[1]=UINT32_C(0x7fffffff);
    capture(&c,gpr,flg); cvtdq2pd_alias(&c);
    CHECK(c.xmm[0].f64[0]==-2147483648.0); CHECK(c.xmm[0].f64[1]==2147483647.0);
    CHECK(unchanged(&c,gpr,flg));
    guest_memory_unbind();
    puts("MOVQ/CVTDQ2PD exact-width boundary and lane tests passed");
    return 0;
}
'''
        source_path.write_text(source, encoding="utf-8")
        command = [compiler, "-std=c11", "-O0", "-I", str(prepare_test_runtime()),
                   str(source_path), str(ROOT / "src/runtime/guest_memory.c"),
                   str(ROOT / "src/runtime/guest_clock.c"), "-o", str(executable)]
        build = subprocess.run(command, text=True, capture_output=True)
        if build.returncode:
            raise AssertionError("SSE width self-test compile failed:\n" + build.stdout + build.stderr)
        result = subprocess.run([str(executable)], text=True, capture_output=True)
        if result.returncode:
            raise AssertionError("SSE width semantic self-test failed:\n" + result.stdout + result.stderr)
        print(result.stdout.strip())


def run_ac1_array_callback_selftest(dis, info, iat) -> None:
    """Pin the helper ABI and the exact constant callback edges in the AC1 image."""
    callers = (
        0x004135E0, 0x0041AC00, 0x004834A0, 0x0055EDF0, 0x007C6FC0,
        0x009113A0, 0x00918CE0, 0x0091A8D0, 0x0092FBE0, 0x00941B90,
        0x009697B0, 0x0098A920, 0x00992880, 0x00AAEC50, 0x015E2270,
        0x015F8370, 0x01609490, 0x016094C0, 0x0163A020, 0x0163A050,
        0x0163A080,
    )
    expected_constructors = {
        0x0040F360, 0x0041A2D0, 0x004830B0, 0x0051E860, 0x0055EAD0,
        0x005637C0, 0x007872B0, 0x007C2CA0, 0x0090F9F0, 0x0090FA80,
        0x009184A0, 0x0091A840, 0x0092FB00, 0x009419B0, 0x009697A0,
        0x009A6FB0, 0x00A0DE10, 0x00A51740, 0x00A517F0, 0x00D72830,
        0x00D72840,
    }
    edges = []
    for caller in callers:
        function = dis.disassemble_function(caller, iat)
        if function is None or not function.blocks:
            raise AssertionError(f"F9FA43 callback fixture caller 0x{caller:08X} did not decode")
        edges.extend((caller, edge) for edge in ac1_array_constructor_call_edges(function))
    assert len(edges) == 22, len(edges)
    assert {edge["constructor"] for _, edge in edges} == expected_constructors
    assert (0x0041AC00, {"callsite": 0x0041AC12,
                         "constructor": 0x0041A2D0, "cleanup": 0x0041AAC0}) in edges
    assert (0x004834A0, {"callsite": 0x004834E3,
                         "constructor": 0x004830B0, "cleanup": 0x004830C0}) in edges
    for _, edge in edges:
        for target in (edge["constructor"], edge["cleanup"]):
            if not validate_callback_code_entry(target, dis, info, iat, {}):
                raise AssertionError(f"F9FA43 argument 0x{target:08X} is not a code entry")
    print("AC1 F9FA43 callback self-test: 22 direct calls, 21 constant constructors, cleanup entries validated")


def run_ac1_schema_callback_selftest(dis, info, iat) -> None:
    """Pin 438EF0's ABI and real recursively discovered callback chains."""
    helper = dis.disassemble_function(AC1_SCHEMA_CALLBACK_HELPER, iat)
    if not helper or not helper.blocks:
        raise AssertionError("AC1 438EF0 schema callback helper did not disassemble")
    instructions = {ins.address: ins for block in helper.blocks.values()
                    for ins in block.instructions}
    ret = instructions.get(0x00438F79)
    if (not ret or ret.mnemonic != "ret" or not ret.operands or
            ret.operands[0].imm != 0x10):
        raise AssertionError("AC1 438EF0 must clean four stack arguments")
    assert instructions[0x00438F3E].op_str == "ebp, dword ptr [esp + 0x1c]"
    assert instructions[0x00438F42].mnemonic == "push"
    assert instructions[0x00438F42].op_str == "esi"
    assert instructions[0x00438F43].op_str == "ecx, ebp"
    assert instructions[0x00438F45].mnemonic == "call"
    assert instructions[0x00438F45].op_str == "dword ptr [esp + 0x1c]"

    caller = dis.disassemble_function(0x00439A50, iat)
    if not caller or not caller.blocks:
        raise AssertionError("AC1 schema callback producer did not disassemble")
    edges = ac1_schema_callback_call_edges(caller)
    match = [edge for edge in edges if edge["callsite"] == 0x00439BCE]
    if match != [{"callsite": 0x00439BCE, "callback": 0x00438860}]:
        raise AssertionError(f"AC1 schema callback arg2 recovery changed: {match}")
    if not validate_callback_code_entry(0x00438860, dis, info, iat, {}):
        raise AssertionError("AC1 0x00438860 callback is not a validated code entry")

    # These two chains are observed in the canonical catalog refresh. The
    # outer producer installs C73600; that callback itself registers C72BB0.
    # A separate producer installs 526BB0, which in turn registers 524FD0.
    # Pin both levels so discovery cannot silently stop after the first edge.
    def require_edge(caller_va: int, callsite: int, target: int) -> None:
        function = dis.disassemble_function(caller_va, iat)
        if not function or not function.blocks:
            raise AssertionError(f"schema callback caller 0x{caller_va:08X} did not disassemble")
        actual = {edge["callsite"]: edge["callback"]
                  for edge in ac1_schema_callback_call_edges(function)}
        if actual.get(callsite) != target:
            raise AssertionError(
                f"schema callback edge 0x{caller_va:08X}/0x{callsite:08X}: "
                f"expected 0x{target:08X}, got {actual.get(callsite)!r}")
        if not validate_callback_code_entry(target, dis, info, iat, {}):
            raise AssertionError(f"schema callback 0x{target:08X} is not a validated code entry")

    require_edge(0x00D28AF0, 0x00D28B17, 0x00C73600)
    require_edge(0x00C73600, 0x00C73768, 0x00C72BB0)
    require_edge(0x00CDF430, 0x00CDF59A, 0x00526BB0)
    require_edge(0x00526BB0, 0x00526C0A, 0x00524FD0)
    print("AC1 438EF0 schema-callback self-test: RET 16 ABI and both recursive real callback chains validated")


def run_ac1_reader_callback_selftest(dis, info, iat) -> None:
    """Pin 9312F0's callback ABI, one external producer, and recursive self edges."""
    helper = dis.disassemble_function(AC1_READER_CALLBACK_HELPER, iat)
    if not helper or not helper.blocks:
        raise AssertionError("AC1 9312F0 reader callback helper did not disassemble")
    instructions = {ins.address: ins for block in helper.blocks.values()
                    for ins in block.instructions}
    returns = [ins for ins in instructions.values() if ins.mnemonic == "ret"]
    if (len(returns) < 3 or any(not ins.operands or ins.operands[0].imm != 0x10
                                for ins in returns)):
        raise AssertionError("AC1 9312F0 must clean four DWORD stack arguments on every return")
    assert instructions[0x009312F0].mnemonic == "push"
    assert instructions[0x009312F0].op_str == "esi"
    assert instructions[0x009312F1].op_str == "esi, ecx"
    # At 9313B5 ESP+1c is entry arg3 after four saved registers. The callback
    # reloads *(arg3) into ECX and passes its saved ESI/reader as stack arg1.
    assert instructions[0x009313B5].op_str == "edi, dword ptr [esp + 0x1c]"
    assert instructions[0x009313F0].op_str == "ecx, dword ptr [edi]"
    assert instructions[0x009313F2].mnemonic == "push"
    assert instructions[0x009313F2].op_str == "esi"
    assert instructions[0x009313F3].mnemonic == "call"
    assert instructions[0x009313F3].op_str == "dword ptr [esp + 0x1c]"

    def require_edge(caller_va: int, callsite: int, target: int) -> None:
        function = dis.disassemble_function(caller_va, iat)
        if not function or not function.blocks:
            raise AssertionError(f"reader callback caller 0x{caller_va:08X} did not disassemble")
        actual = {edge["callsite"]: edge["callback"]
                  for edge in ac1_reader_callback_call_edges(function)}
        if actual.get(callsite) != target:
            raise AssertionError(
                f"reader callback edge 0x{caller_va:08X}/0x{callsite:08X}: "
                f"expected 0x{target:08X}, got {actual.get(callsite)!r}")
        if not validate_callback_code_entry(target, dis, info, iat, {}):
            raise AssertionError(f"reader callback 0x{target:08X} is not a validated code entry")

    require_edge(0x006FA880, 0x006FA928, 0x005FD1D0)
    require_edge(0x006446D0, 0x00644809, 0x006446D0)
    require_edge(0x00A9B370, 0x00A9B548, 0x00A9B370)
    dynamic_caller = dis.disassemble_function(0x00931820, iat)
    if not dynamic_caller or any(edge["callsite"] == 0x00931B97
                                 for edge in ac1_reader_callback_call_edges(dynamic_caller)):
        raise AssertionError("AC1 9312F0 dynamic arg2 at 931B97 must remain undiscovered")
    print("AC1 9312F0 reader-callback self-test: RET 16 / arg2 / ECX / saved-reader ABI, producer, self edges, and dynamic rejection validated")


def unsigned_switch_count(bound: int, branch: str, taken: bool) -> int | None:
    """Size of the unsigned upper range selected by a CMP/branch edge."""
    inclusive = ((branch == "ja" and not taken) or (branch == "jbe" and taken))
    exclusive = ((branch == "jae" and not taken) or (branch == "jb" and taken))
    count = bound + (1 if inclusive else 0)
    return count if (inclusive or exclusive) and 1 <= count <= 256 else None


def remapped_switch_count(table_base: int, map_base: int, remap_bytes: bytes) -> int | None:
    """Share the adjacent DWORD-table/byte-map proof with register switches."""
    delta = (map_base - table_base) & 0xFFFFFFFF
    if not delta or delta % 4 or not remap_bytes:
        return None
    count = delta // 4
    return count if 1 <= count <= 256 and max(remap_bytes) < count else None


def infer_remapped_switch_limit(md, read_va, jump_insn, reg_type: int,
                                mem_type: int, imm_type: int) -> tuple[int, int] | None:
    """Recognize a guarded byte-index remap feeding an indirect dword jump.

    Some MSVC switches first apply `movzx reg, byte [reg + map]` and then use
    that value as the dword-table index. A generic image-bounded table scan
    mistakes the adjacent byte map for more dword pointers. Recover the
    original input bound from the preceding CMP and cap the jump table using
    the largest remapped index actually present in that guarded map.
    """
    if (jump_insn.mnemonic != "jmp" or not jump_insn.operands or
            jump_insn.operands[0].type != mem_type):
        return None
    jump_mem = jump_insn.operands[0].mem
    if not jump_mem.index or jump_mem.scale != 4:
        return None
    jump_index_reg = jump_mem.index

    # Try short contiguous decodes ending exactly at the dispatch. Starting at
    # several nearby byte offsets is safe only when the complete sequence ends
    # at the known instruction boundary and contains the distinctive MOVZX.
    begin = max(0, jump_insn.address - 24)
    candidates = []
    for start in range(begin, jump_insn.address):
        try:
            raw = read_va(start, jump_insn.address - start)
            stream = list(md.disasm(raw, start))
        except Exception:
            continue
        if stream and stream[-1].address + stream[-1].size == jump_insn.address:
            candidates.append(stream)

    # Prefer the shortest contiguous decode; it is least likely to begin in
    # the middle of an unrelated instruction.
    candidates.sort(key=len)
    for stream in candidates:
        for remap_at, remap in enumerate(stream):
            if remap.mnemonic != "movzx" or len(remap.operands) != 2:
                continue
            dst, src = remap.operands
            if (dst.type != reg_type or dst.reg != jump_index_reg or
                    src.type != mem_type or src.size != 1):
                continue
            map_mem = src.mem
            # The source index may differ from the destination/jump index
            # (AC1's 0x0099CBFD uses EAX for the byte map and ECX for dispatch).
            # Restrict this adapter to a single register plus an absolute map
            # displacement; more complex addressing needs separate evidence.
            if bool(map_mem.base) == bool(map_mem.index):
                continue
            map_source_reg = map_mem.base or map_mem.index
            map_base = map_mem.disp & 0xFFFFFFFF
            if not map_base:
                continue

            # Only accept the proven guarded form: CMP source-index, imm;
            # unsigned JA/JAE default branch; then MOVZX. No intervening
            # instruction can mutate the index or replace the condition.
            input_count = None
            for cmp_at in range(remap_at - 1, -1, -1):
                cmp = stream[cmp_at]
                if cmp.mnemonic != "cmp" or len(cmp.operands) != 2:
                    continue
                lhs, rhs = cmp.operands
                if lhs.type != reg_type or lhs.reg != map_source_reg or rhs.type != imm_type:
                    continue
                between = stream[cmp_at + 1:remap_at]
                guards = [item.mnemonic for item in between if item.mnemonic in ("ja", "jae")]
                if (not guards or len(set(guards)) != 1 or
                        any(item.mnemonic not in ("ja", "jae", "nop") for item in between)):
                    continue
                bound = int(rhs.imm)
                input_count = unsigned_switch_count(bound, guards[0], False)
                break
            if input_count is None or not 1 <= input_count <= 256:
                continue
            try:
                remap_bytes = read_va(map_base, input_count)
            except Exception:
                continue
            if not remap_bytes or len(remap_bytes) != input_count:
                continue
            # The selected dword table must end exactly where the byte remap
            # begins, and every guarded map output must select one of its arms.
            table_base = jump_mem.disp & 0xFFFFFFFF
            table_count = remapped_switch_count(table_base, map_base, remap_bytes)
            if table_count is not None:
                return table_count, input_count
    return None


def ac1_pinned_switch_limit(insn) -> int | None:
    """Return the proven bound for AC1's exact GoSellMerchandise dispatch."""
    if insn.address != 0x00EAD7B6 or insn.mnemonic != "jmp" or not insn.operands:
        return None
    from capstone.x86 import X86_OP_MEM, X86_REG_EAX
    operand = insn.operands[0]
    if operand.type != X86_OP_MEM:
        return None
    mem = operand.mem
    if (mem.base == 0 and mem.index == X86_REG_EAX and mem.scale == 4 and
            (mem.disp & 0xFFFFFFFF) == 0x00EAD830):
        return 4
    return None


def register_family(md, reg) -> str:
    name = md.reg_name(reg)
    for names in (("eax", "ax", "al", "ah"), ("ebx", "bx", "bl", "bh"),
                  ("ecx", "cx", "cl", "ch"), ("edx", "dx", "dl", "dh"),
                  ("esi", "si"), ("edi", "di"), ("esp", "sp"), ("ebp", "bp")):
        if name in names:
            return names[0]
    return name


def register_switch_sites(function, md, read_va, is_code, known=None) -> dict:
    """Prove last-definition table loads and unsigned guards on decoded CFG paths."""
    from capstone.x86 import X86_OP_REG, X86_OP_MEM, X86_OP_IMM, X86_REG_EFLAGS
    known = known or {}
    candidates = [block for block in function.blocks.values() if block.instructions
                  and block.instructions[-1].mnemonic == "jmp"
                  and block.instructions[-1].operands
                  and block.instructions[-1].operands[0].type == X86_OP_REG]
    if not candidates:
        return {}
    predecessors = collections.defaultdict(set)
    for block in function.blocks.values():
        successors = set(block.successors)
        last = block.instructions[-1] if block.instructions else None
        if last and last.address in known:
            successors.update(known[last.address]["targets"])
        elif (last and last.mnemonic == "jmp" and last.operands and
              last.operands[0].type == X86_OP_MEM):
            # The donor omits indirect successors in pass 2. Conservatively
            # include its recovered switch arms when proving uniqueness.
            successors.update(function.jump_targets)
        for target in successors:
            predecessors[target].add(block.start)
    detail = {}

    def decoded(ins):
        if ins.address not in detail:
            detail[ins.address] = next(md.disasm(bytes(ins.bytes), ins.address, count=1))
        return detail[ins.address]

    def writes(ins, reg):
        return (ins.mnemonic in ("call", "lcall") or
                any(register_family(md, written) == register_family(md, reg)
                    for written in decoded(ins).regs_access()[1]))

    def guard(path, reg, before):
        # The path records which successor was actually selected, including
        # taken JBE/JB edges over an intervening default-return block.
        for at in range(before - 1, -1, -1):
            branch = path[at]
            if writes(branch, reg):
                return None
            if branch.mnemonic not in ("ja", "jae", "jb", "jbe") or at + 1 >= len(path):
                continue
            if not branch.operands or branch.operands[0].type != X86_OP_IMM:
                continue
            taken = path[at + 1].address == (branch.operands[0].imm & 0xFFFFFFFF)
            if not taken and path[at + 1].address != branch.address + branch.size:
                continue
            for cmp_at in range(at - 1, -1, -1):
                cmp = path[cmp_at]
                if writes(cmp, reg):
                    return None
                if (cmp.mnemonic == "cmp" and len(cmp.operands) == 2 and
                        cmp.operands[0].type == X86_OP_REG and cmp.operands[0].reg == reg and
                        cmp.operands[1].type == X86_OP_IMM):
                    count = unsigned_switch_count(cmp.operands[1].imm, branch.mnemonic, taken)
                    if count is not None:
                        return count, branch.address
                if X86_REG_EFLAGS in decoded(cmp).regs_access()[1]:
                    break
        return None

    sites = {}
    instruction_spans = None
    for block in candidates:
        chain, seen = [block], {block.start}
        while len(chain) < 32 and sum(len(b.instructions) for b in chain) < 128:
            preds = predecessors[chain[0].start]
            if len(preds) != 1:
                break
            previous = next(iter(preds))
            if previous in seen or previous not in function.blocks:
                break
            seen.add(previous)
            chain.insert(0, function.blocks[previous])
        path = [decoded(ins) for part in chain for ins in part.instructions]
        jump = path[-1]
        reg = jump.operands[0].reg
        load_at = next((at for at in range(len(path) - 2, -1, -1) if writes(path[at], reg)), None)
        if load_at is None:
            continue
        load = path[load_at]
        ops = load.operands
        if not (load.mnemonic == "mov" and len(ops) == 2 and
                ops[0].type == X86_OP_REG and ops[0].reg == reg and ops[0].size == 4 and
                ops[1].type == X86_OP_MEM and ops[1].size == 4):
            continue
        mem = ops[1].mem
        if mem.base or not mem.index or mem.scale != 4 or mem.segment:
            continue
        if any(writes(ins, mem.index) for ins in path[load_at + 1:-1]):
            continue
        table = mem.disp & 0xFFFFFFFF
        bound = guard(path, mem.index, load_at)
        input_count = None
        if bound is None:
            remap_at = next((at for at in range(load_at - 1, -1, -1)
                             if writes(path[at], mem.index)), None)
            if remap_at is None:
                continue
            remap = path[remap_at]
            if not (remap.mnemonic == "movzx" and len(remap.operands) == 2 and
                    remap.operands[0].type == X86_OP_REG and remap.operands[0].reg == mem.index and
                    remap.operands[1].type == X86_OP_MEM and remap.operands[1].size == 1):
                continue
            mapping = remap.operands[1].mem
            if bool(mapping.base) == bool(mapping.index) or mapping.scale != 1 or mapping.segment:
                continue
            input_bound = guard(path, mapping.base or mapping.index, remap_at)
            if input_bound is None:
                continue
            input_count, guard_address = input_bound
            raw_map = read_va(mapping.disp & 0xFFFFFFFF, input_count)
            if raw_map is None or len(raw_map) != input_count:
                continue
            count = remapped_switch_count(table, mapping.disp & 0xFFFFFFFF, raw_map)
            if count is None:
                continue
        else:
            count, guard_address = bound
        raw = read_va(table, count * 4)
        if raw is None or len(raw) != count * 4:
            continue
        targets = [int.from_bytes(raw[at:at + 4], "little") for at in range(0, len(raw), 4)]
        if instruction_spans is None:
            instruction_spans = InstructionSpanIndex()
            instruction_spans.add_function({"blocks": [
                {"instructions": [(ins.address, ins.size) for ins in part.instructions]}
                for part in function.blocks.values()]})
        if any(not is_code(target) or instruction_spans.overlaps(target) for target in targets):
            continue
        sites[jump.address] = {
            "jump": jump.address, "load": load.address, "table": table, "count": count,
            "guard": guard_address, "input_count": input_count,
            "register": md.reg_name(reg), "index_register": md.reg_name(mem.index), "targets": targets,
        }
    return sites


def install_ac1_jump_table_bounds(disassembler, register_switches: bool = True) -> None:
    """Apply AC1-local bounds and discover negatively biased switch tables."""
    from types import MethodType
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

    original = disassembler.jump_table_targets
    disassembler.ac1_switch_table_ranges = set()
    disassembler.ac1_active_register_switches = {}

    def bounded(self, insn, limit=256):
        if insn.mnemonic == "jmp" and insn.operands and insn.operands[0].type == X86_OP_REG:
            site = self.ac1_active_register_switches.get(insn.address)
            return list(site["targets"]) if site else []
        pinned_limit = ac1_pinned_switch_limit(insn)
        proven_limit = pinned_limit
        if pinned_limit is not None:
            limit = min(limit, pinned_limit)
        else:
            inferred = infer_remapped_switch_limit(
                self.md, self.read_bytes, insn, X86_OP_REG, X86_OP_MEM, X86_OP_IMM)
            if inferred is not None:
                limit = min(limit, inferred[0])
                proven_limit = inferred[0]
        targets = list(original(insn, limit))
        forward_count = len(targets)
        if (insn.mnemonic == "jmp" and insn.operands and
                insn.operands[0].type == X86_OP_MEM and
                insn.operands[0].mem.scale == 4 and insn.operands[0].mem.index):
            # Only scan backward when the index was explicitly negated in
            # the immediately preceding instruction. Unconditional backward
            # scans can mistake unrelated neighboring code pointers for arms
            # (for example, a byte table adjacent to a switch). The AC1
            # memmove switch at F9FD6E is `neg ecx; jmp [ecx*4+F9FE40]`.
            jump_index = insn.operands[0].mem.index
            neg = next(self.md.disasm(self.read_bytes(insn.address - 2, 2),
                                      insn.address - 2), None)
            bound = infer_negated_switch_bound(
                self.md, self.read_bytes, insn, X86_OP_REG, X86_OP_MEM, X86_OP_IMM)
            if (neg is not None and neg.mnemonic == "neg" and neg.operands and
                    neg.operands[0].type == X86_OP_REG and
                    neg.operands[0].reg == jump_index and bound is not None):
                table_va = insn.operands[0].mem.disp & 0xFFFFFFFF
                self.ac1_switch_table_ranges.add((table_va - 4 * (bound - 1), table_va + 4))
                # CMP/JB proves the selected negated indexes are -1 through
                # -(bound-1); index zero is handled by the forward table scan.
                for index in range(1, min(limit, bound)):
                    raw = self.read_bytes(table_va - 4 * index, 4)
                    if not raw or len(raw) < 4:
                        break
                    target = int.from_bytes(raw, "little")
                    if not self.is_code_address(target):
                        break
                    if target not in targets:
                        targets.append(target)
            if targets or proven_limit is not None:
                table_va = insn.operands[0].mem.disp & 0xFFFFFFFF
                self.ac1_switch_table_ranges.add(
                    (table_va, table_va + 4 * (proven_limit or max(1, forward_count))))
        return targets

    disassembler.jump_table_targets = MethodType(bounded, disassembler)
    if not register_switches:
        return
    original_function = disassembler.disassemble_function

    def recover(self, address, iat=None):
        previous = self.ac1_active_register_switches
        self.ac1_active_register_switches = {}
        try:
            function = original_function(address, iat)
            if function is None or not function.blocks:
                return function
            initial_blocks = len(function.blocks)
            current, rejected = {}, set()
            for _ in range(64):
                sites = register_switch_sites(function, self.md, self.read_bytes,
                                              self.is_code_address, current)
                # New case edges can invalidate a formerly unique predecessor.
                # Never oscillate back to that provisional proof after removal.
                rejected.update(jump for jump, site in current.items() if sites.get(jump) != site)
                sites = {jump: site for jump, site in sites.items() if jump not in rejected}
                if sites == current:
                    for jump, site in sites.items():
                        self.ac1_switch_table_ranges.add((site["table"], site["table"] + 4 * site["count"]))
                        for block in function.blocks.values():
                            if block.instructions and block.instructions[-1].address == jump:
                                block.successors = sorted(set(block.successors) | set(site["targets"]))
                    function.register_switches = list(sites.values())
                    function.register_switch_added_blocks = len(function.blocks) - initial_blocks
                    return function
                current = sites
                self.ac1_active_register_switches = sites
                function = original_function(address, iat)
            raise ValueError(f"register-switch CFG did not converge at 0x{address:08X} after 64 passes")
        finally:
            self.ac1_active_register_switches = previous

    disassembler.disassemble_function = MethodType(recover, disassembler)


def infer_negated_switch_bound(md, read_va, jump_insn, reg_type: int,
                               mem_type: int, imm_type: int) -> int | None:
    """Return the guarded range for `cmp index,N; jb neg; neg index; jmp [index*4+base]`.

    Only this proven unsigned range is used for backward table discovery; a
    code-address heuristic alone can pick up dwords embedded in unrelated
    instructions next to the table.
    """
    if (jump_insn.mnemonic != "jmp" or not jump_insn.operands or
            jump_insn.operands[0].type != mem_type):
        return None
    mem = jump_insn.operands[0].mem
    if not mem.index or mem.scale != 4:
        return None
    neg_addr = jump_insn.address - 2
    try:
        neg = next(md.disasm(read_va(neg_addr, 2), neg_addr), None)
    except Exception:
        return None
    if (neg is None or neg.address + neg.size != jump_insn.address or
            neg.mnemonic != "neg" or not neg.operands or
            neg.operands[0].type != reg_type or neg.operands[0].reg != mem.index):
        return None

    # Try short decodes ending at NEG. Accept only a direct CMP of the index
    # followed by an unsigned lower-bound branch to this exact NEG.
    for start in range(max(0, neg_addr - 24), neg_addr):
        try:
            stream = list(md.disasm(read_va(start, neg_addr - start), start))
        except Exception:
            continue
        if not stream or stream[-1].address + stream[-1].size != neg_addr:
            continue
        for branch_at, branch in enumerate(stream):
            if branch.mnemonic not in ("jb", "jnae", "jc") or not branch.operands:
                continue
            if branch.operands[0].type != imm_type or branch.operands[0].imm != neg_addr:
                continue
            if branch_at == 0:
                continue
            cmp = stream[branch_at - 1]
            if (cmp.mnemonic != "cmp" or len(cmp.operands) != 2 or
                    cmp.operands[0].type != reg_type or
                    cmp.operands[0].reg != mem.index or
                    cmp.operands[1].type != imm_type):
                continue
            bound = int(cmp.operands[1].imm)
            if 1 <= bound <= 256:
                return bound
    return None


def resolve_jump_table_with_limit(lifter, table_va: int, limit: int) -> list[int]:
    """Bound the donor resolver without changing its shared source."""
    targets = []
    if not lifter.read_va:
        return targets
    for index in range(limit):
        raw = lifter.read_va(table_va + 4 * index, 4)
        if not raw or len(raw) < 4:
            break
        target = int.from_bytes(raw, "little")
        if not lifter.in_image(target):
            break
        targets.append(target)
    return targets


def resolve_jump_table_in_decoded_blocks(lifter, table_va: int,
                                         block_starts: set[int],
                                         limit: int = 256) -> list[int]:
    """Resolve switch entries against the function's already validated CFG.

    The donor disassembler tolerates a leading non-code dword before table
    arms; the donor lifter stops at that first dword. AC1 has such a table at
    0x00F9FC08: index zero overlaps the preceding jump, while indices 1..3
    target local blocks. It also has a table at 0x00F9FD04 addressed by
    ECX=count-4, so the four entries are at indices -4..-1. The decoded CFG
    is the authority for arm membership; search both sides of the encoded
    base, retaining only dwords that name local block starts.
    """
    targets = []
    if not lifter.read_va:
        return targets
    # Index zero is shared. Scan outward independently because switch code
    # may bias the index so its table lies before the encoded base.
    for direction in (1, -1):
        misses = 0
        found = False
        for distance in range(limit):
            index = distance * direction
            try:
                raw = lifter.read_va(table_va + 4 * index, 4)
            except Exception:
                break
            if not raw or len(raw) < 4:
                break
            target = int.from_bytes(raw, "little")
            if target in block_starts:
                if target not in targets:
                    targets.append(target)
                misses = 0
                found = True
            else:
                misses += 1
                # Permit an invalid prefix for forward tables whose base is
                # inside/just before an instruction, then stop after a short
                # gap. Backward scans start at the last table entry, so stop
                # at the first gap once a valid run has been found.
                if found and misses >= 2:
                    break
                if not found and misses >= 4:
                    break
    return targets


def translate_directional_string_instruction(insn) -> list[str] | None:
    """Lower x86 string operations with the architectural DF pointer stride.

    The pinned donor lifter always increments ESI/EDI for MOVS/STOS/LODS and
    the compare string ops. AC1's scalar memmove sets DF with STD on its
    overlap path, so that assumption corrupts backward copies.
    """
    parts = insn.mnemonic.lower().split()
    if not parts:
        return None
    if len(parts) == 1 and parts[0] == "cld":
        return ["c->df = 0;"]
    if len(parts) == 1 and parts[0] == "std":
        return ["c->df = 1;"]
    rep = parts[0] if len(parts) > 1 else None
    base = parts[-1]
    if base not in {"movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
                    "lodsb", "lodsw", "lodsd", "cmpsb", "cmpsw", "cmpsd",
                    "scasb", "scasw", "scasd"}:
        return None
    if insn.operands:
        from capstone.x86 import X86_OP_REG
        # Capstone includes the implicit accumulator in STOS/LODS/SCAS. Only
        # reject XMM operands, which identify the SSE MOVSD/CMPSD forms.
        if any(operand.type == X86_OP_REG and
               insn.reg_name(operand.reg).lower().startswith("xmm")
               for operand in insn.operands):
            return None
    width = {"b": 1, "w": 2, "d": 4}[base[-1]]
    read = {1: "rd8", 2: "rd16", 4: "rd32"}[width]
    write = {1: "wr8", 2: "wr16", 4: "wr32"}[width]
    step = f"(c->df ? (uint32_t)-{width} : {width}u)"
    if base.startswith("movs"):
        statement = f"{write}(c->edi, {read}(c->esi)); c->esi += _step; c->edi += _step;"
    elif base.startswith("stos"):
        value = {1: "R8L(c->eax)", 2: "R16(c->eax)", 4: "c->eax"}[width]
        statement = f"{write}(c->edi, {value}); c->edi += _step;"
    elif base.startswith("lods"):
        if width == 1:
            load = f"SET8L(c->eax, {read}(c->esi));"
        elif width == 2:
            load = f"SET16(c->eax, {read}(c->esi));"
        else:
            load = f"c->eax = {read}(c->esi);"
        statement = f"{load} c->esi += _step;"
    else:
        accumulator = {1: "R8L(c->eax)", 2: "R16(c->eax)", 4: "c->eax"}[width]
        if base.startswith("cmps"):
            statement = f"flags_sub(c, {read}(c->esi), {read}(c->edi), {width}); c->esi += _step; c->edi += _step;"
        else:
            statement = f"flags_sub(c, {accumulator}, {read}(c->edi), {width}); c->edi += _step;"
        if rep in ("repe", "repz"):
            return [f"{{ uint32_t _step = {step}; while (c->ecx) {{ {statement} c->ecx--; if (!c->zf) break; }} }}"]
        if rep in ("repne", "repnz"):
            return [f"{{ uint32_t _step = {step}; while (c->ecx) {{ {statement} c->ecx--; if (c->zf) break; }} }}"]
        if rep:
            return [f"{{ uint32_t _step = {step}; while (c->ecx) {{ {statement} c->ecx--; }} }}"]
        return [f"{{ uint32_t _step = {step}; {statement} }}"]

    if rep:
        return [f"{{ uint32_t _step = {step}; while (c->ecx) {{ {statement} c->ecx--; }} }}"]
    return [f"{{ uint32_t _step = {step}; {statement} }}"]


def resolve_tooling(config: dict, args) -> tuple[Path, Path]:
    python = (args.python or Path(config.get("python", PYTHON_DEFAULT))).resolve()
    pcrecomp = (args.pcrecomp or Path(config.get("pcrecomp_root", DUKE_DEFAULT))).resolve()
    if not python.is_file():
        raise RuntimeError(f"Python environment not found: {python}")
    if not pcrecomp.is_dir():
        raise RuntimeError(f"pcrecomp source not found: {pcrecomp}")
    pe_dir = pcrecomp / "tools/pe"
    dis_dir = pcrecomp / "tools/disasm"
    lift_dir = pcrecomp / "tools/lift"
    for path in (pe_dir / "pe_analyze.py", dis_dir / "disasm32.py", lift_dir / "lift32_cpu.py"):
        if not path.is_file():
            raise RuntimeError(f"required pcrecomp tool missing: {path}")
    return python, pcrecomp


def ac1_array_constructor_call_edges(function) -> list[dict]:
    """Extract only proven constant ctor/dtor args for direct calls to F9FA43.

    F9FA43 reads its fourth and fifth stack arguments as constructor and
    exception-cleanup function pointers. In the current AC1 binary these are
    pushed as constants before the remaining three arguments. Require five
    pushes, reject intervening calls/ESP changes, and reject stack writes that
    could overwrite either pointer slot.
    """
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    from capstone.x86_const import X86_REG_EBP, X86_REG_ESP

    edges = []
    for block in function.blocks.values():
        instructions = list(block.instructions)
        for index, insn in enumerate(instructions):
            if (insn.mnemonic != "call" or not insn.operands or
                    insn.operands[0].type != X86_OP_IMM or
                    (insn.operands[0].imm & 0xFFFFFFFF) != AC1_ARRAY_CTOR_HELPER):
                continue
            pushed = []  # nearest push first: arg1, arg2, arg3, ctor, cleanup
            proven = True
            for prev in reversed(instructions[:index]):
                if prev.mnemonic == "push":
                    pushed.append(prev)
                    if len(pushed) == 5:
                        break
                    continue
                if prev.mnemonic in ("call", "ret", "retf", "jmp", "leave", "pop",
                                     "enter", "pushf", "pushfd", "pushfw", "pusha", "pushad",
                                     "popf", "popfd", "popfw", "popa", "popad"):
                    proven = False
                    break
                # Any explicit ESP register use is conservatively rejected;
                # this covers destination writes and aliases such as XCHG.
                if any(operand.type == X86_OP_REG and operand.reg == X86_REG_ESP
                       for operand in prev.operands):
                    proven = False
                    break
                # Reject writes (and conservatively reads) into the ctor/dtor
                # slots. Convert current [ESP+disp] to call-time ESP by
                # accounting for pushes already crossed in this reverse walk:
                # current ESP is call ESP + 4*later pushes.
                if prev.operands and prev.operands[0].type == X86_OP_MEM:
                    mem = prev.operands[0].mem
                    if mem.base == X86_REG_EBP:
                        # Without a proven frame-to-ESP relation, an EBP store
                        # could alias either argument slot.
                        proven = False
                        break
                    if mem.base == X86_REG_ESP:
                        if mem.index:
                            proven = False
                            break
                        slot = int(mem.disp) + 4 * len(pushed)
                        width = max(1, prev.operands[0].size)
                        if ((slot < 16 and slot + width > 12) or
                                (slot < 20 and slot + width > 16)):
                            proven = False
                            break
            if not proven or len(pushed) != 5:
                continue
            ctor_push, cleanup_push = pushed[3], pushed[4]
            if (not ctor_push.operands or ctor_push.operands[0].type != X86_OP_IMM or
                    not cleanup_push.operands or cleanup_push.operands[0].type != X86_OP_IMM):
                continue
            edges.append({
                "callsite": insn.address,
                "constructor": ctor_push.operands[0].imm & 0xFFFFFFFF,
                "cleanup": cleanup_push.operands[0].imm & 0xFFFFFFFF,
            })
    return edges


def ac1_schema_callback_call_edges(function) -> list[dict]:
    """Extract constant arg2 callbacks passed to AC1's 438EF0 schema helper."""
    return ac1_four_dword_arg2_callback_edges(function, AC1_SCHEMA_CALLBACK_HELPER)


def ac1_reader_callback_call_edges(function) -> list[dict]:
    """Extract constant arg2 callbacks passed to AC1's 9312F0 reader helper."""
    return ac1_four_dword_arg2_callback_edges(function, AC1_READER_CALLBACK_HELPER)


def ac1_four_dword_arg2_callback_edges(function, helper_target: int) -> list[dict]:
    """Recover literal arg2 for the two exact AC1 four-DWORD callback helpers."""
    if helper_target not in AC1_ARG2_CALLBACK_HELPERS:
        return []
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    from capstone.x86_const import X86_REG_EBP, X86_REG_ESP

    edges = []
    for block in function.blocks.values():
        instructions = list(block.instructions)
        for index, insn in enumerate(instructions):
            if (insn.mnemonic != "call" or not insn.operands or
                    insn.operands[0].type != X86_OP_IMM or
                    (insn.operands[0].imm & 0xFFFFFFFF) != helper_target):
                continue
            # The helper's RET 16 proves four stack arguments; nearest push is
            # arg1, so its callback is the second push encountered in reverse.
            pushed = []
            proven = True
            for prev in reversed(instructions[:index]):
                if prev.mnemonic == "push":
                    # This stack-argument layout is four DWORDs. Operand-size
                    # override and segment/word pushes change the ESP offsets.
                    if (len(prev.operands) != 1 or
                            prev.operands[0].size != 4):
                        proven = False
                        break
                    pushed.append(prev)
                    if len(pushed) == 4:
                        break
                    continue
                if prev.mnemonic in ("call", "ret", "retf", "jmp", "leave", "pop",
                                     "enter", "pushf", "pushfd", "pushfw", "pusha", "pushad",
                                     "popf", "popfd", "popfw", "popa", "popad"):
                    proven = False
                    break
                if any(operand.type == X86_OP_REG and operand.reg == X86_REG_ESP
                       for operand in prev.operands):
                    proven = False
                    break
                # At the indirect CALL, arg2 is [ESP+4..ESP+7]: arg1 occupies
                # [ESP], followed by the callback at +4. A frame-based access
                # can alias the target slot without a proven EBP-to-ESP
                # relation. Reject indexed ESP accesses; otherwise account
                # for DWORD pushes crossed while scanning back.
                for operand in prev.operands:
                    if operand.type != X86_OP_MEM:
                        continue
                    mem = operand.mem
                    if mem.base == X86_REG_EBP:
                        proven = False
                        break
                    if mem.base == X86_REG_ESP:
                        if mem.index:
                            proven = False
                            break
                        slot = int(mem.disp) + 4 * len(pushed)
                        width = max(1, operand.size)
                        if slot < 8 and slot + width > 4:
                            proven = False
                            break
                if not proven:
                    break
            if not proven or len(pushed) != 4:
                continue
            callback_push = pushed[1]
            if not callback_push.operands or callback_push.operands[0].type != X86_OP_IMM:
                continue
            edges.append({
                "callsite": insn.address,
                "callback": callback_push.operands[0].imm & 0xFFFFFFFF,
            })
    return edges


class InstructionSpanIndex:
    """Page-index exact instruction spans for fast mid-instruction checks."""

    PAGE_SHIFT = 12

    def __init__(self) -> None:
        self._pages: dict[int, list[tuple[int, int]]] = {}

    def add_function(self, function: dict) -> None:
        for block in function["blocks"]:
            for start, size in block["instructions"]:
                if size <= 0:
                    continue
                end = start + size
                span = (start, end)
                # Index a span on every page it touches, so cross-page x86
                # instructions are found without assuming a maximum length.
                for page in range(start >> self.PAGE_SHIFT, ((end - 1) >> self.PAGE_SHIFT) + 1):
                    self._pages.setdefault(page, []).append(span)

    def overlaps(self, target: int) -> bool:
        return any(start < target < end
                   for start, end in self._pages.get(target >> self.PAGE_SHIFT, ()))

    def contains(self, target: int) -> bool:
        """Include exact instruction starts when rejecting interior labels."""
        return any(start <= target < end
                   for start, end in self._pages.get(target >> self.PAGE_SHIFT, ()))


def linear_instruction_span_overlap(target: int, discovered: dict) -> bool:
    """Reference implementation retained for exact-equivalence tests."""
    for function in discovered.values():
        for block in function["blocks"]:
            for start, size in block["instructions"]:
                if start < target < start + size:
                    return True
    return False


def run_instruction_span_index_selftest() -> None:
    # Covers a span crossing into the target page, duplicate spans, exact
    # start/end boundaries, and non-overlap regions.
    discovered = {
        0x1000: {"blocks": [{"instructions": [
            [0x1FFF, 3], [0x1FFF, 3], [0x2004, 5], [0x2FFF, 2],
        ]}]},
        0x3000: {"blocks": [{"instructions": [[0x3008, 1], [0x3010, 6]]}]},
    }
    index = InstructionSpanIndex()
    for function in discovered.values():
        index.add_function(function)
    for target in range(0x1FF0, 0x3020):
        assert index.overlaps(target) == linear_instruction_span_overlap(target, discovered), (
            f"instruction-span index mismatch at 0x{target:08X}"
        )
        assert index.contains(target) == any(
            start <= target < start + size
            for function in discovered.values() for block in function["blocks"]
            for start, size in block["instructions"])


def validate_callback_code_entry(target: int, disassembler, info, iat,
                                 discovered: dict | InstructionSpanIndex) -> bool:
    """Validate a callback as executable code and reject known mid-instruction pointers."""
    if not (info.code_start <= target < info.code_end and
            disassembler.is_code_address(target)):
        return False
    if not disassembler.probes_as_function_body(target):
        return False
    overlaps = (discovered.overlaps(target)
                if isinstance(discovered, InstructionSpanIndex)
                else linear_instruction_span_overlap(target, discovered))
    if overlaps:
        return False
    try:
        candidate = disassembler.disassemble_function(target, iat)
    except Exception:
        return False
    return bool(candidate and candidate.blocks and target in candidate.blocks)


def constant_code_immediate_edges(function) -> list[dict]:
    """Read pointer-sized literals only from recovered MOV/PUSH instructions.

    Use address adjacency across block boundaries too: PUSH return; JMP callee
    is a manual call, not evidence that the pushed continuation is a callback.
    No alignment is imposed; function-boundary evidence is checked separately.
    """
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

    instructions = {ins.address: ins for block in function.blocks.values()
                    for ins in block.instructions}
    edges = []
    for ins in sorted(instructions.values(), key=lambda item: item.address):
        ops = ins.operands
        if (ins.mnemonic == "mov" and len(ops) == 2 and
                ops[0].type in (X86_OP_REG, X86_OP_MEM) and ops[0].size == 4 and
                ops[1].type == X86_OP_IMM and ops[1].size == 4):
            operand = ops[1]
        elif (ins.mnemonic == "push" and len(ops) == 1 and
              ops[0].type == X86_OP_IMM and ops[0].size == 4 and
              # PUSH imm8 is sign extended by Capstone to size 4.
              len(ins.bytes) >= 5 and ins.bytes[-5] == 0x68):
            operand = ops[0]
        else:
            continue
        following = instructions.get(ins.address + ins.size)
        edges.append({
            "callsite": ins.address,
            "target": operand.imm & 0xFFFFFFFF,
            "instruction": f"{ins.mnemonic} {ins.op_str}",
            "manual_call": (ins.mnemonic == "push" and following is not None and
                            following.mnemonic in ("jmp", "ljmp")),
        })
    return edges


def code_immediate_has_function_boundary(target, dis, recovered) -> bool:
    """Require an existing entry or padding/RET/JMP ending at the target.

    These local byte suffixes corroborate an immediate pointer; they never
    discover roots themselves and do not relax the shared entry/span checks.
    Keep this requirement scoped to the generic code-immediate rule.
    """
    if target in recovered:
        return True
    for size, opcodes in ((1, (0xCC, 0x90, 0xC3)), (3, (0xC2,)),
                          (5, (0xE9,)), (2, (0xEB,))):
        start = target - size
        if not dis.is_code_address(start):
            continue
        preceding = dis.read_bytes(start, size)
        if preceding and len(preceding) == size and preceding[0] in opcodes:
            return True
    # MSVC places a function's switch table after its code; the next function
    # may start right at the table's end (e.g. 0x00D00260). Only tables the
    # lifter itself bounded count, optionally followed by CC padding.
    ranges = getattr(dis, "ac1_switch_table_ranges", ())
    cache = getattr(dis, "_ac1_switch_end_cache", None)
    if cache is None or cache[0] != len(ranges):
        cache = (len(ranges), {end for _, end in ranges})
        dis._ac1_switch_end_cache = cache
    for gap_size in range(16):
        end = target - gap_size
        if end in cache[1]:
            gap = dis.read_bytes(end, gap_size) if gap_size else b""
            if gap is not None and all(byte == 0xCC for byte in gap):
                return True
    return False


def code_immediate_rejection(target, dis, info, iat, spans, results) -> str | None:
    """Keep entry validation shared with the ABI-specific callback rules."""
    if not (info.code_start <= target < info.code_end and dis.is_code_address(target)):
        return "non_executable"
    if spans.overlaps(target):
        return "mid_instruction"
    # An instruction boundary inside a known body can be a return address or
    # switch label. It needs independent entry evidence, not just a MOV/PUSH.
    if target not in results and spans.contains(target):
        return "interior_instruction"
    if not code_immediate_has_function_boundary(target, dis, results):
        return "not_function_boundary"
    if not validate_callback_code_entry(target, dis, info, iat, spans):
        return "invalid_entry"
    return None


def initialized_table_ranges(info) -> list[tuple[int, int, str]]:
    """Only file-backed initialized bytes, never the zero-filled data tail."""
    return [(info.image_base + section.virtual_address,
             info.image_base + section.virtual_address + section.raw_size, section.name)
            for section in info.sections
            if section.name.lower() in (".rdata", ".data", "_rdata", ".data1")
            and section.characteristics & 0x40 and not section.characteristics & 0x20000000
            and section.raw_size >= 4]


def loaded_register_transfer(instructions, index, register, md) -> str | None:
    """Prove a MOV result reaches a same-block CALL/JMP before a clobber.

    Re-decode only this small candidate suffix for Capstone's implicit register
    writes (the donor Instruction wrapper retains operands but not regs_access).
    Partial-register writes kill the proof as well. Other calls/branches stop it.
    """
    def family(reg):
        name = md.reg_name(reg)
        for names in (("eax", "ax", "al", "ah"), ("ebx", "bx", "bl", "bh"),
                      ("ecx", "cx", "cl", "ch"), ("edx", "dx", "dl", "dh"),
                      ("esi", "si"), ("edi", "di"), ("esp", "sp"), ("ebp", "bp")):
            if name in names:
                return names[0]
        return name

    from capstone.x86 import X86_OP_REG
    for following in instructions[index + 1:]:
        if following.mnemonic in ("call", "jmp"):
            ops = following.operands
            return (following.mnemonic if len(ops) == 1 and ops[0].type == X86_OP_REG
                    and ops[0].reg == register else None)
        if (following.mnemonic.startswith(("j", "ret", "loop", "int")) or
                following.mnemonic in ("lcall", "ljmp", "ud2", "hlt")):
            return None
        decoded = next(md.disasm(bytes(following.bytes), following.address, count=1), None)
        if decoded is None:
            return None
        if any(family(reg) == family(register) for reg in decoded.regs_access()[1]):
            return None
    return None


def indexed_table_references(function, ranges, md) -> tuple[list[dict], set[int]]:
    """Extract actual memory reads, plus count unhandled pre-scaled loads."""
    from capstone import CS_AC_READ
    from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_REG_FS, X86_REG_GS
    references = {}
    prescaled = set()
    for block in function.blocks.values():
        instructions = block.instructions
        for index, ins in enumerate(instructions):
            if ins.mnemonic == "lea":
                continue
            for operand in ins.operands:
                if operand.type != X86_OP_MEM or not operand.access & CS_AC_READ:
                    continue
                mem = operand.mem
                table = mem.disp & 0xFFFFFFFF
                if mem.segment in (X86_REG_FS, X86_REG_GS):
                    continue
                section = next((name for start, end, name in ranges if start <= table < end), None)
                if section is None:
                    continue
                kind = None
                if mem.index and mem.scale == 4:
                    kind = "indexed"
                elif (ins.mnemonic == "mov" and len(ins.operands) == 2 and
                      ins.operands[0].type == X86_OP_REG and ins.operands[0].size == 4
                      and operand is ins.operands[1] and operand.size == 4):
                    transfer = loaded_register_transfer(instructions, index, ins.operands[0].reg, md)
                    if not mem.base and not mem.index and transfer:
                        kind = "absolute"
                    elif (bool(mem.base) != bool(mem.index) and mem.scale == 1 and transfer == "call"):
                        prescaled.add(ins.address)
                if kind:
                    references[(ins.address, table, kind)] = {
                        "callsite": ins.address, "table": table, "kind": kind,
                        "section": section, "instruction": f"{ins.mnemonic} {ins.op_str}",
                    }
    return list(references.values()), prescaled


class IndexedTableDiscovery:
    """Deferred table sweeps share the immediate rule's validation and queue.

    Revisit tables after each recovered wave: a formerly unproven boundary may
    now be an independently recovered entry, allowing a longer valid prefix.
    Counts describe the last sweep's unique tables/stopping entries, rather
    than multiplying the same rejection by callers or discovery waves.
    """
    def __init__(self, info, dis, enabled=True, entry_cap=1024):
        self.info, self.dis, self.enabled, self.entry_cap = info, dis, enabled, entry_cap
        self.ranges = initialized_table_ranges(info) if enabled else []
        self.references = collections.defaultdict(dict)
        self.prescaled = set()
        self.edges = collections.defaultdict(list)
        self.queued = set()
        self.tables = []
        self.last_count = -1
        self.waves = 0
        self.depth_rejections = 0
        self.late_rejections = collections.Counter()

    def collect(self, function, caller, depth, max_depth):
        if not self.ranges:
            return
        refs, prescaled = indexed_table_references(function, self.ranges, self.dis.md)
        self.prescaled.update(prescaled)
        for ref in refs:
            if depth >= max_depth:
                self.depth_rejections += 1
                continue
            self.references[(ref["table"], ref["kind"])][(caller, ref["callsite"])] = {
                **ref, "caller": caller, "depth": depth + 1, "role": "indexed_table",
            }

    def needs_sweep(self, results):
        return bool(self.references) and self.last_count != len(results)

    def sweep(self, results, spans, iat, queue, queued):
        self.last_count = len(results)
        self.waves += 1
        self.edges.clear()
        self.tables = []
        validation = {}
        switch_ranges = getattr(self.dis, "ac1_switch_table_ranges", ())
        for (table, kind), refs in sorted(self.references.items()):
            end = next(end for start, end, _ in self.ranges if start <= table < end)
            report = {"table": table, "kind": kind, "reference_count": len(refs), "entry_count": 0}
            cap = 1 if kind == "absolute" else self.entry_cap
            # NULL slots are common in function tables (e.g. 0x0191B8C0 starts
            # with three); skip them, but a long zero run ends the table.
            null_run = 0
            report["null_entries"] = 0
            for index in range(cap):
                slot = table + 4 * index
                report["stop_slot"] = slot
                report.pop("stop_target", None)
                if any(start <= slot < stop for start, stop in switch_ranges):
                    report["stop_reason"] = "bounded_switch"
                    break
                if slot + 4 > end:
                    report["stop_reason"] = "section_end"
                    break
                raw = self.dis.read_bytes(slot, 4)
                if raw is None or len(raw) != 4:
                    report["stop_reason"] = "unreadable_entry"
                    break
                target = int.from_bytes(raw, "little")
                report["stop_target"] = target
                if target == 0 and kind != "absolute":
                    null_run += 1
                    if null_run > 8:
                        report["stop_reason"] = "null_run"
                        break
                    continue
                pending_nulls, null_run = null_run, 0
                if target not in validation:
                    validation[target] = code_immediate_rejection(
                        target, self.dis, self.info, iat, spans, results)
                if validation[target]:
                    report["stop_reason"] = validation[target]
                    break
                report["entry_count"] += 1
                report["null_entries"] += pending_nulls
                self.edges[target].extend({**ref, "slot": slot, "index": index, "target": target}
                                          for ref in refs.values())
                if target not in results and target not in queued:
                    queued.add(target)
                    self.queued.add(target)
                    queue.append((target, min(ref["depth"] for ref in refs.values())))
            else:
                report["stop_reason"] = "single_entry" if kind == "absolute" else "entry_cap"
                report["stop_slot"] = table + 4 * cap
                report.pop("stop_target", None)
            self.tables.append(report)

    def summary(self, results):
        rejections = collections.Counter(table["stop_reason"] for table in self.tables
                                         if table["stop_reason"] != "single_entry")
        if self.depth_rejections:
            rejections["depth_limit"] = self.depth_rejections
        return {
            "enabled": self.enabled, "entry_cap": self.entry_cap, "waves": self.waves,
            "pending_sweep": self.needs_sweep(results),
            "candidate_table_count": len({table for table, _ in self.references}),
            "table_count": len({table["table"] for table in self.tables if table["entry_count"]}),
            "accepted_edges": sum(map(len, self.edges.values())),
            "rejected_candidates": dict(sorted(rejections.items())),
            "late_rejected_candidates": dict(sorted(self.late_rejections.items())),
            "prescaled_loads_followed_by_indirect_call": len(self.prescaled),
            "tables": self.tables,
        }


def discover_entry_cone(binary: Path, pcrecomp: Path, max_functions: int, max_depth: int,
                        extra_seeds: list[int], code_immediates: bool = True,
                        indexed_tables: bool = True, register_switches: bool = True):
    sys.path[:0] = [str(pcrecomp / "tools/pe"), str(pcrecomp / "tools/disasm")]
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler

    info = analyze_pe(str(binary))
    code = binary.read_bytes()
    iat = build_iat_map(info)
    dis = Disassembler(code, info.image_base, info.sections)
    install_ac1_jump_table_bounds(dis, register_switches=register_switches)
    entry = info.image_base + info.entry_point_rva
    initial_seeds = sorted({entry, *extra_seeds})
    queue = collections.deque((seed, 0) for seed in initial_seeds)
    queued = set(initial_seeds)
    results = {}
    instruction_spans = InstructionSpanIndex()
    failures = []
    callback_edges = []
    immediate_pending = collections.defaultdict(list)
    immediate_edges = collections.defaultdict(list)
    immediate_queued = set()
    immediate_rejected = collections.Counter()
    immediate_waves = 0
    indexed = IndexedTableDiscovery(info, dis, indexed_tables)
    while (queue or immediate_pending or indexed.needs_sweep(results)) and len(results) < max_functions:
        if not queue:
            # First exhaust the direct/ABI-specific cone, so instruction spans
            # can reject continuations and mid-instruction literals. Repeat
            # after every new wave, scanning newly recovered bodies to closure.
            if immediate_pending:
                immediate_waves += 1
            for target, edges in sorted(immediate_pending.items()):
                reason = code_immediate_rejection(target, dis, info, iat,
                                                 instruction_spans, results)
                if reason:
                    immediate_rejected[reason] += len(edges)
                    continue
                immediate_edges[target].extend(edges)
                if target not in results and target not in queued:
                    queued.add(target)
                    immediate_queued.add(target)
                    queue.append((target, min(edge["depth"] for edge in edges)))
            immediate_pending.clear()
            if not queue and indexed.needs_sweep(results):
                indexed.sweep(results, instruction_spans, iat, queue, queued)
            if not queue:
                break
        address, depth = queue.popleft()
        if address in results:
            continue
        if address in immediate_queued or address in indexed.queued:
            # Earlier roots in this wave may have recovered the instruction
            # containing this target since the batch was validated.
            if instruction_spans.overlaps(address) or instruction_spans.contains(address):
                reason = ("mid_instruction" if instruction_spans.overlaps(address)
                          else "interior_instruction")
                immediate_rejected[reason] += len(immediate_edges.pop(address, []))
                indexed.late_rejections[reason] += len(indexed.edges.pop(address, []))
                queued.discard(address)
                continue
        try:
            function = dis.disassemble_function(address, iat)
        except Exception as exc:  # report concrete decoder limits per function
            failures.append({"address": address, "error": repr(exc)})
            continue
        if function is None or not function.blocks:
            failures.append({"address": address, "error": "not a decodable code address"})
            continue
        instructions = [ins for block in function.blocks.values() for ins in block.instructions]
        results[address] = {
            "address": address,
            "end": function.end,
            "size": function.size,
            "entry_kind": function.entry_kind,
            "is_thunk": function.is_thunk,
            "calls_to": sorted(function.calls_to),
            "jump_targets": sorted(function.jump_targets),
            "instruction_count": len(instructions),
            "block_count": len(function.blocks),
            "depth": depth,
            "blocks": [
                {
                    "start": block.start,
                    "end": block.end,
                    "successors": sorted(set(block.successors)),
                    "is_exit": block.is_exit,
                    "instructions": [[ins.address, ins.size] for ins in block.instructions],
                }
                for block in sorted(function.blocks.values(), key=lambda item: item.start)
            ],
        }
        if getattr(function, "register_switches", None):
            results[address]["register_switches"] = function.register_switches
            results[address]["register_switch_added_blocks"] = function.register_switch_added_blocks
        instruction_spans.add_function(results[address])
        indexed.collect(function, address, depth, max_depth)
        if code_immediates:
            for edge in constant_code_immediate_edges(function):
                target = edge["target"]
                if not (info.code_start <= target < info.code_end and dis.is_code_address(target)):
                    immediate_rejected["non_executable"] += 1
                elif edge.pop("manual_call"):
                    immediate_rejected["manual_call"] += 1
                elif depth >= max_depth:
                    immediate_rejected["depth_limit"] += 1
                else:
                    immediate_pending[target].append({
                        **edge, "caller": address, "role": "code_immediate", "depth": depth + 1,
                    })
        tail_target = direct_tail_thunk_target(function)
        if (tail_target is not None and
                validate_callback_code_entry(tail_target, dis, info, iat, instruction_spans)):
            # Keep the exact architectural jump destination. Do not advance
            # through alignment bytes or rewrite the target based on padding.
            results[address]["is_thunk"] = True
            results[address]["tail_thunk_target"] = tail_target
        if depth >= max_depth:
            continue
        for target in sorted(function.calls_to):
            if target not in results and target not in queued and info.code_start <= target < info.code_end:
                queued.add(target)
                queue.append((target, depth + 1))
        if tail_target is not None and results[address].get("tail_thunk_target") == tail_target:
            if tail_target not in results and tail_target not in queued:
                queued.add(tail_target)
                queue.append((tail_target, depth + 1))
        for target in terminal_external_direct_jump_targets(function):
            if target == tail_target or target in results or target in queued:
                continue
            if not validate_callback_code_entry(target, dis, info, iat, instruction_spans):
                continue
            queued.add(target)
            queue.append((target, depth + 1))
        for call_edge in ac1_array_constructor_call_edges(function):
            for role in ("constructor", "cleanup"):
                target = call_edge[role]
                if not validate_callback_code_entry(target, dis, info, iat, instruction_spans):
                    continue
                callback_edges.append({
                    "caller": address,
                    "callsite": call_edge["callsite"],
                    "role": role,
                    "target": target,
                    "depth": depth + 1,
                })
                if target not in results and target not in queued:
                    queued.add(target)
                    queue.append((target, depth + 1))
        for helper, role in AC1_ARG2_CALLBACK_HELPERS.items():
            for call_edge in ac1_four_dword_arg2_callback_edges(function, helper):
                target = call_edge["callback"]
                if not validate_callback_code_entry(target, dis, info, iat, instruction_spans):
                    continue
                callback_edges.append({
                    "caller": address,
                    "callsite": call_edge["callsite"],
                    "role": role,
                    "helper": helper,
                    "target": target,
                    "depth": depth + 1,
                })
                if target not in results and target not in queued:
                    queued.add(target)
                    queue.append((target, depth + 1))
    for edges in immediate_edges.values():
        callback_edges.extend(edges)
    for edges in indexed.edges.values():
        callback_edges.extend(edges)
    immediate_summary = {
        "enabled": code_immediates,
        "waves": immediate_waves,
        "accepted_edges": sum(map(len, immediate_edges.values())),
        "rejected_candidates": dict(sorted(immediate_rejected.items())),
        "pending_targets": len(immediate_pending),
    }
    return (info, dis, results, failures, len(queue), entry, iat, callback_edges,
            immediate_summary, indexed.summary(results))


FLAG_BITS = {name: 1 << i for i, name in enumerate(('cf', 'pf', 'af', 'zf', 'sf', 'of'))}
FLAG_ALL = sum(FLAG_BITS.values())
FLAG_HELPERS = {
    'flags_add': FLAG_ALL, 'flags_sub': FLAG_ALL,
    'flags_adc': FLAG_ALL, 'flags_sbb': FLAG_ALL,
    'flags_logicz': FLAG_ALL & ~FLAG_BITS['af'],
    'flags_incs': FLAG_ALL & ~FLAG_BITS['cf'],
    'flags_decs': FLAG_ALL & ~FLAG_BITS['cf'],
}


def codegen_instruction_groups(body: str):
    """Retain labels with the instruction they enter, including entry guards."""
    lines = body.splitlines()
    if len(lines) < 3 or lines[1] != '{' or lines[-1] != '}':
        raise ValueError('unexpected lifted function shape for codegen pass')
    prefix, groups, pending, group = lines[:2], [], [], []
    for line in lines[2:-1]:
        if re.fullmatch(r'L_[0-9A-F]{8}:', line):
            if group:
                groups.append(group)
                group = []
            pending.append(line)
        elif 'AC1_SET_GUEST_EIP(c,' in line:
            if group:
                groups.append(group)
            group = pending + [line]
            pending = []
        elif group:
            group.append(line)
        elif not pending:
            # A sparse function's entry guard runs once, before any local
            # label. It must not become a successor of its lowest VA block.
            prefix.append(line)
        else:
            pending.append(line)
    if group:
        groups.append(group)
    return prefix, pending, groups


def flag_flow(body, return_live=FLAG_ALL, call_defines=None, mode='cfg'):
    """Instruction CFG; writes describe emitted helper semantics, not x86 UB."""
    prefix, pending, groups = codegen_instruction_groups(body)
    texts = [re.sub(r'/\*.*?\*/', '', '\n'.join(g), flags=re.S) for g in groups]
    labels = {label: i for i, text in enumerate(texts)
              for label in re.findall(r'^(L_[0-9A-F]{8}):', text, re.M)}
    reads, writes, successors, eligible, calls, exits = [], [], [], [], {}, {}
    for i, text in enumerate(texts):
        helpers = re.findall(r'\b(flags_\w+)\(c\s*,', text)
        simple = (len(helpers) == 1 and helpers[0] in FLAG_HELPERS and
                  not re.search(r'\b(?:if|while|for|goto|return)\b|atomic_', text))
        write = FLAG_HELPERS[helpers[0]] if simple else 0
        read = sum(bit for flag, bit in FLAG_BITS.items() if re.search(r'\bc->'+flag+r'\b', text))
        if any(helper in ('flags_adc', 'flags_sbb') for helper in helpers):
            read |= FLAG_BITS['cf']
        direct = re.findall(r'\bAC1_CALL\(c, L_([0-9A-F]{8}), 0x\1u\)', text)
        direct_call = (len(direct) == 1 and
                       not re.search(r'\b(?:if|while|for|goto|return)\b', text))
        if direct_call:
            target = int(direct[0], 16)
            calls[i] = target
            write |= (call_defines or {}).get(target, 0)
        pointer = re.sub(r'\bc->\w+', 'member', text)
        safe = '|'.join(FLAG_HELPERS) + '|AC1_SET_GUEST_EIP|push32|pop32'
        pointer = re.sub(r'\b(?:'+safe+r')\(c(?=[,)])', 'safe(', pointer)
        opaque = bool(re.search(r'\bc\b|RECOMP_TODO\(|ac1_unsupported_instruction\(', pointer))
        # AC1_CALL guards/slow paths keep incoming diagnostic flags live. A
        # proven callee definition can kill successor demands, never this read.
        if opaque:
            read = FLAG_ALL
        if re.search(r'/\*\s*[0-9A-Fa-f]{8}:\s*(?:lahf|pushfd|pushf)\b', '\n'.join(groups[i])):
            read = FLAG_ALL
        targets = re.findall(r'\bgoto (L_[0-9A-F]{8});', text)
        edges = {labels[t] for t in targets if t in labels}
        terminal = bool(re.search(r'^\s*(?:goto L_[0-9A-F]{8};|(?:dispatch_jmp|guest_ret)\()', text, re.M))
        plain_return = ('return;' in text and
                        not re.search(r'\b(?:if|while|for|switch)\b', text))
        terminal |= plain_return
        if not terminal and i+1 < len(groups): edges.add(i+1)
        if 'return;' in text or (i+1 == len(groups) and not terminal):
            read |= return_live
            exits[i] = 'escape' if opaque else 'return'
        if any(t not in labels for t in targets):
            read = FLAG_ALL
            exits[i] = 'escape'
        if terminal and not edges and i not in exits: exits[i] = 'escape'
        if mode == 'straight' and (targets or i+1 < len(groups) and
                                  re.search(r'^L_[0-9A-F]{8}:', texts[i+1], re.M)):
            read = FLAG_ALL
        reads.append(read); writes.append(write); successors.append(edges); eligible.append(simple)
    entry = 0
    guards = re.findall(r'\bgoto (L_[0-9A-F]{8});', '\n'.join(prefix[2:]))
    if guards:
        if len(guards) != 1 or guards[0] not in labels:
            raise ValueError('FLAGABI cannot resolve sparse entry guard')
        entry = labels[guards[0]]
    live = [0]*len(groups)
    changed = True
    while changed:
        changed = False
        for i in reversed(range(len(groups))):
            out = 0
            for target in successors[i]: out |= live[target]
            value = reads[i] | (out & ~writes[i])
            if value != live[i]: live[i] = value; changed = True
    after = []
    for edges in successors:
        out = 0
        for target in edges: out |= live[target]
        after.append(out)
    return dict(prefix=prefix, pending=pending, groups=groups, reads=reads,
                writes=writes, successors=successors, eligible=eligible,
                calls=calls, exits=exits, entry=entry, live=live, after=after)


def flag_flow_emit(flow):
    output = flow['prefix'] + flow['pending']
    for i, group in enumerate(flow['groups']):
        if not flow['eligible'][i]: output.extend(group); continue
        mask = flow['writes'][i] & (flow['after'][i] | flow['reads'][i])
        text = '\n'.join(group)
        match = re.search(r'\b('+'|'.join(FLAG_HELPERS)+r')\(c\s*,', text)
        depth, end = 1, match.end()
        while depth and end < len(text):
            depth += (text[end] == '(') - (text[end] == ')')
            end += 1
        if depth: raise ValueError('unbalanced generated flag helper in FLAGABI')
        text = text[:end-1] + f', 0x{mask:02X}u' + text[end-1:]
        text = text[:match.start()] + match.group(1)+'_m' + text[match.start()+len(match.group(1)):]
        output.extend(text.splitlines())
    return '\n'.join(output+['}'])


def flag_abi_must_define(flow, definitions):
    """Intersection of definitions on every reachable return; unknown tails=0."""
    if not flow['groups']: return 0
    incoming = {flow['entry']: 0}
    queue = [flow['entry']]
    outgoing = {}
    while queue:
        i = queue.pop()
        written = flow['writes'][i]
        if i in flow['calls']: written |= definitions.get(flow['calls'][i], 0)
        value = incoming[i] | written
        outgoing[i] = value
        for target in flow['successors'][i]:
            merged = incoming[target] & value if target in incoming else value
            if target not in incoming or incoming[target] != merged:
                incoming[target] = merged; queue.append(target)
    returns = [outgoing[i] if kind == 'return' else 0
               for i, kind in flow['exits'].items() if i in outgoing]
    result = FLAG_ALL if returns else 0
    for value in returns: result &= value
    return result


def flag_abi_program(bodies, pinned=(), private_variants=False):
    """Public dispatch entries retain all flags; private bodies require proof.

    No caller proof is inferred from missing observed edges. Optional direct-only
    copies are separate C functions, never exported through guest dispatch. Each
    use must have a zero successor demand. Callee definitions reach a monotone
    fixed point using actual emitted flag semantics (logical AF/INC CF survive).
    """
    from collections import defaultdict, deque
    pinned = set(pinned)
    unsafe = {a for a,b in bodies.items() if re.search(
        r'\b(?:fs|gs):|c->(?:fs|gs)\b|\b(?:seh|setjmp|longjmp)\b|RECOMP_TODO\(|ac1_unsupported_instruction\(', b, re.I)}
    flows = {a: flag_flow(b) for a,b in bodies.items()}
    parents = defaultdict(set)
    for a,flow in flows.items():
        for target in flow['calls'].values(): parents[target].add(a)
    definitions = {a:0 for a in bodies}
    queue, queued = deque(bodies), set(bodies)
    updates = 0
    while queue:
        a = queue.popleft(); queued.remove(a)
        value = 0 if a in unsafe else flag_abi_must_define(flows[a], definitions)
        if value & definitions[a] != definitions[a]:
            raise ValueError(f'non-monotone FLAGABI definition at 0x{a:08X}')
        if value != definitions[a]:
            definitions[a] = value; updates += 1
            for caller in parents[a]:
                if caller not in queued: queue.append(caller); queued.add(caller)
    public, private, narrowed_sites = {}, {}, 0
    worth_cloning = {}
    demands = defaultdict(int)
    pending = deque()
    def transform(a, return_live, is_private):
        nonlocal narrowed_sites
        if a in unsafe: return bodies[a]
        flow = flag_flow(bodies[a], return_live, definitions)
        text = flag_flow_emit(flow)
        for i,target in flow['calls'].items():
            demands[target] |= flow['after'][i]
            if (not private_variants or flow['after'][i] or
                target not in bodies or target in pinned or target in unsafe): continue
            # Only material copies: public helpers/masks are compared without
            # changing function identity. Dispatch wrappers remain untouched.
            if target not in private:
                if target not in worth_cloning:
                    ordinary = flag_flow_emit(flag_flow(bodies[target], FLAG_ALL, definitions))
                    narrowed = flag_flow_emit(flag_flow(bodies[target], 0, definitions))
                    worth_cloning[target] = ordinary != narrowed
                if not worth_cloning[target]: continue
                private[target] = None
                pending.append(target)
            original = f'AC1_CALL(c, L_{target:08X}, 0x{target:08X}u)'
            # Match by instruction group, not global replacement: another call
            # to this callee in the same caller may consume returned flags.
            group = '\n'.join(flow['groups'][i])
            replacement = group.replace(original, f'AC1_CALL(c, ac1_abi_{target:08X}, 0x{target:08X}u)')
            if group == replacement: raise ValueError('FLAGABI lost direct callsite')
            if text.count(group) != 1: raise ValueError('FLAGABI ambiguous instruction group')
            text = text.replace(group, replacement, 1)
            narrowed_sites += 1
        if is_private:
            text = text.replace(f'void L_{a:08X}(CPU *c)', f'void ac1_abi_{a:08X}(CPU *c)', 1)
        return text
    for a in bodies: public[a] = transform(a, FLAG_ALL, False)
    while pending:
        a = pending.popleft()
        private[a] = transform(a, 0, True)
    report = dict(public_entries_all_live=len(bodies), public_returns_narrowed=0,
                  private_functions_narrowed=len(private), direct_sites_narrowed=narrowed_sites,
                  private_returns_narrowed=sum(len(re.findall(r'\breturn;', b)) for b in private.values()),
                  proven_defining_callees=sum(bool(v) for v in definitions.values()),
                  definition_updates=updates, unsafe_functions=len(unsafe),
                  pinned_functions=len(pinned), caller_return_demands={f'0x{a:08X}':v for a,v in sorted(demands.items())})
    return public, private, report



def flag_liveness_function(body: str, mode: str = 'cfg') -> str:
    """Only kill writes proved dead in emitted semantics, never x86 undefined bits.

    Calls, escapes, unknown CPU helpers and full flag observations are barriers.
    Functions using FS/GS (including SEH) retain their complete original flags.
    Return flags stay live: this is deliberately not a cross-call FLAGABI pass.
    """
    if mode == 'off' or re.search(r'\b(?:fs|gs):|c->(?:fs|gs)\b|\b(?:seh|longjmp)\b', body, re.I):
        return body
    prefix, pending, groups = codegen_instruction_groups(body)
    if not groups:
        return body
    texts = [re.sub(r'/\*.*?\*/', '', '\n'.join(group), flags=re.S) for group in groups]
    labels = {label: i for i, text in enumerate(texts)
              for label in re.findall(r'^(L_[0-9A-F]{8}):', text, re.M)}
    reads, writes, successors, eligible = [], [], [], []
    for i, text in enumerate(texts):
        calls = re.findall(r'\b(flags_\w+)\(c\s*,', text)
        # Repeated string bodies and atomics contain intra-instruction control
        # flow; retain those bodies until an instruction-local CFG is modeled.
        simple = (len(calls) == 1 and calls[0] in FLAG_HELPERS and
                  not re.search(r'\b(?:if|while|for|goto|return)\b|atomic_', text))
        write = FLAG_HELPERS[calls[0]] if simple else 0
        read = sum(bit for flag, bit in FLAG_BITS.items() if re.search(r'\bc->' + flag + r'\b', text))
        if calls and calls[0] in ('flags_adc', 'flags_sbb'):
            read |= FLAG_BITS['cf']
        pointer_uses = re.sub(r'\bc->\w+', 'member', text)
        safe = '|'.join(FLAG_HELPERS) + '|AC1_SET_GUEST_EIP|push32|pop32'
        pointer_uses = re.sub(r'\b(?:' + safe + r')\(c(?=[,)])', 'safe(', pointer_uses)
        if re.search(r'\bc\b|RECOMP_TODO\(|ac1_unsupported_instruction\(', pointer_uses):
            read = FLAG_ALL
        if re.search(r'/\*\s*[0-9A-Fa-f]{8}:\s*(?:lahf|pushfd|pushf)\b', '\n'.join(groups[i])):
            read = FLAG_ALL
        targets = re.findall(r'\bgoto (L_[0-9A-F]{8});', text)
        edges = {labels[t] for t in targets if t in labels}
        # A direct unconditional goto has no fallthrough. Conditional returns
        # and switch-like generated blocks conservatively retain fallthrough.
        terminal = bool(re.search(r'^\s*(?:goto L_[0-9A-F]{8};|(?:dispatch_jmp|guest_ret)\()',
                                  text, re.M))
        if not terminal and i + 1 < len(groups):
            edges.add(i + 1)
        if i + 1 == len(groups) or 'return;' in text or any(t not in labels for t in targets):
            read = FLAG_ALL
        if mode == 'straight' and (targets or i + 1 < len(groups) and
                                  re.search(r'^L_[0-9A-F]{8}:', texts[i + 1], re.M)):
            read = FLAG_ALL
        reads.append(read)
        writes.append(write)
        successors.append(edges)
        eligible.append(simple)
    live = [0] * len(groups)
    changed = True
    while changed:
        changed = False
        for i in reversed(range(len(groups))):
            out = 0
            for target in successors[i]:
                out |= live[target]
            value = reads[i] | (out & ~writes[i])
            if value != live[i]:
                live[i] = value
                changed = True
    output = prefix + pending
    for i, group in enumerate(groups):
        if eligible[i]:
            out = 0
            for target in successors[i]:
                out |= live[target]
            # Include same-instruction observations/barriers conservatively.
            mask = writes[i] & (out | reads[i])
            text = '\n'.join(group)
            match = re.search(r'\b(' + '|'.join(FLAG_HELPERS) + r')\(c\s*,', text)
            depth, end = 1, match.end()
            # The initial '(' precedes c; scan after the first comma. Nested
            # operand expressions remain evaluated even with a zero mask.
            while depth and end < len(text):
                if text[end] == '(':
                    depth += 1
                elif text[end] == ')':
                    depth -= 1
                end += 1
            if depth:
                raise ValueError('unbalanced generated flag helper')
            text = text[:end - 1] + f', 0x{mask:02X}u' + text[end - 1:]
            text = text[:match.start()] + match.group(1) + '_m' + text[match.start() + len(match.group(1)):]
            output.extend(text.splitlines())
        else:
            output.extend(group)
    return '\n'.join(output + ['}'])


def specialize_direct_calls(body: str, lifted: set[int]) -> tuple[str, set[int], int]:
    """Bind only successful main-image entries, after cached bodies are lifted.

    Keep the CRT handoff in dispatch for its one-time diagnostic. Indirect
    operands, imports, DLL entries and failed lifts retain the slow path.
    """
    callees, count = set(), 0
    def replace(match):
        nonlocal count
        target = int(match.group(1), 16)
        if target not in lifted or target == 0x00401375:
            return match.group(0)
        callees.add(target)
        count += 1
        return f"AC1_CALL(c, L_{target:08X}, 0x{target:08X}u)"
    body = re.sub(r"\bdispatch\(c, 0x([0-9A-Fa-f]{8})u\)", replace, body)
    return body, callees, count


def native_instruction_hash(spec: dict, read_va) -> str:
    """Hash sparse recovered code, including each instruction's VA and size."""
    import struct
    instructions = sorted({(a, n) for b in spec['blocks'] for a, n in b['instructions']})
    digest = hashlib.sha256()
    for address, size in instructions:
        raw = read_va(address, size)
        if len(raw) != size: raise ValueError(f"short native code read at 0x{address:08X}")
        digest.update(struct.pack('<IH', address, size))
        digest.update(raw)
    return digest.hexdigest()


def validate_native_bindings(entries: list, functions: list[dict], lifted: set[int], read_va, md) -> dict:
    specs = {f['address']: f for f in functions}
    result = {}
    for entry in entries:
        if entry.get('kind') == 'region':
            continue
        address = int(entry['address'], 0)
        if address in result: raise ValueError(f"duplicate native binding 0x{address:08X}")
        if address not in lifted: raise ValueError(f"native binding 0x{address:08X} has no translated oracle")
        for key in ('implementation', 'plan'):
            if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', entry[key]):
                raise ValueError(f"native binding 0x{address:08X}: invalid C identifier {key}")
        spec = specs[address]
        actual = native_instruction_hash(spec, read_va)
        if actual != entry['instruction_sha256']:
            raise ValueError(f"native binding 0x{address:08X}: instruction hash expected {entry['instruction_sha256']}, got {actual}")
        # Verification executes both bodies on a serialized guest owner. Calls,
        # imports and external tail transfers could yield or affect host state.
        addresses = {a for b in spec['blocks'] for a, _ in b['instructions']}
        for block in spec['blocks']:
            for va, size in block['instructions']:
                insn = next(md.disasm(read_va(va, size), va, count=1))
                if insn.mnemonic.startswith(('call', 'int', 'sys', 'lock')):
                    raise ValueError(f"native oracle 0x{address:08X} is not a pure leaf: {va:08X} {insn.mnemonic}")
                if insn.mnemonic.startswith(('j', 'loop')):
                    from capstone.x86 import X86_OP_IMM
                    if not insn.operands or insn.operands[0].type != X86_OP_IMM or insn.operands[0].imm not in addresses:
                        raise ValueError(f"native oracle 0x{address:08X} has external/indirect transfer at 0x{va:08X}")
        result[address] = entry
    return result


def wrap_native_binding(body: str, address: int, entry: dict) -> str:
    signature = f'void L_{address:08X}(CPU *c)'
    if body.count(signature) != 1: raise ValueError(f"native binding 0x{address:08X}: unexpected function signature")
    original = f'ac1_translated_{address:08X}'
    body = body.replace(signature, f'static void {original}(CPU *c)', 1)
    impl, plan = entry['implementation'], entry['plan']
    return (body + f'\nvoid {impl}(CPU *);\nint {plan}(const CPU *, ac1_native_plan *);\n'
            f'ac1_native_binding ac1_binding_{address:08X} = {{.va=0x{address:08X}u, '
            f'.name="{impl}", .body={impl}, .plan={plan}}};\n'
            f'void L_{address:08X}(CPU *c) {{ ac1_native_invoke(c, &ac1_binding_{address:08X}, {original}); }}\n')


# Audited pinned helpers only: every CPU access is flag state, with no GPR
# pointer escaping or diagnostic callback. Unit tests check the actual header.
LOCAL_GPR_FREE_HELPERS = frozenset({
    'AC1_SET_GUEST_EIP', 'op_shl', 'op_shr', 'op_sar', 'op_rol', 'op_ror',
    'op_shld', 'op_shrd', 'op_bsf', 'op_bsr', 'shr32', 'shl32',
    'eflags_pack', 'eflags_unpack',
})
LOCAL_ESP_HELPERS = frozenset({'push32', 'pop32'})
def validate_region_bindings(entries, functions, lifted, read_va, md):
    specs = {f['address']: f for f in functions}
    result = {}
    for entry in entries:
        if entry.get('kind') != 'region': continue
        address = int(entry['address'], 0)
        if address in result: raise ValueError(f'duplicate native region {address:08X}')
        if address not in lifted: raise ValueError(f'region {address:08X} has no translated oracle')
        for key in ('implementation', 'plan', 'pending'):
            if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', entry[key]): raise ValueError(f'invalid region C identifier {key}')
        journal = entry.get('verification') == 'write_journal'
        if not 0 <= int(entry['context_bytes']) <= 8192: raise ValueError('region context must fit bounded storage')
        for key in ('residual_predicate', 'residual_hook'):
            if key in entry and not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', entry[key]):
                raise ValueError(f'invalid region C identifier {key}')
        if native_instruction_hash(specs[address], read_va) != entry['instruction_sha256']:
            raise ValueError(f'region {address:08X}: entry instruction hash changed')
        members = {}
        for member in entry['members']:
            va = int(member['address'], 0)
            if va in members or va not in lifted: raise ValueError(f'region {address:08X}: duplicate/missing member {va:08X}')
            if native_instruction_hash(specs[va], read_va) != member['instruction_sha256']:
                raise ValueError(f'region {address:08X}: member {va:08X} instruction hash changed')
            ranges = member.get('subsumed_ranges', [])
            if not ranges: raise ValueError(f'region {address:08X}: member {va:08X} has no subsumed ranges')
            for region_range in ranges:
                lo, hi = int(region_range['start'], 0), int(region_range['end'], 0)
                bounds=[(a,a+n) for block in specs[va]['blocks'] for a,n in block['instructions']]
                if lo < min(a for a,z in bounds) or hi > max(z for a,z in bounds) or lo >= hi:
                    raise ValueError(f'region {address:08X}: invalid subsumed range {lo:08X}..{hi:08X}')
                if hashlib.sha256(read_va(lo, hi-lo)).hexdigest() != region_range['sha256']:
                    raise ValueError(f'region {address:08X}: subsumed range hash changed at {lo:08X}')
            members[va] = member
        if address not in members: raise ValueError(f'region {address:08X}: entry absent from pinned members')
        boundary = int(entry['boundary'], 0) if not journal else None
        if not journal and boundary not in {b['start'] for b in specs[address]['blocks']}:
            raise ValueError(f'region {address:08X}: effect boundary is not a recovered block entry')
        guards = {(int(t['site'],0),int(t['target'],0)) for t in entry.get('guarded_transfers',[])}
        indirect = {int(t['site'],0):{int(v,0) for v in t['targets']} for t in entry.get('indirect_calls',[])}
        for call in entry.get('indirect_calls',[]):
            if not {int(v,0) for v in call.get('native_targets',call['targets'])} <= {int(v,0) for v in call['targets']}:
                raise ValueError('region native callback whitelist is outside closed callback set')
        seen_guards=set();seen_indirect=set()
        for va in members:
            spec = specs[va]
            instructions = {a for b in spec['blocks'] for a, _ in b['instructions']}
            for b in spec['blocks']:
                for pc, size in b['instructions']:
                    if not journal and va == address and pc >= boundary: continue
                    if not any(int(r['start'],0) <= pc and pc+size <= int(r['end'],0)
                               for r in members[va]['subsumed_ranges']):
                        raise ValueError(f'region {address:08X}: uncovered instruction {pc:08X}')
                    insn = next(md.disasm(read_va(pc,size),pc,count=1))
                    if insn.mnemonic.startswith(('int','sys','lock')) or insn.mnemonic in ('xadd','cmpxchg','cmpxchg8b','in','out','hlt','cli','sti'):
                        raise ValueError(f'region {address:08X}: unsafe instruction {pc:08X} {insn.mnemonic}')
                    if insn.mnemonic.startswith(('call','j','loop')):
                        from capstone.x86 import X86_OP_IMM
                        if not insn.operands or insn.operands[0].type != X86_OP_IMM:
                            if journal and pc in indirect:
                                if not indirect[pc] <= members.keys(): raise ValueError('region indirect whitelist has unpinned target')
                                seen_indirect.add(pc);continue
                            if journal and (pc,0) in guards: seen_guards.add((pc,0));continue
                            raise ValueError(f'region {address:08X}: unclosed indirect transfer {pc:08X}')
                        target = insn.operands[0].imm
                        if target not in instructions and target not in members:
                            if journal and (pc,target) in guards: seen_guards.add((pc,target));continue
                            raise ValueError(f'region {address:08X}: unpinned target {target:08X} at {pc:08X}')
        if guards != seen_guards or set(indirect) != seen_indirect: raise ValueError('region has stale guarded/indirect transfer declaration')
        result[address] = entry
    return result


def emit_region_sources(regions, lifted, bodies, output_dir, tu_header):
    originals = dict(zip(lifted,bodies))
    for address, entry in regions.items():
        if entry.get('verification') == 'write_journal':
            emit_journal_region_source(address,entry,originals,output_dir,tu_header)
            continue
        members = {int(m['address'],0) for m in entry['members']}
        boundary = int(entry['boundary'],0)
        name = lambda va: f'ac1_region_{address:08X}_m_{va:08X}'
        declarations = '\n'.join(f'static void {name(va)}(CPU *);' for va in sorted(members) if va != address)
        def close_calls(body):
            body = re.sub(r'AC1_CALL\(c,\s*L_([0-9A-F]{8}),\s*0x[0-9A-F]+u\)',
                          lambda m: f'{name(int(m[1],16))}(c)' if int(m[1],16) in members else
                                    f'ac1_native_region_forbidden(c,0x{m[1]}u)',body)
            body = re.sub(r'dispatch(?:_jmp)?\(c,\s*([^;)]+)\)',r'ac1_native_region_forbidden(c,\1)',body)
            return body
        root = originals[address]
        marker = f'L_{boundary:08X}:\n'
        if root.count(marker) != 1: raise ValueError(f'region {address:08X}: missing unique boundary label')
        prefix = root.replace(marker,marker+f'    AC1_SET_GUEST_EIP(c,0x{boundary:08X}u); return;\n')
        oracle = close_calls(prefix).replace(f'void L_{address:08X}(CPU *c)',f'void ac1_region_oracle_{address:08X}(CPU *c)',1)
        clones=[]
        for va in sorted(members-{address}):
            clones.append(close_calls(originals[va]).replace(f'void L_{va:08X}(CPU *c)',f'static void {name(va)}(CPU *c)',1))
        residual = prefix.replace(f'void L_{address:08X}(CPU *c)',f'void ac1_region_residual_{address:08X}(CPU *c, const ac1_region_plan *plan)',1)
        sites={int(s['site'],0):s for s in entry.get('residual_calls',[])}
        for site,s in sites.items():
            target=int(s['target'],0)
            expression=rf'AC1_CALL\(c, L_{target:08X}, 0x{target:08X}u\)(?=;[^\n]* /\* {site:08X}:)'
            replacement=(f'{{ if({entry["residual_predicate"]}(c,plan,0x{site:08X}u)) {name(target)}(c); '
                         f'else {{ c->esp += {4+int(s["ret_pop"])}u; }} }}')
            residual,n=re.subn(expression,replacement,residual)
            if n!=1: raise ValueError(f'region residual call site {site:08X}: matched {n}')
        residual=close_calls(residual)
        for hook in entry.get('residual_hooks',[]):
            pc=int(hook,0)
            token=f'    AC1_SET_GUEST_EIP(c, 0x{pc:08X}u);'
            if residual.count(token)!=1: raise ValueError(f'region residual hook {pc:08X}: not unique')
            residual=residual.replace(token,f'    {entry["residual_hook"]}(c,plan,0x{pc:08X}u);\n'+token)
        tail='void ac1_region_continue_%08X(CPU *c)\n{\n    AC1_DECLARE_LOCAL_ARENA;\n'%address+root[root.index(marker):]
        extra=''
        if entry.get('residual_calls'): extra+=f'int {entry["residual_predicate"]}(const CPU *,const ac1_region_plan *,uint32_t);\n'
        if entry.get('residual_hooks'): extra+=f'void {entry["residual_hook"]}(CPU *,const ac1_region_plan *,uint32_t);\n'
        source=tu_header+'#include "ac1_native.h"\n'+extra+declarations+'\n'+'\n\n'.join(clones+[oracle,residual,tail])+'\n'
        write_text_if_changed(output_dir/f'initial_region_{address:08X}.c',source)
    return [f'initial_region_{va:08X}.c' for va in sorted(regions)]


def emit_journal_region_source(address,entry,originals,output_dir,tu_header):
    members={int(m['address'],0) for m in entry['members']}
    graph={va:{int(v,16) for v in re.findall(r'AC1_CALL\(c,\s*L_([0-9A-F]{8}),',originals[va])}&members for va in members}
    graph[address].update(int(v,0) for call in entry.get('indirect_calls',[]) for v in call['targets'])
    visited=set();path=[];recursive=set()
    def visit(va):
        if va in path: recursive.update(path[path.index(va):]);return
        if va in visited:return
        path.append(va)
        for child in graph[va]:visit(child)
        path.pop();visited.add(va)
    for va in members:visit(va)
    # These adapters have no CPU argument and deliberately intercept writes.
    # Keep their existing memory API; ordinary guest bodies capture the base.
    journal_header=tu_header.replace('#include "ac1_local_memory.h"',
                                    '#define AC1_DECLARE_LOCAL_ARENA ((void)0)')
    source=journal_header+'#define AC1_REGION_JOURNAL_ADAPTERS\n#include "ac1_region_journal.h"\n'
    # This project-owned SIMD implementation is compiled in the journal TU so
    # its stack/output writes use exactly the same transactional adapters.
    math_names=[]
    for va in (0x653400,0x42ee70,0x1176de0):
        for suffix in ('','_plan'):
            old=f'ac1_native_{va:08X}{suffix}';new=f'ac1_r_{address:08X}_math_{va:08X}{suffix}'
            math_names.append(old);source+=f'#define {old} {new}\n'
    source+='#include "ac1_native_leaf.c"\n'+''.join(f'#undef {n}\n' for n in math_names)
    for flavor in ('oracle','fused'):
        name=lambda va:f'ac1_r_{address:08X}_{flavor}_{va:08X}'
        def qualifier(va):
            # Keep large unchanged pose/CRT kernels as direct private calls.
            # Forcing the entire recursive-sized cone through GCC's SRA uses
            # gigabytes and duplicates cold math paths without removing work.
            inline=flavor=='fused' and va not in recursive and va<0x00f90000 and (va in (address,0x654090,0x6c4ff0) or originals[va].count('\n')<220)
            return 'static inline __attribute__((always_inline)) void' if inline else 'static void'
        source+='\n'.join(f'{qualifier(v)} {name(v)}(CPU *);' for v in sorted(members))+'\n'
        for va in sorted(members):
            body=originals[va].replace(f'void L_{va:08X}(CPU *c)',f'{qualifier(va)} {name(va)}(CPU *c)',1)
            if flavor=='fused' and va in (0x653400,0x42ee70):
                start=body.index('{')+1
                fast=f'ac1_r_{address:08X}_math_{va:08X}'
                body=body[:start]+f'\n    if({fast}_plan(c,NULL)) {{ {fast}(c);return; }}\n'+body[start:]
            body=re.sub(r'AC1_CALL\(c,\s*L_([0-9A-F]{8}),\s*0x[0-9A-F]+u\)',
                lambda m:f'{name(int(m[1],16))}(c)' if int(m[1],16) in members else f'ac1_native_region_forbidden(c,0x{m[1]}u)',body)
            for indirect in entry.get('indirect_calls',[]):
                pc=int(indirect['site'],0)
                expression=rf'dispatch\(c, ([^;]+)\)(?=;[^\n]* /\* {pc:08X}:)'
                native_targets={int(v,0) for v in indirect.get('native_targets',indirect['targets'])}
                cases=' '.join(f'case {v}u: '+(name(int(v,0)) if flavor=='oracle' or int(v,0) in native_targets else f'ac1_r_{address:08X}_oracle_{int(v,0):08X}')+'(c);break;' for v in indirect['targets'])
                body,n=re.subn(expression,lambda m:'{ uint32_t target='+m[1]+'; switch(target) { '+cases+' default: ac1_native_region_forbidden(c,target); } }',body)
                if va==address and n!=1: raise ValueError(f'journal indirect site {pc:08X} matched {n}')
            body=re.sub(r'dispatch(?:_jmp)?\(c,\s*([^;)]+)\)',r'ac1_native_region_forbidden(c,\1)',body)
            # All writes in this closed TU must go through the CPU adapters.
            if re.search(r'\b(?:guest_atomic_|guest_execution_write\s*\(|memcpy\s*\(|memmove\s*\(|memset\s*\()',body):
                raise ValueError(f'journal region {address:08X}: unaudited write helper in {va:08X}')
            source+=body+'\n\n'
    source+=f'''void ac1_region_oracle_{address:08X}(CPU *c) {{ ac1_r_{address:08X}_oracle_{address:08X}(c); }}
void {entry['implementation']}(CPU *c,const ac1_region_plan *plan) {{
    (void)plan; CPU local; memcpy(&local,c,sizeof(local));
    ac1_r_{address:08X}_fused_{address:08X}(&local); memcpy(c,&local,sizeof(local));
}}
'''
    write_text_if_changed(output_dir/f'initial_region_{address:08X}.c',source)


def wrap_region_binding(body, address, entry):
    original=f'ac1_translated_{address:08X}'
    body=body.replace(f'void L_{address:08X}(CPU *c)',f'static void {original}(CPU *c)',1)
    impl,prepare,pending=entry['implementation'],entry['plan'],entry['pending']
    if entry.get('verification') == 'write_journal':
        return body+f'''
void {impl}(CPU *,const ac1_region_plan *);
void ac1_region_oracle_{address:08X}(CPU *);
void ac1_native_journal_invoke(CPU *,ac1_native_region *,void (*)(CPU *),void (*)(CPU *));
ac1_native_region ac1_region_binding_{address:08X} = {{.va=0x{address:08X}u,
 .name="{impl}",.body={impl}}};
void L_{address:08X}(CPU *c) {{ if(!ac1_region_binding_{address:08X}.enabled) {{ {original}(c);return; }}
 ac1_native_journal_invoke(c,&ac1_region_binding_{address:08X},
 {original},ac1_region_oracle_{address:08X}); }}
'''
    return body+f'''
void {impl}(CPU *, const ac1_region_plan *);
int {prepare}(const CPU *, ac1_region_plan *);
void {pending}(const CPU *, ac1_pending_upload *);
void ac1_region_oracle_{address:08X}(CPU *);
void ac1_region_continue_{address:08X}(CPU *);
ac1_native_region ac1_region_binding_{address:08X} = {{.va=0x{address:08X}u,
 .boundary={entry['boundary']}u, .name="{impl}", .context_size={int(entry['context_bytes'])},
 .prepare={prepare}, .body={impl}, .pending={pending}}};
void L_{address:08X}(CPU *c) {{ if(!ac1_region_binding_{address:08X}.enabled) {{ {original}(c);return; }}
 ac1_native_region_invoke(c,&ac1_region_binding_{address:08X},
 {original},ac1_region_oracle_{address:08X},ac1_region_continue_{address:08X}); }}
'''




def declare_local_memory_base(body: str) -> str:
    """Capture the stable arena for every generated CPU entry, including oracles."""
    pattern = r'(?m)^(?:static )?void \w+\(CPU \*c\)\s*\{'
    return re.sub(pattern, lambda m: m[0] + '\n    AC1_DECLARE_LOCAL_ARENA;', body)


def local_register_function(body: str) -> str:
    """GPR locals with conservative CFG dirty sets and audited CPU boundaries."""
    from collections import deque
    if re.search(r'\b(?:fs|gs):|c->(?:fs|gs)\b|\b(?:seh|setjmp|longjmp)\b',body,re.I):return body
    registers='eax ecx edx ebx esp ebp esi edi'.split()
    clean=re.sub(r'/\*.*?\*/','',body,flags=re.S)
    names='|'.join(registers)
    if re.search(r'(?:^|[=(,;{?:]|\*\))\s*&\s*\(*\s*c->(?:'+names+r')\b',clean,re.M):return body
    referenced=set(re.findall(r'\bc->('+names+r')\b',clean))
    stack='|'.join(sorted(LOCAL_ESP_HELPERS))
    if re.search(r'\b(?:'+stack+r')\(c(?=[,)])',clean):referenced.add('esp')
    registers=[r for r in registers if r in referenced]
    if not registers:return body
    all_regs=set(registers)
    gpr=re.compile(r'\bc->('+'|'.join(registers)+r')\b')
    def spill(dirty):return ' '.join(f'c->{r} = _ac1_regs.{r};' for r in registers if r in dirty)
    reload=' '.join(f'_ac1_regs.{r} = c->{r};' for r in registers)
    esp_spill,esp_reload='c->esp = _ac1_regs.esp;','_ac1_regs.esp = c->esp;'
    pure=r'flags_\w+|'+'|'.join(sorted(LOCAL_GPR_FREE_HELPERS))
    lines=body.splitlines()
    if len(lines)<3 or lines[1]!='{' or lines[-1]!='}':raise ValueError('unexpected lifted function shape for local registers')
    groups=[];group=[]
    for line in lines[2:-1]:
        if 'AC1_SET_GUEST_EIP(c,' in line or re.fullmatch(r'L_[0-9A-F]{8}:',line):
            if group:groups.append(group)
            group=[]
        group.append(line)
    if group:groups.append(group)
    texts=[re.sub(r'/\*.*?\*/','', '\n'.join(g),flags=re.S) for g in groups]
    labels={label:i for i,text in enumerate(texts) for label in re.findall(r'^(L_[0-9A-F]{8}):',text,re.M)}
    full=[];partial=[];writes=[];edges=[];unknown_edges=False
    for i,text in enumerate(texts):
        pointer=re.sub(r'\bc->\w+','member',text)
        pointer=re.sub(r'\b(?:'+pure+'|'+stack+r')\(c(?=[,)])','safe(',pointer)
        full.append(bool(re.search(r'\bc\b|RECOMP_TODO\(|ac1_unsupported_instruction\(',pointer)))
        partial.append(not full[-1] and bool(re.search(r'\b(?:'+stack+r')\(c(?=[,)])',text)))
        changed=set(re.findall(r'\bc->('+'|'.join(registers)+r')\s*\)*\s*(?:(?:<<|>>|[+*/%&|^\-])?=(?!=)|\+\+|--)',text))
        changed.update(re.findall(r'(?:\+\+|--)\s*\(*\s*c->('+'|'.join(registers)+r')\b',text))
        changed.update(re.findall(r'\bSET(?:8[HL]|16)\(\s*\(*\s*c->('+'|'.join(registers)+r')\b',text))
        macros=set(re.findall(r'\b([A-Z][A-Z0-9_]+)\(',text))
        if macros-{'AC1_SET_GUEST_EIP','R8H','R8L','R16','SET8H','SET8L','SET16'}:
            changed.update(gpr.findall(text))
        writes.append(changed)
        targets=re.findall(r'\bgoto (L_[0-9A-F]{8});',text)
        unknown_edges |= any(t not in labels for t in targets) or text.count('goto ')!=len(targets)
        successors={labels[t] for t in targets if t in labels}
        control=re.sub(r'AC1_SET_GUEST_EIP\(c,\s*0x[0-9A-F]+u\);','',text)
        control=re.sub(r'^L_[0-9A-F]{8}:','',control,flags=re.M)
        terminal=bool(re.fullmatch(r'\s*goto L_[0-9A-F]{8};\s*',control))
        terminal |= 'return;' in text and not re.search(r'\b(?:if|while|for|switch)\b',text)
        if not terminal and i+1<len(groups):successors.add(i+1)
        edges.append(successors)
    def outgoing(i,before):
        if full[i]:return set()
        return (before|writes[i])-({'esp'} if partial[i] else set())
    incoming={0:set()};queue=deque([0])
    while queue:
        i=queue.popleft();after=outgoing(i,incoming[i])
        for target in edges[i]:
            merged=incoming.get(target,set())|after
            if target not in incoming or merged!=incoming[target]:incoming[target]=merged;queue.append(target)
    output=lines[:2]+[
        '    struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; } _ac1_regs;',
        '    _Static_assert(STACK_BASE((CPU *)0) == 0u, "local GPR stack helpers require flat zero stack base");',
        '    '+reload,
    ]
    for i,group in enumerate(groups):
        before=all_regs if unknown_edges else incoming.get(i,all_regs)
        after=outgoing(i,before)
        text='\n'.join(group)
        if full[i]:
            sync=spill(before)
            if sync:output.append('    '+sync)
        else:
            if partial[i] and 'esp' in before:output.append('    '+esp_spill)
            text=gpr.sub(lambda m:m[0] if partial[i] and m[1]=='esp' else '_ac1_regs.'+m[1],text)
            at_return=(esp_reload+' ' if partial[i] else '')+spill(after)
            text=re.sub(r'\breturn;','{ '+at_return+' return; }',text)
        edge_reload=reload if full[i] else esp_reload if partial[i] else ''
        fence=edge_reload+' __asm__ __volatile__("" ::: "memory"); '
        text=re.sub(r'\bgoto (L_[0-9A-F]{8});',lambda m:'{ '+fence+'goto '+m[1]+'; }',text)
        output.extend(text.splitlines())
        if edge_reload:output.append('    '+edge_reload)
    output.extend(('    '+spill(all_regs),'}'))
    return '\n'.join(output)


def local_register_selection(selection: str, lifted: set[int]) -> set[int]:
    """Explicit per-function admission; malformed/unlifted entries fail closed."""
    if selection == 'off':
        return set()
    if selection == 'all':
        return lifted
    if not re.fullmatch(r'(?:0x)?[0-9a-fA-F]{8}(?:,(?:0x)?[0-9a-fA-F]{8})*', selection):
        raise ValueError('local-regs must be off, all, or comma-separated eight-digit hexadecimal VAs')
    selected = {int(part, 16) for part in selection.split(',')}
    if selected - lifted:
        raise ValueError('local-regs selection is not lifted: ' +
                         ', '.join(f'0x{va:08X}' for va in sorted(selected - lifted)))
    return selected


def profile_count_entry(body: str, address: int) -> str:
    """Count public entries in a dedicated build; normal bodies stay identical.

    Applied after native wrapping so direct calls and indirect dispatch count
    once, including native fallbacks. This mode is for counts, never timing.
    """
    pattern = rf'(void L_{address:08X}\(CPU \*c\)\s*\{{)'
    replacement = (r'\1\n    extern void ac1_profile_entry(uint32_t);\n'
                   f'    ac1_profile_entry(0x{address:08X}u);')
    result, count = re.subn(pattern, replacement, body)
    if count != 1:
        raise ValueError(f'profile count entry 0x{address:08X}: expected one public definition, got {count}')
    return result


def emit_lift(binary: Path, pcrecomp: Path, functions: list[dict], output_dir: Path,
              functions_per_tu: int, translation_unit_buckets: int = 0,
              emission_cache=None, direct_calls: bool = True, native_overrides=None,
              flag_liveness: str = 'off', local_regs: str = 'off', profile_counts=()) -> dict:
    sys.path.insert(0, str(pcrecomp / "tools/lift"))
    import pefile
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    from lift32_cpu import Lifter as BaseLifter

    class AC1Lifter(BaseLifter):
        """AC1-specific instruction semantics missing from the donor lifter."""
        def translate(self, insn, labels):
            if insn.mnemonic == "int3":
                return ac1_int3_translation(insn)
            narrow_memory_sse = ac1_sse_width_translation(self, insn)
            if narrow_memory_sse is not None:
                return narrow_memory_sse
            if insn.mnemonic in ("movhlps", "movlhps"):
                destination, source = insn.operands
                dst = self._xi(destination)
                src = self._xi(source)
                if insn.mnemonic == "movhlps":
                    return [
                        f"{{ uint64_t _lane = c->xmm[{src}].u64[1]; "
                        f"c->xmm[{dst}].u64[0] = _lane; }}"
                    ]
                return [
                    f"{{ uint64_t _lane = c->xmm[{src}].u64[0]; "
                    f"c->xmm[{dst}].u64[1] = _lane; }}"
                ]
            if insn.mnemonic == "stc":
                return ["c->cf = 1;"]
            if insn.mnemonic == "clc":
                return ["c->cf = 0;"]
            if insn.mnemonic == "cmc":
                return ["c->cf ^= 1u;"]
            if insn.mnemonic == "popal":
                return [
                    "c->edi = pop32(c); c->esi = pop32(c); c->ebp = pop32(c); "
                    "(void)pop32(c); c->ebx = pop32(c); c->edx = pop32(c); "
                    "c->ecx = pop32(c); c->eax = pop32(c);"
                ]
            if insn.mnemonic == "rdtsc":
                return [
                    "guest_rdtsc(c);"
                ]
            if insn.mnemonic == "cpuid":
                return ["guest_cpuid(c);"]
            if insn.mnemonic == "fxam":
                # x87 stack tags are not represented in CPU. This classifies
                # the double-backed ST(0) values the model can represent. A
                # binary64 subnormal loaded to x87 is normal in ext80 range.
                return [
                    "{ double _v = *fst(c, 0); uint32_t _sw = c->fpu_sw & ~0x4700u; "
                    "if (signbit(_v)) _sw |= 0x0200u; "
                    "_sw |= isnan(_v) ? 0x0100u : isinf(_v) ? 0x0500u : "
                    "_v == 0.0 ? 0x4000u : 0x0400u; c->fpu_sw = _sw; }"
                ]
            if insn.mnemonic == "xlatb":
                return ["SET8L(c->eax, rd8((uint32_t)(c->ebx + R8L(c->eax))));"]
            if insn.mnemonic == "pmuludq":
                dst = self._xi(insn.operands[0])
                src = self._xm(insn, insn.operands[1])
                return [
                    f"{{ XMM _s = {src}; XMM _d = c->xmm[{dst}]; "
                    f"c->xmm[{dst}].u64[0] = (uint64_t)_d.u32[0] * _s.u32[0]; "
                    f"c->xmm[{dst}].u64[1] = (uint64_t)_d.u32[2] * _s.u32[2]; }}"
                ]
            if insn.mnemonic == "pmaxsw":
                dst = self._xi(insn.operands[0])
                src = self._xm(insn, insn.operands[1])
                return [pmaxsw_lanes_code(dst, src)]
            if insn.mnemonic == "rcpps":
                dst = self._xi(insn.operands[0])
                src = self._xm(insn, insn.operands[1])
                return [
                    f"{{ XMM _s = {src}; "
                    f"c->xmm[{dst}].u32[0] = ac1_rcpps_lane(_s.u32[0]); "
                    f"c->xmm[{dst}].u32[1] = ac1_rcpps_lane(_s.u32[1]); "
                    f"c->xmm[{dst}].u32[2] = ac1_rcpps_lane(_s.u32[2]); "
                    f"c->xmm[{dst}].u32[3] = ac1_rcpps_lane(_s.u32[3]); }}"
                ]
            if insn.mnemonic == "rsqrtss":
                dst = self._xi(insn.operands[0])
                source = insn.operands[1]
                src_bits = (f"c->xmm[{self._xi(source)}].u32[0]"
                            if self._is_xmm(source)
                            else f"rd32({self.addr_expr(insn, source)})")
                return [
                    f"{{ uint32_t _src = {src_bits}; "
                    f"c->xmm[{dst}].u32[0] = ac1_rsqrtss_lane(_src); }}"
                ]
            if insn.mnemonic == "rsqrtps":
                dst = self._xi(insn.operands[0])
                src = self._xm(insn, insn.operands[1])
                return [rsqrtps_lanes_code(dst, src)]
            return super().translate(insn, labels)

    pe = pefile.PE(str(binary), fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    image_size = pe.OPTIONAL_HEADER.SizeOfImage

    def read_va(va: int, count: int) -> bytes:
        return pe.get_data((va - image_base) & 0xFFFFFFFF, count)

    reloc_vas = set()
    if pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].VirtualAddress:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
        for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for entry in block.entries:
                if entry.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]:
                    reloc_vas.add((image_base + entry.rva) & 0xFFFFFFFF)
    lifter = AC1Lifter(str(binary), image_size, read_va)
    # pcrecomp's constructor defaults to its historical 0x11000000 base.
    # AC1's preferred base is 0x00400000; wrong bounds disable switch-table
    # recovery even when the table and all of its targets are in the PE.
    lifter.image_lo = image_base
    lifter.image_hi = image_base + image_size
    lifter.reloc_vas = reloc_vas

    lifted = []
    bodies = []
    failures = []
    instruction_counts = {}
    for spec in functions:
        address, size = spec["address"], spec["size"]
        try:
            if emission_cache is None:
                body = lift_exact_function(lifter, spec, read_va, X86_OP_IMM, X86_OP_MEM)
            else:
                body = emission_cache.get_or_lift(
                    spec, lambda: lift_exact_function(
                        lifter, spec, read_va, X86_OP_IMM, X86_OP_MEM))
            bodies.append(body)
            lifted.append(address)
            instruction_counts[address] = spec["instruction_count"]
        except Exception as exc:  # preserve other real bodies when one is unsupported
            failures.append({"address": address, "error": repr(exc)})
    bindings = validate_native_bindings(native_overrides or [], functions, set(lifted), read_va, lifter.md)
    regions = validate_region_bindings(native_overrides or [], functions, set(lifted), read_va, lifter.md)
    if set(bindings) & set(regions): raise ValueError('leaf and region entries overlap')
    pe.close()
    # Specialize after the complete success set is known. Cached bodies remain
    # independent of catalog membership; failed/removed callees cannot become
    # dangling C symbols. Bink uses this emitter with its own image base.
    direct_callees, direct_sites = {}, 0
    if direct_calls and image_base == 0x00400000:
        successful = set(lifted)
        for index, address in enumerate(lifted):
            bodies[index], direct_callees[address], sites = specialize_direct_calls(bodies[index], successful)
            direct_sites += sites
    abi_private, abi_report = {}, None
    if flag_liveness == 'abi':
        public, abi_private, abi_report = flag_abi_program(
            dict(zip(lifted, bodies)), pinned=set(bindings), private_variants=True)
        bodies = [public[a] for a in lifted]
    elif flag_liveness != 'off':
        bodies = [flag_liveness_function(body, flag_liveness) for body in bodies]
    selected_locals = local_register_selection(local_regs, set(lifted))
    local_count, local_excluded = 0, []
    for index, address in enumerate(lifted):
        if address in selected_locals:
            transformed = local_register_function(bodies[index])
            if transformed != bodies[index]:
                local_count += 1
            else:
                local_excluded.append(f'0x{address:08X}')
            bodies[index] = transformed
            if address in abi_private:
                abi_private[address] = local_register_function(abi_private[address])
    for index, address in enumerate(lifted):
        if address in abi_private:
            bodies[index] += '\n\n' + abi_private[address]
    # Keep L_VA as the one public entry for direct transfers AND the dispatch
    # table. Retain a private, callable translated oracle behind that entry.
    region_bodies = [declare_local_memory_base(body) for body in bodies] if regions else []
    for index, address in enumerate(lifted):
        if address in bindings:
            bodies[index] = wrap_native_binding(bodies[index], address, bindings[address])
        elif address in regions:
            bodies[index] = wrap_region_binding(bodies[index], address, regions[address])
    count_entries = set(profile_counts)
    if len(count_entries) > 32 or count_entries - set(lifted):
        raise ValueError('profile counts require at most 32 successfully lifted entries: ' +
                         ', '.join(f'0x{a:08X}' for a in sorted(count_entries-set(lifted))))
    for index, address in enumerate(lifted):
        if address in count_entries:
            bodies[index] = profile_count_entry(bodies[index], address)
    bodies = [declare_local_memory_base(body) for body in bodies]
    if translation_unit_buckets < 0:
        raise ValueError("translation_unit_buckets must be zero or positive")
    if not translation_unit_buckets and functions_per_tu < 1:
        raise ValueError("functions_per_tu must be at least 1")
    tu_header = "\n".join([
        "/* Generated by tools/lift_ac1.py from the locally installed AC1 DX9 PE. */",
        '#include "cpu.h"',
        FP_ADAPTER_HELPERS,
        '#include "ac1_local_memory.h"',
        "#if defined(AC1_TRACE_EIP)",
        "#define AC1_SET_GUEST_EIP(cpu, va) ((cpu)->eip = (va))",
        "#else",
        "#define AC1_SET_GUEST_EIP(cpu, va) ((void)0)",
        "#endif",
        "",
    ])
    direct_prototypes = 0
    body_by_address = dict(zip(lifted, bodies))
    def tu_header_for(addresses: list[int]) -> str:
        nonlocal direct_prototypes
        # Per-shard prototypes avoid parsing all 100k entries in every TU.
        callees = set().union(*(direct_callees.get(a, set()) for a in addresses))
        direct_prototypes += len(callees)
        native_header = '#include "ac1_native.h"\n' if any(a in bindings or a in regions for a in addresses) else ''
        private_needed = set().union(*(set(int(v, 16) for v in re.findall(
            r'AC1_CALL\(c, ac1_abi_([0-9A-F]{8}),', body_by_address[a])) for a in addresses)) if abi_private else set()
        return (tu_header + native_header +
                "".join(f"void L_{a:08X}(CPU *);\n" for a in sorted(callees)) +
                "".join(f"void ac1_abi_{a:08X}(CPU *);\n" for a in sorted(private_needed)) + "\n")

    tu_names = emit_region_sources(regions,lifted,region_bodies,output_dir,tu_header) if regions else []
    if translation_unit_buckets:
        buckets = partition_functions_by_bucket(
            [{"address": address, "body": body}
             for address, body in zip(lifted, bodies)], translation_unit_buckets)
        for bucket_index in range(translation_unit_buckets):
            tu_path = output_dir / f"initial_{bucket_index:04d}.c"
            tu_names.append(tu_path.name)
            bucket = buckets[bucket_index]
            write_text_if_changed(tu_path, tu_header_for(
                [item["address"] for item in bucket]) + "\n\n".join(
                    item["body"] for item in bucket) + "\n")
    else:
        for index in range(0, len(bodies), functions_per_tu):
            tu_path = output_dir / f"initial_{index // functions_per_tu:04d}.c"
            tu_names.append(tu_path.name)
            group_addresses = lifted[index:index + functions_per_tu]
            write_text_if_changed(tu_path, tu_header_for(group_addresses) + "\n\n".join(
                bodies[index:index + functions_per_tu]) + "\n")
    # Keep the original stable source path as a harmless marker; CMake globs
    # each bounded initial_NNNN.c translation unit alongside this file.
    write_text_if_changed(output_dir / "initial.c",
        "/* Generated function bodies are split across initial_NNNN.c files. */\n")
    if image_base == 0x00400000:
        registry = ['/* Generated native binding registry; no guest bytes. */', '#include "ac1_native.h"']
        registry.extend(f'extern ac1_native_binding ac1_binding_{a:08X};' for a in sorted(bindings))
        registry.append('ac1_native_binding *const ac1_native_bindings[] = {')
        registry.extend(f'    &ac1_binding_{a:08X},' for a in sorted(bindings))
        if not bindings: registry.append('    NULL,')
        registry.extend(['};', f'const size_t ac1_native_binding_count = {len(bindings)}u;', ''])
        registry.extend(f'extern ac1_native_region ac1_region_binding_{a:08X};' for a in sorted(regions))
        registry.append('ac1_native_region *const ac1_native_regions[] = {')
        registry.extend(f'    &ac1_region_binding_{a:08X},' for a in sorted(regions))
        if not regions: registry.append('    NULL,')
        registry.extend(['};',f'const size_t ac1_native_region_count = {len(regions)}u;',''])
        write_text_if_changed(output_dir / 'initial_native.c', '\n'.join(registry))
    active_tus = set(tu_names)
    for old_tu in output_dir.glob("initial_[0-9][0-9][0-9][0-9].c"):
        if old_tu.name not in active_tus:
            old_tu.unlink()
    for old_tu in output_dir.glob('initial_region_*.c'):
        if old_tu.name not in active_tus: old_tu.unlink()

    list_path = output_dir / "initial_list.h"
    list_lines = [
        "/* Generated function list; do not edit. */",
        "#pragma once",
        "#define LIFTED_FUNCS(X) \\",
    ]
    list_lines.extend(
        f"    X({address:08X})" + (" \\" if index + 1 < len(lifted) else "")
        for index, address in enumerate(lifted)
    )
    write_text_if_changed(list_path, "\n".join(list_lines) + "\n")

    header_path = output_dir / "initial_dispatch.h"
    write_text_if_changed(header_path,
        "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#include \"cpu.h\"\n"
        "typedef void (*ac1_lifted_fn)(CPU *);\n"
        "typedef struct { uint32_t guest_va; ac1_lifted_fn function; } ac1_lifted_entry;\n"
        "extern const ac1_lifted_entry ac1_lifted_functions[];\n"
        "extern const size_t ac1_lifted_function_count;\n"
        "ac1_lifted_fn ac1_lifted_lookup(uint32_t guest_va);\n",
    )
    dispatch_path = output_dir / "initial_dispatch.c"
    dispatch_lines = [
        "/* Generated sorted guest VA dispatch table; do not edit. */",
        '#include "initial_dispatch.h"',
        "",
    ]
    dispatch_lines.extend(f"void L_{address:08X}(CPU *);" for address in sorted(lifted))
    dispatch_lines.extend(("", "const ac1_lifted_entry ac1_lifted_functions[] = {"))
    dispatch_lines.extend(f"    {{ 0x{address:08X}u, L_{address:08X} }}," for address in sorted(lifted))
    dispatch_lines.extend(("};", f"const size_t ac1_lifted_function_count = {len(lifted)}u;", "",
        "ac1_lifted_fn ac1_lifted_lookup(uint32_t guest_va) {",
        "    size_t lo = 0, hi = ac1_lifted_function_count;",
        "    while (lo < hi) {",
        "        size_t mid = lo + (hi - lo) / 2;",
        "        uint32_t at = ac1_lifted_functions[mid].guest_va;",
        "        if (at < guest_va) lo = mid + 1; else hi = mid;",
        "    }",
        "    return lo < ac1_lifted_function_count && ac1_lifted_functions[lo].guest_va == guest_va",
        "        ? ac1_lifted_functions[lo].function : 0;",
        "}",
    ))
    write_text_if_changed(dispatch_path, "\n".join(dispatch_lines) + "\n")
    return {
        "lifted": lifted,
        "translation_units": tu_names,
        "translation_unit_buckets": translation_unit_buckets,
        "lift_failures": failures,
        "instruction_counts": instruction_counts,
        "relocation_entries": len(reloc_vas),
        "direct_call_sites": direct_sites,
        "profile_count_entries": [f'{a:08X}' for a in sorted(count_entries)],
        "direct_callee_declarations": direct_prototypes,
        "native_overrides": [f"0x{a:08X}" for a in sorted(bindings)],
        "flag_abi": abi_report,
        'local_regs': {'requested': local_regs, 'admitted_count': local_count,
                       'excluded_functions': local_excluded},
    }


def lift_exact_function(lifter, spec: dict, read_va, X86_OP_IMM: int, X86_OP_MEM: int) -> str:
    """Lift exactly the reachable instructions in recovered basic blocks.

    Function extents are often sparse: a jump table can put reachable blocks
    far apart. Passing the min/max address span to the upstream linear lifter
    decodes intervening table data as x86 instructions. Reconstructing the
    block instruction set here preserves the upstream translator while keeping
    those gaps out of generated code.
    """
    from capstone.x86 import X86_OP_REG

    address = spec["address"]
    blocks = spec.get("blocks", [])
    decoded = {}
    for block in blocks:
        for insn_address, size in block["instructions"]:
            if insn_address in decoded:
                continue
            raw = read_va(insn_address, size)
            insns = list(lifter.md.disasm(raw, insn_address, count=1))
            if len(insns) != 1 or insns[0].size != size:
                raise RuntimeError(f"instruction changed while lifting at 0x{insn_address:08X}")
            decoded[insn_address] = insns[0]
    insns = [decoded[va] for va in sorted(decoded)]
    if not insns:
        raise RuntimeError("no reachable instructions in discovered function")
    if address not in decoded:
        raise RuntimeError(f"function entry 0x{address:08X} is absent from its recovered blocks")

    lifter.jumptables = {}
    lifter.func_start = address
    lifter.func_end = max(block["end"] for block in blocks)
    validate_ac1_mmx_interval(address, spec, decoded)
    labels = set()
    block_starts = {block["start"] for block in blocks}
    register_switches = {site["jump"]: site for site in spec.get("register_switches", [])}
    for jump, site in register_switches.items():
        if (jump not in decoded or decoded[jump].mnemonic != "jmp" or
                decoded[jump].operands[0].type != X86_OP_REG or
                lifter.md.reg_name(decoded[jump].operands[0].reg) != site["register"]):
            raise ValueError(f"register-switch jump changed at 0x{jump:08X}")
        for target in site["targets"]:
            if target not in decoded:
                raise ValueError(f"register-switch 0x{jump:08X} missing internal case 0x{target:08X}")
            labels.add(target)
    for insn in insns:
        if insn.mnemonic.startswith("j") or insn.mnemonic.startswith("loop"):
            for operand in insn.operands:
                if operand.type == X86_OP_IMM and operand.imm in decoded:
                    labels.add(operand.imm)
        if insn.mnemonic == "jmp" and insn.operands and insn.operands[0].type == X86_OP_MEM:
            operand = insn.operands[0]
            if operand.mem.index and operand.mem.scale == 4:
                table_va = operand.mem.disp & 0xFFFFFFFF
                if lifter.in_image(table_va):
                    inferred = infer_remapped_switch_limit(
                        lifter.md, read_va, insn, X86_OP_REG, X86_OP_MEM, X86_OP_IMM)
                    if inferred is not None:
                        targets = resolve_jump_table_with_limit(lifter, table_va, inferred[0])
                    else:
                        # Prefer the CFG's validated local blocks. This also
                        # recovers tables with a leading non-code slot that
                        # the generic lifter's image-bounded scan truncates.
                        targets = resolve_jump_table_in_decoded_blocks(
                            lifter, table_va, block_starts)
                        if not targets:
                            targets = lifter.resolve_jumptable(table_va)
                    if targets:
                        lifter.jumptables[insn.address] = targets
                        labels.update(target for target in targets if target in decoded)

    output = [f"void L_{address:08X}(CPU *c)", "{"]
    entry_jump = entry_guard_lines(address, [insn.address for insn in insns])
    output.extend(entry_jump)
    for index, insn in enumerate(insns):
        if insn.address in labels or (entry_jump and insn.address == address):
            output.append(f"L_{insn.address:08X}:")
        output.append(f"    AC1_SET_GUEST_EIP(c, 0x{insn.address:08X}u);")
        if insn.address in register_switches:
            # Read the architectural register, not the table a second time:
            # intervening instructions may have changed memory since the MOV.
            translated = [f"{{ uint32_t _jt = {lifter._target(insn, insn.operands[0])};"]
            for target in sorted(set(register_switches[insn.address]["targets"])):
                translated.append(f"  if (_jt == GVA(0x{target:08X})) goto L_{target:08X};")
            translated.append("  dispatch_jmp(c, _jt); return; }")
        else:
            translated = ac1_mmx_translation(lifter, insn)
            if translated is None:
                translated = ac1_state_translation(lifter, insn)
            if translated is None:
                translated = translate_directional_string_instruction(insn)
        if translated is None:
            translated = lifter.translate(insn, labels)
        controls = {"fwait", "wait", "fnop", "fninit", "finit", "fnclex", "fclex", "fnstcw", "fstcw",
                    "fldcw", "fnstsw", "fstsw", "fnstenv", "fstenv", "fldenv", "fnsave", "fsave", "frstor",
                    "fincstp", "fdecstp", "ffree", "ffreep", "femms"}
        if insn.mnemonic.startswith("f") and insn.mnemonic not in controls:
            prefix = [f"if (!ac1_x87_check(c, 0x{insn.address:08X}u)) return;",
                      f"c->fpu_ip=GVA(0x{insn.address:08X}); c->fpu_cs=c->cs; "
                      f"c->fpu_opcode=0x{((insn.opcode[0]&7)<<8)|insn.modrm:03X}u;"]
            for operand in insn.operands:
                if operand.type == X86_OP_MEM:
                    base = lifter.md.reg_name(operand.mem.base) if operand.mem.base else ""
                    segment = lifter.md.reg_name(operand.mem.segment) if operand.mem.segment else (
                        "ss" if base in ("esp", "ebp") else "ds")
                    prefix.append(f"c->fpu_dp={lifter.addr_expr(insn,operand)}; c->fpu_ds=c->{segment};")
                    break
            if insn.mnemonic in ("fst", "fstp") and insn.operands[0].type == X86_OP_REG:
                st_index = lifter._st_idx(insn.operands[0])  # not `index`: the loop uses it below
                prefix.append(f"ac1_x87_set_tag(c,(unsigned)(c->fpu_top+{st_index})&7u,0);")
            translated = prefix + translated
        for line in translated:
            output.append(f"    {line:<60} /* {insn.address:08X}: {insn.mnemonic} {insn.op_str} */")
        next_decoded = insns[index + 1].address if index + 1 < len(insns) else None
        target = fallthrough_target(insn.mnemonic, insn.address + insn.size, next_decoded)
        if target is not None:
            output.append(f"    dispatch(c, 0x{target:08X}u); return;")
    output.append("}")
    return "\n".join(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=CONFIG_PATH,
                        help="discovery configuration (allows private seed-set A/B comparisons)")
    parser.add_argument("--selftest", action="store_true", help="run the focused block-entry ordering check")
    parser.add_argument('--local-regs', default='off', metavar='off|all|VA,...',
                        help='function-gated GPR locals; off restores CPU-struct registers')
    parser.add_argument('--flag-liveness', choices=('off', 'straight', 'cfg', 'abi'), default='cfg',
                        help='constant-mask flag liveness; abi adds proved direct-only return contracts')
    parser.add_argument("--no-direct-calls", action="store_true",
                        help="keep constant main-catalog transfers in dispatch for A/B comparisons")
    parser.add_argument('--profile-count-entries', default='',
                        help='comma-separated hex entries to instrument in a dedicated count-only build (max 32)')
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--pcrecomp", type=Path)
    parser.add_argument("--python", type=Path)
    parser.add_argument("--output-dir", type=Path, default=OUT_DEFAULT)
    parser.add_argument("--max-functions", type=int)
    parser.add_argument("--max-depth", type=int)
    parser.add_argument("--seed", action="append", default=[], metavar="VA",
                        help="also discover from a guest VA (repeatable; decimal or 0x-prefixed)")
    parser.add_argument("--seed-file", type=Path,
                        help="additional guest VA seeds as a JSON array or one address per line")
    parser.add_argument("--initializer-tables", type=Path,
                        default=ROOT / "config/startup_initializers.json",
                        help="fingerprint-pinned JSON containing executable initializer pointer tables")
    parser.add_argument("--initializer-group", choices=("all", "c_initterm_e", "cpp_initializer", "none"),
                        default="all", help="which pinned initializer pointer table(s) to seed")
    parser.add_argument("--functions-per-tu", type=int,
                        help="maximum generated functions per C translation unit")
    parser.add_argument("--translation-unit-buckets", type=int,
                        help="fixed address-hash partition count; overrides functions-per-tu grouping")
    parser.add_argument("--no-emission-cache", action="store_true",
                        help="disable the persistent generated-body cache")
    parser.add_argument("--no-code-immediates", action="store_true",
                        help="disable callback discovery from decoded MOV/PUSH imm32 operands")
    parser.add_argument("--no-indexed-tables", action="store_true",
                        help="disable initialized-data function-table discovery")
    parser.add_argument("--no-register-switches", action="store_true",
                        help="disable guarded register-loaded switch recovery for A/B comparison")
    args = parser.parse_args()
    config_path = args.config.resolve()
    if args.selftest:
        config = json.loads(config_path.read_text(encoding="utf-8"))
        python, pcrecomp = resolve_tooling(config, args)
        binary = (args.binary or Path(config["binary"])).resolve()
        if sha256(binary) != config["sha256"]:
            parser.error(f"installed executable SHA-256 does not match {config_path}")
        run_selftest()
        run_ac1_switch_selftest(binary, pcrecomp)
        run_terminal_external_direct_jump_selftest(binary, pcrecomp)
        return 0
    driver_path = Path(__file__).resolve()
    driver_hash = sha256(driver_path)
    config_bytes = config_path.read_bytes()
    config_hash = sha256_bytes(config_bytes)
    config = json.loads(config_bytes)
    python, pcrecomp = resolve_tooling(config, args)
    binary = (args.binary or Path(config["binary"])).resolve()
    output_dir = args.output_dir.resolve()
    if not output_dir.is_relative_to((ROOT / "work").resolve()):
        parser.error(f"output-dir must be under {ROOT / 'work'}")
    binary_hash = sha256(binary)
    if binary_hash != config["sha256"]:
        parser.error(f"installed executable SHA-256 does not match {config_path}")
    revision = subprocess.check_output(["git", "-C", str(pcrecomp), "rev-parse", "HEAD"], text=True).strip()
    if revision != config["pcrecomp_revision"]:
        parser.error(f"pcrecomp revision mismatch: expected {config['pcrecomp_revision']}, got {revision}")
    donor_paths = {
        "pcrecomp_pe_analyze": pcrecomp / "tools/pe/pe_analyze.py",
        "pcrecomp_disasm32": pcrecomp / "tools/disasm/disasm32.py",
        "pcrecomp_lifter": pcrecomp / "tools/lift/lift32_cpu.py",
        "initializer_analyzer": ROOT / "tools/analyze_initializer_tables.py",
    }
    input_fingerprints: dict[str, tuple[Path, str]] = {
        "driver": (driver_path, driver_hash),
        "config": (config_path, config_hash),
        "binary": (binary, binary_hash),
        **{name: (path, sha256(path)) for name, path in donor_paths.items()},
    }
    cache_module = ROOT / "tools/emission_cache.py"
    input_fingerprints["emission_cache"] = (cache_module, sha256(cache_module))
    max_functions = args.max_functions or config["initial_discovery"]["max_functions"]
    max_depth = args.max_depth if args.max_depth is not None else config["initial_discovery"]["max_depth"]
    functions_per_tu = args.functions_per_tu or config.get("functions_per_translation_unit", 100)
    translation_unit_buckets = (args.translation_unit_buckets
                                if args.translation_unit_buckets is not None
                                else config.get("translation_unit_buckets", 0))
    if translation_unit_buckets < 0:
        parser.error("translation-unit-buckets must be zero or positive")
    extra_seeds = [int(seed, 0) for seed in config["initial_discovery"].get("startup_seeds", [])]
    extra_seeds.extend(int(seed, 0) for seed in args.seed)
    seed_file_hash = None
    if args.seed_file:
        seed_path = args.seed_file.resolve()
        seed_bytes = seed_path.read_bytes()
        seed_file_hash = sha256_bytes(seed_bytes)
        input_fingerprints["seed_file"] = (seed_path, seed_file_hash)
        seed_text = seed_bytes.decode("utf-8").strip()
        try:
            seed_values = json.loads(seed_text)
            if not isinstance(seed_values, list):
                parser.error("seed JSON must be an array")
        except json.JSONDecodeError:
            seed_values = [
                line.split("#", 1)[0].strip()
                for line in seed_text.splitlines()
            ]
            seed_values = [value for value in seed_values if value]
        extra_seeds.extend(
            int(value, 0) if isinstance(value, str) else int(value)
            for value in seed_values
        )
    initializer_report = None
    initializer_config_hash = None
    if args.initializer_group != "none":
        sys.path.insert(0, str(ROOT / "tools"))
        from analyze_initializer_tables import analyze as analyze_initializer_tables
        initializer_path = args.initializer_tables.resolve()
        initializer_bytes = initializer_path.read_bytes()
        initializer_config = json.loads(initializer_bytes)
        initializer_config_hash = sha256_bytes(initializer_bytes)
        input_fingerprints["initializer_tables"] = (initializer_path, initializer_config_hash)
        if args.initializer_group != "all":
            initializer_config["tables"] = [
                table for table in initializer_config["tables"]
                if table["name"] == args.initializer_group
            ]
            if not initializer_config["tables"]:
                parser.error(f"initializer table group {args.initializer_group!r} is not configured")
        initializer_report = analyze_initializer_tables(binary, initializer_config)
        if any(table["unique_non_executable_pointer_count"] for table in initializer_report["tables"]):
            parser.error("initializer table contains non-null pointers outside executable PE sections")
        extra_seeds.extend(initializer_report["unique_executable_pointers_across_tables"])
    extra_seeds = sorted(set(extra_seeds))
    output_dir.mkdir(parents=True, exist_ok=True)
    report_path = output_dir / "lift-report.json"
    invalidate_success_report(report_path)
    start_time = time.perf_counter()
    discovery_start = time.perf_counter()
    (info, dis, catalog, discovery_failures, queued, entry, iat, callback_edges,
     immediate_summary, indexed_summary) = discover_entry_cone(
        binary, pcrecomp, max_functions, max_depth, extra_seeds,
        code_immediates=not args.no_code_immediates, indexed_tables=not args.no_indexed_tables,
        register_switches=not args.no_register_switches)
    discovery_seconds = time.perf_counter() - discovery_start
    catalog_doc = {
        "binary_sha256": binary_hash,
        "pcrecomp_revision": revision,
        "entry_point": entry,
        "image_base": info.image_base,
        "code_start": info.code_start,
        "code_end": info.code_end,
        "iat_count": len(iat),
        "max_functions": max_functions,
        "max_depth": max_depth,
        "extra_seeds": extra_seeds,
        "initializer_table_summary": ({
            "config": str(args.initializer_tables.resolve()),
            "tables": [
                {key: table[key] for key in (
                    "name", "start_va", "end_va_exclusive", "entry_count", "null_count",
                    "nonnull_count", "unique_executable_pointer_count",
                    "unique_non_executable_pointer_count")}
                for table in initializer_report["tables"]
            ],
            "unique_executable_pointer_count": initializer_report[
                "unique_executable_pointer_count_across_tables"],
        } if initializer_report else None),
        "queue_remaining": queued,
        "discovered_count": len(catalog),
        "discovery_failures": discovery_failures,
        "discovered_callback_edges": callback_edges,
        "code_immediate_discovery": immediate_summary,
        "indexed_table_discovery": indexed_summary,
        "register_switch_discovery": {
            "enabled": not args.no_register_switches,
            "site_count": len({site["jump"] for spec in catalog.values()
                               for site in spec.get("register_switches", [])}),
            "added_blocks": sum(spec.get("register_switch_added_blocks", 0) for spec in catalog.values()),
            "sites": [{"caller": spec["address"], **site} for spec in catalog.values()
                      for site in spec.get("register_switches", [])],
        },
        "functions": sorted(catalog.values(), key=lambda item: item["address"]),
    }
    catalog_path = output_dir / "functions.json"
    write_text_if_changed(catalog_path, json.dumps(catalog_doc, indent=2) + "\n")
    funcs_path = output_dir / "functions.txt"
    write_text_if_changed(funcs_path, "".join(
        f"0x{f['address']:08X} {f['size']} ac1_{f['address']:08X}\n"
        for f in catalog_doc["functions"]))
    output = output_dir / "initial.c"
    emit_start = time.perf_counter()
    emission_cache = None
    if not args.no_emission_cache:
        sys.path.insert(0, str(ROOT / "tools"))
        from emission_cache import EmissionCache
        semantic_inputs = {
            "binary_sha256": binary_hash,
            "driver_sha256": driver_hash,
            "pcrecomp_revision": revision,
            "pcrecomp_inputs": {
                name: expected_hash
                for name, (_path, expected_hash) in sorted(input_fingerprints.items())
                if name.startswith("pcrecomp_")
            },
            "image_base": info.image_base,
            "python": sys.version,
            "capstone_version": importlib.metadata.version("capstone"),
            "pefile_version": importlib.metadata.version("pefile"),
        }
        emission_cache = EmissionCache(
            ROOT / "work/emission_cache/ac1_bodies.sqlite3", semantic_inputs)
    try:
        lift_result = emit_lift(binary, pcrecomp, catalog_doc["functions"], output_dir,
                                functions_per_tu, translation_unit_buckets,
                                emission_cache, direct_calls=not args.no_direct_calls,
                                native_overrides=config.get('native_overrides', []),
                                flag_liveness=args.flag_liveness, local_regs=args.local_regs,
                                profile_counts=[int(s, 16) for s in args.profile_count_entries.split(',') if s])
    except Exception:
        if emission_cache is not None:
            emission_cache.close()
        raise
    emit_seconds = time.perf_counter() - emit_start
    todo_count = sum((output_dir / name).read_text(encoding="utf-8").count("RECOMP_TODO")
                     for name in lift_result["translation_units"])
    report = {
        **{key: value for key, value in catalog_doc.items() if key != "functions"},
        "discovered_functions": len(catalog),
        "lifted_function_count": len(lift_result["lifted"]),
        "lifted_functions": lift_result["lifted"],
        "lift_failures": lift_result["lift_failures"],
        "unsupported_count": todo_count,
        "relocation_entries": lift_result["relocation_entries"],
        "flag_liveness": args.flag_liveness,
        "flag_abi": lift_result["flag_abi"],
        "local_regs": lift_result["local_regs"],
        "direct_call_sites": lift_result["direct_call_sites"],
        "profile_count_entries": lift_result['profile_count_entries'],
        "direct_callee_declarations": lift_result["direct_callee_declarations"],
        "native_overrides": lift_result["native_overrides"],
        "translation_unit_buckets": lift_result["translation_unit_buckets"],
        "outputs": {
            "c": output.name,
            "translation_units": lift_result["translation_units"],
            "function_list": "initial_list.h",
            "dispatch_header": "initial_dispatch.h",
            "dispatch_source": "initial_dispatch.c",
            "catalog": catalog_path.name,
            "function_bounds": funcs_path.name,
        },
        "elapsed_seconds": round(time.perf_counter() - start_time, 3),
        "discovery_seconds": round(discovery_seconds, 3),
        "emit_seconds": round(emit_seconds, 3),
        "emission_cache": ({
            "enabled": True,
            "semantic_stamp": emission_cache.semantic_stamp,
            **emission_cache.stats,
        } if emission_cache is not None else {"enabled": False}),
        "pcrecomp_root": str(pcrecomp),
        "pcrecomp_lifter_sha256": input_fingerprints["pcrecomp_lifter"][1],
        "generation_input_sha256": {
            name: expected_hash
            for name, (_path, expected_hash) in sorted(input_fingerprints.items())
        },
        "seed_file_sha256": seed_file_hash,
        "python": str(python),
        "capstone_version": importlib.metadata.version("capstone"),
        "pefile_version": importlib.metadata.version("pefile"),
        "limitations": [
            "Discovery follows bounded direct calls and validated direct tail thunks from the PE entry point and explicit --seed targets; arbitrary indirect calls, exports, and generic data-pointer targets remain uncovered.",
            "No menu, graphics, or gameplay has been verified through this generated slice.",
            "Callback discovery follows validated AC1 F9FA43 constructor/cleanup, 438EF0 schema-helper arg2, and 9312F0 reader-helper arg2 ABIs, plus decoded MOV/PUSH imm32 code pointers unless --no-code-immediates is set; arbitrary virtual calls, data-only callback records, and exports remain uncovered.",
            "Initialized-data indexed function tables and proven same-block absolute-load CALL/JMP slots are scanned forward with a 1024-entry cap unless --no-indexed-tables is set; pre-scaled register tables are counted but not followed.",
            "Any RECOMP_TODO emitted by the upstream lifter remains a concrete translation gap.",
            "The inspected DX9 image has no base-relocation directory, so the first runtime must preserve its preferred image base 0x00400000 unless relocation evidence changes.",
        ],
    }
    report["adapters"] = [
        "Exact reachable instructions from recovered basic blocks; no linear decode across gaps.",
        "MOVHLPS/MOVLHPS 64-bit lane moves.",
        "STC/CLC/CMC carry flag operations and POPAD register restore.",
            "FXAM classifies nonempty double-backed ST(0) values; empty x87 tag state is not modeled.",
            "XLATB reads DS:EBX+unsigned AL into AL, preserving upper EAX and flags.",
        "RDTSC through the runtime guest clock helper.",
        "CPUID through the runtime guest CPUID helper.",
        "Optional AC1_TRACE_EIP instrumentation updates CPU.eip before each instruction.",
    ]
    report["lift_driver_sha256"] = driver_hash
    report["config_sha256"] = config_hash
    report["initializer_tables_sha256"] = initializer_config_hash
    try:
        current_revision = subprocess.check_output(
            ["git", "-C", str(pcrecomp), "rev-parse", "HEAD"], text=True).strip()
    except Exception as exc:
        if emission_cache is not None:
            emission_cache.close()
        print(f"error: unable to recheck pcrecomp revision before report: {exc}", file=sys.stderr)
        return 2
    if current_revision != revision:
        if emission_cache is not None:
            emission_cache.close()
        print("error: pcrecomp revision changed during lift; no success report written", file=sys.stderr)
        return 2

    def current_inputs_match() -> bool:
        if not generation_inputs_unchanged(input_fingerprints):
            return False
        try:
            return subprocess.check_output(
                ["git", "-C", str(pcrecomp), "rev-parse", "HEAD"],
                text=True).strip() == revision
        except Exception:
            return False

    # Verify once before cache publication. The cache module repeats this guard
    # around its staged database replacement; the success report is written
    # only after publication and receives the final cache timing/status.
    if not current_inputs_match():
        if emission_cache is not None:
            emission_cache.close()
        print("error: generation inputs changed during lift; no success report written",
              file=sys.stderr)
        return 2
    if emission_cache is not None:
        try:
            published = emission_cache.publish(current_inputs_match)
        except Exception as exc:
            emission_cache.close()
            print(f"error: emission cache publication failed: {exc}; no success report written",
                  file=sys.stderr)
            return 2
        publish_status = emission_cache.stats["publish_status"]
        if not published and publish_status in ("guard-failed", "guard-error", "module-changed"):
            emission_cache.close()
            print(f"error: emission cache publication rejected ({publish_status}); "
                  "no success report written", file=sys.stderr)
            return 2
        report["emission_cache"] = {
            "enabled": True,
            "semantic_stamp": emission_cache.semantic_stamp,
            **emission_cache.stats,
        }
        emission_cache.close()
    report["elapsed_seconds"] = round(time.perf_counter() - start_time, 3)
    try:
        write_report_if_inputs_unchanged(
            report_path, json.dumps(report, indent=2) + "\n",
            input_fingerprints)
    except RuntimeError as exc:
        if emission_cache is not None:
            emission_cache.close()
        print(f"error: {exc}; no success report written", file=sys.stderr)
        return 2
    print(f"entry=0x{entry:08X}; discovered={len(catalog)}; lifted={len(lift_result['lifted'])}; "
          f"discovery failures={len(discovery_failures)}; lift failures={len(lift_result['lift_failures'])}; "
          f"RECOMP_TODO={todo_count}; remaining call queue={queued}; out={output_dir}")
    if lift_result['flag_abi'] is not None:
        abi = lift_result['flag_abi']
        print('FLAGABI: public entries all-live={public_entries_all_live}; '
              'public narrowed={public_returns_narrowed}; '
              'private functions/returns narrowed={private_functions_narrowed}/{private_returns_narrowed}; '
              'direct sites narrowed={direct_sites_narrowed}; '
              'proved defining callees={proven_defining_callees}'.format(**abi))
    return 0 if lift_result["lifted"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
