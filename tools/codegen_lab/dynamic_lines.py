"""Cycles and backend stalls of the city profiles, attributed to the lifted code shape behind each sample.

usage: python tools/codegen_lab/dynamic_lines.py SYMBOLS SNAPSHOT RUN_DIR...   (city_run.sh with CITY_EVENTS=instructions:u,cpu-cycles:u,raw-stall-backend:u)
Each sample's address is mapped to its ARM64 instruction (llvm-objdump) and,
through the line table, to the generated C statement of the snapshot's units
(same generation as the profiled build). The statement and the instruction are
classified separately; exclusive counts of the main thread are summed per class.
Skid moves a stalled load's cycles to a nearby instruction, so the C statement
classes are the more reliable view.
"""
import collections, csv, io, json, os, re, shutil, subprocess, sys, pathlib

NDK = pathlib.Path(os.environ['LOCALAPPDATA'])/'Android/Sdk/ndk/28.2.13676358'
SP = NDK/'simpleperf/bin/windows/x86_64/simpleperf.exe'
BIN = NDK/'toolchains/llvm/prebuilt/windows-x86_64/bin'
EVENTS = ('instructions:u', 'cpu-cycles:u', 'raw-stall-backend:u')
sym, snap, runs = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), [pathlib.Path(p) for p in sys.argv[3:]]
out_dir = pathlib.Path('work/codegen_lab/experiments/dynamic_lines'); out_dir.mkdir(parents=True, exist_ok=True)

# 1. Samples per address, main thread.
counts = collections.defaultdict(collections.Counter)
for run in runs:
    data = run/'perf.data'
    dso = subprocess.run([str(SP), 'report', '-i', str(data), '--sort', 'dso'], capture_output=True, text=True).stdout
    lib = re.search(r'/data/app/\S*/libac1_android\.so', dso)[0]
    symfs = run/'symfs_lines'; shutil.rmtree(symfs, ignore_errors=True)
    (symfs/lib.lstrip('/')).parent.mkdir(parents=True); shutil.copy(sym, symfs/lib.lstrip('/'))
    tids = subprocess.run([str(SP), 'report', '-i', str(data), '--sort', 'tid', '--csv'], capture_output=True, text=True).stdout
    tid = next(csv.reader(io.StringIO(tids[tids.index('Overhead'):].split('\n', 1)[1])))[1]
    rep = subprocess.run([str(SP), 'report', '-i', str(data), '--symfs', str(symfs), '--tids', tid, '--dsos', lib,
                          '--sort', 'vaddr_in_file,symbol', '--csv'], capture_output=True, text=True).stdout
    shutil.rmtree(symfs)
    for block in re.split(r'^Event: ', rep, flags=re.M)[1:]:
        event = block.split()[0]
        for r in csv.DictReader(io.StringIO(block[block.index('Overhead'):])):
            counts[int(r['VaddrInFile'], 16)][event] += int(r['EventCount'])
            counts[int(r['VaddrInFile'], 16)]['symbol:' + r['Symbol']] = 1
total = collections.Counter()
for v in counts.values():
    for e in EVENTS: total[e] += v[e]

# 2. Instructions and lines for the sampled addresses of corpus functions.
units = {p.name: p for p in (snap/'units').glob('*.c')}
corpus = {c['va'][2:].upper() for c in json.loads((snap/'manifest.json').read_text())['corpus']}
def sym_of(v): return next((k[7:] for k in v if k.startswith('symbol:')), '')
fn_re = re.compile(r'^(?:L|ac1_translated)_([0-9A-F]{8})$')
game = sorted(a for a, v in counts.items() if fn_re.match(sym_of(v)))
picked = [a for a in game if fn_re.match(sym_of(counts[a]))[1] in corpus]
lines = subprocess.run([str(BIN/'llvm-symbolizer.exe'), f'--obj={sym}', '--no-inlines', '--output-style=GNU'],
                       input='\n'.join(f'0x{a:x}' for a in picked), capture_output=True, text=True).stdout.split('\n')
