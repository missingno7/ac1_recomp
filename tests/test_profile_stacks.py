"""Compile a real x64 native chain and check suspended unwind ancestry."""
import bisect
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from profile_report import load_symbols
from lift_ac1 import profile_count_entry
from profile_inclusive import analyze

def main():
    out = ROOT / 'work/engine_prof/contract'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'fixture.exe'
    subprocess.run(['C:/msys64/mingw64/bin/gcc.exe', '-std=c11', '-O2', '-g',
                    '-Wall', '-Wextra', '-Werror', '-Isrc/runtime',
                    'tests/profile_stack_fixture.c', 'src/runtime/ac1_profile.c',
                    '-o', str(exe)], cwd=ROOT, check=True)
    env = dict(os.environ, AC1_PROFILE='1', AC1_PROFILE_STACKS='1',
               AC1_PROFILE_COUNTS='00401000,00402000,00403000',
               AC1_PROFILE_DIR=str(out), AC1_PROFILE_INTERVAL='0.2')
    subprocess.run([str(exe)], env=env, check=True)
    rvas, names = load_symbols(exe, 'C:/msys64/mingw64/bin/nm.exe')
    captures = [(p, json.loads(p.read_text())) for p in out.glob('raw_*.json')]
    latest = max(d['run_start'] for _, d in captures)
    total = good = bad = parked_guest = 0
    for p, d in captures:
        if d['run_start'] != latest: continue
        assert d['errors'] == d['dropped'] == 0, d
        counts = p.with_name(p.stem.replace('raw_', 'counts_')+'.bin').read_bytes()
        for index, a, b, c in struct.iter_unpack('<QQQQ', counts):
            assert index == 0 and (a,b,c) == (1,2,1)
        mod = next(m for m in d['modules'] if m['path'].lower() == str(exe).lower())
        raw = p.with_name(p.stem.replace('raw_', 'stacks_')+'.bin').read_bytes()
        fmt = struct.Struct('<IIQ'+'Q'*d['stack_depth'])
        assert len(raw) == d['samples']*fmt.size
        samples=list(struct.iter_unpack('<QQQIIIIII', p.with_suffix('.bin').read_bytes()))
        for sample, stack in zip(samples,fmt.iter_unpack(raw)):
            depth, status, ticks, *pcs = stack
            chain = []
            for pc in pcs[:depth]:
                index = bisect.bisect_right(rvas, pc-mod['base'])-1
                if index >= 0 and mod['base'] <= pc < mod['base']+mod['size']:
                    chain.append(names[index])
            if 'L_00403000' not in chain: continue
            total += 1
            expected = ['L_00403000', 'L_00402000', 'L_00402000', 'L_00401000']
            guest = [n for n in chain if n in expected]
            good += guest == expected
            bad += bool(status)
            parked_guest += bool(sample[6]&8)
    assert total > 100 and good == total and bad == 0, (total, good, bad)
    assert parked_guest>10, 'host waits lost guest ancestry'
    disabled = out / 'disabled'
    disabled.mkdir(exist_ok=True)
    subprocess.run([str(exe)], env=dict(env, AC1_PROFILE='0', AC1_PROFILE_DIR=str(disabled)), check=True)
    assert not list(disabled.iterdir()), 'disabled sampler emitted output'
    body='void L_00401000(CPU *c) {\n    (void)c;\n}'
    instrumented=profile_count_entry(body,0x00401000)
    assert instrumented.count('ac1_profile_entry(0x00401000u);')==1
    assert instrumented.endswith('    (void)c;\n}')
    try: profile_count_entry(body,0x00402000)
    except ValueError: pass
    else: raise AssertionError('unknown count entry accepted')
    report=analyze(out,exe,seconds=1)
    shares={r['va']:r['cpu_percent'] for r in report['inclusive']}
    assert max(shares.values())-min(shares.values())<1e-8, shares
    expected_path=['00401000','00402000','00402000','00403000']
    assert any(r['path']==expected_path and r['self_ms']>0 for r in report['trees'])
    assert all(r['path'] for r in report['host_chains']), 'empty native chain'
    assert abs(sum(r['ms'] for r in report['host_symbols']) -
               sum(r['ms'] for r in report['host_callers'])) < 1e-8
    print(f'PASS: {good}/{total} nested guest chains, no truncated walks; disabled emits nothing')

if __name__ == '__main__': main()
