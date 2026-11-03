"""How much of the GPR call-boundary traffic exact contracts could remove (decoded x86, hot corpus).

usage: python tools/codegen_lab/gpr_census.py SNAPSHOT
Today a GPR-local function spills the registers written since the last sync
before every CPU-taking call and reloads every register after it (Clang drops
reloads whose value is dead). A register can stay in the caller's local across
a call only if the callee, transitively, neither reads nor writes it: its
footprint. Indirect calls, imports and unknown targets have every register in
their footprint. This counts, per hot-corpus call site, today's dirty spills
and live reloads and the part outside the callee's footprint.
"""
import collections, json, sys, pathlib
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
import function_facts as F

snap = ROOT/'work/codegen_lab/snapshots'/sys.argv[1]
manifest = json.loads((snap/'manifest.json').read_text())
corpus = {int(c['va'], 16): c['weight'] for c in manifest['corpus']}
prog = F.Program()
gf = {}
for i, a in enumerate(prog.functions):
    gf[a] = prog.gpr_facts(a)
ALL = F.GPR_ALL

# Footprint: registers a function or anything it calls may read or write.
own, callees, callers = {}, {}, collections.defaultdict(set)
for a, blocks in gf.items():
    fp, cs = 0, set()
    for _, _, insns in blocks:
        for va, m, (r, w), t in insns:
            fp |= r | w
            if t and t.kind in ('direct', 'tail'):
                if t.target in gf: cs.add(t.target)
                else: fp = ALL
            elif t and t.kind in ('import', 'indirect', 'tail-indirect'):
                fp = ALL
    own[a], callees[a] = fp, cs
    for c in cs: callers[c].add(a)
footprint = dict(own)
queue = collections.deque(gf); queued = set(gf)
while queue:
    a = queue.popleft(); queued.discard(a)
    new = footprint[a]
    for c in callees[a]: new |= footprint[c]
    if new != footprint[a]:
        footprint[a] = new
        for b in callers[a]:
            if b not in queued: queue.append(b); queued.add(b)
print(f'{len(gf)} functions; footprint all eight registers: {sum(v == ALL for v in footprint.values())}')


def site_footprint(t):
    return footprint.get(t.target, ALL) if t.kind == 'direct' else ALL


def analyse(a):
    blocks = {s: (succ, insns) for s, succ, insns in gf[a]}
    referenced = 0
    for _, insns in blocks.values():
        for _, _, (r, w), _ in insns: referenced |= r | w
    # Liveness: a return or tail transfer may hand every register to the caller.
    live_in = {s: 0 for s in blocks}
    changed = True
    while changed:
        changed = False
        for s, (succ, insns) in blocks.items():
            live = 0
            for t in succ: live |= live_in.get(t, ALL)
            for va, m, (r, w), t in reversed(insns):
                if t and t.kind in ('ret', 'tail', 'tail-indirect'): live = ALL
                elif t and t.kind in ('direct', 'import', 'indirect'): live |= site_footprint(t)
                else: live = r | (live & ~w)
            if live != live_in[s]: live_in[s] = live; changed = True
    # Dirty since the last sync, today (every call spills) and with contracts
    # (a call spills only what its footprint holds).
    rows = []
    for mode in ('today', 'contract'):
        dirty_in = {a: 0}; order = collections.deque([a]); seen_out = {}
        while order:
            s = order.popleft(); succ, insns = blocks[s]; d = dirty_in[s]
            for va, m, (r, w), t in insns:
                if t and t.kind in ('direct', 'import', 'indirect'):
                    d = 0 if mode == 'today' else d & ~site_footprint(t)
                else:
                    d |= w
            for t in succ:
                if t not in blocks: continue
                new = dirty_in.get(t, 0) | d
                if t not in dirty_in or new != dirty_in[t]: dirty_in[t] = new; order.append(t)
        sites = []
        for s, (succ, insns) in blocks.items():
            if s not in dirty_in: continue
            d = dirty_in[s]
            live_after = []
            live = 0
            for t in succ: live |= live_in.get(t, ALL)
            outs = []
            for va, m, (r, w), t in reversed(insns):
                outs.append(live)
                if t and t.kind in ('ret', 'tail', 'tail-indirect'): live = ALL
                elif t and t.kind in ('direct', 'import', 'indirect'): live |= site_footprint(t)
                else: live = r | (live & ~w)
            outs.reverse()
            for (va, m, (r, w), t), out in zip(insns, outs):
                if t and t.kind in ('direct', 'import', 'indirect'):
                    fp = site_footprint(t)
                    spill = d & referenced & ((ALL if mode == 'today' else fp))
                    reload = out & referenced & ((ALL if mode == 'today' else fp))
                    sites.append((va, t.kind, spill, reload, fp))
                    d = 0 if mode == 'today' else d & ~fp
                else:
                    d |= w
        rows.append(sites)
    return rows


bits = lambda m: bin(m).count('1')
tot = collections.Counter(); per_reg = collections.Counter(); kinds = collections.Counter()
for a, wgt in corpus.items():
    if a not in gf: continue
    today, contract = analyse(a)
    for (va, kind, s0, r0, fp), (_, _, s1, r1, _) in zip(sorted(today), sorted(contract)):
        kinds[kind] += 1
        kinds[f'{kind} with a transparent register'] += fp != ALL and bool((s0 | r0) & ~fp)
        tot['spills today'] += wgt * bits(s0); tot['spills contract'] += wgt * bits(s1)
        tot['reloads today'] += wgt * bits(r0); tot['reloads contract'] += wgt * bits(r1)
        for k in range(8):
            if (s0 | r0) >> k & 1 and not (fp >> k & 1): per_reg[F.GPRS[k]] += wgt
print('hot-corpus call sites:', dict(kinds))
for k in ('spills', 'reloads'):
    t0, t1 = tot[f'{k} today'], tot[f'{k} contract']
    print(f'  weighted {k:8s} today {t0:8.3f}  with exact footprints {t1:8.3f}  ({100*(t1-t0)/t0 if t0 else 0:+.1f}%)')
print('  removable traffic by register (weighted sites):', {r: round(v, 3) for r, v in per_reg.most_common()})
