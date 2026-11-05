"""Lifted against native ARM64 for functions with a native reconstruction: dependency structure and traffic.

usage: python tools/codegen_lab/gap.py SYMBOLS VA [VA ...]

Takes both functions' machine code from an unstripped library and walks each
instruction list once in layout order, taking the fall-through side of
conditional branches. The model tracks when each register value is ready and
which store fills each memory location, so a load from a location stored
earlier is a store-to-load hop on the dependency chain. It reports the longest
chain and what is on it.

Latencies are nominal structure weights, not a cycle prediction (llvm-mca is
not in the NDK, and no public model of the X4 is used here). A load from a
matching store costs the 5-cycle hop measured on the phone (docs/codegen-lab.md);
a load that a narrower store partly covers is marked as a forwarding hazard.
"""
import collections, json, os, re, subprocess, sys, pathlib
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]; sys.path.insert(0, str(ROOT/'tools'/'codegen_lab'))
import audit as A

LAT = dict(load=4, alu=1, fmul=3, fadd=2, fcvt=3, fdiv=10, fmov=3, simd=2, hop=5, hazard=12)


def code(sym, name):
    nm = subprocess.run([str(A.BIN/'llvm-nm.exe'), '--defined-only', '--print-size', sym], capture_output=True, text=True).stdout
    m = re.search(rf'^([0-9a-f]+) ([0-9a-f]+) \w {name}$', nm, re.M)
    start, size = int(m[1], 16), int(m[2], 16)
    dis = subprocess.run([str(A.BIN/'llvm-objdump.exe'), '-d', '--no-show-raw-insn', f'--start-address=0x{start:x}',
                          f'--stop-address=0x{start+size:x}', sym], capture_output=True, text=True).stdout
    return [re.sub(r'\s+', ' ', x[2].split('//')[0].split(';')[0].strip()) for x in re.finditer(r'^\s*([0-9a-f]+):\s+(.*)$', dis, re.M)]


def canon(r):
    m = re.fullmatch(r'([wxsdqvhb])(\d+)(?:\.\w+)?(?:\[\d+\])?', r)
    if not m: return 'sp' if r in ('sp', 'wsp') else None
    return ('v' if m[1] in 'sdqvhb' else 'x') + m[2]


def regs_in(text):
    return [c for c in (canon(x) for x in re.findall(r'\b[wxsdqvhb]\d+(?:\.\w+)?(?:\[\d+\])?|\bw?sp\b', text)) if c]


def latency(op, t):
    if op.startswith(('fmul', 'fnmul', 'fmadd')): return LAT['fmul']
    if op.startswith(('fadd', 'fsub', 'fabs', 'fneg')): return LAT['fadd']
    if op.startswith(('fdiv', 'fsqrt')): return LAT['fdiv']
    if op.startswith(('fcvt', 'scvtf', 'ucvtf')): return LAT['fcvt']
    if op == 'fmov' and re.search(r'\b[wx]\d+', t): return LAT['fmov']
    if re.search(r'\b[sdqvh]\d+\b|\bv\d+\.', t): return LAT['simd']
    return LAT['alu']


