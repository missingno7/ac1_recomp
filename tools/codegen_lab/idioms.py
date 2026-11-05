"""Whole-program census of x86 stack idioms behind the lifted-versus-native gap.

usage: python tools/codegen_lab/idioms.py
Scans every recovered function's decoded instructions, within basic blocks.
  lane read     a 16-byte store to a frame slot, then a narrower load from it
  wide reload   a narrow store into a slot, then a 16-byte load covering it
  whole reload  a 16-byte store, then a 16-byte load of the same slot
"""
import collections, json, re, sys, pathlib
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]; sys.path.insert(0, str(ROOT/'tools'))
import function_facts as F
p = F.Program()
counts = collections.Counter(); fns = collections.defaultdict(set)
hot = {}
for c in json.loads((ROOT/'work/codegen_lab/snapshots/x0f0/manifest.json').read_text())['corpus']: hot[int(c['va'], 16)] = c['weight']
hot_w = collections.Counter()
for a, fn in p.functions.items():
    d = None
    for b in fn['blocks']:
        recent = []                                  # (base reg, disp, size) of stores in this block
        ok = False
        for va, size in b['instructions']:
            raw = p.image[va - p.base: va - p.base + 1]
        if d is None:
            d = p.decode(a)
        for va, _ in b['instructions']:
            i = d[va]
            if i.mnemonic == 'call': recent = []; continue
            if not i.operands: continue
            mem = [(k, op) for k, op in enumerate(i.operands) if op.type == 3]
            if not mem or i.mnemonic == 'lea': continue
            k, op = mem[0]
            base = i.reg_name(op.mem.base) if op.mem.base else None
            if base not in ('esp', 'ebp') or op.mem.index: continue
            w = op.size
            if k == 0 and i.mnemonic not in ('cmp', 'test', 'push'):
                recent = [s for s in recent if not (s[0] == base and s[1] < op.mem.disp + w and op.mem.disp < s[1] + s[2])]
                recent.append((base, op.mem.disp, w))
            else:
                for s in reversed(recent):
                    if s[0] == base and s[1] < op.mem.disp + w and op.mem.disp < s[1] + s[2]:
                        kind = ('lane read' if s[2] == 16 and w < 16 else 'whole reload' if s[2] == 16 and w == 16 else
                                'wide reload' if w == 16 and s[2] < 16 else None)
                        if kind:
                            counts[kind] += 1; fns[kind].add(a)
                            if a in hot: hot_w[kind] += hot[a]
                        break
print(f'{len(p.functions)} functions')
for k in ('lane read', 'whole reload', 'wide reload'):
    print(f'{k:13s} {counts[k]:6d} sites in {len(fns[k]):5d} functions; {sum(a in hot for a in fns[k]):2d} hot functions holding {100*sum(hot[a] for a in fns[k] if a in hot):.1f}% of profiled cycles')
