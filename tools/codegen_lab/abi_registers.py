"""Which guest registers cross hot direct calls in real code (decoded x86, weighted by the phone profile).

usage: python tools/codegen_lab/abi_registers.py
For each sampled function's direct calls to lifted callees: the registers the
callee reads before writing (entry use: arguments like this in ecx, esp) and
those the caller reads after the call before redefining them (results like
eax, callee-saved values like esi). Weighted by the caller's GPR-state cycles
in the city profile, this ranks which registers a private call ABI should
carry.
"""
import collections, json, re, sys, pathlib
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]; sys.path.insert(0, str(ROOT/'tools'))
import function_facts as F

S = json.load(open(ROOT/'work/codegen_lab/experiments/dynamic_lines/summary.json'))
cyc = collections.Counter()
for k, v in S['by_fn_class'].items():
    m = re.match(r'L_([0-9A-F]{8}) cpu gpr', k)
    if m: cyc[int(m[1], 16)] += v['cpu-cycles:u']
prog = F.Program()
cache = {}
def gf(a):
    if a not in cache: cache[a] = prog.gpr_facts(a)
    return cache[a]

def entry_use(a):
    """Registers read at entry before being written, up to the first transfer (calls read nothing from locals)."""
    blocks = {s: (succ, ins) for s, succ, ins in gf(a)}
    live_in = dict.fromkeys(blocks, 0); changed = True
    while changed:
        changed = False
        for s, (succ, ins) in blocks.items():
            live = 0
            for t in succ: live |= live_in.get(t, 0)
            for va, m, (r, w), t in reversed(ins):
                live = 0 if t and t.kind in ('ret', 'tail', 'tail-indirect', 'direct', 'import', 'indirect') else r | (live & ~w)
                if t and t.kind in ('direct', 'import', 'indirect'): live |= r   # the call's own operands (esp)
            if live != live_in[s]: live_in[s] = live; changed = True
    return live_in.get(a, 0)

carry_in, carry_out, sites = collections.Counter(), collections.Counter(), 0
total = sum(cyc.values())
for a, w in cyc.most_common(400):
    if a not in prog.functions: continue
    after = F.gpr_live_after_calls(gf(a))
    for _, _, ins in gf(a):
        for va, m, _, t in ins:
            if t and t.kind == 'direct' and t.target in prog.functions:
                sites += 1
                e = entry_use(t.target); o = after.get(va, 0)
                for i, r in enumerate(F.GPRS):
                    if e >> i & 1: carry_in[r] += w
                    if o >> i & 1: carry_out[r] += w
print(f'{sites} direct call sites in the 400 functions with the most GPR-state cycles '
      f'({100*sum(w for a, w in cyc.most_common(400))/total:.0f}% of them); weight = caller GPR-state cycles')
print(f'{"register":9s} {"read by callee at entry":>24s} {"read by caller after":>22s}')
for r in F.GPRS:
    print(f'{r:9s} {carry_in[r]/1e6:24.1f} {carry_out[r]/1e6:22.1f}')