def chain(ins):
    """Longest dependency chain walking the list once, over versioned definitions.

    Each instruction is a node with the time its result is ready and the node
    that made it wait, so the chain is a real path through the code."""
    node = {}                                  # n -> (ready time, predecessor node, how it was reached)
    defs = {}                                  # register -> defining node
    mem, val, fresh = {}, {}, [0]
    best = (0, None); c = collections.Counter()

    def ready(r):
        d = defs.get(r)
        return (node[d][0], d) if d is not None else (0, None)

    def vid(r):
        if r not in val: fresh[0] += 1; val[r] = (fresh[0], 0)
        return val[r]

    for n, t in enumerate(ins):
        op = t.split()[0]
        if op in ('ret', 'b', 'br') or op.startswith('b.') or op in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            continue
        m = A.parse_mem(t)
        if m:
            base = canon(m['base']); b = vid(base)
            idx = canon(m['index'].split()[0].rstrip(',')) if m['index'] else None
            i = vid(idx) if idx else (0, 0)
            key = (b[0], i[0]); lo = b[1] + i[1] + m['imm']; hi = lo + m['width']
            addr = max([ready(base)] + ([ready(idx)] if idx else []), key=lambda x: x[0])
            if m['store']:
                src = [ready(canon(r)) for r in m['regs'] if canon(r)]
                best_src = max(src + [addr], key=lambda x: x[0])
                node[n] = (best_src[0], best_src[1], 'store')
                mem.setdefault(key, []).append((lo, hi, n, m['width']))
                continue
            start, pred, how = addr[0] + LAT['load'], addr[1], 'load'
            c['loads'] += 1
            for s_lo, s_hi, s_n, s_w in reversed(mem.get(key, [])):
                if s_lo <= lo and hi <= s_hi:
                    how = 'hop' + ('' if s_w == hi - lo else f' {s_w}B>{hi-lo}B')
                    start, pred = max(addr[0], node[s_n][0]) + LAT['hop'], s_n
                    c['hops'] += 1
                    if s_w != hi - lo: c['narrowing hops'] += 1
                    break
                if s_lo < hi and lo < s_hi:
                    start, pred, how = max(addr[0], node[s_n][0]) + LAT['hazard'], s_n, 'hazard'
                    c['hazards'] += 1
                    break
            node[n] = (start, pred, how)
            for r in m['regs']:
                d = canon(r)
                if d: defs[d] = n; fresh[0] += 1; val[d] = (fresh[0], 0)
            if start > best[0]: best = (start, n)
            continue
        parts = [p.strip() for p in t[len(op):].split(',')]
        rs = regs_in(t[len(op):])
        if not rs: continue
        if op in ('cmp', 'cmn', 'tst', 'fcmp'): d, src = 'nzcv', rs
        else: d, src = rs[0], rs[1:]
        if op in ('ins', 'bfi', 'bfxil', 'movk') or (op == 'mov' and '[' in parts[0]): src = rs
        if op in ('csel', 'cset', 'csinc', 'csetm', 'fcsel', 'cinc'): src = src + ['nzcv']
        got = max([ready(s) for s in src] + [(0, None)], key=lambda x: x[0])
        r = got[0] + latency(op, t)
        node[n] = (r, got[1], op)
        defs[d] = n
        if op in ('subs', 'adds', 'ands'): defs['nzcv'] = n
        if op in ('add', 'sub') and len(parts) >= 3 and parts[2].startswith('#') and len(src) == 1:
            k = int(parts[2][1:], 0) << (12 if 'lsl #12' in t else 0)
            v = vid(src[0]); val[d] = (v[0], v[1] + (k if op == 'add' else -k))
        elif op == 'mov' and len(src) == 1 and d[0] == src[0][0] and '[' not in t: val[d] = vid(src[0])
        else: fresh[0] += 1; val[d] = (fresh[0], 0)
        if r > best[0]: best = (r, n)
    path, n, hops = [], best[1], collections.Counter()
    while n is not None:
        time, pred, how = node[n]
        path.append((n, ins[n], how, time))
        if how.startswith(('hop', 'hazard')): hops[how.split()[0]] += 1
        n = pred
    path.reverse()
    return dict(length=best[0], instructions=len(ins), loads=c['loads'], hops=c['hops'], narrowing=c['narrowing hops'],
                hazards=c['hazards'], path=path, hops_on_path=dict(hops))


def main():
    sym = sys.argv[1]
    for va in sys.argv[2:]:
        name = f'{int(va, 16):08X}'
        for label, s in (('lifted', f'ac1_translated_{name}'), ('native', f'ac1_native_{name}')):
            r = chain(code(sym, s))
            print(f'{name} {label:6s}: {r["instructions"]:4d} instructions, longest chain {r["length"]:4d} (nominal), {len(r["path"]):3d} steps, '
                  f'loads {r["loads"]}, hops {r["hops"]} ({r["narrowing"]} narrowing), hazards {r["hazards"]}, on the chain {r["hops_on_path"]}')
            pathlib.Path(ROOT/f'work/codegen_lab/audit/chain_{name}_{label}.txt').write_text(
                '\n'.join(f'{n:4d} t={tm:4d} {how:14s} {t}' for n, t, how, tm in r['path']))


if __name__ == '__main__':
    main()
