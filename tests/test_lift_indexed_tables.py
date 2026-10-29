"""Decoded initialized-data table discovery, without running the game."""
import collections
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from test_lift_code_immediates import CodeImmediateTests, ROOT, lift


class IndexedTableTests(unittest.TestCase):
    def setUp(self):
        self.fixture = CodeImmediateTests()
        self.fixture.setUp()
        self.md = self.fixture.md

    def function(self, address, code):
        return self.fixture.function(address, code)

    def section(self, name=".data", size=0x300, flags=0xC0000040):
        return SimpleNamespace(name=name, virtual_address=0x2000, raw_size=size,
                               virtual_size=0x1000, characteristics=flags)

    def test_only_initialized_allowed_non_executable_bytes(self):
        info = SimpleNamespace(image_base=0, sections=[
            self.section(), self.section("_RDATA"), self.section(".data1"),
            self.section(".rdata"), self.section(".idata"),
            self.section(flags=0xC0000080), self.section(flags=0xE0000040),
            self.section(size=0)])
        ranges = lift.initialized_table_ranges(info)
        self.assertEqual([name for _, _, name in ranges], [".data", "_RDATA", ".data1", ".rdata"])
        self.assertTrue(all(start == 0x2000 and end == 0x2300 for start, end, _ in ranges))

    def test_any_scaled_memory_read_but_no_lea_or_write_only_operand(self):
        fn = self.function(0x1000,
                           "8b048500200000 8b948500200000 ff348500200000 "
                           "ff148500200000 ff248500200000 833c850020000001 "
                           "89048500200000 8d048500200000 8b044500200000 "
                           "648b048500200000 c3")
        refs, count = lift.indexed_table_references(fn, [(0x2000, 0x2300, ".data")], self.md)
        self.assertEqual(len(refs), 6)
        self.assertEqual([ref["instruction"].split()[0] for ref in refs],
                         ["mov", "mov", "push", "call", "jmp", "cmp"])
        self.assertTrue(all(ref["table"] == 0x2000 and ref["kind"] == "indexed" for ref in refs))
        self.assertFalse(count)
        self.assertFalse(lift.indexed_table_references(fn, [], self.md)[0])

    def test_absolute_slots_and_prescaled_count_require_preserved_call_register(self):
        for transfer in ("ffd0", "ffe0"):
            fn = self.function(0x1000, "a100200000 51 90 " + transfer)
            refs, _ = lift.indexed_table_references(fn, [(0x2000, 0x2300, ".data")], self.md)
            self.assertEqual(len(refs), 1)
            self.assertEqual(refs[0]["kind"], "absolute")
        for clobber in ("b001", "b801000000", "f7e1", "ffd1", "7500"):
            fn = self.function(0x1000, "a100200000 " + clobber + " ffd0")
            self.assertFalse(lift.indexed_table_references(
                fn, [(0x2000, 0x2300, ".data")], self.md)[0], clobber)
        fn = self.function(0x1000, "8b8100200000 90 ffd0")
        refs, count = lift.indexed_table_references(fn, [(0x2000, 0x2300, ".data")], self.md)
        self.assertFalse(refs)  # no attempt to infer the scale of ECX
        self.assertEqual(count, {0x1000})
        # The same bytes split across blocks cannot establish the proof.
        fn = self.function(0x1000, "a100200000 ffd0")
        last = fn.blocks[0x1000].instructions.pop()
        fn.blocks[last.address] = SimpleNamespace(instructions=[last])
        self.assertFalse(lift.indexed_table_references(fn, [(0x2000, 0x2300, ".data")], self.md)[0])

    def environment(self, values=None, boundary=True, switch=False):
        functions = {
            0x1000: self.function(0x1000, "8b048500200000 c3"),
            0x1100: self.function(0x1100, "8b048500210000 c3"),
            0x1200: self.function(0x1200, "c3"),
            0x1300: self.function(0x1300, "c3"),
        }
        memory = {}
        for address, entries in (values or {0x2000: [0x1100, 0x1200, 0], 0x2100: [0x1300, 0]}).items():
            for index, target in enumerate(entries):
                memory.update({address + index * 4 + offset: byte
                               for offset, byte in enumerate(struct.pack("<I", target))})
        reads = collections.Counter()
        md = self.md

        class Disassembler:
            def __init__(self, *args):
                self.md = md
                self.ac1_switch_table_ranges = {(0x2000, 0x2008)} if switch else set()

            def is_code_address(self, address):
                return 0x1000 <= address < 0x1800

            def probes_as_function_body(self, address):
                return address in functions

            def disassemble_function(self, address, iat):
                return functions.get(address)

            def read_bytes(self, address, size):
                reads[address] += 1
                if address >= 0x2000:
                    return bytes(memory.get(address + index, 0) for index in range(size))
                return (b"\xcc" if boundary else b"\0") * size

        info = SimpleNamespace(image_base=0, entry_point_rva=0x1000,
                               code_start=0x1000, code_end=0x1800, sections=[self.section()])
        return info, Disassembler, reads

    def discover(self, enabled=True, **kwargs):
        info, disassembler, reads = self.environment(**kwargs)
        modules = {"pe_analyze": SimpleNamespace(analyze_pe=lambda _: info, build_iat_map=lambda _: {}),
                   "disasm32": SimpleNamespace(Disassembler=disassembler)}
        with (patch.dict(sys.modules, modules), patch.object(lift, "install_ac1_jump_table_bounds"),
              patch.object(Path, "read_bytes", return_value=b""), patch.object(sys, "path", sys.path[:])):
            result = lift.discover_entry_cone(Path("fixture"), ROOT, 100, 32, [],
                                             indexed_tables=enabled)
        return result, reads

    def test_recursive_closure_and_provenance(self):
        result, reads = self.discover()
        self.assertEqual(set(result[2]), {0x1000, 0x1100, 0x1200, 0x1300})
        self.assertFalse(result[3])
        self.assertEqual(result[4], 0)
        edges = result[7]
        self.assertEqual({(edge["table"], edge["slot"], edge["target"]) for edge in edges},
                         {(0x2000, 0x2000, 0x1100), (0x2000, 0x2004, 0x1200),
                          (0x2100, 0x2100, 0x1300)})
        self.assertTrue(all(edge["role"] == "indexed_table" for edge in edges))
        self.assertEqual(result[9]["table_count"], 2)
        self.assertEqual(result[9]["rejected_candidates"], {"non_executable": 2})
        self.assertNotIn(0x1FFC, reads)  # forward from the displacement itself

    def test_disable_and_bounded_switch_exclusion(self):
        for kwargs in ({"enabled": False}, {"switch": True}):
            result, reads = self.discover(**kwargs)
            self.assertEqual(set(result[2]), {0x1000})
            self.assertFalse(result[7])
            self.assertNotIn(0x2000, reads)
        result, _ = self.discover(switch=True)
        self.assertEqual(result[9]["rejected_candidates"], {"bounded_switch": 1})

    def test_stop_at_first_bad_entry_without_skipping_it(self):
        for bad, boundary, reason in ((0, True, "non_executable"),
                                      (0x1400, True, "invalid_entry"),
                                      (0x1100, False, "not_function_boundary")):
            result, reads = self.discover(values={0x2000: [bad, 0x1200]}, boundary=boundary)
            self.assertEqual(set(result[2]), {0x1000})
            self.assertEqual(result[9]["rejected_candidates"], {reason: 1})
            self.assertNotIn(0x2004, reads)

    def test_instruction_overlap_and_recovered_entry_exception(self):
        for target, reason in ((0x1001, "mid_instruction"), (0x1007, "interior_instruction")):
            result, reads = self.discover(values={0x2000: [target, 0x1200]})
            self.assertEqual(result[9]["rejected_candidates"], {reason: 1})
            self.assertNotIn(0x2004, reads)
        result, _ = self.discover(values={0x2000: [0x1000, 0]}, boundary=False)
        self.assertEqual(set(result[2]), {0x1000})
        self.assertEqual([edge["target"] for edge in result[7]], [0x1000])

    def test_hard_cap_section_end_and_single_entry_limit(self):
        info, disassembler, _ = self.environment(values={0x2000: [0x1100, 0x1200, 0x1300]})
        for kind, size, cap, expected, reason in (("indexed", 0x300, 2, 2, "entry_cap"),
                                                  ("indexed", 6, 1024, 1, "section_end"),
                                                  ("absolute", 0x300, 1024, 1, "single_entry")):
            info.sections = [self.section(size=size)]
            discovery = lift.IndexedTableDiscovery(info, disassembler(), entry_cap=cap)
            discovery.references[(0x2000, kind)][(0x1000, 0x1000)] = {
                "caller": 0x1000, "callsite": 0x1000, "table": 0x2000, "depth": 1,
                "kind": kind, "role": "indexed_table"}
            discovery.sweep({}, lift.InstructionSpanIndex(), {}, collections.deque(), set())
            self.assertEqual(discovery.tables[0]["entry_count"], expected)
            self.assertEqual(discovery.tables[0]["stop_reason"], reason)


if __name__ == "__main__":
    unittest.main()
