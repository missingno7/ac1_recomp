"""The integer-helper inline experiment preserves its complete observable state."""
import importlib.util
import re
import shutil
import subprocess
import unittest

import test_codegen_flags as flags

ROOT, lift = flags.ROOT, flags.lift


class X87InlineTests(unittest.TestCase):
    def test_transformation_only_changes_the_helper_declaration(self):
        spec = importlib.util.spec_from_file_location('x87_prepare', ROOT/'tools/prepare_runtime.py')
        prepare = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(prepare)
        source = prepare.FP_STATE_HELPERS
        transformed = prepare.x87_inline_helpers(source)
        # Independently compare every byte of the signature/body and surrounding
        # helpers. The only inserted text must be the declaration's inline gate.
        before, body = source.split('static inline uint64_t ac1_x87_integer(', 1)
        changed, changed_body = transformed.split('AC1_X87_INTEGER_INLINE uint64_t ac1_x87_integer(', 1)
        self.assertEqual(body, changed_body)
        self.assertTrue(changed.startswith(before))
        gate = changed[len(before):]
        self.assertIn('__attribute__((always_inline))', gate)
        self.assertRegex(gate, r'#else\s+#define AC1_X87_INTEGER_INLINE static inline\s+#endif')
        self.assertNotRegex(gate, r'\bc->|\breturn\b')
        for invalid in ('', source+source):
            with self.assertRaises(SystemExit): prepare.x87_inline_helpers(invalid)

    @unittest.skipUnless(shutil.which('gcc') and shutil.which('objdump'), 'native compiler required')
    def test_out_of_line_and_forced_inline_are_bit_exact(self):
        work = ROOT/'work/x87-inline-tests'; work.mkdir(parents=True, exist_ok=True)
        include = lift.prepare_test_runtime()
        for mode, options in (('optimized', []), ('out-of-line', ['-fno-inline'])):
            with self.subTest(mode=mode):
                directory = work/mode; directory.mkdir(exist_ok=True)
                objects = []
                for name, defines in (('baseline', []), ('inline', ['-DAC1_FORCE_INLINE_X87=1'])):
                    # Constant widths match real FIST adapters; RC/truncation stay dynamic.
                    source = '#include "cpu.h"\nuint64_t '+name+'_integer(CPU*c,double v,unsigned b,unsigned t,uint32_t va) {\n'
                    source += 'switch(b) {\n'+''.join(
                        f'case {b}: return ac1_x87_integer(c,v,{b},t,va);\n' for b in (16,32,64))
                    source += 'default: return ac1_x87_integer(c,v,b,t,va);}}\n'
                    path = directory/(name+'.c'); path.write_text(source); obj = path.with_suffix('.o')
                    subprocess.run(['gcc', '-O2', *options, '-fwrapv', '-fno-strict-aliasing',
                        '-ffp-contract=off', '-I', str(include), *defines, '-c', str(path), '-o', str(obj)],
                        check=True, capture_output=True, timeout=60)
                    asm = subprocess.check_output(['objdump', '-d', str(obj)], text=True)
                    branches = re.findall(r'\b(?:call|jmp)\b[^\n]*<ac1_x87_integer[^>]*>', asm)
                    if name == 'baseline' and options: self.assertTrue(branches, asm)
                    if name == 'inline':
                        self.assertFalse(branches, asm)
                        self.assertNotRegex(asm, r'<ac1_x87_integer[^>]*>:', asm)
                    objects.append(str(obj))
                exe = directory/'oracle.exe'
                subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off',
                    '-I', str(include), str(ROOT/'tests/x87_integer_inline.c'), *objects, '-lm', '-o', str(exe)],
                    check=True, capture_output=True, timeout=60)
                run = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60)
                self.assertIn('bit-exact x87 integer inline cases', run.stdout)
                (directory/'result.txt').write_text(run.stdout)
