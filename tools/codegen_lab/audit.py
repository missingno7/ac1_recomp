"""Compiler-quality audit: correlates decoded x86, generated C, GVN remarks and final AArch64.

usage: python tools/codegen_lab/audit.py SNAPSHOT SYMBOLS [--perf PERF.DATA --tid TID]

SYMBOLS is an unstripped library holding the snapshot's code (its line table
names the same generated units). Each ARM64 instruction of the snapshot's
corpus functions is tied to its C line, guest instruction and, when a profile
is given, its sampled cycles, then classified. Writes annotated listings and
summary.json under work/codegen_lab/audit/SNAPSHOT. Nothing is compiled.

Redundant loads are graded by what lies between a load and the earlier access
to the same address in the same superblock (no branch target in between):
  proven     nothing that can write it: no call, and every store between is
             the same base at a disjoint offset, or a CPU-struct store (the
             struct is outside the guest arena);
  suspected  a call, or a guest store through another base register, is
             between. Only an aliasing or frame-privacy argument could remove it.
Instruction counts are not cycles. Sampled cycles are attribution with skid.
"""
import argparse, collections, json, os, re, subprocess, sys, pathlib
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]; sys.path.insert(0, str(ROOT/'tools'))
import function_facts as F

NDK = pathlib.Path(os.environ['LOCALAPPDATA'])/'Android/Sdk/ndk/28.2.13676358'
BIN = NDK/'toolchains/llvm/prebuilt/windows-x86_64/bin'
SP = NDK/'simpleperf/bin/windows/x86_64/simpleperf.exe'
LAB = ROOT/'work/codegen_lab'
CALLER_SAVED = [str(k) for k in range(19)] + ['30']
MEM = re.compile(r'(ldp|stp|ldr\w*|str\w*|ldur\w*|stur\w*)\s+(.*)$')
BRANCH = re.compile(r'(?:b|b\.\w+|cbn?z|tbn?z|ret|br)\s')
SIZE = {'b': 1, 'h': 2, 's': 4, 'w': 4, 'd': 8, 'x': 8, 'q': 16}


def parse_mem(t):
    m = MEM.match(t)
    if not m or '[' not in m[2]: return None
    head, inner = m[2].split('[', 1)
    regs = [x.strip() for x in head.split(',') if x.strip()]
    tail = inner.split(']', 1)
    parts = [x.strip() for x in tail[0].split(',')]
    width = SIZE.get(regs[0][0], 4)
    if m[1].endswith(('rb', 'urb', 'rsb', 'ursb')): width = 1
    elif m[1].endswith(('rh', 'urh', 'rsh', 'ursh')): width = 2
    pair = m[1] in ('ldp', 'stp')
    index = parts[1] if len(parts) > 1 and not parts[1].startswith('#') else None
    imm = 0
    if len(parts) > 1 and parts[1].startswith('#'): imm = int(parts[1][1:], 0)
    post = tail[1].strip().startswith(',') or tail[1].strip().startswith('!')
    return dict(op=m[1], store=m[1].startswith('st'), regs=regs, base=parts[0], index=index, imm=imm,
                width=width * (2 if pair else 1), pair=pair, scaled='lsl' in tail[0], writeback=post)


def reg_id(r):
    r = r.strip()
    if r in ('sp', 'wsp'): return 'sp'
    m = re.fullmatch(r'[wx](\d+|zr)', r)
    return m[1] if m else None


