"""Binding generation rejects unsafe/changed oracles before emitting C."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
from lift_ac1 import native_instruction_hash, validate_native_bindings, wrap_native_binding, specialize_direct_calls
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


class NativeBindings(unittest.TestCase):
    def setUp(self):
        self.md = Cs(CS_ARCH_X86, CS_MODE_32); self.md.detail = True
        self.raw = b"\x31\xc0\xc3"
        self.spec = {"address": 0x401000, "blocks": [{"instructions": [[0x401000, 2], [0x401002, 1]]}]}
        self.read = lambda a, n: self.raw[a-0x401000:a-0x401000+n]
        self.entry = {"address": "0x401000", "implementation": "ac1_native_fixture", "plan": "fixture_plan",
                      "instruction_sha256": native_instruction_hash(self.spec, self.read)}

    def validate(self):
        return validate_native_bindings([self.entry], [self.spec], {0x401000}, self.read, self.md)

    def test_binding_preserves_one_public_entry(self):
        self.assertEqual(list(self.validate()), [0x401000])
        body = wrap_native_binding("void L_00401000(CPU *c)\n{ c->eax=0; c->esp+=4; }", 0x401000, self.entry)
        self.assertIn("static void ac1_translated_00401000", body)
        self.assertIn("ac1_native_invoke(c, &ac1_binding_00401000, ac1_translated_00401000)", body)
        direct, _, _ = specialize_direct_calls("dispatch(c, 0x00401000u)", {0x401000})
        self.assertIn("AC1_CALL(c, L_00401000", direct)

    def test_changed_code(self):
        self.raw = b"\x33\xc0\xc3"
        with self.assertRaisesRegex(ValueError, "instruction hash"): self.validate()

    def test_unsafe_oracle(self):
        self.raw = b"\xff\xd0\xc3" # indirect call eax
        self.entry['instruction_sha256'] = native_instruction_hash(self.spec, self.read)
        with self.assertRaisesRegex(ValueError, "not a pure leaf"): self.validate()

    def test_external_transfer(self):
        self.raw = b"\xeb\x7f\xc3"
        self.entry['instruction_sha256'] = native_instruction_hash(self.spec, self.read)
        with self.assertRaisesRegex(ValueError, "external/indirect transfer"): self.validate()

    def test_identifier_and_missing_oracle(self):
        self.entry['plan'] = 'bad();'
        with self.assertRaisesRegex(ValueError, "invalid C identifier"): self.validate()
        with self.assertRaisesRegex(ValueError, "no translated oracle"):
            validate_native_bindings([self.entry], [self.spec], set(), self.read, self.md)


if __name__ == '__main__': unittest.main()
