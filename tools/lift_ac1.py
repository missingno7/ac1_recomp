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
AC1_ARRAY_CTOR_HELPER = 0x00F9FA43
AC1_SCHEMA_CALLBACK_HELPER = 0x00438EF0
AC1_READER_CALLBACK_HELPER = 0x009312F0
AC1_ARG2_CALLBACK_HELPERS = {
    AC1_SCHEMA_CALLBACK_HELPER: "schema_callback",
    AC1_READER_CALLBACK_HELPER: "reader_callback",
}

# These are the only MMX intervals in the current observed AC1 catalog. Each
# begins with a MOVQ from memory, contains no calls or x87 operations, and ends
# with EMMS before returning. Keep MMX lowering scoped to these closed paths
# until the CPU model grows full x87/MMX physical-register alias semantics.
AC1_MMX_FUNCTION_COUNTS = {
    0x01023410: 30,
    0x01023520: 29,
}
AC1_MMX_MNEMONICS = {"movq", "pcmpgtw", "paddw", "psubw", "pand"}

AC1_MMX_LOCAL_HELPERS = r"""
static inline void ac1_mmx_enter(CPU *c, int *active)
{
    if (!*active) { c->fpu_top = 0; *active = 1; }
}
static inline uint64_t ac1_mmx_read(CPU *c, uint64_t regs[8], uint8_t *valid,
                                    int *active, unsigned reg)
{
    ac1_mmx_enter(c, active);
    if (reg >= 8u || !(*valid & (uint8_t)(1u << reg))) {
        ac1_unsupported_instruction(c->eip, "uninitialized register in bounded MMX interval");
        return 0u;
    }
    return regs[reg];
}
static inline void ac1_mmx_write(CPU *c, uint64_t regs[8], uint8_t *valid,
                                 int *active, unsigned reg, uint64_t value)
{
    ac1_mmx_enter(c, active);
    if (reg >= 8u) {
        ac1_unsupported_instruction(c->eip, "invalid register in bounded MMX interval");
        return;
    }
    regs[reg] = value;
    *valid |= (uint8_t)(1u << reg);
}
static inline void ac1_mmx_pcmpgtw(CPU *c, uint64_t regs[8], uint8_t *valid,
                                   int *active, unsigned dst, uint64_t src)
{
    uint64_t left = ac1_mmx_read(c, regs, valid, active, dst), result = 0u;
    for (unsigned lane = 0; lane < 4u; ++lane) {
        unsigned shift = lane * 16u;
        uint16_t a = (uint16_t)(left >> shift), b = (uint16_t)(src >> shift);
        if ((uint16_t)(a ^ 0x8000u) > (uint16_t)(b ^ 0x8000u))
            result |= UINT64_C(0xffff) << shift;
    }
    ac1_mmx_write(c, regs, valid, active, dst, result);
}
static inline void ac1_mmx_paddw(CPU *c, uint64_t regs[8], uint8_t *valid,
                                 int *active, unsigned dst, uint64_t src)
{
    uint64_t left = ac1_mmx_read(c, regs, valid, active, dst), result = 0u;
    for (unsigned lane = 0; lane < 4u; ++lane) {
        unsigned shift = lane * 16u;
        uint16_t sum = (uint16_t)((uint16_t)(left >> shift) + (uint16_t)(src >> shift));
        result |= (uint64_t)sum << shift;
    }
    ac1_mmx_write(c, regs, valid, active, dst, result);
}
static inline void ac1_mmx_psubw(CPU *c, uint64_t regs[8], uint8_t *valid,
                                 int *active, unsigned dst, uint64_t src)
{
    uint64_t left = ac1_mmx_read(c, regs, valid, active, dst), result = 0u;
    for (unsigned lane = 0; lane < 4u; ++lane) {
        unsigned shift = lane * 16u;
        uint16_t difference = (uint16_t)((uint16_t)(left >> shift) - (uint16_t)(src >> shift));
        result |= (uint64_t)difference << shift;
    }
    ac1_mmx_write(c, regs, valid, active, dst, result);
}
static inline void ac1_mmx_pand(CPU *c, uint64_t regs[8], uint8_t *valid,
                                int *active, unsigned dst, uint64_t src)
{
    uint64_t left = ac1_mmx_read(c, regs, valid, active, dst);
    ac1_mmx_write(c, regs, valid, active, dst, left & src);
}
static inline void ac1_mmx_emms(CPU *c, int *active)
{
    *active = 0;
    c->fpu_top = 0;
}
""".strip()


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
    """Lower the observed MOVQ/word-arithmetic MMX forms with exact m64 widths."""
    from capstone.x86 import X86_OP_MEM, X86_OP_REG

    if insn.mnemonic == "emms":
        return ["ac1_mmx_emms(c, &ac1_mmx_active);"]
    if insn.mnemonic not in AC1_MMX_MNEMONICS or len(insn.operands) != 2:
        return None

    destination, source = insn.operands

    def mm_register(operand) -> int | None:
        if operand.type != X86_OP_REG or operand.size != 8:
            return None
        name = insn.reg_name(operand.reg)
        if len(name) == 3 and name.startswith("mm") and name[2] in "01234567":
            return int(name[2])
        return None

    def source_expression(operand) -> str | None:
        register = mm_register(operand)
        if register is not None:
            return ("ac1_mmx_read(c, ac1_mmx, &ac1_mmx_valid, "
                    f"&ac1_mmx_active, {register}u)")
        if operand.type == X86_OP_MEM and operand.size == 8:
            return f"rd64({lifter.addr_expr(insn, operand)})"
        return None

    destination_register = mm_register(destination)
    source_register = mm_register(source)
    source_value = source_expression(source)
    if insn.mnemonic == "movq":
        if destination_register is not None and source_value is not None:
            return [f"ac1_mmx_write(c, ac1_mmx, &ac1_mmx_valid, &ac1_mmx_active, "
                    f"{destination_register}u, {source_value});"]
        if (destination.type == X86_OP_MEM and destination.size == 8 and
                source_register is not None):
            return [f"wr64({lifter.addr_expr(insn, destination)}, "
                    f"ac1_mmx_read(c, ac1_mmx, &ac1_mmx_valid, &ac1_mmx_active, "
                    f"{source_register}u));"]
        return None

    if destination_register is None or source_value is None:
        return None
    helper = {
        "pcmpgtw": "ac1_mmx_pcmpgtw",
        "paddw": "ac1_mmx_paddw",
        "psubw": "ac1_mmx_psubw",
        "pand": "ac1_mmx_pand",
    }.get(insn.mnemonic)
    if helper is None:
        return None
    return [f"{helper}(c, ac1_mmx, &ac1_mmx_valid, &ac1_mmx_active, "
            f"{destination_register}u, {source_value});"]


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
        ("0f65c1", "ac1_mmx_pcmpgtw", "ac1_mmx_read"),
        ("0ffdc1", "ac1_mmx_paddw", "ac1_mmx_read"),
        ("0ff9c1", "ac1_mmx_psubw", "ac1_mmx_read"),
        ("0fdbc1", "ac1_mmx_pand", "ac1_mmx_read"),
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
                    "-I", str(ROOT / "src/runtime"),
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
        command = [compiler, "-std=c11", "-O0", "-I", str(ROOT / "src/runtime"),
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


def install_ac1_jump_table_bounds(disassembler) -> None:
    """Apply AC1-local bounds and discover negatively biased switch tables."""
    from types import MethodType
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

    original = disassembler.jump_table_targets

    def bounded(self, insn, limit=256):
        pinned_limit = ac1_pinned_switch_limit(insn)
        if pinned_limit is not None:
            limit = min(limit, pinned_limit)
        else:
            inferred = infer_remapped_switch_limit(
                self.md, self.read_bytes, insn, X86_OP_REG, X86_OP_MEM, X86_OP_IMM)
            if inferred is not None:
                limit = min(limit, inferred[0])
        targets = list(original(insn, limit))
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
        return targets

    disassembler.jump_table_targets = MethodType(bounded, disassembler)


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


def discover_entry_cone(binary: Path, pcrecomp: Path, max_functions: int, max_depth: int,
                        extra_seeds: list[int], code_immediates: bool = True):
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
    instruction_spans = InstructionSpanIndex()
    failures = []
    callback_edges = []
    immediate_pending = collections.defaultdict(list)
    immediate_edges = collections.defaultdict(list)
    immediate_queued = set()
    immediate_rejected = collections.Counter()
    immediate_waves = 0
    while (queue or immediate_pending) and len(results) < max_functions:
        if not queue:
            # First exhaust the direct/ABI-specific cone, so instruction spans
            # can reject continuations and mid-instruction literals. Repeat
            # after every new wave, scanning newly recovered bodies to closure.
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
            if not queue:
                break
        address, depth = queue.popleft()
        if address in results:
            continue
        if address in immediate_queued:
            # Earlier roots in this wave may have recovered the instruction
            # containing this target since the batch was validated.
            if instruction_spans.overlaps(address) or instruction_spans.contains(address):
                reason = ("mid_instruction" if instruction_spans.overlaps(address)
                          else "interior_instruction")
                immediate_rejected[reason] += len(immediate_edges.pop(address, []))
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
        instruction_spans.add_function(results[address])
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
    immediate_summary = {
        "enabled": code_immediates,
        "waves": immediate_waves,
        "accepted_edges": sum(map(len, immediate_edges.values())),
        "rejected_candidates": dict(sorted(immediate_rejected.items())),
        "pending_targets": len(immediate_pending),
    }
    return (info, dis, results, failures, len(queue), entry, iat, callback_edges,
            immediate_summary)


def emit_lift(binary: Path, pcrecomp: Path, functions: list[dict], output_dir: Path,
              functions_per_tu: int, translation_unit_buckets: int = 0,
              emission_cache=None) -> dict:
    sys.path.insert(0, str(pcrecomp / "tools/lift"))
    import pefile
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    from lift32_cpu import Lifter as BaseLifter

    class AC1Lifter(BaseLifter):
        """AC1-specific instruction semantics missing from the donor lifter."""
        def translate(self, insn, labels):
            if insn.mnemonic == "int3":
                return ac1_int3_translation(insn)
            if self.func_start in AC1_MMX_FUNCTION_COUNTS:
                mmx = ac1_mmx_translation(self, insn)
                if mmx is not None:
                    return mmx
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
    pe.close()
    if translation_unit_buckets < 0:
        raise ValueError("translation_unit_buckets must be zero or positive")
    if not translation_unit_buckets and functions_per_tu < 1:
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
    def tu_header_for(addresses: list[int]) -> str:
        if any(address in AC1_MMX_FUNCTION_COUNTS for address in addresses):
            return tu_header + AC1_MMX_LOCAL_HELPERS + "\n"
        return tu_header

    tu_names = []
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
        "translation_unit_buckets": translation_unit_buckets,
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
    validate_ac1_mmx_interval(address, spec, decoded)
    labels = set()
    block_starts = {block["start"] for block in blocks}
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
    if address in AC1_MMX_FUNCTION_COUNTS:
        output.append("    uint64_t ac1_mmx[8] = {0}; uint8_t ac1_mmx_valid = 0u; int ac1_mmx_active = 0;")
    entry_jump = entry_guard_lines(address, [insn.address for insn in insns])
    output.extend(entry_jump)
    for index, insn in enumerate(insns):
        if insn.address in labels or (entry_jump and insn.address == address):
            output.append(f"L_{insn.address:08X}:")
        output.append(f"    AC1_SET_GUEST_EIP(c, 0x{insn.address:08X}u);")
        translated = translate_directional_string_instruction(insn)
        if translated is None:
            translated = lifter.translate(insn, labels)
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
    parser.add_argument("--translation-unit-buckets", type=int,
                        help="fixed address-hash partition count; overrides functions-per-tu grouping")
    parser.add_argument("--no-emission-cache", action="store_true",
                        help="disable the persistent generated-body cache")
    parser.add_argument("--no-code-immediates", action="store_true",
                        help="disable callback discovery from decoded MOV/PUSH imm32 operands")
    args = parser.parse_args()
    if args.selftest:
        config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        python, pcrecomp = resolve_tooling(config, args)
        binary = (args.binary or Path(config["binary"])).resolve()
        if sha256(binary) != config["sha256"]:
            parser.error("installed executable SHA-256 does not match config/ac1_dx9.json")
        run_selftest()
        run_ac1_switch_selftest(binary, pcrecomp)
        run_terminal_external_direct_jump_selftest(binary, pcrecomp)
        return 0
    driver_path = Path(__file__).resolve()
    driver_hash = sha256(driver_path)
    config_bytes = CONFIG_PATH.read_bytes()
    config_hash = sha256_bytes(config_bytes)
    config = json.loads(config_bytes)
    python, pcrecomp = resolve_tooling(config, args)
    binary = (args.binary or Path(config["binary"])).resolve()
    output_dir = args.output_dir.resolve()
    if not output_dir.is_relative_to((ROOT / "work").resolve()):
        parser.error(f"output-dir must be under {ROOT / 'work'}")
    binary_hash = sha256(binary)
    if binary_hash != config["sha256"]:
        parser.error("installed executable SHA-256 does not match config/ac1_dx9.json")
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
        "config": (CONFIG_PATH, config_hash),
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
     immediate_summary) = discover_entry_cone(
        binary, pcrecomp, max_functions, max_depth, extra_seeds,
        code_immediates=not args.no_code_immediates)
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
                                emission_cache)
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
    return 0 if lift_result["lifted"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
