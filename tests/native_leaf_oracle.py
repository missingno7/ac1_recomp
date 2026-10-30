"""Compare native leaves with real private translated bodies on synthetic inputs.

Run after generation: python -B tests/native_leaf_oracle.py [--portable].
Generated fixtures and executables remain under ignored work/native_oracle/.
The unattended gameplay verifier is an additional, separate validation step.
"""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--portable', action='store_true')
    parser.add_argument('--cases', type=int, default=10000)
    parser.add_argument('--compiler', default='C:/msys64/mingw64/bin/gcc.exe')
    args = parser.parse_args()
    if args.cases < 1: parser.error('cases must be positive')
    addresses = [0x01176DE0, 0x00653400, 0x0042EE70]
    bodies = {}
    for path in (ROOT/'work/lift_initial').glob('initial_[0-9][0-9][0-9][0-9].c'):
        text = path.read_text(encoding='utf-8')
        for va in addresses:
            match = re.search(rf'(?:static )?void (?:L|ac1_translated)_{va:08X}\(CPU \*c\)\n\{{(.*?)\n\}}', text, re.S)
            if match: bodies[va] = f'static void oracle_{va:08X}(CPU *c)\n{{{match[1]}\n}}'
        if len(bodies) == len(addresses): break
    if set(bodies) != set(addresses): raise SystemExit('missing translated oracle; generate the catalog first')
    out = ROOT/'work/native_oracle'; out.mkdir(parents=True, exist_ok=True)
    source = r'''
#include "ac1_native.h"
#include <stdio.h>
static unsigned char memory[0x2000000];
uint8_t *ac1_direct_arena = memory;
#undef guest_execution_read
#undef guest_execution_write
void guest_execution_read(uint32_t a,void *o,size_t n){memcpy(o,memory+a,n);}
void guest_execution_write(uint32_t a,const void *v,size_t n){memcpy(memory+a,v,n);}
void ac1_unsupported_instruction(uint32_t a,const char*s){fprintf(stderr,"%08x %s\n",a,s);exit(2);}
#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))
'''
    source += '\n'.join(bodies.values())
    for va in addresses:
        source += f'\nvoid ac1_native_{va:08X}(CPU*);\nint ac1_native_{va:08X}_plan(const CPU*,ac1_native_plan*);\n'
        source += (f'static ac1_native_binding binding_{va:08X}={{.va=0x{va:08X},.name="{va:08X}",'
                   f'.body=ac1_native_{va:08X},.plan=ac1_native_{va:08X}_plan}};\n')
    source += 'ac1_native_binding *const ac1_native_bindings[]={' + ','.join(f'&binding_{va:08X}' for va in addresses) + '};\n'
    source += f'const size_t ac1_native_binding_count={len(addresses)};\n'
    source += r'''
static unsigned seed=0x584735;
static unsigned rng(void){seed^=seed<<13;seed^=seed>>17;return seed^=seed<<5;}
int main(void){
'''
    source += f'_putenv_s("AC1_NATIVE_VERIFY","{args.cases}"); ac1_native_init();\n'
    source += 'wr32(0x1680f90,0x3f800000);\n'
    # One additional call exercises the NULL eligibility plan after the budget.
    source += f'for(unsigned i=0;i<{args.cases+1};++i){{\n'
    source += r'''
  CPU c; memset(&c,0x5a,sizeof(c));c.calls=0;c.esp=0x10000+(rng()&15);c.ebp=rng();
  for(unsigned k=0;k<3;++k){
    unsigned p=0x20000+16*k;wr32(c.esp+4+4*k,p);
    for(unsigned j=0;j<4;++j){float f=((int)(rng()%20001)-10000)/32.0f;wrss(p+4*j,f);}
  }
  ac1_native_invoke(&c,&binding_01176DE0,oracle_01176DE0);
  c.esp=0x10000+(rng()&15);
  unsigned a=0x21000,b=0x22000,out=0x23000;
  if(i%4==0)out=a; else if(i%4==1)out=b; else if(i%4==2)out=a+4;
  wr32(c.esp+4,out);wr32(c.esp+8,a);wr32(c.esp+12,b);
  for(unsigned k=0;k<16;++k){
    float x=((int)(rng()%20001)-10000)/128.0f;
    float y=((int)(rng()%20001)-10000)/128.0f;
    wrss(a+4*k,x);wrss(b+4*k,y);
  }
  ac1_native_invoke(&c,&binding_00653400,oracle_00653400);
  c.esp=0x10000+(rng()&15);
  unsigned ptrs[5]={0x25000,0x26000,0x27000,0x28000,0x29000};
  if(i%5)ptrs[0]=ptrs[i%5]; /* out aliases q, v, scale or unit */
  for(unsigned k=0;k<5;++k)wr32(c.esp+4+4*k,ptrs[k]);
  for(unsigned k=1;k<5;++k)for(unsigned j=0;j<4;++j){
    float f=((int)(rng()%20001)-10000)/1024.0f;wrss(ptrs[k]+4*j,f);
  }
  ac1_native_invoke(&c,&binding_0042EE70,oracle_0042EE70);
 }
 ac1_native_shutdown(); puts("native leaf differential oracle passed"); return 0;
}
'''
    fixture = out/'fixture.c'; fixture.write_text(source, encoding='utf-8')
    exe = out/('portable.exe' if args.portable else 'sse2.exe')
    flags = ['-DAC1_NATIVE_PORTABLE'] if args.portable else []
    subprocess.run([args.compiler, '-O2', '-g', '-DAC1_TRACE_EIP', '-DAC1_DIRECT_MEMORY',
                    '-ffp-contract=off', '-fno-strict-aliasing', '-I'+str(ROOT/'src/runtime'),
                    *flags, str(fixture), str(ROOT/'src/runtime/ac1_native.c'),
                    str(ROOT/'src/runtime/ac1_native_leaf.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__': main()
