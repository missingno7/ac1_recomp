"""Compiled masked-helper differential oracle."""
import shutil
import subprocess
import unittest

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


class MaskedHelperTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_masked_helpers_all_masks_widths_and_boundary_values(self):
        runtime = lift.RUNTIME
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
