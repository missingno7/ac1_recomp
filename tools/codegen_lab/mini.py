"""One-function compile experiments with the exact production Android flags.

usage: python tools/codegen_lab/mini.py SNAPSHOT VA VARIANT[,VARIANT...] [--ir]
       python tools/codegen_lab/mini.py corpus SNAPSHOT VARIANT[,VARIANT...]

Extracts a function from a snapshot's generated unit into a small translation
unit (the unit's header plus that function), applies a named source variant,
and compiles it for ARM64 in a second or two, so no unit or game build is needed.
Results are cached by content under work/codegen_lab/mini. Variants are lab-only
text rewrites: they ask what LLVM can do given some information, and are not
correctness claims or lifter changes.
"""
import hashlib, json, re, subprocess, sys, pathlib
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'tools'), str(ROOT/'tools'/'codegen_lab')]
import codegen_lab as lab
import audit as A
OUT = lab.LAB/'mini'; OUT.mkdir(exist_ok=True)


def extract(snapshot, va):
    snap = lab.LAB/'snapshots'/snapshot
    man = json.loads((snap/'manifest.json').read_text())
    unit = next(c['unit'] for c in man['corpus'] if int(c['va'], 16) == va)
    src = (snap/'units'/unit).read_text(encoding='utf-8', errors='replace')
    head = src[:re.search(r'^(?:static )?void (?:L|ac1_translated)_[0-9A-F]{8}\(CPU \*c\)', src, re.M).start()]
    head = '\n'.join(l for l in head.split('\n') if not re.match(r'void L_[0-9A-F]{8}\(CPU \*\);', l))
    name = f'{va:08X}'
    m = re.search(rf'^(?:static )?void (?:ac1_translated|L)_{name}\(CPU \*c\)\n\{{.*?^\}}', src, re.M | re.S)
    body = m[0].replace('static void', 'void', 1)
    body = re.sub(r'^void (?:ac1_translated|L)_' + name, 'void audit_fn', body, flags=re.M)
    protos = ''.join(f'void {s}(CPU *);\n' for s in sorted(set(re.findall(r'\bL_[0-9A-F]{8}(?=[,(])', body))))
    return snap, man, head + protos, body


FIXED = r'''
/* Variant: guest stack addresses are 64-bit base + displacement (no 32-bit wrap). */
#define V_RD(name, type) static inline type v_##name(uint8_t *b, uint64_t a) { type v; memcpy(&v, b + a, sizeof v); return v; }
#define V_WR(name, type) static inline void v_##name(uint8_t *b, uint64_t a, type v) { memcpy(b + a, &v, sizeof v); }
V_RD(rd8, uint8_t) V_RD(rd16, uint16_t) V_RD(rd32, uint32_t) V_RD(rdss, float) V_RD(rdsd, double) V_RD(rdxm, XMM) V_RD(rdf32, float) V_RD(rdf64, double)
V_WR(wr8, uint8_t) V_WR(wr16, uint16_t) V_WR(wr32, uint32_t) V_WR(wrss, float) V_WR(wrsd, double) V_WR(wrxm, XMM) V_WR(wrf32, float) V_WR(wrf64, double)
#define V_ADDR(r, d) ((uint64_t)_ac1_regs.r + (d))
'''


def stack64(head, body):
    """Stack-based accesses compute their address in 64 bits; push/pop use it too."""
    n = [0]
    def rw(m):
        n[0] += 1
        d = int(m[3], 16)
        return f'v_{m[1]}(_ac1_local_arena, V_ADDR({m[2]}, {d - (1 << 32) if d >> 31 else d}){m[4]}'
    body = re.sub(r'\b(rd8|rd16|rd32|rdss|rdsd|rdxm|rdf32|rdf64|wr8|wr16|wr32|wrss|wrsd|wrxm|wrf32|wrf64)\(\(_ac1_regs\.(esp|ebp) \+ 0x([0-9A-F]+)u\)([,)])', rw, body)
    body = re.sub(r'\b(rd8|rd16|rd32|rdss|rdsd|rdxm|rdf32|rdf64|wr8|wr16|wr32|wrss|wrsd|wrxm|wrf32|wrf64)\(_ac1_regs\.(esp|ebp)([,)])',
                  lambda m: (n.__setitem__(0, n[0] + 1) or f'v_{m[1]}(_ac1_local_arena, V_ADDR({m[2]}, 0){m[3]}'), body)
    return head + FIXED, body, n[0]


