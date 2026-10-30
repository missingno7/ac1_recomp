"""Experimental native x87 lowering: functional equivalence with the exact lowering."""
import shutil
import subprocess
import unittest

import test_lift_direct_calls as direct

lift, ROOT = direct.lift, direct.ROOT


class NativeX87Tests(unittest.TestCase):
    def lifted(self, raw):
        from capstone.x86 import X86_OP_IMM, X86_OP_MEM
        from lift32_cpu import Lifter
        read = lambda va, n: raw[va-0x401000:va-0x401000+n]
        lifter = Lifter('synthetic x87 fixture', 0x1000, read)
        instructions = list(lifter.md.disasm(raw, 0x401000))
        spec = {'address': 0x401000, 'size': len(raw), 'blocks': [{'start': 0x401000, 'end': 0x401000+len(raw),
                'instructions': [[i.address, i.size] for i in instructions]}]}
        return lift.flag_liveness_function(lift.lift_exact_function(lifter, spec, read, X86_OP_IMM, X86_OP_MEM))

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_block_local_stack_matches_exact_lowering(self):
        # fld [esp+4]; fld st0; fmul st0,st1; fld [esp+8]; faddp; fxch; fsubrp; fistp [esp+0xc];
        # fld1; fcomp [esp+4]; fnstsw ax; test ah,5; jp over fldz (unbalanced join); fstp [esp+0x10]; ret
        raw = bytes.fromhex('d9442404d9c0d8c9d9442408dec1d9c9dee9db5c240cd9e8d85c2404dfe0f6c4057a02d9eed95c2410c3')
        exact = self.lifted(raw)
        native = lift.native_x87_function(exact)
        for gone in ('fpush(', 'fpop(', 'fst(c', 'ac1_x87_check', 'fpu_ip=', 'ac1_x87_set_tag'):
            self.assertNotIn(gone, native)
        self.assertIn('c->fpu_top = (int)((_ac1_x87_top - ', native)
        source = '#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
        source += exact.replace('void L_00401000(', 'void exact_L_00401000(', 1) + '\n'
        source += native.replace('void L_00401000(', 'void native_L_00401000(', 1) + '\n'
        work = ROOT / 'work/x87-native-tests'
        work.mkdir(parents=True, exist_ok=True)
        path = work / 'fixture.c'
        path.write_text(source)
        exe = path.with_suffix('.exe')
        subprocess.run(['gcc', '-O2', '-std=c11', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off',
                        '-I', str(lift.prepare_test_runtime()), str(path), str(ROOT / 'tests/x87_native_semantics.c'),
                        '-lm', '-o', str(exe)], check=True, capture_output=True, timeout=60)
        run = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=30)
        self.assertIn('4096 native-x87 functional CPU and memory cases passed', run.stdout)

    def test_stack_observers_keep_the_exact_lowering(self):
        body = '\n'.join(['void L_00401000(CPU *c)', '{', '    fpush(c, 1.0);',
                          '    ac1_x87_save(c, c->esp);', '    c->esp += 4; return;', '}'])
        self.assertEqual(lift.native_x87_function(body), body)


if __name__ == '__main__':
    unittest.main()