class Function:
    """One function: ARM64 instructions with provenance, then classification."""

    def __init__(self, va, sym, ins):
        self.va, self.sym, self.ins = va, sym, ins        # ins: dicts with addr, text, line, gva, cline
        self.cpu = {'0'}

    def analyse(self):
        ins = self.ins
        targets = set()
        for i in ins:
            m = re.match(r'(?:b|b\.\w+|cbn?z|tbn?z)\s+(?:[^,]*,\s*)*0x([0-9a-f]+)', i['text'])
            if m: targets.add(int(m[1], 16))
        for i in ins:
            m = re.match(r'mov\s+x(19|2[0-8]), x(\d+)', i['text'])
            if m and m[2] in self.cpu: self.cpu.add(m[1])
        val, log, fresh = {}, [], [0]

        def new():
            fresh[0] += 1
            return (fresh[0], 0)

        def get(r):
            n = reg_id(r)
            if n is None or n == 'zr': return (0, 0)
            if n not in val: val[n] = new()
            return val[n]

        for pos, i in enumerate(ins):
            t = i['text']
            if i['addr'] in targets: val, log = {}, []
            mem = parse_mem(t)
            op = t.split()[0] if t else ''
            if mem:
                self.classify_mem(i, mem, pos)
                b, bo = get(mem['base'])
                if mem['index']:
                    idx = mem['index'].split()[0] if ' ' in mem['index'] else mem['index']
                    v, off = get(idx)
                    key, lo = (b, v, bo, mem['scaled']), off + mem['imm']
                else:
                    key, lo = (b, None, 0, False), bo + mem['imm']
                span = (lo, lo + mem['width'])
                if not mem['store']:
                    self.grade_load(i, key, span, log)
                else:
                    self.grade_store(i, key, span, log)
                log.append(dict(key=key, span=span, store=mem['store'], cls=i['cls'], pos=pos))
                if not mem['store']:
                    for r in mem['regs']:
                        if reg_id(r): val[reg_id(r)] = new()
                if mem['writeback'] and reg_id(mem['base']): val[reg_id(mem['base'])] = new()
                continue
            i['cls'] = 'branch' if BRANCH.match(t + ' ') else None
            if op in ('bl', 'blr'):
                i['cls'] = 'call'
                for r in CALLER_SAVED: val.pop(r, None)
                log.append(dict(key=None, span=(0, 0), store=True, cls='call', pos=pos))
                continue
            if i['cls'] == 'branch': continue
            parts = [x.strip() for x in t[len(op):].split(',')]
            d = reg_id(parts[0]) if parts and parts[0] else None
            if op in ('add', 'sub') and len(parts) >= 3 and parts[2].startswith('#') and reg_id(parts[1]):
                r0, o0 = get(parts[1])
                k = int(parts[2][1:], 0) << (12 if 'lsl #12' in t else 0)
                if d: val[d] = (r0, o0 + (k if op == 'add' else -k))
                i['cls'] = 'add imm 32' if parts[0].startswith('w') else 'add imm 64'
            elif op == 'mov' and len(parts) == 2 and reg_id(parts[1]) not in (None, 'zr'):
                if d: val[d] = get(parts[1])
                i['cls'] = 'move'
            else:
                if d: val[d] = new()
                i['cls'] = ('fmov gpr-fp' if op == 'fmov' and re.search(r'\b[wx]\d+', t) else
                            'fcvt' if op.startswith('fcvt') else
                            'fp/simd' if re.search(r'\b[sdqvh]\d+\b', t) else 'alu')
        self.find_patterns()

    def classify_mem(self, i, mem, pos):
        base, x86 = mem['base'], i.get('x86')
        if base == 'sp':
            frame = (mem['pair'] and pos < 14) or any(x['text'].startswith('ret') for x in self.ins[pos:pos+6])
            i['cls'] = 'host frame save' if frame else 'host spill'
        elif base[0] == 'x' and base[1:] in self.cpu and not mem['index']:
            off = mem['imm']
            f = ('gpr' if off < 0x20 else 'eip' if off < 0x24 else 'flags' if off < 0x48 else 'x87 st' if 0x50 <= off < 0x90
                 else 'x87 top' if off < 0x98 else 'xmm' if off < 0x120 else 'other')
            i['cls'] = 'cpu ' + f
        elif i.get('guest_kind'):
            i['cls'] = 'guest ' + i['guest_kind']
        elif mem['index']:
            i['cls'] = 'indexed'
        else:
            i['cls'] = 'other mem'
        i['cls'] += ' store' if mem['store'] else ' load'

    def grade_load(self, i, key, span, log):
        barrier = []
        for e in reversed(log):
            if e['key'] is None:
                barrier.append('call'); continue
            if e['key'] == key and e['span'][0] <= span[0] and span[1] <= e['span'][1]:
                i['redundant'] = dict(kind='forward' if e['store'] else 'repeat', source=e['pos'],
                                      grade='suspected' if barrier else 'proven', blockers=sorted(set(barrier)))
                return
            if not e['store']: continue
            if e['key'] == key:
                if e['span'][0] < span[1] and span[0] < e['span'][1]:
                    i['partial_overlap'] = e['pos']
                    return
                continue                                  # same base, disjoint offsets
            same_base = e['key'][0] == key[0] and e['key'][1:3] == key[1:3]
            if same_base: continue
            if e['cls'].startswith('cpu') != i['cls'].startswith('cpu'): continue   # the struct is outside the arena
            barrier.append('guest store' if e['cls'].startswith('guest') or e['cls'].startswith('indexed') else 'store')

    def grade_store(self, i, key, span, log):
        if not i['cls'].startswith('cpu'): return
        for e in reversed(log):
            if e['key'] is None: return
            if e['key'] == key and not e['store'] and e['span'][0] < span[1] and span[0] < e['span'][1]: return
            if e['key'] == key and e['store'] and span[0] <= e['span'][0] and e['span'][1] <= span[1]:
                self.ins[e['pos']]['dead_store'] = True
                return

    def find_patterns(self):
        """Patterns provable from adjacent machine code."""
        ins = self.ins
        def used_later(reg, start):
            """Is reg read after `start` before being redefined, within the next instructions?"""
            for j in range(start, min(start + 24, len(ins))):
                t = ins[j]['text']
                if BRANCH.match(t + ' ') or t.startswith(('bl', 'blr')): return True
                ops = t.split(None, 1)[1] if ' ' in t or '\t' in t else ''
                names = re.findall(r'\b[wx]' + reg + r'\b', ops)
                if not names: continue
                first = ops.split(',')[0]
                mem = parse_mem(t)
                defines = re.search(r'\b[wx]' + reg + r'\b', first) and not (mem and mem['store'])
                others = re.findall(r'\b[wx]' + reg + r'\b', ops.split(',', 1)[1]) if ',' in ops else []
                if defines and not others: return False
                return True
            return True
        for k, i in enumerate(ins):
            t = i['text']
            m = re.match(r'fmov\s+([wx])(\d+), ([sd])(\d+)$', t)
            if m:                                         # FP to GPR only to store it
                for j in range(k + 1, min(k + 10, len(ins))):
                    n = parse_mem(ins[j]['text'])
                    if n and n['store'] and not n['pair'] and n['regs'][0] == m[1] + m[2] and not used_later(m[2], j + 1):
                        i['pattern'] = 'fp value stored through gpr'
                        ins[j]['pattern'] = 'fp value stored through gpr (store)'
                        break
                    if reg_id(m[1] + m[2]) in re.findall(r'[wx](\d+)', ins[j]['text']): break
            m = re.match(r'fmov\s+([sd])(\d+), ([wx])(\d+)$', t)
            if m and k:                                   # GPR load only to move it to FP
                p = parse_mem(ins[k - 1]['text'])
                if p and not p['store'] and not p['pair'] and p['regs'][0] == m[3] + m[4] and not used_later(m[4], k + 1):
                    i['pattern'] = 'fp value loaded through gpr'
                    ins[k - 1]['pattern'] = 'fp value loaded through gpr (load)'
            m = re.match(r'(?:add|sub)\s+w(\d+), w(\d+), #', t)
            if m and k + 1 < len(ins):                    # 32-bit address arithmetic used only as an arena index
                for j in range(k + 1, min(k + 6, len(ins))):
                    n = parse_mem(ins[j]['text'])
                    if n and n['index'] and 'w' + m[1] == n['index'].split()[0].rstrip(','):
                        if not used_later(m[1], j + 1): i['pattern'] = 'address add only feeds a load/store'
                        break
                    if re.search(r'\b[wx]' + m[1] + r'\b', ins[j]['text']): break


