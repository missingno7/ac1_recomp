"""Experimental native x87 lowering: functional equivalence with the exact lowering."""
import shutil
import subprocess
import sys
import unittest

import test_lift_direct_calls as direct

lift, ROOT = direct.lift, direct.ROOT
sys.path.insert(0, str(ROOT / 'tools'))


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

    def lifted_at(self, raw, base):
        from capstone.x86 import X86_OP_IMM, X86_OP_MEM
        from lift32_cpu import Lifter
        read = lambda va, n: raw[va-base:va-base+n]
        lifter = Lifter('synthetic x87 fixture', 0x1000, read)
        instructions = list(lifter.md.disasm(raw, base))
        spec = {'address': base, 'size': len(raw), 'blocks': [{'start': base, 'end': base+len(raw),
                'instructions': [[i.address, i.size] for i in instructions]}]}
        body = lift.flag_liveness_function(lift.lift_exact_function(lifter, spec, read, X86_OP_IMM, X86_OP_MEM))
        return body, instructions

    def region(self, instructions, deltas=None):
        import function_facts as facts
        r = facts.x87_region(instructions[0].address, facts.linear_facts(instructions), lambda t: (deltas or {}).get(t))
        return r, r.lowering()

    def run_region_fixture(self, name, caller, callee=None, deltas=None, changes=True):
        exact, instructions = self.lifted_at(caller, 0x401000)
        result, regions = self.region(instructions, deltas)
        self.assertIsNotNone(regions, result.status)
        native = lift.native_x87_function(exact, regions)
        (self.assertNotEqual if changes else self.assertEqual)(native, lift.native_x87_function(exact))
        source = '#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
        source += exact.replace('void L_00401000(', 'void exact_L_00401000(', 1) + '\n'
        source += native.replace('void L_00401000(', 'void native_L_00401000(', 1) + '\n'
        callee_body = self.lifted_at(callee, 0x401100)[0] if callee else 'void L_00401100(CPU *c) { (void)c; }'
        source += callee_body.replace('void L_00401100(', 'void callee_L_00401100(', 1) + '\n'
        source += 'void L_00401100(CPU *c) { callee_L_00401100(c); }\n'
        work = ROOT / 'work/x87-native-tests'
        work.mkdir(parents=True, exist_ok=True)
        path = work / f'region_{name}.c'
        path.write_text(source)
        exe = path.with_suffix('.exe')
        build = subprocess.run(['gcc', '-O2', '-std=c11', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off',
                                '-I', str(lift.prepare_test_runtime()), str(path), str(ROOT / 'tests/x87_region_semantics.c'),
                                '-lm', '-o', str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(build.returncode, 0, build.stderr[-3000:])
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('4096 region-x87 functional CPU and memory cases passed', run.stdout)
        return native, result

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_regions_keep_top_across_a_loop_and_joins(self):
        # fld [esp+4]; mov ecx,3; again: fld st0; fmul [esp+8]; faddp; dec ecx; jnz again;
        # fld [esp+8]; fcomp st1; fnstsw ax (TOP observed at depth 1); test ah,1; jz skip; fchs;
        # skip: fld1; fxch st1; fsubp; fstp [esp+0xc]; ret
        raw = bytes.fromhex('d9442404b903000000d9c0d84c2408dec14975f5d9442408d8d9dfe0f6c4017402d9e0'
                            'd9e8d9c9dee9d95c240cc3')
        native, result = self.run_region_fixture('loop', raw)
        self.assertEqual([k for k, _ in result.anchors], ['entry'])
        # One TOP load (entry); stores at fnstsw, the return and the dead fall-off end.
        self.assertEqual(native.count('_ac1_x87_top = (uint32_t)c->fpu_top;'), 1)
        self.assertEqual(native.count('c->fpu_top = (int)'), 3)
        blocks = lift.native_x87_function(self.lifted_at(raw, 0x401000)[0])
        self.assertGreater(blocks.count('c->fpu_top = (int)'), 3)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_regions_with_known_and_unknown_calls_and_a_disagreeing_join(self):
        # fld [esp+4]; call callee (net +1); faddp; test eax,eax; jz skip; call eax (unknown);
        # fstp [esp+0x10]; skip (join: entry depth 1 / after-call depth -1): fst [esp+0xc]; fstp st0; ret
        raw = bytes.fromhex('d9442404e8f7000000dec185c07406ffd0d95c2410d954240cddd8c3')
        callee = bytes.fromhex('d9e8d84c240cc3')   # fld1; fmul [esp+0xc]; ret
        callee_result = self.region(self.lifted_at(callee, 0x401100)[1])[0]
        self.assertEqual(callee_result.delta, 1)
        native, result = self.run_region_fixture('calls', raw, callee, {0x401100: 1})
        self.assertEqual(sorted(k for k, _ in result.anchors), ['call', 'entry', 'join'])
        self.assertEqual(result.call_delta, {0x401004: 1, 0x40100F: None})

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_unbalanced_join_keeps_its_anchor(self):
        # Its only label is a join whose predecessors disagree: region mode is today's lowering.
        raw = bytes.fromhex('d9442404d9c0d8c9d9442408dec1d9c9dee9db5c240cd9e8d85c2404dfe0f6c4057a02d9eed95c2410c3')
        self.run_region_fixture('unbalanced', raw, changes=False)

    def test_stack_observers_keep_the_exact_lowering(self):
        body = '\n'.join(['void L_00401000(CPU *c)', '{', '    fpush(c, 1.0);',
                          '    ac1_x87_save(c, c->esp);', '    c->esp += 4; return;', '}'])
        self.assertEqual(lift.native_x87_function(body), body)


if __name__ == '__main__':
    unittest.main()
