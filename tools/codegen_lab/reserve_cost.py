"""Static cost of reserving host registers in real lifted code (hot corpus, ARM64).

usage: python tools/codegen_lab/reserve_cost.py SNAPSHOT
Compiles the snapshot's units with the snapshot's flags, once as they are and
once with -ffixed-x20..x27 (the eight registers a pinned guest-GPR protocol
would take), and compares each corpus function's machine code, weighted by its
profile weight. The code itself is unchanged, so this isolates the register
pressure: spills and extra instructions the reservation alone causes.
"""
import json, sys, pathlib, subprocess, concurrent.futures as cf
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]; sys.path.insert(0, str(ROOT/'tools'))
import codegen_lab as lab
snap = lab.LAB/'snapshots'/sys.argv[1]
manifest = json.loads((snap/'manifest.json').read_text())
out = lab.LAB/'experiments'/'reserve_cost'/sys.argv[1]; out.mkdir(parents=True, exist_ok=True)
VARIANTS = {'base': [], 'esp': ['-ffixed-x24'], 'esp_esi': ['-ffixed-x24', '-ffixed-x26'],
            'all8': [f'-ffixed-x{r}' for r in range(20, 28)]}
units = sorted({c['unit'] for c in manifest['corpus']})
def build(job):
    v, unit = job
    o = out/f'{pathlib.Path(unit).stem}_{v}.o'
    subprocess.run([str(lab.CLANG), *manifest['android_flags'], *VARIANTS[v], '-I', str(snap/'runtime'), '-I', str(snap/'units'),
                    '-c', str(snap/'units'/unit), '-o', str(o)], check=True)
    return v, unit, o
with cf.ThreadPoolExecutor(8) as pool:
    objs = {(v, u): o for v, u, o in pool.map(build, [(v, u) for v in VARIANTS for u in units])}
keys = ('instructions', 'stack loads', 'stack stores', 'struct loads', 'struct stores', 'guest loads', 'guest stores', 'branches')
tot = {v: dict.fromkeys(keys, 0.0) for v in VARIANTS}; worst = []
W = 0
for c in manifest['corpus']:
    name = f'L_{int(c["va"], 16):08X}'
    m = {}
    for v in VARIANTS:
        ins = lab.function_insns(objs[(v, c['unit'])], {name, 'ac1_translated_' + name[2:]})
        if not ins: break
        m[v] = lab.arm64_metrics(next(iter(ins.values())))
    if len(m) != len(VARIANTS): continue
    W += c['weight']
    for v in VARIANTS:
        for k in keys: tot[v][k] += c['weight'] * m[v].get(k, 0)
    worst.append((c['weight'] * (m['all8']['instructions'] - m['base']['instructions']), c['va'], m['base']['instructions'],
                  m['all8']['instructions'], m['base'].get('stack loads', 0) + m['base'].get('stack stores', 0),
                  m['all8'].get('stack loads', 0) + m['all8'].get('stack stores', 0)))
print(f'{len(worst)} corpus functions, {100*W:.1f}% of profiled cycles; weighted per-function means')
print(f'{"metric":14s} ' + ' '.join(f'{v:>16s}' for v in VARIANTS))
for k in keys:
    b = tot['base'][k] / W
    print(f'{k:14s} ' + ' '.join(f'{tot[v][k]/W:8.2f} ({100*(tot[v][k]-tot["base"][k])/tot["base"][k] if tot["base"][k] else 0:+5.1f}%)' for v in VARIANTS))
print('largest weighted growth with all eight reserved (va: instructions, spill ops):')
for w, va, i0, i1, s0, s1 in sorted(worst, reverse=True)[:8]:
    print(f'  {va}: {i0} -> {i1}, spills {s0} -> {s1}')