SPLIT = r"""
/* Variant: 16-byte guest accesses are two 8-byte accesses, not a 16-byte memcpy. */
static inline void s_wrxm(uint8_t *b, uint32_t a, XMM v) {
    memcpy(b + a, &v.u64[0], 8); memcpy(b + (uint32_t)(a + 8u), &v.u64[1], 8); }
static inline XMM s_rdxm(uint8_t *b, uint32_t a) {
    XMM v; memcpy(&v.u64[0], b + a, 8); memcpy(&v.u64[1], b + (uint32_t)(a + 8u), 8); return v; }
#undef wrxm
#undef rdxm
#define wrxm(a, v) s_wrxm(_ac1_local_arena, (a), (v))
#define rdxm(a) s_rdxm(_ac1_local_arena, (a))
"""
VEC = r"""
/* Variant: 16-byte guest accesses are one unaligned vector access. */
typedef uint8_t v16 __attribute__((vector_size(16), aligned(1), may_alias));
static inline void s_wrxm(uint8_t *b, uint32_t a, XMM v) { v16 t; memcpy(&t, &v, 16); *(v16 *)(b + a) = t; }
static inline XMM s_rdxm(uint8_t *b, uint32_t a) { v16 t = *(v16 *)(b + a); XMM v; memcpy(&v, &t, 16); return v; }
#undef wrxm
#undef rdxm
#define wrxm(a, v) s_wrxm(_ac1_local_arena, (a), (v))
#define rdxm(a) s_rdxm(_ac1_local_arena, (a))
"""


def stack64_split(head, body, helper=None):
    h, b, n = stack64(head, body)
    h = h.replace('V_RD(rdxm, XMM) ', '').replace('V_WR(wrxm, XMM) ', '')
    h += r"""
static inline void v_wrxm(uint8_t *b, uint64_t a, XMM v) { memcpy(b + a, &v.u64[0], 8); memcpy(b + a + 8, &v.u64[1], 8); }
static inline XMM v_rdxm(uint8_t *b, uint64_t a) { XMM v; memcpy(&v.u64[0], b + a, 8); memcpy(&v.u64[1], b + a + 8, 8); return v; }
""" + SPLIT
    return h, b, n


def stack64_vec(head, body):
    h, b, n = stack64(head, body)
    h = h.replace('V_RD(rdxm, XMM) ', '').replace('V_WR(wrxm, XMM) ', '')
    h += VEC + r"""
static inline void v_wrxm(uint8_t *b, uint64_t a, XMM v) { v16 t; memcpy(&t, &v, 16); *(v16 *)(b + a) = t; }
static inline XMM v_rdxm(uint8_t *b, uint64_t a) { v16 t = *(v16 *)(b + a); XMM v; memcpy(&v, &t, 16); return v; }
"""
    return h, b, n


def lanefp(head, body):
    """Stack-forwarded lanes stay floats, when all four of a rebuild's lanes came from floats."""
    lanes = set(re.findall(r'(_ac1_lane_[0-9A-F]+) = ac1_f32_bits\(', body))
    other = set(re.findall(r'(_ac1_lane_[0-9A-F]+) = (?!ac1_f32_bits\()', body)) - lanes
    lanes -= other
    n = [0]
    def rebuild(m):
        names = [x.strip() for x in m[1].split(',')]
        if not all(x in lanes for x in names): return m[0]
        n[0] += 1
        return 'ac1_xmm_from_f32(' + ', '.join(names) + ')'
    used = {}
    b2 = re.sub(r'ac1_xmm_from_u32\(([^()]*)\)', rebuild, body)
    keep = {x for args in re.findall(r'ac1_xmm_from_u32\(([^;]*?)\);', b2) for x in re.findall(r'_ac1_lane_[0-9A-F]+', args)}
    fp = lanes - keep
    for x in fp:
        b2 = re.sub(rf'\b{x} = ac1_f32_bits\(([^;]*)\);', rf'{x} = \1;', b2)
        b2 = re.sub(rf'uint32_t {x}\b', f'float {x}', b2)
    h = head + r"""
static inline XMM ac1_xmm_from_f32(float a, float b, float c, float d) { XMM x; x.vf32 = (__typeof__(x.vf32)){a, b, c, d}; return x; }
"""
    return h, b2, n[0]


VARIANTS = {'base': lambda h, b: (h, b, 0), 'stack64': stack64, 'split': lambda h, b: (h + SPLIT, b, 0), 'lanefp': lanefp,
            'vector_access': lambda h, b: ('#define AC1_VECTOR_ACCESS\n' + h, b, 0),
            'vec': lambda h, b: (h + VEC, b, 0), 'stack64+split': stack64_split, 'stack64+vec': stack64_vec}


def compile_variant(snapshot, va, variant, ir=False):
    snap, man, head, body = extract(snapshot, va)
    head, body, sites = VARIANTS[variant](head, body)
    text = head + '\n' + body + '\n'
    flags = [f for f in man['android_flags'] if f not in ('-g', '-gline-tables-only')]
    key = hashlib.sha256((text + ' '.join(flags) + str(ir)).encode()).hexdigest()[:16]
    c = OUT/f'{va:08X}_{variant}_{key}.c'
    o = c.with_suffix('.ll' if ir else '.o')
    if not o.exists():
        c.write_text(text, encoding='utf-8')
        cmd = [str(lab.CLANG), *flags, '-I', str(snap/'runtime'), '-I', str(snap/'units')]
        cmd += ['-S', '-emit-llvm', '-fno-discard-value-names'] if ir else ['-c']
        r = subprocess.run(cmd + [str(c), '-o', str(o)], capture_output=True, text=True)
        if r.returncode: raise SystemExit(r.stderr[-1500:])
    return o, sites