def symbolize(sym, addrs):
    """Outermost generated-C line of each address (inlined accessors map to their call site)."""
    r = subprocess.run([str(BIN/'llvm-symbolizer.exe'), f'--obj={sym}', '--inlines'],
                       input='\n'.join(f'0x{a:x}' for a in addrs), capture_output=True, text=True).stdout
    out, chunks = {}, r.split('\n\n')
    for a, chunk in zip(addrs, chunks):
        frames = [l for l in chunk.strip().split('\n') if ':' in l]
        loc = None
        for f in reversed(frames):
            m = re.search(r'([^/\\]+\.c):(\d+)(?::\d+)?$', f)
            if m and int(m[2]) and m[1].startswith('initial_'):
                loc = (m[1], int(m[2])); break
        out[a] = loc
    return out


def samples(perf, sym, tid):
    if not perf: return {}
    dso = subprocess.run([str(SP), 'report', '-i', perf, '--sort', 'dso'], capture_output=True, text=True).stdout
    lib = re.search(r'/data/app/\S*/libac1_android\.so', dso)[0]
    rep = subprocess.run([str(SP), 'report', '-i', perf, '--tids', tid, '--dsos', lib, '--sort', 'vaddr_in_file', '--csv'],
                         capture_output=True, text=True).stdout
    import csv, io
    return {int(r['VaddrInFile'], 16): int(r['EventCount']) for r in csv.DictReader(io.StringIO(rep[rep.index('Overhead'):]))}


