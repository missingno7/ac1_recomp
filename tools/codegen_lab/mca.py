"""llvm-mca on whole short functions, calibrated against cycles measured on the phone.

usage: python tools/codegen_lab/mca.py SYMBOLS MCA_EXE
Feeds each function's real instructions (branches removed, so one straight
block) to llvm-mca under several CPU models, with loads assumed not to alias
stores (default) and assumed to (--noalias=false). Simulated cycles are a
diagnostic: the table prints the phone measurement next to them, and if a model
cannot reproduce the measured ordering it must not be used to explain it.
llvm-mca does not model store-to-load forwarding.
"""
import json, re, subprocess, sys, pathlib
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]; sys.path[:0] = [str(ROOT/'tools'/'codegen_lab')]
import gap
sym, mca = sys.argv[1], sys.argv[2]
OUT = ROOT/'work/codegen_lab/mini'
MEASURED = {'ac1_translated_00428540': 110, 'ac1_native_00428540': 27, 'ac1_translated_00653400': 29,
            'ac1_native_00653400': 70, 'ac1_translated_01176DE0': 35, 'ac1_native_01176DE0': 39}


def block(name):
    keep = []
    for t in gap.code(sym, name):
        op = t.split()[0]
        if op in ('ret', 'b', 'br', 'cbz', 'cbnz', 'tbz', 'tbnz', 'nop') or op.startswith('b.'): continue
        if op == 'adrp': t = re.sub(r'adrp (x\d+),.*', r'mov \1, #0', t)
        if op == 'adr': t = re.sub(r'adr (x\d+),.*', r'mov \1, #0', t)
        keep.append(re.sub(r'\s*<[^>]*>', '', t))
    return '\n'.join(keep) + '\n'


def run(text, cpu, extra=()):
    r = subprocess.run([mca, '-mtriple=aarch64', f'-mcpu={cpu}', '-iterations=100', '--timeline=false', '--instruction-info=false',
                        '--resource-pressure=false', *extra], input=text, capture_output=True, text=True)
    m = re.search(r'Total Cycles:\s+(\d+)', r.stdout)
    if not m: return None, (r.stderr or r.stdout)[:200]
    return int(m[1]) / 100, re.search(r'uOps Per Cycle:\s+([\d.]+)', r.stdout)[1]


cpus = ('cortex-x4', 'neoverse-v2', 'cortex-a720', 'cortex-x1')
print(f'{"function":26s} {"phone":>6s} ' + ' '.join(f'{c:>12s}' for c in cpus) + ' ' + ' '.join(f'{c[:9]+" dep":>13s}' for c in cpus[:2]))
for name, cycles in MEASURED.items():
    text = block(name)
    cells = [run(text, c)[0] for c in cpus]
    dep = [run(text, c, ('--noalias=false',))[0] for c in cpus[:2]]
    print(f'{name:26s} {cycles:6d} ' + ' '.join(f'{x if x is not None else "fail":>12}' for x in cells) + ' ' + ' '.join(f'{x if x is not None else "fail":>13}' for x in dep))
