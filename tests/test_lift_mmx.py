"""Shared CPU MMX ABI, state instructions and real-function native/vector oracles."""
import sys
import unittest

import test_lift_code_immediates as common
import run_mmx_general

lift = common.lift
sys.path.insert(0, str(lift.DUKE_DEFAULT / "tools/lift"))
from lift32_cpu import Lifter
from capstone.x86 import X86_OP_MEM, X86_OP_IMM


class GeneralMMXTests(unittest.TestCase):
    def setUp(self):
        self.lifter = Lifter("unit-fixture",0x10000,None)

    def decode(self, raw):
        return next(self.lifter.md.disasm(bytes.fromhex(raw),0x00401100,count=1))

    def test_native_instructions_state_audio_and_bink(self):
        report = run_mmx_general.run()
        self.assertIn("general MMX/state oracle passed",report["output"])

    def test_xmm_forms_remain_sse(self):
        for raw in ("660ffec1", "660fdbc1", "f30f7e00", "660f6e00"):
            insn=self.decode(raw)
            self.assertIsNone(lift.ac1_mmx_translation(self.lifter,insn),insn.mnemonic)

    def test_unknown_mmx_reports_the_instruction(self):
        insn=self.decode("0f0fc196")  # AMD 3DNow PFRCP, outside the observed MMX set
        with self.assertRaisesRegex(NotImplementedError,r"00401100.*pfrcp"):
            lift.ac1_mmx_translation(self.lifter,insn)

    def test_sixteen_bit_environment_is_an_explicit_diagnostic(self):
        insn=self.decode("66d930")
        text=" ".join(lift.ac1_state_translation(self.lifter,insn))
        self.assertIn("16-bit x87 environment",text)
        self.assertIn("0x00401100",text)

    def test_shared_emitter_uses_cpu_registers_outside_havok(self):
        raw=bytes.fromhex("0f6fc10fdbc20f77c3")
        instructions=list(self.lifter.md.disasm(raw,0x00401100))
        spec={"address":0x00401100,"size":len(raw),"blocks":[{
            "start":0x00401100,"end":0x00401100+len(raw),
            "instructions":[(ins.address,ins.size) for ins in instructions]}]}
        body=lift.lift_exact_function(self.lifter,spec,
            lambda va,n:raw[va-0x00401100:va-0x00401100+n],X86_OP_IMM,X86_OP_MEM)
        self.assertIn("ac1_mmx_read(c, 1u)",body)
        self.assertIn("ac1_mmx_write(c, 0u",body)
        self.assertNotIn("ac1_mmx[8]",body)
        self.assertNotIn("RECOMP_TODO",body)

    def test_register_fstp_keeps_straight_line_fallthrough(self):
        # fstp st(1); fstp st(0); ret -- regression: the x87 tag prefix once
        # reused the loop index and emitted dispatch() to the next instruction.
        raw=bytes.fromhex("ddd9ddd8c3")
        instructions=list(self.lifter.md.disasm(raw,0x00401100))
        spec={"address":0x00401100,"size":len(raw),"blocks":[{
            "start":0x00401100,"end":0x00401100+len(raw),
            "instructions":[(ins.address,ins.size) for ins in instructions]}]}
        body=lift.lift_exact_function(self.lifter,spec,
            lambda va,n:raw[va-0x00401100:va-0x00401100+n],X86_OP_IMM,X86_OP_MEM)
        self.assertIn("ac1_x87_set_tag(c,(unsigned)(c->fpu_top+1)&7u,0);",body)
        self.assertNotIn("dispatch(c, 0x",body)


if __name__ == "__main__":
    unittest.main()