def metrics(o):
    ins = lab.function_insns(o, {'audit_fn'})['audit_fn']
    c = dict(lab.arm64_metrics(ins))
    texts = [t for _, t in ins]
    c['fcvt'] = sum(t.startswith('fcvt') for t in texts)
    c['gpr-fp moves'] = sum(t.startswith('fmov') and bool(re.search(r'\b[wx]\d+', t)) for t in texts)
    c['store pairs of x'] = sum(bool(re.match(r'stp\s+x', t)) for t in texts)
    mem = [A.parse_mem(t) for t in texts]
    cpu = {'x0'} | {m[1] for m in (re.match(r'mov\s+(x\d+), x0$', t) for t in texts) if m}
    c['memory loads'] = sum(1 for m in mem if m and not m['store'] and m['base'] not in cpu and m['base'] != 'sp')
    c['memory stores'] = sum(1 for m in mem if m and m['store'] and m['base'] not in cpu and m['base'] != 'sp')
    c['bytes stored'] = sum(m['width'] for m in mem if m and m['store'] and m['base'] not in cpu and m['base'] != 'sp')
    c['cpu struct ops'] = sum(1 for m in mem if m and m['base'] in cpu)
    return c, texts


def corpus(snapshot, variants):
    """Every corpus function under each variant, one compile at a time; weighted totals and per-function changes."""
    man = json.loads((lab.LAB/'snapshots'/snapshot/'manifest.json').read_text())
    keys = ['instructions', 'memory loads', 'memory stores', 'bytes stored', 'cpu struct ops', 'stack loads', 'stack stores', 'fcvt', 'gpr-fp moves']
    rows, failed = {}, []
    for c in man['corpus']:
        va = int(c['va'], 16)
        try:
            r = {v: metrics(compile_variant(snapshot, va, v)[0])[0] for v in variants}
        except SystemExit as e:
            failed.append((c['va'], str(e)[:200])); continue
        rows[c['va']] = dict(weight=c['weight'], metrics={v: {k: r[v].get(k, 0) for k in keys} for v in variants})
        print('.', end='', flush=True)
    (OUT/'corpus.json').write_text(json.dumps(rows, indent=1))
    print(); print(f'{len(rows)} functions, {len(failed)} failed {failed[:3]}')
    W = sum(r['weight'] for r in rows.values())
    print(f'{"weighted mean":16s} ' + ' '.join(f'{v:>14s}' for v in variants))
    for k in keys:
        base = sum(r['weight'] * r['metrics'][variants[0]][k] for r in rows.values()) / W
        cells = [sum(r['weight'] * r['metrics'][v][k] for r in rows.values()) / W for v in variants]
        print(f'{k:16s} ' + ' '.join(f'{x:8.2f} {100*(x-base)/base if base else 0:+5.0f}%' for x in cells))
    for v in variants[1:]:
        d = sorted(((r['metrics'][v]['instructions'] - r['metrics'][variants[0]]['instructions'], va, r['metrics'][variants[0]]['instructions']) for va, r in rows.items()))
        worse = [x for x in d if x[0] > max(2, 0.02 * x[2])]
        print(f'{v}: improved {sum(x[0] < 0 for x in d)}, unchanged {sum(x[0] == 0 for x in d)}, worse by >2%: {len(worse)} {[(x[1], x[0]) for x in worse[-4:]]}; best {[(x[1], x[0]) for x in d[:3]]}')


def main():
    if sys.argv[1] == 'corpus':
        return corpus(sys.argv[2], sys.argv[3].split(','))
    snapshot, va, variants = sys.argv[1], int(sys.argv[2], 16), sys.argv[3].split(',')
    rows = {}
    for v in variants:
        o, sites = compile_variant(snapshot, va, v)
        rows[v], texts = metrics(o)
        (OUT/f'{va:08X}_{v}.s').write_text('\n'.join(texts), encoding='utf-8')
        if '--ir' in sys.argv: print(v, 'IR:', compile_variant(snapshot, va, v, ir=True)[0].name)
        print(f'{v}: {sites} accesses rewritten')
    keys = ['instructions', 'memory loads', 'memory stores', 'bytes stored', 'cpu struct ops', 'stack loads', 'stack stores',
            'branches', 'calls', 'fcvt', 'gpr-fp moves', 'store pairs of x']
    print(f'{"metric":20s} ' + ' '.join(f'{v:>13s}' for v in variants))
    for k in keys:
        print(f'{k:20s} ' + ' '.join(f'{rows[v].get(k, 0):13d}' for v in variants))


if __name__ == '__main__':
    main()
