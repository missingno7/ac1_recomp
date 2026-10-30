"""Flag dataflow edges and compiled masked-helper differential oracle."""
import re
import shutil
import subprocess
import unittest
from unittest.mock import patch

import test_lift_code_immediates as common

lift, ROOT = common.lift, common.ROOT


def function(*instructions):
    lines = ['void L_00401000(CPU *c)', '{']
    for i, text in enumerate(instructions):
        if isinstance(text, tuple):
            label, text = text
            lines.append(label + ':')
        lines.extend([f'    AC1_SET_GUEST_EIP(c, 0x{0x401000+i:08X}u);', '    ' + text])
    return '\n'.join(lines + ['}'])


class FlagLivenessTests(unittest.TestCase):
    def masks(self, source, mode='cfg'):
        return re.findall(r'flags_\w+_m\(.*?, (0x[0-9A-F]+)u\)',
                          lift.flag_liveness_function(source, mode))

    def test_straight_line_and_return_contract(self):
        source = function('c->eax = flags_add(c, c->eax, 1, 4);',
                          'flags_sub(c, rd32(c->eax), 3, 4);', 'guest_ret(c, 0); return;')
        self.assertEqual(self.masks(source), ['0x00', '0x3F'])
        self.assertEqual(lift.flag_liveness_function(source, 'off'), source)

    def test_diamond_and_backedge_union(self):
        source = function('flags_sub(c, c->eax, 0, 4);',
                          'if (c->zf) goto L_00401004;',
                          'flags_sub(c, c->eax, 2, 4);',
                          'goto L_00401005;',
                          ('L_00401004', 'if (c->cf) goto L_00401000;'),
                          ('L_00401005', 'flags_add(c, 1, 2, 4);'), 'guest_ret(c, 0); return;')
        source = source.replace('    AC1_SET_GUEST_EIP(c, 0x00401000u);',
                                'L_00401000:\n    AC1_SET_GUEST_EIP(c, 0x00401000u);')
        self.assertEqual(self.masks(source), ['0x09', '0x00', '0x3F'])
        self.assertEqual(self.masks(source, 'straight')[0], '0x3F')

    def test_sparse_entry_guard_is_outside_lowest_block(self):
        source = function(('L_00401000', 'flags_add(c, 1, 2, 4);'),
                          'if (c->zf) goto L_00401000;',
                          ('L_00401002', 'guest_ret(c, 0); return;'))
        source = source.replace('{\n', '{\n    goto L_00401002;\n', 1)
        prefix, pending, groups = lift.codegen_instruction_groups(source)
        self.assertEqual(prefix[-1], '    goto L_00401002;')
        self.assertFalse(pending)
        self.assertNotIn('goto L_00401002;', '\n'.join(groups[0]))
        self.assertEqual(self.masks(source), ['0x3F'])

    def test_preserved_lanes_and_carry_input(self):
        source = function('flags_add(c, 1, 2, 4);', 'flags_incs(c, 3, 4);',
                          'flags_logicz(c, 5, 4);', 'c->eax = flags_adc(c, 4, 5, 4);',
                          'guest_ret(c, 0); return;')
        self.assertEqual(self.masks(source), ['0x00', '0x00', '0x01', '0x3F'])
        # INC never reads CF; its masked helper leaves it untouched. A later
        # ADC would require the earlier producer when no logical kill intervenes.
        source = function('flags_add(c, 1, 2, 4);', 'flags_incs(c, 3, 4);',
                          'flags_adc(c, 4, 5, 4);', 'guest_ret(c, 0); return;')
        self.assertEqual(self.masks(source), ['0x01', '0x00', '0x3F'])

    def test_observers_escapes_and_unknown_helpers(self):
        for observer in ('dispatch(c, c->eax);', 'dispatch_jmp(c, c->eax); return;',
                         '/* indirect jmp */ dispatch_jmp(c, c->eax); return;',
                         'SET8H(c->eax, (c->sf<<7)|(c->zf<<6)|(c->af<<4)|(c->pf<<2)|2|c->cf); /* 00401001: lahf */',
                         'push32(c, eflags_pack(c));', 'ac1_unknown(c);',
                         'RECOMP_TODO(0x401000, "trap");',
                         'c->eax = (c->sf << 7) | (c->zf << 6) | (c->af << 4) | (c->pf << 2) | c->cf;'):
            source = function('flags_add(c, 1, 2, 4);', observer,
                              'flags_sub(c, 3, 4, 4);', 'guest_ret(c, 0); return;')
            # LAHF needs its five observed lanes; others need all six.
            expected = '0x1F' if 'c->sf <<' in observer else '0x3F'
            self.assertEqual(self.masks(source)[0], expected, observer)

    def test_seh_and_intra_instruction_loops_are_untouched(self):
        source = function('flags_add(c, 1, 2, 4); /* fs:[0] */', 'guest_ret(c, 0); return;')
        self.assertEqual(lift.flag_liveness_function(source), source)
        source = function('while (c->ecx) { flags_sub(c, 1, 2, 4); if(c->zf) break; }',
                          'guest_ret(c, 0); return;')
        self.assertEqual(lift.flag_liveness_function(source), source)

    def test_generated_direct_transfers_with_masks(self):
        import test_lift_direct_calls as direct
        original = lift.specialize_direct_calls
        def transform(*args):
            body, callees, count = original(*args)
            return lift.flag_liveness_function(body), callees, count
        with patch.object(lift, 'specialize_direct_calls', side_effect=transform):
            direct.DirectCallsTests('test_generated_calls_and_tail_transfers').test_generated_calls_and_tail_transfers()

    def test_generated_switches_with_masks(self):
        import test_lift_register_switches as switches
        original = lift.lift_exact_function
        with patch.object(lift, 'lift_exact_function',
                          side_effect=lambda *a, **k: lift.flag_liveness_function(original(*a, **k))):
            switches.RegisterSwitchTests().test_compile_execute_local_cases_default_ret8_and_unknown_fallback()

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_masked_helpers_all_masks_widths_and_boundary_values(self):
        runtime = lift.prepare_test_runtime()
        work = ROOT / 'work/codegen-tests'
        work.mkdir(parents=True, exist_ok=True)
        operations = [('add', 4), ('sub', 4), ('adc', 4), ('sbb', 4),
                      ('logicz', 3), ('incs', 3), ('decs', 3)]
        checks = []
        for name, argc in operations:
            args = 'a, b, sz' if argc == 4 else 'a, sz'
            checks.append(f'''
            {{ CPU original=seed, masked=seed;
               uint32_t a=values[x], b=values[y];
               uint32_t expected=flags_{name}(&original,{args});
               uint32_t actual=flags_{name}_m(&masked,{args},mask);
               assert(expected==actual);
               const unsigned bits[6]={{1,2,4,8,16,32}};
               uint32_t before[6]={{seed.cf,seed.pf,seed.af,seed.zf,seed.sf,seed.of}};
               uint32_t after[6]={{masked.cf,masked.pf,masked.af,masked.zf,masked.sf,masked.of}};
               uint32_t full[6]={{original.cf,original.pf,original.af,original.zf,original.sf,original.of}};
               for(unsigned k=0;k<6;k++) assert(after[k]==((mask&bits[k])?full[k]:before[k]));
               masked.cf=seed.cf; masked.pf=seed.pf; masked.af=seed.af;
               masked.zf=seed.zf; masked.sf=seed.sf; masked.of=seed.of;
               assert(!memcmp(&masked,&seed,sizeof seed));
            }}''')
        source = work / 'flags.c'
        source.write_text('''#include "cpu.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 uint32_t values[]={0,1,0x7f,0x80,0xff,0x100,0x7fff,0x8000,0xffff,
                    0x10000,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
 for(unsigned f=0;f<64;f++) {
  CPU seed={0}; seed.cf=f&1; seed.pf=(f>>1)&1; seed.af=(f>>2)&1;
  seed.zf=(f>>3)&1; seed.sf=(f>>4)&1; seed.of=(f>>5)&1;
  for(unsigned mask=0;mask<64;mask++) for(int sz=1;sz<=4;sz*=2)
   for(unsigned x=0;x<14;x++) for(unsigned y=0;y<14;y++) {
''' + '\n'.join(checks) + '\n}} puts("masked flags: 16859136 operation cases passed"); return 0; }')
        exe = work / 'flags.exe'
        subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-I', str(runtime),
                        str(source), '-o', str(exe)], check=True, capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60)
        self.assertIn('cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