where = {}
# Line 0 (merged or hoisted code) takes the previous line of the same function.
last = {}
for a, i in zip(picked, range(0, len(lines), 2)):
    m = re.search(r'([^/\\:]+\.c):(\d+)', lines[i+1] if i+1 < len(lines) else '')
    s_ = sym_of(counts[a])
    if m and int(m[2]): last[s_] = (m[1], int(m[2]))
    where[a] = (m[1], int(m[2])) if m and int(m[2]) else last.get(s_)
syms = sorted({sym_of(counts[a]) for a in game})
dis = ''
for k in range(0, len(syms), 400):
    dis += subprocess.run([str(BIN/'llvm-objdump.exe'), '-d', '--no-show-raw-insn',
                           '--disassemble-symbols=' + ','.join(syms[k:k+400]), str(sym)], capture_output=True, text=True).stdout
insn = {int(m[1], 16): m[2].strip() for m in re.finditer(r'^\s*([0-9a-f]+):\s+(.*)$', dis, re.M)}
# The CPU pointer: the callee-saved register a function copies x0 into.
cpu_regs = {}
for m in re.finditer(r'^[0-9a-f]+ <(\w+)>:\n((?:.*\n)*?)(?=\n|\Z)', dis, re.M):
    cpu_regs[m[1]] = set(re.findall(r'mov\s+(x(?:19|2[0-8])), x0\b', m[2])) | {'x0'}

src_cache = {}
def statement(file, line):
    if file not in units: return None
    if file not in src_cache: src_cache[file] = units[file].read_text(encoding='utf-8', errors='replace').split('\n')
    lines_ = src_cache[file]
    return lines_[line-1] if 0 < line <= len(lines_) else None

def c_class(text):
    code = text.split('/*')[0]
    guest = re.findall(r'\b(?:ac1_base_)?(?:rd|wr)(?:8|16|32|64|xm|f32|f64|80)?\w*\(([^;]*)', code)
    if re.search(r'\bAC1_CALL\(|\bdispatch(?:_jmp)?\(', code): return 'call/dispatch + syncs'
    if re.search(r'\breturn;', code) and re.search(r'c->\w+ = _ac1_', code): return 'return syncs'
    if guest:
        stack = any(re.search(r'\besp\b|\bebp\b', g) for g in guest)
        return 'guest stack access' if stack else 'guest other access'
    if re.search(r'\bc->st\[', code): return 'x87 slot sync'
    if re.search(r'\bc->fpu_top\b', code) and not re.search(r'_ac1_st\d', code): return 'x87 TOP sync'
    if re.search(r'\bc->(?:st|top|fpu_\w+|x87\w*)\b|ac1_x87|\bfp_', code): return 'x87 bookkeeping'
    if re.search(r'_ac1_xmm|\bxmm\b|sse_|ac1_lane', code): return 'xmm compute'
    if re.search(r'flags_\w+\(|_ac1_f\.', code): return 'flags'
    if re.search(r'\bgoto\b|\bif\b', code): return 'branch'
    if re.search(r'AC1_SET_GUEST_EIP|\bc->eip\b', code): return 'eip'
    return 'gpr compute'

BRANCH = re.compile(r'(?:b|b\.\w+|cbn?z|tbn?z|ret|bl|blr|br)\s')
VREG = re.compile(r'\b[sdqvh]\d+\b')
def asm_class(t, cpu):
    m = re.match(r'(ld\w*|st\w*)\s+(.*)$', t)
    if not m:
        if BRANCH.match(t + ' '): return 'branch'
        if VREG.search(t): return 'fp/simd'
        if re.match(r'(?:add|sub)\s+w\d+, w\d+, #', t): return 'alu: 32-bit add/sub immediate'
        return 'alu: other'
    inner = re.search(r'\[([^\]]*)\]', m[2])
    if not inner: return 'alu'
    base = inner[1].split(',')[0].strip()
    rest = inner[1].split(',')[1:]
    indexed = bool(rest) and not rest[0].strip().startswith('#')
    kind = ('stack' if base == 'sp' else 'guest' if indexed and 'uxtw' in inner[1] else 'indexed (x87 slots)' if indexed and 'lsl #3' in inner[1]
            else 'indexed other' if indexed else 'cpu' if base in cpu else 'other mem')
    if kind == 'cpu':
        off = int(o[1], 0) if (o := re.search(r'#(-?0x[0-9a-f]+|-?\d+)', inner[1])) else 0
        kind = ('cpu gpr' if off < 0x20 else 'cpu eip' if off < 0x24 else 'cpu flags' if off < 0x48 else 'cpu x87 st' if 0x50 <= off < 0x90
                else 'cpu x87 top' if 0x90 <= off < 0x98 else 'cpu xmm' if 0x98 <= off < 0x120 else 'cpu other')
    return f'{kind} {"store" if m[1].startswith("st") else "load"}'

