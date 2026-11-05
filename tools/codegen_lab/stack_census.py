"""What guest stack loads in hot code are, from the decoded x86 alone.

usage: python tools/codegen_lab/stack_census.py SNAPSHOT
For each stack load of each corpus function, walking its recovered blocks in
address order, this finds what last touched that frame slot in the same call
window (between calls) and what lies between. Offsets compare only within one
esp base (see function_facts.stack_accesses). Weights are profile weights per
function; the counts are static.

  argument       a slot above the return address: filled by the caller
  saved register pop of a slot the function itself pushed
  no prior store no store to the slot earlier in this function's walk
  across a call  the slot was stored before a call in the function
  store forward  stored in the same block with no other store or call between
  past stack store   same block, only other frame stores between (disjoint slots)
  past unknown store same block, a store through a non-frame pointer between
  other block    stored in an earlier block of the walk, no call between
"""
import collections, json, sys, pathlib
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]; sys.path.insert(0, str(ROOT/'tools'))
import function_facts as F
from capstone.x86 import X86_OP_MEM
snap = ROOT/'work/codegen_lab/snapshots'/sys.argv[1]
man = json.loads((snap/'manifest.json').read_text())
p = F.Program()
tot, weighted, per_fn = collections.Counter(), collections.Counter(), {}
fns = 0; escaped_w = 0; all_w = 0
for c in man['corpus']:
    a = int(c['va'], 16)
    if a not in p.functions: continue
    fns += 1; all_w += c['weight']
    acc, esc = F.stack_accesses(p, a)
    if esc: escaped_w += c['weight']
    d = p.decode(a); fn = p.functions[a]
    start_of = {}
    for b in fn['blocks']:
        for va, _ in b['instructions']: start_of[va] = b['start']
    slot, calls_seen = {}, 0                    # frame offset -> (va, calls at the time, stores at the time, block)
    stores_since = []                           # (va, frame?) in walk order
    for va in sorted(d):
        insn = d[va]; m = insn.mnemonic
        if m == 'call': calls_seen += 1
        fa = acc.get(va, [])
        for store, off, size in fa:
            if store: continue
            k = None
            if off >= 0 and off < (1 << 32):    k = 'argument' if off > 0 or m != 'pop' else 'argument'
            prev = next((slot[o] for o in range(off, off - 16, -1) if o in slot and o + slot[o][4] > off), None)
            if prev is None:
                k = k or 'no prior store'
            elif m == 'pop' and prev[5] == 'push':
                k = 'saved register'
            elif prev[1] != calls_seen:
                k = 'across a call'
            else:
                between = [s for s in stores_since if s[0] > prev[0] and s[0] < va]
                if start_of[prev[0]] != start_of[va]: k = 'other block'
                elif any(not s[1] for s in between): k = 'past unknown store'
                elif between: k = 'past stack store'
                else: k = 'store forward'
            tot[k] += 1; weighted[k] += c['weight']
        wrote_mem = any(op.type == X86_OP_MEM and op.access & 2 for op in insn.operands) or m in ('push', 'movsd', 'movsb', 'stosd')
        frame_store = any(s[0] for s in fa)
        for store, off, size in fa:
            if store: slot[off] = (va, calls_seen, 0, 0, size, m)
        if wrote_mem and m != 'call': stores_since.append((va, frame_store))
T, W = sum(tot.values()), sum(weighted.values())
print(f'{fns} functions, {100*escaped_w/all_w:.0f}% of their weight has a frame address that leaves esp/ebp')
print(f'{"guest stack load":22s} {"count":>7s} {"share":>7s} {"weighted":>9s}')
for k, n in tot.most_common(): print(f'{k:22s} {n:7d} {100*n/T:6.1f}% {100*weighted[k]/W:8.1f}%')
