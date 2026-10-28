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
import subprocess
import sys
import time
from pathlib import Path

# The user's existing projects are read-only references, including caches.
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATH = ROOT / "config/ac1_dx9.json"
DUKE_DEFAULT = Path("D:/Prog/dukemp_recomp/.deps/pcrecomp")
PYTHON_DEFAULT = Path("D:/Prog/dukemp_recomp/.venv/Scripts/python.exe")
OUT_DEFAULT = ROOT / "work/lift_initial"


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


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_text_if_changed(path: Path, text: str) -> bool:
    """Preserve output mtimes unless the generated content changed."""
    data = text.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def entry_guard_lines(function_start: int, ordered_addresses: list[int]) -> list[str]:
    """Return the branch needed when a backward edge sorts before entry."""
    if not ordered_addresses or function_start not in ordered_addresses:
        raise ValueError("recovered instruction set must contain the function entry")
    if ordered_addresses[0] == function_start:
        return []
    return [f"    goto L_{function_start:08X};"]


def fallthrough_target(mnemonic: str, next_pc: int, next_decoded: int | None) -> int | None:
    """Return the real next guest VA when sorted C would cross a code gap."""
    terminal = {"jmp", "ret", "retn", "retf", "iret", "iretd", "hlt", "int3"}
    if mnemonic.split()[-1] in terminal:
        return None
    return None if next_decoded == next_pc else next_pc


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
    # XLATB indexes EBX with unsigned AL, replaces only AL, and leaves flags.
    eax, ebx, flags = 0x123456FE, 0xFFFF0000, 0x8D7
    table = bytes((i * 7 + 3) & 0xFF for i in range(256))
    al = table[((ebx & 0xFFFFFFFF) + (eax & 0xFF)) & 0xFF]
    after = (eax & 0xFFFFFF00) | al
    assert after == 0x123456F5 and flags == 0x8D7
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
        remapped_body = lift_exact_function(
            remap_lifter, remapped_spec,
            lambda va, count: pe.get_data((va - image_base) & 0xFFFFFFFF, count),
            X86_OP_IMM, X86_OP_MEM)
        for target in sorted(real_targets):
            assert f"_jt == GVA(0x{target:08X})" in remapped_body
        for target in sorted(remap2_targets):
            assert f"_jt == GVA(0x{target:08X})" in remapped_body
        assert "0x01010300" not in remapped_body
    finally:
        pe.close()
    print("AC1 actual-base jump-table self-test: both indirect switches recovered")


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
                input_count = bound + (1 if guards[0] == "ja" else 0)
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
            delta = (map_base - table_base) & 0xFFFFFFFF
            if not delta or delta % 4:
                continue
            table_count = delta // 4
            max_index = max(remap_bytes)
            if not 1 <= table_count <= 256 or max_index >= table_count:
                continue
            return table_count, input_count
    return None


def install_ac1_jump_table_bounds(disassembler) -> None:
    """Apply AC1-local bounds for guarded byte-remapped switch tables."""
    from types import MethodType
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

    original = disassembler.jump_table_targets

    def bounded(self, insn, limit=256):
        inferred = infer_remapped_switch_limit(
            self.md, self.read_bytes, insn, X86_OP_REG, X86_OP_MEM, X86_OP_IMM)
        if inferred is not None:
            limit = min(limit, inferred[0])
        return original(insn, limit)

    disassembler.jump_table_targets = MethodType(bounded, disassembler)


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


