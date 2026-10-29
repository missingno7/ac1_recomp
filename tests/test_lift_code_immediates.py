"""Focused decoded-immediate discovery contracts; no game execution."""
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("lift_ac1", ROOT / "tools/lift_ac1.py")
lift = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lift)


class CodeImmediateTests(unittest.TestCase):
    def setUp(self):
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True

    def function(self, address, code, calls=()):
        instructions = list(self.md.disasm(bytes.fromhex(code), address))
        end = instructions[-1].address + instructions[-1].size
        return SimpleNamespace(
            blocks={address: SimpleNamespace(start=address, end=end, successors=[],
                                             is_exit=True, instructions=instructions)},
            end=end, size=end-address, entry_kind="start", is_thunk=False,
            calls_to=set(calls), jump_targets=set())

    def test_only_pointer_sized_decoded_operands(self):
        fn = self.function(0x1000,
                           "b823100000 bd24100000 c744241c25100000 6826100000 "
                           "66b82710 66c7012810 66682910 6a10 "
                           "b02a 8b052b100000 052c100000 c3")
        edges = lift.constant_code_immediate_edges(fn)
        self.assertEqual([edge["target"] for edge in edges], [0x1023, 0x1024, 0x1025, 0x1026])
        self.assertTrue(all(not edge["manual_call"] for edge in edges))
        # Bytes not present in a recovered block are never scanned.
        fn.blocks[0x1000].instructions = fn.blocks[0x1000].instructions[:1]
        self.assertEqual(len(lift.constant_code_immediate_edges(fn)), 1)

    def test_manual_call_across_block_boundary_and_indirect_jump(self):
        for jump in ("eb00", "ffe0"):
            fn = self.function(0x1000, "6820100000 " + jump)
            ins = fn.blocks[0x1000].instructions.pop()
            fn.blocks[ins.address] = SimpleNamespace(instructions=[ins])
            self.assertTrue(lift.constant_code_immediate_edges(fn)[0]["manual_call"])
        fn = self.function(0x1000, "6820100000 90 eb00")
        self.assertFalse(lift.constant_code_immediate_edges(fn)[0]["manual_call"])

    def test_function_boundary_suffixes(self):
        target = 0x1100
        for suffix in ("cc", "90", "c3", "c21000", "e978563412", "ebfe"):
            raw = bytes.fromhex(suffix)
            dis = SimpleNamespace(
                is_code_address=lambda address: address >= target - len(raw),
                read_bytes=lambda address, size: raw[address - (target - len(raw)):][:size])
            with self.subTest(suffix=suffix):
                self.assertTrue(lift.code_immediate_has_function_boundary(target, dis, {}))
        for raw in (b"\0" * 5, bytes.fromhex("e878563412"), bytes.fromhex("c210"),
                    bytes.fromhex("e9785634"), bytes.fromhex("eb")):
            dis = SimpleNamespace(is_code_address=lambda address: True,
                                  read_bytes=lambda address, size: raw[-size:])
            with self.subTest(rejected=raw.hex()):
                self.assertFalse(lift.code_immediate_has_function_boundary(target, dis, {}))
                self.assertTrue(lift.code_immediate_has_function_boundary(target, dis, {target: {}}))
        # Section-start entries have no predecessor unless independently recovered.
        dis = SimpleNamespace(is_code_address=lambda address: address >= target,
                              read_bytes=lambda *_: self.fail("read outside code section"))
        self.assertFalse(lift.code_immediate_has_function_boundary(target, dis, {}))
        self.assertTrue(lift.code_immediate_has_function_boundary(target, dis, {target: {}}))

    def discover(self, enabled=True, max_depth=32, max_functions=100):
        # A discovers B; B discovers unaligned C. Direct root D contains a
        # rejected instruction boundary and midpoint. B also points into C's
        # second instruction, testing revalidation within the same wave.
        functions = {
            0x1000: self.function(0x1000, "b800110000 b801120000 b802120000 "
                                 "b800190000 b800180000 b800300000 6800130000 ffe0", [0x1200]),
            0x1100: self.function(0x1100, "b803140000 b808140000 c3"),
            0x1200: self.function(0x1200, "90 b878563412 c3"),
            0x1403: self.function(0x1403, "b800110000 c3"),
            # Valid independently, but an interior instruction once C is known.
            0x1408: self.function(0x1408, "c3"),
            0x1800: self.function(0x1800, "c3"),  # decodable numeric literal, no boundary
        }
        class Disassembler:
            def __init__(self, *args):
                pass

            def is_code_address(self, target):
                return 0x1000 <= target < 0x2000

            def probes_as_function_body(self, target):
                return target in functions

            def read_bytes(self, address, size):
                return (b"\0" if address < 0x1800 <= address + size else b"\xcc") * size

            def disassemble_function(self, target, iat):
                return functions.get(target)

        info = SimpleNamespace(image_base=0x1000, entry_point_rva=0,
                               code_start=0x1000, code_end=0x2000, sections=[])
        modules = {"pe_analyze": SimpleNamespace(analyze_pe=lambda _: info,
                                                 build_iat_map=lambda _: {}),
                   "disasm32": SimpleNamespace(Disassembler=Disassembler)}
        with (patch.dict(sys.modules, modules), patch.object(lift, "install_ac1_jump_table_bounds"),
              patch.object(Path, "read_bytes", return_value=b""), patch.object(sys, "path", sys.path[:])):
            return lift.discover_entry_cone(Path("fixture"), ROOT, max_functions,
                                            max_depth, [], enabled)

    def test_closure_provenance_and_rejections(self):
        _, _, catalog, failures, remaining, _, _, edges, summary = self.discover()
        self.assertEqual(set(catalog), {0x1000, 0x1100, 0x1200, 0x1403})
        self.assertFalse(failures)
        self.assertEqual(remaining, 0)
        self.assertEqual(summary["pending_targets"], 0)
        self.assertEqual({(e["caller"], e["target"], e["role"]) for e in edges},
                         {(0x1000, 0x1100, "code_immediate"),
                          (0x1100, 0x1403, "code_immediate"),
                          (0x1403, 0x1100, "code_immediate")})
        self.assertEqual(summary["rejected_candidates"], {
            "interior_instruction": 2, "mid_instruction": 1,
            "invalid_entry": 1, "manual_call": 1, "non_executable": 2,
            "not_function_boundary": 1})

    def test_disable_restores_direct_cone(self):
        result = self.discover(enabled=False)
        self.assertEqual(set(result[2]), {0x1000, 0x1200})
        self.assertFalse(result[7])
        self.assertFalse(result[8]["enabled"])

    def test_limits_are_observable(self):
        result = self.discover(max_depth=1)
        self.assertNotIn(0x1403, result[2])
        self.assertEqual(result[8]["rejected_candidates"]["depth_limit"], 2)
        result = self.discover(max_functions=2)
        self.assertGreater(result[8]["pending_targets"], 0)


if __name__ == "__main__":
    unittest.main()
