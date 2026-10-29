"""Direct main-image transfers preserve dispatch accounting and guest ABI."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest

import test_lift_code_immediates as common

lift, ROOT = common.lift, common.ROOT
sys.path.insert(0, str(lift.DUKE_DEFAULT / "tools/lift"))
from lift32_cpu import Lifter
from capstone.x86 import X86_OP_IMM, X86_OP_MEM


class DirectCallsTests(unittest.TestCase):
    def test_only_successful_main_entries_are_specialized(self):
        source = ("dispatch(c, 0x00401100u); dispatch(c, 0xF1000000u); "
                  "dispatch(c, 0x18001000u); dispatch(c, _ct); "
                  "dispatch_jmp(c, 0x00401100u); dispatch(c, 0x00401375u);")
        body, callees, count = lift.specialize_direct_calls(source, {0x401100, 0x401375})
        self.assertEqual(count, 1)
        self.assertEqual(callees, {0x401100})
        self.assertEqual(body, source.replace("dispatch(c, 0x00401100u)",
                                             "AC1_CALL(c, L_00401100, 0x00401100u)"))
        self.assertEqual(lift.specialize_direct_calls(source, set()), (source, set(), 0))

    @unittest.skipUnless(shutil.which("gcc"), "native compiler required")
    def test_generated_calls_and_tail_transfers(self):
        work = ROOT / "work/direct_calls_test"
        work.mkdir(parents=True, exist_ok=True)
        raw = bytearray(b"\xcc" * 0x300)
        # push ebx; call B; mov edx,eax; pop ebx; jmp C. B adds 3; C adds 1.
        raw[:14] = (b"\x53\xe8" + struct.pack("<i", 0x401100-0x401006) +
                    b"\x89\xc2\x5b\xe9" + struct.pack("<i", 0x401200-0x40100E))
        raw[0x100:0x104] = bytes.fromhex("83c003c3")
        raw[0x200:0x202] = bytes.fromhex("40c3")
        read = lambda va, size: bytes(raw[va-0x401000:va-0x401000+size])
        lifter = Lifter("fixture", 0x1000, read)
        bodies = []
        for va, size in ((0x401000, 14), (0x401100, 4), (0x401200, 2)):
            instructions = list(lifter.md.disasm(read(va, size), va))
            spec = {"address": va, "size": size, "blocks": [{"start": va, "end": va+size,
                    "instructions": [[i.address, i.size] for i in instructions]}]}
            body = lift.lift_exact_function(lifter, spec, read, X86_OP_IMM, X86_OP_MEM)
            bodies.append(lift.specialize_direct_calls(body, {0x401100, 0x401200})[0])
        source = work / "generated.c"
        source.write_text('#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
                          'void L_00401100(CPU *);void L_00401200(CPU *);\n' + "\n".join(bodies))
        for direct_memory in (False, True):
            for direct_calls in (False, True):
                exe = work / f"check-{direct_memory}-{direct_calls}.exe"
                flags = (["-DAC1_DIRECT_MEMORY"] if direct_memory else []) + (
                    [] if direct_calls else ["-DAC1_NO_DIRECT_CALLS"])
                subprocess.run(["gcc", "-O2", "-fwrapv", "-fno-strict-aliasing", *flags,
                    "-I", str(lift.prepare_test_runtime()), str(source),
                    str(ROOT / "tests/direct_calls_semantics.c"), "-o", str(exe)], check=True,
                    capture_output=True, timeout=60)
                result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=15)
                self.assertIn("2000 nested/tail cases", result.stdout)
                for arg, code in (("budget", 81), ("depth", 82)):
                    stopped = subprocess.run([str(exe), arg], capture_output=True, timeout=15)
                    self.assertEqual(stopped.returncode, code, stopped.stderr)


if __name__ == "__main__":
    unittest.main()