def source_lines(unit):
    """Per line of a generated unit: its guest VA (from the address comment) and sync role."""
    lines = unit.read_text(encoding='utf-8', errors='replace').split('\n')
    va, roles, last, last_fn = [0] * (len(lines) + 2), [''] * (len(lines) + 2), 0, False
    for n, text in enumerate(lines, 1):
        m = re.search(r'/\*\s*([0-9A-F]{8}):', text)
        if m: last = int(m[1], 16)
        va[n] = last
        code = text.split('/*')[0]
        roles[n] = ('entry sync' if '_ac1_regs.' in code and '= c->' in code and 'STACK_BASE' in lines[n - 2] else
                    'reload' if re.match(r'\s*(?:\{ uint32_t _ac1_v|_ac1_regs\.\w+ = c->)', code) else
                    'spill' if re.match(r'\s*c->\w+ = _ac1_regs\.', code) else
                    'x87 top' if re.match(r'\s*c->fpu_top = |\s*_ac1_x87_top = ', code) else
                    'call' if re.search(r'AC1_CALL\(|dispatch\(', code) else
                    'return' if 'return;' in code else '')
    return va, roles, lines


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('snapshot'); ap.add_argument('symbols')
    ap.add_argument('--perf'); ap.add_argument('--tid')
    a = ap.parse_args()
    snap = LAB/'snapshots'/a.snapshot
    manifest = json.loads((snap/'manifest.json').read_text())
    out = LAB/'audit'/a.snapshot; (out/'functions').mkdir(parents=True, exist_ok=True)
    cyc = samples(a.perf, a.symbols, a.tid)
    program = F.Program()
    sources = {}
    fns, skipped = [], collections.Counter()
    nm = subprocess.run([str(BIN/'llvm-nm.exe'), '--defined-only', '--print-size', a.symbols], capture_output=True, text=True).stdout
    table = {}
    for line in nm.split('\n'):
        p = line.split()
        if len(p) == 4 and re.fullmatch(r'(?:L|ac1_translated)_[0-9A-F]{8}', p[3]): table[p[3]] = (int(p[0], 16), int(p[1], 16))
    for c in manifest['corpus']:
        name = f'{int(c["va"], 16):08X}'
        sym = f'ac1_translated_{name}' if f'ac1_translated_{name}' in table and table[f'ac1_translated_{name}'][1] > 64 else f'L_{name}'
        if sym not in table: skipped['no symbol'] += 1; continue
        start, size = table[sym]
        dis = subprocess.run([str(BIN/'llvm-objdump.exe'), '-d', '--no-show-raw-insn', f'--start-address=0x{start:x}',
                              f'--stop-address=0x{start+size:x}', a.symbols], capture_output=True, text=True).stdout
        ins = [dict(addr=int(m[1], 16), text=re.sub(r'\s+', ' ', m[2].split('//')[0].split(';')[0].strip()))
               for m in re.finditer(r'^\s*([0-9a-f]+):\s+(.*)$', dis, re.M)]
        if not ins: skipped['no code'] += 1; continue
        loc = symbolize(a.symbols, [i['addr'] for i in ins])
        unit = c['unit']
        if unit not in sources: sources[unit] = source_lines(snap/'units'/unit)
        va_of, roles, text = sources[unit]
        decoded = program.decode(int(c['va'], 16)) if int(c['va'], 16) in program.functions else {}
        stack, escapes = F.stack_accesses(program, int(c['va'], 16)) if decoded else ({}, [])
        prev = None
        for i in ins:
            l = loc.get(i['addr']) or prev
            prev = l
            i['cycles'] = cyc.get(i['addr'], 0)
            if not l or l[0] != unit or l[1] >= len(text): i['line'] = None; continue
            i['line'], i['role'], i['c'] = l[1], roles[l[1]], text[l[1] - 1].strip()[:170]
            gva = va_of[l[1]]
            i['gva'] = gva
            x = decoded.get(gva)
            if x is not None:
                i['x86'] = f'{x.mnemonic} {x.op_str}'
                mem_op = any(op.type == 3 and not op.mem.segment for op in x.operands) and x.mnemonic != 'lea'
                if gva in stack or x.mnemonic in ('push', 'pop', 'call', 'ret'):
                    i['guest_kind'] = 'stack'
                elif re.search(r'\[e[sb]p\b', x.op_str) and mem_op:
                    i['guest_kind'] = 'stack'
                elif mem_op:
                    i['guest_kind'] = 'other'
        f = Function(c['va'], sym, ins); f.analyse()
        f.weight, f.escapes, f.stack_vas = c['weight'], escapes, len(stack)
        fns.append(f)
        with open(out/'functions'/f'{name}.txt', 'w', encoding='utf-8') as o:
            last = None
            for i in ins:
                if i.get('line') != last:
                    last = i.get('line')
                    o.write(f"\n; {i.get('gva', 0):08X}  {i.get('x86', '')}\n;   C {i.get('line')} [{i.get('role', '')}] {i.get('c', '')}\n")
                note = ' '.join(filter(None, [i.get('cls'), i.get('pattern'), 'DEAD' if i.get('dead_store') else '',
                                              str(i.get('redundant')) if i.get('redundant') else '']))
                o.write(f"  {i['addr']:x}: {i['text']:44s} {i['cycles']/1e6:7.1f}M  {note}\n")
    # Totals, weighted by each instruction's sampled cycles and by count.
    tot = collections.Counter(); cy = collections.Counter(); ex = {}
    def add(k, i, f):
        tot[k] += 1; cy[k] += i['cycles']
        ex.setdefault(k, []).append((i['cycles'], f.va, i['addr']))
    for f in fns:
        for i in f.ins:
            add('class: ' + str(i.get('cls')), i, f)
            if i.get('role'): add('c role: ' + i['role'], i, f)
            if i.get('pattern'): add('pattern: ' + i['pattern'], i, f)
            if i.get('dead_store'): add('dead cpu store', i, f)
            if 'partial_overlap' in i: add('load overlaps a narrower/wider store', i, f)
            r = i.get('redundant')
            if r:
                c = (i.get('cls') or '').rsplit(' ', 1)[0]
                add(f'redundant {r["grade"]}: {r["kind"]} {c}' + (f' ({"+".join(r["blockers"])})' if r['blockers'] else ''), i, f)
    all_cycles, all_ins = sum(cy[k] for k in cy if k.startswith('class: ')), sum(tot[k] for k in tot if k.startswith('class: '))
    doc = dict(functions=len(fns), skipped=dict(skipped), instructions=all_ins, cycles=all_cycles,
               escaping_frames=sum(bool(f.escapes) for f in fns),
               totals={k: dict(instructions=tot[k], cycles=cy[k], top=sorted(ex[k], reverse=True)[:5]) for k in tot})
    (out/'summary.json').write_text(json.dumps(doc, indent=1))
    print(f'{len(fns)} functions ({dict(skipped)}), {all_ins} instructions, {all_cycles/1e9:.2f} G sampled cycles')
    print(f'{"finding":70s} {"insns":>7s} {"%insn":>6s} {"%cyc":>6s}')
    for k in sorted(tot, key=lambda k: -cy[k]):
        print(f'{k[:70]:70s} {tot[k]:7d} {100*tot[k]/all_ins:6.2f} {100*cy[k]/max(all_cycles,1):6.2f}')


if __name__ == '__main__':
    main()
