"""Small-scale test of region-mode native x87 on the hot corpus, before any generation change.

usage: python tools/codegen_lab/x87_regions_experiment.py SNAPSHOT INPUTS.pkl
SNAPSHOT is a codegen-lab snapshot generated with --x87 native and no XMM
contracts; INPUTS.pkl holds the bodies the lifter gave native_x87_function
(captured from the same generation). For every native-x87 corpus function the
block-anchored and the region lowering are run through the generator's later
passes; the block-anchored result must equal the snapshot's function exactly.
The region body replaces it in a copy of the unit, which is compiled for ARM64
with the snapshot's flags; the two functions' machine code is compared.
"""
import json, pickle, re, sys, pathlib, subprocess, collections
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
import lift_ac1 as lift
import function_facts as facts
import codegen_lab as lab

snap = lab.LAB/'snapshots'/sys.argv[1]
inputs = pickle.loads(pathlib.Path(sys.argv[2]).read_bytes())
manifest = json.loads((snap/'manifest.json').read_text())
out = lab.LAB/'experiments'/'x87_regions'/sys.argv[1]; out.mkdir(parents=True, exist_ok=True)
corpus = {int(c['va'], 16): c for c in manifest['corpus']}
targets = sorted((a for a in corpus if a in inputs), key=lambda a: -corpus[a]['weight'])
print(f'{len(targets)} of {len(corpus)} corpus functions are native x87 ({100*sum(corpus[a]["weight"] for a in targets):.1f}% of cycles)')

program = facts.Program()
need = set(program.functions)
fa = {a: program.facts(a) for a in need}
delta = facts.x87_deltas(program, fa)


def later_passes(body):
    return lift.declare_local_memory_base(lift.drop_unrecorded_eip_markers(
        lift.fence_backward_branches(lift.local_xmm_function(lift.local_register_function(body, 'all'), None))))


def function_text(source, address):
    m = re.search(rf'^(?:static )?void L_{address:08X}\(CPU \*c\)\n\{{.*?\n\}}', source, re.M | re.S)
    return m


rows = []
for a in targets:
    region = facts.x87_region(a, fa[a], lambda t: delta.get(t))
    lowering = region.lowering()
    unit = snap/'units'/corpus[a]['unit']
    source = unit.read_text(encoding='utf-8')
    m = function_text(source, a)
    base = later_passes(lift.native_x87_function(inputs[a]))
    if m is None or base not in m[0]:
        rows.append(dict(va=f'0x{a:08X}', status='baseline rebuild differs from the snapshot')); continue
    if lowering is None:
        rows.append(dict(va=f'0x{a:08X}', status=region.status, weight=corpus[a]['weight'])); continue
    new = later_passes(lift.native_x87_function(inputs[a], lowering))
    if new == base:
        rows.append(dict(va=f'0x{a:08X}', status='unchanged (no removable sync)', weight=corpus[a]['weight'])); continue
    variant = out/f'{unit.stem}_{a:08X}.c'
    variant.write_text(source[:m.start()] + m[0].replace(base, new) + source[m.end():], encoding='utf-8')
    objs = {}
    for name, path in (('base', unit), ('region', variant)):
        obj = out/f'{unit.stem}_{a:08X}_{name}.o'
        lab.checked([lab.CLANG, *manifest['android_flags'], '-I', snap/'runtime', '-I', snap/'units', '-c', path, '-o', obj])
        objs[name] = lab.arm64_metrics(lab.function_insns(obj, {f'L_{a:08X}'})[f'L_{a:08X}'])
    rows.append(dict(va=f'0x{a:08X}', status='compared', weight=corpus[a]['weight'], anchors=len(region.anchors),
                     blocks=region.blocks, base=dict(objs['base']), region=dict(objs['region'])))
    b, r = objs['base'], objs['region']
    print(f'0x{a:08X} {100*corpus[a]["weight"]:5.2f}%  insns {b["instructions"]:5d} -> {r["instructions"]:5d}  '
          f'TOP ld/st {b["cpu x87 top loads"]}/{b["cpu x87 top stores"]} -> {r["cpu x87 top loads"]}/{r["cpu x87 top stores"]}  '
          f'stack {b["stack loads"] + b["stack stores"]} -> {r["stack loads"] + r["stack stores"]}  anchors {len(region.anchors)}/{region.blocks} blocks')
(out/'results.json').write_text(json.dumps(rows, indent=1))
cmp = [r for r in rows if r['status'] == 'compared']
W = sum(r["weight"] for r in cmp) or 1
keys = ('instructions', 'cpu x87 top loads', 'cpu x87 top stores', 'stack loads', 'stack stores', 'struct loads', 'struct stores',
        'guest loads', 'guest stores', 'branches', 'missed forwards')
print(f'\n{len(cmp)} functions changed ({100*W:.1f}% of cycles); weighted per-function means, base -> region:')
for k in keys:
    wb = sum(r['weight'] * r['base'].get(k, 0) for r in cmp) / W; wr = sum(r['weight'] * r['region'].get(k, 0) for r in cmp) / W
    print(f'  {k:22s} {wb:8.2f} -> {wr:8.2f}  ({100*(wr-wb)/wb if wb else 0:+.1f}%)')
for s_, n in collections.Counter(r['status'] for r in rows).most_common(): print(f'  {n:3d} {s_}')