by_c, by_asm, by_fn, by_all, by_joint, by_fn_class = (collections.defaultdict(collections.Counter) for _ in range(6))
for a in game:
    v = counts[a]; s_ = sym_of(v); t = insn.get(a, '')
    ac = ('native ' if s_.startswith('ac1_translated_') else '') + (asm_class(t, cpu_regs.get(s_, {'x0'})) if t else 'unknown')
    for e in EVENTS: by_all[ac][e] += v[e]; by_fn_class[s_ + ' ' + ac][e] += v[e]
unmapped = collections.Counter()
for a in picked:
    v = counts[a]; s = sym_of(v); w = where.get(a)
    text = statement(*w) if w else None
    cc = c_class(text) if text else 'no source line'
    t = insn.get(a, '')
    ac = asm_class(t, cpu_regs.get(s, {'x0'})) if t else 'unknown'
    if s.startswith('ac1_translated_'): cc = 'native leaf'
    jc = ac
    if ac.startswith('guest') and text:
        jc = ac + (' (stack)' if cc == 'guest stack access' else ' (other)' if cc == 'guest other access' else ' (in ' + cc + ')')
    elif cc == 'native leaf': jc = 'native leaf'
    for e in EVENTS:
        by_c[cc][e] += v[e]; by_asm[ac][e] += v[e]; by_fn[s][e] += v[e]; by_joint[jc][e] += v[e]
corpus_cycles = sum(v['cpu-cycles:u'] for v in by_fn.values())
print(f'main thread: corpus functions hold {100*corpus_cycles/total["cpu-cycles:u"]:.1f}% of cycles '
      f'({len(by_fn)} functions sampled, {len(picked)} addresses)')
def table(title, d):
    print(f'\n{title} (share of corpus instructions / cycles / backend stalls, stall per cycle)')
    tot = {e: sum(v[e] for v in d.values()) for e in EVENTS}
    for k, v in sorted(d.items(), key=lambda kv: -kv[1]['cpu-cycles:u']):
        i, cy, st = (100 * v[e] / tot[e] for e in EVENTS)
        print(f'  {k:24s} {i:5.1f}% {cy:5.1f}% {st:5.1f}%   {v["raw-stall-backend:u"]/max(v["cpu-cycles:u"],1):.2f}')
game_cycles = sum(v['cpu-cycles:u'] for v in by_all.values())
print(f'all translated game code: {100*game_cycles/total["cpu-cycles:u"]:.1f}% of main-thread cycles, {len(syms)} functions')
table('all game code by ARM64 instruction', by_all)
table('corpus by lifted C statement', by_c)
table('corpus by ARM64 instruction, guest accesses split by their C statement', by_joint)
json.dump(dict(total=total, by_c=by_c, by_asm=by_asm, by_fn=by_fn, by_all=by_all, by_joint=by_joint, by_fn_class=by_fn_class), open(out_dir/'summary.json', 'w'), indent=1)
# Hottest lines for inspection.
hot = sorted(picked, key=lambda a: -counts[a]['cpu-cycles:u'])[:60]
with open(out_dir/'hot_lines.txt', 'w', encoding='utf-8') as f:
    for a in hot:
        v = counts[a]; w = where.get(a); text = statement(*w) if w else ''
        f.write(f'{100*v["cpu-cycles:u"]/corpus_cycles:5.2f}% stall {v["raw-stall-backend:u"]/max(v["cpu-cycles:u"],1):.2f} '
                f'{sym_of(v)} {insn.get(a, "")!s:40.40s} | {(text or "").strip()[:150]}\n')
print(f'\nhot lines: {out_dir/"hot_lines.txt"}')