def discover_entry_cone(binary: Path, pcrecomp: Path, max_functions: int, max_depth: int,
                        extra_seeds: list[int]):
    sys.path[:0] = [str(pcrecomp / "tools/pe"), str(pcrecomp / "tools/disasm")]
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler

    info = analyze_pe(str(binary))
    code = binary.read_bytes()
    iat = build_iat_map(info)
    dis = Disassembler(code, info.image_base, info.sections)
    install_ac1_jump_table_bounds(dis)
    entry = info.image_base + info.entry_point_rva
    initial_seeds = sorted({entry, *extra_seeds})
    queue = collections.deque((seed, 0) for seed in initial_seeds)
    queued = set(initial_seeds)
    results = {}
    failures = []
    while queue and len(results) < max_functions:
        address, depth = queue.popleft()
        if address in results:
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
        if depth >= max_depth:
            continue
        for target in sorted(function.calls_to):
            if target not in results and target not in queued and info.code_start <= target < info.code_end:
                queued.add(target)
                queue.append((target, depth + 1))
    return info, dis, results, failures, len(queue), entry, iat


def emit_lift(binary: Path, pcrecomp: Path, functions: list[dict], output_dir: Path,
              functions_per_tu: int) -> dict:
    sys.path.insert(0, str(pcrecomp / "tools/lift"))
    import pefile
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    from lift32_cpu import Lifter as BaseLifter

    class AC1Lifter(BaseLifter):
        """AC1-specific instruction semantics missing from the donor lifter."""
        def translate(self, insn, labels):
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
            body = lift_exact_function(lifter, spec, read_va, X86_OP_IMM, X86_OP_MEM)
            bodies.append(body)
            lifted.append(address)
            instruction_counts[address] = spec["instruction_count"]
        except Exception as exc:  # preserve other real bodies when one is unsupported
            failures.append({"address": address, "error": repr(exc)})
    pe.close()
    if functions_per_tu < 1:
        raise ValueError("functions_per_tu must be at least 1")
    tu_header = "\n".join([
        "/* Generated by tools/lift_ac1.py from the locally installed AC1 DX9 PE. */",
        '#include "cpu.h"',
        FP_ADAPTER_HELPERS,
        "#if defined(AC1_TRACE_EIP)",
        "#define AC1_SET_GUEST_EIP(cpu, va) ((cpu)->eip = (va))",
        "#else",
        "#define AC1_SET_GUEST_EIP(cpu, va) ((void)0)",
        "#endif",
        "",
    ])
    tu_names = []
    for index in range(0, len(bodies), functions_per_tu):
        tu_path = output_dir / f"initial_{index // functions_per_tu:04d}.c"
        tu_names.append(tu_path.name)
        write_text_if_changed(tu_path, tu_header + "\n\n".join(
            bodies[index:index + functions_per_tu]) + "\n")
    # Keep the original stable source path as a harmless marker; CMake globs
    # each bounded initial_NNNN.c translation unit alongside this file.
    write_text_if_changed(output_dir / "initial.c",
        "/* Generated function bodies are split across initial_NNNN.c files. */\n")
    active_tus = set(tu_names)
    for old_tu in output_dir.glob("initial_[0-9][0-9][0-9][0-9].c"):
        if old_tu.name not in active_tus:
            old_tu.unlink()

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
        "lift_failures": failures,
        "instruction_counts": instruction_counts,
        "relocation_entries": len(reloc_vas),
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
    labels = set()
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
                    targets = (resolve_jump_table_with_limit(lifter, table_va, inferred[0])
                               if inferred is not None
                               else lifter.resolve_jumptable(table_va))
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
        for line in lifter.translate(insn, labels):
            output.append(f"    {line:<60} /* {insn.address:08X}: {insn.mnemonic} {insn.op_str} */")
        next_decoded = insns[index + 1].address if index + 1 < len(insns) else None
        target = fallthrough_target(insn.mnemonic, insn.address + insn.size, next_decoded)
        if target is not None:
            output.append(f"    dispatch(c, 0x{target:08X}u); return;")
    output.append("}")
    return "\n".join(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="run the focused block-entry ordering check")
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
    args = parser.parse_args()
    if args.selftest:
        config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        python, pcrecomp = resolve_tooling(config, args)
        binary = (args.binary or Path(config["binary"])).resolve()
        if sha256(binary) != config["sha256"]:
            parser.error("installed executable SHA-256 does not match config/ac1_dx9.json")
        run_selftest()
        run_ac1_switch_selftest(binary, pcrecomp)
        return 0
    config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    python, pcrecomp = resolve_tooling(config, args)
    binary = (args.binary or Path(config["binary"])).resolve()
    output_dir = args.output_dir.resolve()
    if not output_dir.is_relative_to((ROOT / "work").resolve()):
        parser.error(f"output-dir must be under {ROOT / 'work'}")
    if sha256(binary) != config["sha256"]:
        parser.error("installed executable SHA-256 does not match config/ac1_dx9.json")
    revision = subprocess.check_output(["git", "-C", str(pcrecomp), "rev-parse", "HEAD"], text=True).strip()
    if revision != config["pcrecomp_revision"]:
        parser.error(f"pcrecomp revision mismatch: expected {config['pcrecomp_revision']}, got {revision}")
    max_functions = args.max_functions or config["initial_discovery"]["max_functions"]
    max_depth = args.max_depth if args.max_depth is not None else config["initial_discovery"]["max_depth"]
    functions_per_tu = args.functions_per_tu or config.get("functions_per_translation_unit", 100)
    extra_seeds = [int(seed, 0) for seed in config["initial_discovery"].get("startup_seeds", [])]
    extra_seeds.extend(int(seed, 0) for seed in args.seed)
    if args.seed_file:
        seed_text = args.seed_file.read_text(encoding="utf-8").strip()
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
        initializer_config = json.loads(args.initializer_tables.read_text(encoding="utf-8"))
        initializer_config_hash = sha256(args.initializer_tables)
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
    start_time = time.perf_counter()
    info, dis, catalog, discovery_failures, queued, entry, iat = discover_entry_cone(
        binary, pcrecomp, max_functions, max_depth, extra_seeds
    )
    catalog_doc = {
        "binary_sha256": config["sha256"],
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
        "functions": sorted(catalog.values(), key=lambda item: item["address"]),
    }
    catalog_path = output_dir / "functions.json"
    write_text_if_changed(catalog_path, json.dumps(catalog_doc, indent=2) + "\n")
    funcs_path = output_dir / "functions.txt"
    write_text_if_changed(funcs_path, "".join(
        f"0x{f['address']:08X} {f['size']} ac1_{f['address']:08X}\n"
        for f in catalog_doc["functions"]))
    output = output_dir / "initial.c"
    lift_result = emit_lift(binary, pcrecomp, catalog_doc["functions"], output_dir,
                            functions_per_tu)
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
        "pcrecomp_root": str(pcrecomp),
        "pcrecomp_lifter_sha256": sha256(pcrecomp / "tools/lift/lift32_cpu.py"),
        "python": str(python),
        "capstone_version": importlib.metadata.version("capstone"),
        "pefile_version": importlib.metadata.version("pefile"),
        "limitations": [
            "Discovery follows bounded direct calls from the PE entry point and any explicit --seed targets; indirect calls, callbacks, exports and data-pointer targets are not yet seeded.",
            "No menu, graphics, or gameplay has been verified through this generated slice.",
            "The table-seeded startup set includes validated C/C++ initializer functions but does not yet cover every indirect callback, export, or data-pointer target.",
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
    report["lift_driver_sha256"] = sha256(Path(__file__).resolve())
    report["config_sha256"] = sha256(CONFIG_PATH)
    report["initializer_tables_sha256"] = initializer_config_hash
    write_text_if_changed(output_dir / "lift-report.json", json.dumps(report, indent=2) + "\n")
    print(f"entry=0x{entry:08X}; discovered={len(catalog)}; lifted={len(lift_result['lifted'])}; "
          f"discovery failures={len(discovery_failures)}; lift failures={len(lift_result['lift_failures'])}; "
          f"RECOMP_TODO={todo_count}; remaining call queue={queued}; out={output_dir}")
    return 0 if lift_result["lifted"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
