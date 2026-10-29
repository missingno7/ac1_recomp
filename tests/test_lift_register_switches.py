"""Guarded register-loaded switches: CFG proof, local emission and execution."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import test_lift_code_immediates as common

lift, ROOT = common.lift, common.ROOT
sys.path[:0] = [str(lift.DUKE_DEFAULT / "tools/disasm"), str(lift.DUKE_DEFAULT / "tools/pe"),
                str(lift.DUKE_DEFAULT / "tools/lift")]
from disasm32 import Disassembler, BasicBlock
from pe_analyze import Section
from lift32_cpu import Lifter
from capstone.x86 import X86_OP_IMM, X86_OP_MEM


class RegisterSwitchTests(unittest.TestCase):
    def fixture(self, gap=b"", remap=False, source_ecx=False, split=False, enabled=True):
        base, table = 0x00401000, 0x00402000
        data = bytearray(b"\xcc" * 0x1100)
        prefix = bytes.fromhex("8bc1 8b542404 83fa02 7605 33c0 c20800")
        if remap:
            prefix = bytes.fromhex("8bc1 8b4c2404 83f904 7605 33c0 c20800" if source_ecx
                                   else "8bc1 8b542404 83fa04 7605 33c0 c20800")
            prefix += bytes.fromhex("0fb691" if source_ecx else "0fb692") + struct.pack("<I", table + 12)
        load_address = base + len(prefix)
        code = prefix + bytes.fromhex("8b1495") + struct.pack("<I", table) + gap
        jump_address = base + len(code)
        if split:
            code += b"\xe9" + struct.pack("<i", (base + 0x40) - (base + len(code) + 5))
            jump_address = base + 0x40
            data[0x40:0x42] = bytes.fromhex("ffe2")
        else:
            code += bytes.fromhex("ffe2")
        data[:len(code)] = code
        targets = [base + 0x100 + 0x20 * i for i in range(4)]
        for index, target in enumerate(targets):
            at = target - base
            data[at:at + 8] = b"\xb8" + struct.pack("<I", 11 * (index + 1)) + bytes.fromhex("c20800")
        data[0x1000:0x1010] = struct.pack("<4I", *targets)
        if remap:
            data[0x100C:0x1011] = bytes([2, 0, 1, 0, 2])
        sections = [Section(".text", 0x1000, 0x800, 0, 0x800, 0x60000020),
                    Section(".rdata", 0x2000, 0x100, 0x1000, 0x100, 0x40000040)]
        dis = Disassembler(bytes(data), 0x00400000, sections)
        lift.install_ac1_jump_table_bounds(dis, register_switches=enabled)
        fn = dis.disassemble_function(base, {})
        return dis, fn, data, targets, load_address, jump_address

    def spec(self, fn):
        return {"address": fn.address, "size": fn.size,
                "register_switches": getattr(fn, "register_switches", []),
                "blocks": [{"start": b.start, "end": b.end,
                            "instructions": [[ins.address, ins.size] for ins in b.instructions]}
                           for b in fn.blocks.values()]}

    def body(self, dis, fn):
        lifter = Lifter("fixture", 0x3000, dis.read_bytes)
        lifter.image_lo, lifter.image_hi = 0x00400000, 0x00403000
        return lift.lift_exact_function(lifter, self.spec(fn), dis.read_bytes, X86_OP_IMM, X86_OP_MEM)

    def test_guarded_cases_remain_internal_and_stop_at_proven_bound(self):
        dis, fn, _, targets, load, jump = self.fixture()
        self.assertEqual(len(fn.register_switches), 1)
        self.assertEqual(fn.register_switches[0]["targets"], targets[:3])
        self.assertTrue(set(targets[:3]) <= fn.blocks.keys())
        self.assertNotIn(targets[3], fn.blocks)
        self.assertFalse(fn.calls_to)
        self.assertEqual(fn.blocks[load].successors, targets[:3])
        text = self.body(dis, fn)
        for target in targets[:3]:
            self.assertIn(f"if (_jt == GVA(0x{target:08X})) goto L_{target:08X};", text)
            self.assertIn(f"L_{target:08X}:", text)
        self.assertIn("uint32_t _jt = c->edx;", text)
        self.assertIn("dispatch_jmp(c, _jt); return;", text)
        self.assertNotIn("/* indirect jmp */", text)
        self.assertIn((0x402000, 0x40200C), dis.ac1_switch_table_ranges)

    def test_indexed_discovery_must_not_claim_switch(self):
        dis, fn, _, _, _, _ = self.fixture()
        info = type("Info", (), {"image_base": 0x400000, "sections": dis.sections,
                                  "code_start": 0x401000, "code_end": 0x401800})()
        tables = lift.IndexedTableDiscovery(info, dis)
        tables.collect(fn, fn.address, 0, 32)
        import collections
        tables.sweep({}, lift.InstructionSpanIndex(), {}, collections.deque(), set())
        self.assertFalse(tables.edges)
        self.assertEqual(tables.tables[0]["stop_reason"], "bounded_switch")

    def test_unrelated_gap_and_single_predecessor_chain(self):
        for gap, split in ((bytes.fromhex("90 43 8bf6"), False), (b"", True)):
            dis, fn, _, targets, _, _ = self.fixture(gap=gap, split=split)
            self.assertEqual(fn.register_switches[0]["targets"], targets[:3])
            self.assertIn("uint32_t _jt = c->edx;", self.body(dis, fn))

    def test_partial_implicit_and_full_clobbers_remain_indirect(self):
        for gap in (bytes.fromhex("b201"), bytes.fromhex("ba00104000"),
                    bytes.fromhex("f7e1"), bytes.fromhex("87d1"), bytes.fromhex("e800000000")):
            dis, fn, _, targets, _, _ = self.fixture(gap=gap)
            self.assertFalse(fn.register_switches, gap.hex())
            self.assertFalse(set(targets[:3]) & fn.blocks.keys())
            self.assertIn("/* indirect jmp */", self.body(dis, fn))

    def test_separate_index_clobber_is_rejected(self):
        dis, fn, data, _, load, jump = self.fixture(enabled=False)
        # MOV EAX,[EDX*4+table]; INC EDX; JMP EAX. The last definition of
        # EAX is a table load, but the index is not preserved to the jump.
        at = load - 0x401000
        data[at:at + 10] = bytes.fromhex("8b049500204000 42 ffe0")
        dis = Disassembler(bytes(data), dis.image_base, dis.sections)
        lift.install_ac1_jump_table_bounds(dis)
        self.assertFalse(dis.disassemble_function(fn.address, {}).register_switches)

    def test_ambiguous_predecessor_is_not_a_last_definition_proof(self):
        dis, fn, _, _, _, jump = self.fixture(split=True, enabled=False)
        extra = 0x401070
        fn.blocks[extra] = BasicBlock(extra, extra, [], [jump])
        self.assertFalse(lift.register_switch_sites(fn, dis.md, dis.read_bytes, dis.is_code_address))

    def test_two_level_byte_remap_including_different_source_register(self):
        for source_ecx in (False, True):
            dis, fn, _, targets, _, _ = self.fixture(remap=True, source_ecx=source_ecx)
            site = fn.register_switches[0]
            self.assertEqual(site["count"], 3)
            self.assertEqual(site["input_count"], 5)
            self.assertEqual(site["targets"], targets[:3])
            self.assertNotIn(targets[3], fn.blocks)
            self.assertIn("uint32_t _jt = c->edx;", self.body(dis, fn))

    def test_missing_guard_and_overwritten_guard_flags_are_rejected(self):
        for change in (bytes.fromhex("90 90 90"), bytes.fromhex("83fb02")):
            dis, fn, data, _, _, _ = self.fixture(enabled=False)
            data[6:9] = change  # remove CMP EDX or compare a different index
            dis = Disassembler(bytes(data), dis.image_base, dis.sections)
            lift.install_ac1_jump_table_bounds(dis)
            self.assertFalse(dis.disassemble_function(fn.address, {}).register_switches)
        dis, fn, data, _, _, _ = self.fixture(enabled=False)
        old = bytes(data[:25])
        data[:26] = old[:9] + b"\x43" + old[9:]  # INC EBX replaces the CMP flags
        dis = Disassembler(bytes(data), dis.image_base, dis.sections)
        lift.install_ac1_jump_table_bounds(dis)
        self.assertFalse(dis.disassemble_function(fn.address, {}).register_switches)

    def test_fallthrough_ja_and_jae_guards(self):
        for opcode, count in ((0x77, 3), (0x73, 2)):
            dis, fn, data, targets, _, _ = self.fixture(enabled=False)
            code = (bytes.fromhex("8bc18b54240483fa02") + bytes([opcode, 9]) +
                    bytes.fromhex("8b149500204000 ffe2 33c0c20800"))
            data[:len(code)] = code
            dis = Disassembler(bytes(data), dis.image_base, dis.sections)
            lift.install_ac1_jump_table_bounds(dis)
            fn = dis.disassemble_function(fn.address, {})
            self.assertEqual(fn.register_switches[0]["targets"], targets[:count])

    def test_disabled_path_retains_indirect_dispatch(self):
        dis, fn, _, targets, _, _ = self.fixture(enabled=False)
        self.assertFalse(getattr(fn, "register_switches", []))
        self.assertFalse(set(targets[:3]) & fn.blocks.keys())
        self.assertIn("/* indirect jmp */", self.body(dis, fn))

    def test_compile_execute_local_cases_default_ret8_and_unknown_fallback(self):
        compiler = shutil.which("gcc")
        if not compiler:
            self.skipTest("gcc unavailable")
        # Change the table after loading EDX. Re-reading memory at JMP would
        # dispatch DEADBEEF rather than the loaded internal case address.
        gap = bytes.fromhex("c70500204000efbeadde")
        dis, fn, _, _, _, jump = self.fixture(gap=gap)
        with tempfile.TemporaryDirectory(prefix="register_switch_", dir=ROOT / "work") as tmp:
            temp = Path(tmp)
            generated = temp / "switch.c"
            generated.write_text('#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n' + self.body(dis, fn))
            executable = temp / "switch.exe"
            command = [compiler, "-std=c11", "-O0", f"-DFIXTURE_JUMP=0x{jump:08X}u",
                       "-I", str(ROOT / "src/runtime"), str(generated),
                       str(ROOT / "tests/register_switch_semantics.c"),
                       str(ROOT / "src/runtime/guest_memory.c"), str(ROOT / "src/runtime/guest_clock.c"),
                       "-lm", "-o", str(executable)]
            result = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("register switch semantics passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
