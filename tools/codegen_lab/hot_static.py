"""Hot-path machine code of two snapshots' corpus functions, cold call paths removed from both.

usage: python tools/codegen_lab/hot_static.py SNAPSHOT_A SNAPSHOT_B
Compiles each snapshot's corpus units for ARM64 with its own flags plus
-include cpu.h -include hot_path.h (guest calls without the dispatcher
fallback and the scheduler checkpoint), measures each corpus function (P_<VA>
when a private body exists, else L_<VA>) and prints weighted totals and the
functions that grow the most.
"""
import json, sys, pathlib, subprocess, concurrent.futures as cf
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]; sys.path.insert(0, str(ROOT/'tools'))
import codegen_lab as lab
names = sys.argv[1:3]
snaps = {n: lab.LAB/'snapshots'/n for n in names}
mans = {n: json.loads((snaps[n]/'manifest.json').read_text()) for n in names}
out = lab.LAB/'experiments'/'hot_static'; out.mkdir(parents=True, exist_ok=True)
def build(job):
    n, unit = job
    o = out/f'{n}_{pathlib.Path(unit).stem}.o'
    subprocess.run([str(lab.CLANG), *mans[n]['android_flags'], '-I', str(snaps[n]/'runtime'), '-I', str(snaps[n]/'units'),
                    '-include', 'cpu.h', '-include', str(ROOT/'tools/codegen_lab/hot_path.h'),
                    '-c', str(snaps[n]/'units'/unit), '-o', str(o)], check=True)
    return (n, unit), o
jobs = [(n, u) for n in names for u in sorted({c['unit'] for c in mans[n]['corpus']})]
with cf.ThreadPoolExecutor(8) as pool:
    objs = dict(pool.map(build, jobs))
keys = ('instructions', 'cpu gpr loads', 'cpu gpr stores', 'cpu xmm loads', 'cpu xmm stores', 'stack loads', 'stack stores',
        'guest loads', 'guest stores', 'branches', 'calls')
tot = {n: dict.fromkeys(keys, 0.0) for n in names}; rows = []; W = 0
for c in mans[names[0]]['corpus']:
    va = f'{int(c["va"], 16):08X}'
    m = {}
    for n in names:
        unit = next(x['unit'] for x in mans[n]['corpus'] if x['va'] == c['va'])
        fns = lab.function_insns(objs[(n, unit)], {f'ac1_translated_{va}', f'P_{va}', f'L_{va}'})
        body = fns.get(f'ac1_translated_{va}') or fns.get(f'P_{va}') or fns.get(f'L_{va}')
        if not body: break
        m[n] = lab.arm64_metrics(body)
    if len(m) != 2: continue
    W += c['weight']
    for n in names:
        for k in keys: tot[n][k] += c['weight'] * m[n].get(k, 0)
    a, b = (m[n] for n in names)
    rows.append((c['weight'] * (b['instructions'] - a['instructions']), c['va'], a['instructions'], b['instructions'],
                 a.get('cpu gpr loads', 0) + a.get('cpu gpr stores', 0), b.get('cpu gpr loads', 0) + b.get('cpu gpr stores', 0),
                 a.get('stack loads', 0) + a.get('stack stores', 0), b.get('stack loads', 0) + b.get('stack stores', 0)))
print(f'hot path, {len(rows)} corpus functions ({100*W:.1f}% of profiled cycles): {names[1]} vs {names[0]}, weighted means')
for k in keys:
    a, b = tot[names[0]][k] / W, tot[names[1]][k] / W
    print(f'  {k:16s} {a:9.2f} -> {b:9.2f}  ({100*(b-a)/a if a else 0:+5.1f}%)')
print('largest weighted changes (instructions, GPR state ops, spill ops):')
for r in sorted(rows, key=lambda r: -abs(r[0]))[:10]:
    print(f'  {r[1]}: insns {r[2]} -> {r[3]}, GPR state {r[4]} -> {r[5]}, spills {r[6]} -> {r[7]}')
