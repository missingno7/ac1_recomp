#!/usr/bin/env python3
"""Code-generation lab: measure the lifter as a compiler (docs/codegen-lab.md).

    profile  NAME --perf DATA --symbols SO      exclusive main-thread weights
    snapshot NAME --profile NAME                immutable inputs for a corpus
    build    SNAPSHOT VARIANT [--remarks]       ARM64 + i686 objects, benchmarks
    static   SNAPSHOT/VARIANT                   machine-code metrics
    bench    SNAPSHOT/VARIANT RUN [--no-x86]    correctness gate, counters, timing
    compare  A B [--aa A2]                      improvements and regressions

Everything lands under work/codegen_lab/ (ignored: it holds generated game
code and images). Snapshots, builds and runs are written once; a name that
exists is an error. Each has a manifest with source/binary hashes, compiler
versions, flags and switches. Raw tool outputs are kept beside the results.
"""
import argparse, collections, csv, glob, hashlib, io, json, os, pathlib, re, shutil, statistics, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parents[1]
LAB = ROOT/'work/codegen_lab'
CONFIG = json.loads((ROOT/'config/codegen_lab.json').read_text(encoding='utf-8'))
SDK = pathlib.Path(os.environ['LOCALAPPDATA'])/'Android/Sdk'
NDK = SDK/'ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64'
CLANG, OBJDUMP = NDK/'bin/clang.exe', NDK/'bin/llvm-objdump.exe'
SIMPLEPERF = SDK/'ndk/28.2.13676358/simpleperf/bin/windows/x86_64/simpleperf.exe'
ADB = SDK/'platform-tools/adb.exe'
MINGW32 = pathlib.Path('C:/msys64/mingw32')
GAME_EXE = pathlib.Path(r'C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe')
DEVICE = 'R5CX41MRV6V'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    return h.hexdigest()


def new_dir(path):
    if path.exists(): raise SystemExit(f'{path.relative_to(ROOT)} exists; lab results are immutable, choose another name')
    path.mkdir(parents=True)
    return path


def run(argv, **kw):
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, **kw)


def checked(argv, **kw):
    r = run(argv, **kw)
    if r.returncode: raise SystemExit(f'failed: {" ".join(map(str, argv))[:300]}\n{r.stdout[-2000:]}{r.stderr[-2000:]}')
    return r


def mingw_env():
    return dict(os.environ, PATH=str(MINGW32/'bin') + os.pathsep + os.environ['PATH'])


def version(argv, env=None):
    return run(argv, env=env).stdout.splitlines()[0].strip()


def va_of(symbol):
    m = re.fullmatch(r'_?(?:L|ac1_translated)_([0-9A-F]{8})', symbol)
    return m and int(m[1], 16)


# ---------------------------------------------------------------- profile

def cmd_profile(a):
    """Exclusive (self) cycles per guest function on the busiest thread.

    A flat report counts each sample once, at its leaf frame, so summing these
    never double-counts inclusive time; the L_ wrapper and ac1_translated_ body
    of one guest function are added together."""
    out = new_dir(LAB/'profiles'/a.name)
    perf, symbols = pathlib.Path(a.perf), pathlib.Path(a.symbols)
    dso = checked([SIMPLEPERF, 'report', '-i', perf, '--sort', 'dso']).stdout
    lib = re.search(r'/data/app/\S*/libac1_android\.so', dso)[0]
    symfs = out/'symfs'
    (symfs/lib.lstrip('/')).parent.mkdir(parents=True)
    shutil.copy(symbols, symfs/lib.lstrip('/'))
    tids = checked([SIMPLEPERF, 'report', '-i', perf, '--sort', 'tid', '--csv']).stdout
    rows = list(csv.reader(io.StringIO(tids[tids.index('Overhead'):])))
    tid = a.tid or rows[1][1]
    rep = checked([SIMPLEPERF, 'report', '-i', perf, '--symfs', symfs, '--tids', tid, '--sort', 'symbol', '--csv']).stdout
    (out/'report.csv').write_text(rep, encoding='utf-8')
    total, weights = 0, collections.Counter()
    for r in csv.DictReader(io.StringIO(rep[rep.index('Overhead'):])):
        n = int(r['EventCount']); total += n
        va = va_of(r['Symbol'])
        if va: weights[va] += n
    shutil.rmtree(symfs)
    doc = dict(name=a.name, perf=str(perf), perf_sha256=sha256(perf), symbols_sha256=sha256(symbols), thread=tid,
               event='cpu-cycles', total=total, kind='exclusive',
               functions={f'0x{va:08X}': n for va, n in weights.most_common()})
    (out/'profile.json').write_text(json.dumps(doc, indent=1), encoding='utf-8')
    print(f'{len(weights)} guest functions, {100*sum(weights.values())/total:.1f}% of thread {tid}; top: ' +
          ', '.join(f'0x{va:08X} {100*n/total:.2f}%' for va, n in weights.most_common(5)))


# ---------------------------------------------------------------- snapshot

def android_flags():
    """Exact compile arguments of a generated unit in the last Android build."""
    found = sorted(ROOT.glob('android/app/.cxx/*/*/arm64-v8a/compile_commands.json'), key=os.path.getmtime)
    if not found: raise SystemExit('no Android compile_commands.json: run tools/build_android.ps1 first')
    for entry in json.loads(found[-1].read_text()):
        if re.search(r'initial_\d{4}\.c$', entry['file']):
            argv = entry.get('arguments') or entry['command'].split()
            break
    keep, skip = [], False
    for arg in argv[1:]:
        if skip: skip = False; continue
        if arg in ('-o', '-c', '-MF', '-MT'): skip = True; continue
        if arg.startswith('-I') or arg == '-MD' or arg.endswith('.c'): continue
        keep.append(arg)
    return keep, str(found[-1].relative_to(ROOT))


def extract_image(out):
    sys.path.insert(0, 'D:/Prog/dukemp_recomp/.venv/Lib/site-packages')
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from capstone.x86 import X86_OP_MEM, X86_OP_IMM
    pe = pefile.PE(str(GAME_EXE), fast_load=True)
    base, size = pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.SizeOfImage
    blob = bytearray(size)
    for s in pe.sections:
        data = s.get_data(); blob[s.VirtualAddress:s.VirtualAddress+len(data)] = data[:max(0, size - s.VirtualAddress)]
    (out/'image.bin').write_bytes(bytes(blob))
    catalog = json.loads((ROOT/'work/lift_initial/functions.json').read_text())
    funcs = {f['address']: f for f in (catalog['functions'] if isinstance(catalog, dict) else catalog)}
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
    offsets, ends = [], []
    for leaf in CONFIG['leaves']:
        va = int(leaf, 16); end = funcs[va]['end']; ends.append(end)
        for insn in md.disasm(bytes(blob[va-base:end-base]), va):
            branch = insn.group(1) or insn.group(2)
            if insn.mnemonic.startswith('call'): raise SystemExit(f'{leaf}: call at 0x{insn.address:08X}; not a leaf')
            for op in insn.operands:
                if op.type == X86_OP_MEM and op.mem.base == 0 and base <= (op.mem.disp & 0xffffffff) < base + size:
                    offsets.append(insn.address - base + insn.disp_offset)
                if op.type == X86_OP_IMM and not branch and base <= (op.imm & 0xffffffff) < base + size:
                    raise SystemExit(f'{leaf}: address immediate at 0x{insn.address:08X}')
    (out/'lab_patches.h').write_text(
        '/* generated by tools/codegen_lab.py: original leaves and their absolute data operands */\n'
        f'static const uint32_t leaf_start[] = {{{", ".join(l + "u" for l in CONFIG["leaves"])}}};\n'
        f'static const uint32_t leaf_end[] = {{{", ".join(f"0x{e:08X}u" for e in ends)}}};\n'
        f'static const uint32_t disp_offsets[] = {{{", ".join(f"0x{o:X}u" for o in offsets)}}};\n', encoding='utf-8')
    return dict(exe_sha256=sha256(GAME_EXE), image_base=f'0x{base:08X}', patched_operands=len(offsets))


def cmd_snapshot(a):
    out = new_dir(LAB/'snapshots'/a.name)
    profile = json.loads((LAB/'profiles'/a.profile/'profile.json').read_text())
    leaves = [int(v, 16) for v in CONFIG['leaves']]
    hot = [int(v, 16) for v in profile['functions']][:a.top or CONFIG['hot_top']]
    corpus = list(dict.fromkeys(leaves + hot))
    units = {}
    for path in sorted((ROOT/'work/lift_initial').glob('initial_[0-9][0-9][0-9][0-9].c')):
        text = path.read_text(encoding='utf-8')
        for va in corpus:
            if va not in units and re.search(rf'^(?:static )?void (?:L|ac1_translated)_{va:08X}\(CPU \*c\)$', text, re.M):
                units[va] = path.name
    missing = [f'0x{va:08X}' for va in corpus if va not in units]
    if missing: raise SystemExit('no generated body for ' + ', '.join(missing))
    (out/'units').mkdir(); (out/'runtime').mkdir(); (out/'x86').mkdir()
    for name in sorted(set(units.values())): shutil.copy(ROOT/'work/lift_initial'/name, out/'units'/name)
    for h in (ROOT/'work/lift_initial').glob('*.h'): shutil.copy(h, out/'units'/h.name)
    for f in [*(ROOT/'src/runtime').glob('*.h'), ROOT/'src/runtime/ac1_native_leaf.c']: shutil.copy(f, out/'runtime'/f.name)
    image = extract_image(out/'x86')
    flags, flags_source = android_flags()
    report = json.loads((ROOT/'work/lift_initial/lift-report.json').read_text())
    git = run(['git', '-C', ROOT, 'rev-parse', 'HEAD']).stdout.strip()
    dirty = run(['git', '-C', ROOT, 'status', '--porcelain', '--', 'tools/lift_ac1.py', 'tools/prepare_runtime.py',
                 'config/ac1_dx9.json', 'src/runtime']).stdout.split('\n')
    total = profile['total']
    manifest = dict(
        name=a.name, created=time.strftime('%Y-%m-%d %H:%M:%S'), git_head=git, dirty=[d for d in dirty if d],
        lift_options=dict(flag_liveness=report.get('flag_liveness'), x87=report.get('x87'), xmm_abi=report.get('xmm_abi', 'off'),
                          local_regs=report.get('local_regs', {}).get('requested')),
        android_flags=flags, android_flags_source=flags_source,
        profile=dict(name=a.profile, sha256=sha256(LAB/'profiles'/a.profile/'profile.json'), kind=profile['kind']),
        corpus=[dict(va=f'0x{va:08X}', unit=units[va], leaf=va in leaves,
                     weight=profile['functions'].get(f'0x{va:08X}', 0) / total) for va in corpus],
        image=image,
        files={str(p.relative_to(out)).replace('\\', '/'): sha256(p) for p in sorted(out.rglob('*')) if p.is_file()})
    (out/'manifest.json').write_text(json.dumps(manifest, indent=1), encoding='utf-8')
    w = sum(c['weight'] for c in manifest['corpus'])
    print(f'snapshot {a.name}: {len(corpus)} functions in {len(set(units.values()))} units, '
          f'{100*w:.1f}% of main-thread cycles (exclusive); git {git[:8]}{" dirty" if manifest["dirty"] else ""}')


# ---------------------------------------------------------------- build

def x86_flags(android):
    drop = re.compile(r'--target=|--sysroot=|-march=|-fPIC|-fstack-protector|-D_FORTIFY_SOURCE|-DANDROID|-no-canonical-prefixes|-funwind-tables|-pthread')
    return ['--target=i686-w64-windows-gnu', f'--sysroot={MINGW32.as_posix()}', '-msse2', '-mfpmath=sse',
            *[f for f in android if not drop.match(f)]]


def cmd_build(a):
    snap = LAB/'snapshots'/a.snapshot
    manifest = json.loads((snap/'manifest.json').read_text())
    variant = CONFIG['variants'][a.variant]
    out = new_dir(LAB/'builds'/a.snapshot/a.variant)
    arm, x86 = manifest['android_flags'] + variant['defines'], x86_flags(manifest['android_flags']) + variant['defines']
    inc = ['-I', snap/'runtime', '-I', snap/'units']
    (out/'arm64').mkdir(); (out/'x86').mkdir()
    if a.remarks: (out/'remarks').mkdir()
    procs = []
    for unit in sorted((snap/'units').glob('initial_*.c')):
        remarks = ['-fsave-optimization-record=yaml', f'-foptimization-record-file={out/"remarks"/(unit.stem + ".yaml")}',
                   '-foptimization-record-passes=gvn'] if a.remarks else []
        procs.append(subprocess.Popen([str(x) for x in [CLANG, *arm, *remarks, *inc, '-c', unit, '-o', out/'arm64'/(unit.stem + '.o')]]))
        procs.append(subprocess.Popen([str(x) for x in [CLANG, *x86, *inc, '-c', unit, '-o', out/'x86'/(unit.stem + '.o')]]))
        while sum(p.poll() is None for p in procs) >= 8: time.sleep(0.2)
    if any(p.wait() for p in procs): raise SystemExit('a unit failed to compile')
    # Benchmarks run the leaves' code exactly as the unit build compiled it: each
    # leaf's function section (with -ffunction-sections, one per function), the
    # unit's read-only data and their relocations, renamed lifted_<VA>.
    # Native references and harnesses use the snapshot's base flags.
    bench = out/'bench'; bench.mkdir()
    gcc, gnu_objcopy = MINGW32/'bin/gcc.exe', MINGW32/'bin/objcopy.exe'
    arm_leaves, x86_leaves, extracted = [], [], {}
    for leaf in manifest['corpus']:
        if not leaf['leaf']: continue
        name, unit = f'{int(leaf["va"], 16):08X}', pathlib.Path(leaf['unit']).stem
        syms = function_insns(out/'arm64'/(unit + '.o'), {f'ac1_translated_{name}', f'L_{name}'})
        sym = f'ac1_translated_{name}' if f'ac1_translated_{name}' in syms else f'L_{name}'
        dst = bench/f'leaf_{name}_arm64.o'
        checked([NDK/'bin/llvm-objcopy.exe', f'--only-section=.text.{sym}', f'--only-section=.rela.text.{sym}',
                 '--only-section=.rodata*', f'--globalize-symbol={sym}', '--redefine-sym', f'{sym}=lifted_{name}',
                 out/'arm64'/(unit + '.o'), dst])
        # COFF: dropping sections makes GNU objcopy point relocations at the wrong
        # symbols, so the whole unit object is linked with only the leaf renamed;
        # what it references and nobody defines gets a trapping stub below.
        dst86 = bench/f'unit_{name}_x86.o'
        checked([gnu_objcopy, '--redefine-sym', f'_{sym}=_lifted_{name}', '--globalize-symbol', f'_lifted_{name}',
                 out/'x86'/(unit + '.o'), dst86], env=mingw_env())
        arm_leaves.append(dst); x86_leaves.append(dst86)
        extracted[leaf['va']] = dict(symbol=sym, arm64_insns=len(syms[sym]),
                                     x86_insns=len(function_insns(dst86, {f'lifted_{name}'}).get(f'lifted_{name}', [])))
    checked([CLANG, *manifest['android_flags'], *inc, '-c', snap/'runtime/ac1_native_leaf.c', '-o', bench/'native_arm64.o'])
    checked([CLANG, *manifest['android_flags'], *inc, '-c', ROOT/'tools/codegen_lab/bench_arm64.c', '-o', bench/'harness_arm64.o'])
    link = [f for f in manifest['android_flags'] if f.startswith(('--target', '--sysroot'))]
    checked([CLANG, *link, '-o', bench/'bench_arm64', *arm_leaves, bench/'native_arm64.o', bench/'harness_arm64.o', '-lm'])
    nm = MINGW32/'bin/nm.exe'
    defined, undefined = set(), set()
    for o in x86_leaves:
        for line in checked([nm, o], env=mingw_env()).stdout.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == 'U': undefined.add(parts[1])
            elif len(parts) == 3 and parts[1].isupper(): defined.add(parts[2])
    provided = {'_ac1_direct_arena', '_guest_execution_read', '_guest_execution_write', '_ac1_unsupported_instruction'}
    stubs = sorted(sym[1:] for sym in undefined - defined - provided if sym.startswith('_'))
    (bench/'stubs_x86.c').write_text('/* generated: never called by the leaves */\n' +
                                     ''.join(f'void {sym}(void) {{ __builtin_trap(); }}\n' for sym in stubs), encoding='utf-8')
    harness_defines = [f for f in x86 if f.startswith('-D')]
    checked([gcc, '-O2', '-msse2', '-mfpmath=sse', *harness_defines, '-I', snap/'runtime', '-I', snap/'x86',
             ROOT/'tools/codegen_lab/roundtrip_x86.c', bench/'stubs_x86.c', *x86_leaves, '-Wl,--large-address-aware',
             '-o', bench/'roundtrip_x86.exe'], env=mingw_env())
    doc = dict(snapshot=a.snapshot, snapshot_manifest_sha256=sha256(snap/'manifest.json'), variant=a.variant,
               defines=variant['defines'], note=variant.get('note'), arm64_flags=arm, x86_flags=x86,
               compilers=dict(clang=version([CLANG, '--version']), mingw_gcc=version([gcc, '--version'], mingw_env())),
               benchmark_leaves=extracted,
               files={str(p.relative_to(out)).replace('\\', '/'): sha256(p) for p in sorted(out.rglob('*')) if p.is_file()})
    (out/'manifest.json').write_text(json.dumps(doc, indent=1), encoding='utf-8')
    print(f'build {a.snapshot}/{a.variant}: {len(procs)//2} units for ARM64 and i686; benchmark leaves (instructions ARM64/x86): '
          + ', '.join(f'{va} {e["arm64_insns"]}/{e["x86_insns"]}' for va, e in extracted.items()))


# ---------------------------------------------------------------- static analysis

MEM = re.compile(r'(ldr|ldur|ldp|str|stur|stp|ldrb|ldrh|strb|strh|ldurb|ldurh|sturb|sturh|ldrsw|ldrsh|ldrsb|ldursw)\s+(.*)$')
WIDTH = {'b': 1, 'h': 2, 's': 4, 'w': 4, 'd': 8, 'x': 8, 'q': 16}


def function_insns(obj, wanted):
    dis = run([OBJDUMP, '-d', '--no-show-raw-insn', *(['-M', 'intel'] if 'x86' in str(obj) else []), obj]).stdout
    out = {}
    for m in re.finditer(r'^[0-9a-f]+ <(\S+?)>:\n(.*?)(?=^[0-9a-f]+ <|\Z)', dis, re.M | re.S):
        name = m[1].lstrip('_') if m[1].startswith('_') and m[1][1:] in wanted else m[1]
        if name in wanted:
            out[name] = [(int(x[1], 16), x[2].split('//')[0].split('#')[0].strip() if 'x86' in str(obj) else x[2].split('//')[0].strip())
                         for x in re.finditer(r'^\s*([0-9a-f]+):\s+(.*)$', m[2], re.M)]
    return out


def arm64_metrics(ins):
    """Counts plus block-local redundancy, tracking address registers as root+offset."""
    c = collections.Counter(instructions=len(ins))
    targets = {int(t, 16) for _, t in ins for t in re.findall(r'^(?:b|b\.\w+|cbn?z|tbn?z)\s+(?:[^,]*,\s*)*0x([0-9a-f]+)', t)}
    ids = [0]; val = {}; log = []; arena = set()
    reg = lambda r: (None if not re.fullmatch(r'[wx](\d+|zr)|w?sp', r) else 'sp' if 'sp' in r else re.fullmatch(r'[wx](\d+|zr)', r)[1])
    imm = lambda t: int(m[1], 0) if (m := re.search(r'#(-?0x[0-9a-f]+|-?\d+)', t)) else 0
    def get(r):
        n = reg(r)
        if n in (None, 'zr'): return (0, 0)
        if n not in val: ids[0] += 1; val[n] = (ids[0], 0)
        return val[n]
    for addr, t in ins:
        if addr in targets: val, log = {}, []
        op = t.split()[0] if t else ''
        m = MEM.match(t)
        if m:
            mnem, operands = m[1], m[2]
            regs = [x.strip() for x in operands.split('[')[0].split(',') if x.strip()]
            width = WIDTH.get(regs[0][0], 4) if regs else 4
            if mnem in ('ldrb', 'strb', 'ldurb', 'sturb', 'ldrsb'): width = 1
            if mnem in ('ldrh', 'strh', 'ldurh', 'sturh', 'ldrsh'): width = 2
            if mnem in ('ldp', 'stp'): width *= 2
            inner = [x.strip() for x in re.search(r'\[([^\]]*)\]', operands)[1].split(',')]
            idx = inner[1] if len(inner) > 1 and not inner[1].startswith('#') else None
            b, bo = get(inner[0])
            if idx:
                if 'uxtw' in operands: arena.add(b)
                i, io = get(idx); key, lo = (b, i), bo + io
            else:
                key, lo = (b, None), bo + imm(','.join(inner[1:]))
            kind = 'stack' if reg(inner[0]) == 'sp' else 'guest' if idx or b in arena else 'struct'
            store = mnem.startswith('st')
            c[f'{kind} {"stores" if store else "loads"}'] += 1
            span = (lo, lo + width)
            if not store:
                for k2, r2, st2 in reversed(log):
                    if k2 == 'call': break
                    if k2 == key and r2[0] <= span[0] and span[1] <= r2[1]:
                        c['missed forwards' if st2 else 'repeated loads'] += 1; break
            else:
                for k2, r2, st2 in reversed(log):
                    if k2 == 'call': break
                    if k2 == key and not st2 and r2[0] < span[1] and span[0] < r2[1]: break
                    if k2 == key and st2 and span[0] <= r2[0] and r2[1] <= span[1]: c['dead stores'] += 1; break
            log.append((key, span, store))
            if not store:
                for r in regs[:2 if mnem == 'ldp' else 1]:
                    if reg(r) is not None: ids[0] += 1; val[reg(r)] = (ids[0], 0)
            continue
        if op in ('bl', 'blr'):
            c['calls'] += 1
            for n in [str(k) for k in range(19)] + ['30']: val.pop(n, None)
            log.append(('call', (0, 0), True)); continue
        if op in ('b', 'br', 'ret') or op.startswith('b.') or op in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            c['branches'] += 1
            if op in ('b', 'br', 'ret'): val, log = {}, []
            continue
        parts = [x.strip() for x in t[len(op):].split(',')]
        d = reg(parts[0]) if parts and parts[0] else None
        if d is None: continue
        if op in ('add', 'sub') and len(parts) >= 3 and parts[2].startswith('#') and reg(parts[1]) is not None:
            r0, o0 = get(parts[1]); k = imm(parts[2]) << (12 if 'lsl #12' in t else 0)
            val[d] = (r0, o0 + (k if op == 'add' else -k))
            if parts[0].startswith('w'): c['32-bit address adds'] += 1
        elif op == 'mov' and len(parts) == 2 and reg(parts[1]) not in (None, 'zr'):
            val[d] = get(parts[1])
        else:
            ids[0] += 1; val[d] = (ids[0], 0)
    return c


def x86_metrics(ins):
    c = collections.Counter(instructions=len(ins))
    for _, t in ins:
        op = t.split()[0] if t else ''
        if '[' in t:
            dst_mem = re.match(r'\w+\s+\w+ ptr \[', t) is not None or re.match(r'\w+\s+\[', t) is not None
            c['memory stores' if dst_mem and not op.startswith(('cmp', 'test')) else 'memory loads'] += 1
            if re.search(r'\[esp|\[ebp', t): c['host stack accesses'] += 1
        if op in ('push', 'pop'): c['push/pop'] += 1
        if op == 'call': c['calls'] += 1
        if op.startswith('j'): c['branches'] += 1
    return c


def cmd_static(a):
    build = LAB/'builds'/a.build
    bm = json.loads((build/'manifest.json').read_text())
    snap = json.loads((LAB/'snapshots'/bm['snapshot']/'manifest.json').read_text())
    result = {}
    for f in snap['corpus']:
        name = f'{int(f["va"], 16):08X}'; unit = pathlib.Path(f['unit']).stem
        wanted = {f'ac1_translated_{name}', f'L_{name}'}
        row = dict(weight=f['weight'], leaf=f['leaf'])
        for arch, metrics in (('arm64', arm64_metrics), ('x86', x86_metrics)):
            fns = function_insns(build/arch/(unit + '.o'), wanted)
            body = fns.get(f'ac1_translated_{name}') or fns.get(f'L_{name}') or []
            row[arch] = dict(metrics(body))
        result[f['va']] = row
    if (build/'remarks').exists(): result['_remarks'] = remark_blockers(build/'remarks', {f'L_{int(f["va"], 16):08X}' for f in snap['corpus']} |
                                                                   {f'ac1_translated_{int(f["va"], 16):08X}' for f in snap['corpus']})
    (build/'static.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    for arch in ('arm64', 'x86'):
        total = collections.Counter()
        for va, row in result.items():
            if va.startswith('_'): continue
            for k, v in row[arch].items(): total[k] += row['weight'] * v
        print(f'{arch}: ' + ', '.join(f'{k} {v:.1f}' for k, v in sorted(total.items())) + '  (profile-weighted per 1.0 of thread time)')
    if '_remarks' in result:
        print('GVN loads not eliminated, by clobber: ' + ', '.join(f'{k} {v}' for k, v in list(result['_remarks'].items())[:6]))


def remark_blockers(remarks_dir, wanted):
    """Group GVN LoadClobbered remarks in corpus functions by what clobbered the load."""
    by = collections.Counter(); sources = {}
    for f in glob.glob(f'{remarks_dir}/*.yaml'):
        for doc in open(f, encoding='utf-8', errors='replace').read().split('\n--- !'):
            m = re.search(r'^Function:\s+(\S+)', doc, re.M)
            if not m or m[1] not in wanted or 'LoadClobbered' not in doc: continue
            c = re.search(r"ClobberedBy:\s+(\w+)\s+DebugLoc:\s+\{ File: '([^']*)',\s*Line: (\d+),\s*Column: (\d+)", doc, re.S)
            if not c: by['(unlocated)'] += 1; continue
            if 'initial_' not in c[2]: by[f'{c[1]} in {pathlib.Path(c[2]).name}:{c[3]}'] += 1; continue
            text = sources.setdefault(c[2], open(c[2], encoding='utf-8', errors='replace').read().splitlines())[int(c[3]) - 1]
            word = re.match(r'(\w+)', text[int(c[4]) - 1:])
            by[f'{c[1]} {word[1] if word else "?"}'] += 1
    return dict(by.most_common())


# ---------------------------------------------------------------- isolated benchmarks

def adb(*args, timeout=600):
    return run([ADB, '-s', DEVICE, *args], timeout=timeout)


def shell(cmd, timeout=600):
    return adb('shell', cmd, timeout=timeout)


def parse_stat(text):
    counts = {}
    for m in re.finditer(r'^\s*([\d,]+)\s+(\S+)', text, re.M):
        counts[m[2]] = int(m[1].replace(',', ''))
    return counts


def cmd_bench(a):
    build = LAB/'builds'/a.build
    bm = json.loads((build/'manifest.json').read_text())
    game = ''
    if not a.no_arm64:
        # Someone playing owns the phone; a game left in the background is
        # paused (presents wait for a surface) and is recorded, not touched.
        if 'org.ac1.recomp' in shell('dumpsys window | grep mCurrentFocus').stdout:
            raise SystemExit('the game is in the foreground on the phone; not benchmarking (use --no-arm64 for x86 only)')
        game = shell('pidof org.ac1.recomp').stdout.strip()
    out = new_dir(LAB/'runs'/(a.build.replace('/', '__') + '__' + a.run))
    raw = out/'raw'; raw.mkdir()
    cfg = CONFIG['bench']; n, reps = a.calls or cfg['calls_per_rep'], a.reps or cfg['reps']
    leaves = [f'{int(v, 16):08X}' for v in CONFIG['leaves']]
    result = dict(build=a.build, build_manifest_sha256=sha256(build/'manifest.json'), calls_per_rep=n, reps=reps,
                  started=time.strftime('%Y-%m-%d %H:%M:%S'))
    if not a.no_arm64:
        remote = f'/data/local/tmp/codegen_lab/{bm["files"]["bench/bench_arm64"][:16]}'
        shell(f'mkdir -p {remote}')
        adb('push', build/'bench/bench_arm64', f'{remote}/bench')
        shell(f'chmod 755 {remote}/bench')
        device = dict(model=shell('getprop ro.product.model').stdout.strip(), kernel=shell('uname -r').stdout.strip(),
                      background_game_pid=game or None,
                      temp_start=shell('cat /sys/class/thermal/thermal_zone0/temp').stdout.strip(),
                      cpu7_max=shell('cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq').stdout.strip())
        gate = shell(f'{remote}/bench check {cfg["check_calls"]}')
        (raw/'arm64_check.txt').write_text(gate.stdout + gate.stderr)
        result['arm64_gate'] = dict(passed=gate.returncode == 0, lines=gate.stdout.split('\n')[:len(leaves)])
        if gate.returncode:
            result['arm64'] = 'not measured: correctness gate failed'
        else:
            # Which counters exist here: ask the PMU, do not assume.
            events = []
            for e in cfg['events']:
                probe = shell(f'simpleperf stat -e {e} {remote}/bench run empty 1000')
                (raw/f'probe_{e.replace(":", "_")}.txt').write_text(probe.stdout + probe.stderr)
                if probe.returncode == 0 and parse_stat(probe.stdout + probe.stderr).get(e, 0) > 0: events.append(e)
            device['events'] = events
            counters = collections.defaultdict(lambda: collections.defaultdict(list))
            subjects = ['empty'] + leaves + [f'native_{l}' for l in leaves]
            for r in range(reps):
                for s in subjects:
                    leaf, native = (s[7:], ' native') if s.startswith('native_') else (s, '')
                    st = shell(f'taskset {cfg["cpu_mask"]} simpleperf stat -e {",".join(events)} {remote}/bench run {leaf} {n}{native}')
                    (raw/f'stat_{s}_{r}.txt').write_text(st.stdout + st.stderr)
                    for e, v in parse_stat(st.stdout + st.stderr).items():
                        if e in events: counters[s][e].append(v / n)
            tm = shell(f'taskset {cfg["cpu_mask"]} {remote}/bench time {n} {reps}')
            (raw/'arm64_time.txt').write_text(tm.stdout + tm.stderr)
            for m in re.finditer(r'^time (\S+) ([\d.]+)', tm.stdout, re.M): counters[m[1]]['ns'].append(float(m[2]))
            device['temp_end'] = shell('cat /sys/class/thermal/thermal_zone0/temp').stdout.strip()
            # A game brought to the foreground during the run competes for the core.
            if 'org.ac1.recomp' in shell('dumpsys window | grep mCurrentFocus').stdout:
                device['disturbed'] = 'the game was in the foreground at the end of the run'
            result['device'] = device
            result['arm64'] = {s: dict(v) for s, v in counters.items()}
    if not a.no_x86:
        exe, image = build/'bench/roundtrip_x86.exe', LAB/'snapshots'/bm['snapshot']/'x86/image.bin'
        gate = run([exe, image, 'check', str(cfg['check_calls'])])
        (raw/'x86_check.txt').write_text(gate.stdout + gate.stderr)
        result['x86_gate'] = dict(passed=gate.returncode == 0, lines=gate.stdout.strip().split('\n'))
        if gate.returncode:
            result['x86'] = 'not measured: correctness gate failed'
        else:
            tm = run([exe, image, 'time', str(n), str(reps)])
            (raw/'x86_time.txt').write_text(tm.stdout + tm.stderr)
            x = collections.defaultdict(lambda: collections.defaultdict(list))
            for m in re.finditer(r'^time (\S+) ([\d.]+)', tm.stdout, re.M): x[m[1]]['ref_cycles'].append(float(m[2]))
            result['x86'] = {s: dict(v) for s, v in x.items()}
    (out/'results.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    print_run(result)
    print(f'raw results: {out.relative_to(ROOT)}')


def central(metric, vals):
    """Host x86 cycles: the minimum (a shared PC only ever adds cycles); phone
    counters and timings: the median."""
    return min(vals) if metric == 'ref_cycles' else statistics.median(vals)


def net(series, subject, metric, harness):
    """Per call, minus the harness-only subject (same statistic)."""
    vals = series.get(subject, {}).get(metric)
    base = series.get(harness, {}).get(metric)
    if not vals: return None
    return central(metric, vals) - (central(metric, base) if base else 0.0)


def spread(vals):
    """Relative interquartile range: robust to single disturbed repetitions."""
    if len(vals) < 4 or not statistics.median(vals): return 0.0
    q1, _, q3 = statistics.quantiles(vals, n=4)
    return (q3 - q1) / statistics.median(vals)


def print_run(r):
    for arch, harness, metrics in (('arm64', 'empty', ('instructions:u', 'cpu-cycles:u', 'raw-stall-backend:u', 'ns')),
                                   ('x86', 'empty', ('ref_cycles',))):
        gate = r.get(f'{arch}_gate')
        if not gate: continue
        print(f'{arch} correctness gate: {"passed" if gate["passed"] else "FAILED"} ({"; ".join(gate["lines"])})')
        data = r.get(arch)
        if not isinstance(data, dict): continue
        for s in sorted(data):
            if s in ('empty', 'original_empty'): continue
            h = 'original_empty' if s.startswith('original_') else harness
            cells = []
            for m in metrics:
                v = net(data, s, m, h)
                if v is not None: cells.append(f'{m.split(":")[0]} {v:8.2f} (±{100*spread(data[s][m]):.1f}%)')
            print(f'  {s:18s} ' + '  '.join(cells))


# ---------------------------------------------------------------- compare

def cmd_compare(a):
    cfg = CONFIG['compare']
    ra, rb = (json.loads((LAB/'runs'/x/'results.json').read_text()) for x in (a.a, a.b))
    aa = json.loads((LAB/'runs'/a.aa/'results.json').read_text()) if a.aa else None
    if aa and aa['build'] not in (ra['build'], rb['build']): raise SystemExit('the A/A run must repeat A or B')
    lines = [f'# {a.b} vs {a.a}', '']
    for r, label in ((ra, 'A'), (rb, 'B')):
        for arch in ('arm64', 'x86'):
            g = r.get(f'{arch}_gate')
            if g and not g['passed']: lines.append(f'**{label} {arch} correctness gate FAILED: performance not compared.**')
        if r.get('device', {}).get('disturbed'): lines.append(f'**{label} ARM64 run possibly disturbed: {r["device"]["disturbed"]}.**')
    for arch, harness, metrics in (('arm64', 'empty', ('instructions:u', 'cpu-cycles:u', 'raw-stall-backend:u', 'ns')),
                                   ('x86', 'empty', ('ref_cycles',))):
        da, db = ra.get(arch), rb.get(arch)
        if not isinstance(da, dict) or not isinstance(db, dict): continue
        lines += ['', f'## {arch}', '', '| subject | metric | A | B | change | noise | verdict |', '|---|---|---|---|---|---|---|']
        for s in sorted(set(da) & set(db)):
            if s in ('empty', 'original_empty'): continue
            h = 'original_empty' if s.startswith('original_') else harness
            for m in metrics:
                va_, vb = net(da, s, m, h), net(db, s, m, h)
                if va_ is None or vb is None or not va_: continue
                change = (vb - va_) / va_
                noise = max(spread(da[s][m]), spread(db[s][m]))
                if aa and isinstance(aa.get(arch), dict) and s in aa[arch]:
                    # The A/A run repeats whichever side was built the same way.
                    same = net(da if aa['build'] == ra['build'] else db, s, m, h)
                    vaa = net(aa[arch], s, m, h)
                    if vaa and same: noise = max(noise, abs(vaa - same) / same)
                limit = max(cfg['min_relative_change'], cfg['noise_multiple'] * noise)
                verdict = 'improved' if change < -limit else 'REGRESSED' if change > limit else ''
                lines.append(f'| {s} | {m.split(":")[0]} | {va_:.2f} | {vb:.2f} | {100*change:+.1f}% | {100*noise:.1f}% | {verdict} |')
        if arch == 'x86':
            lines += ['', 'Recompilation efficiency (original cycles / lifted cycles, net of harness):', '']
            for leaf in CONFIG['leaves']:
                s = f'{int(leaf, 16):08X}'
                for r, label in ((ra, 'A'), (rb, 'B')):
                    o, l = net(r['x86'], f'original_{s}', 'ref_cycles', 'original_empty'), net(r['x86'], s, 'ref_cycles', 'empty')
                    if o and l: lines.append(f'- {s} {label}: {o/l:.2f}')
    sa, sb = (LAB/'builds'/r['build']/'static.json' for r in (ra, rb))
    if sa.exists() and sb.exists():
        lines += ['', '## static machine code (profile-weighted)', '']
        ja, jb = json.loads(sa.read_text()), json.loads(sb.read_text())
        for arch in ('arm64', 'x86'):
            ta, tb = collections.Counter(), collections.Counter()
            for va in ja:
                if va.startswith('_') or va not in jb: continue
                for k, v in ja[va][arch].items(): ta[k] += ja[va]['weight'] * v
                for k, v in jb[va][arch].items(): tb[k] += jb[va]['weight'] * v
            lines.append(f'{arch}: ' + ', '.join(f'{k} {100*(tb[k]-ta[k])/ta[k]:+.1f}%' for k in sorted(ta) if ta[k]))
            ranked = sorted((jb[va][arch].get('instructions', 0) - ja[va][arch].get('instructions', 0), va) for va in ja
                            if not va.startswith('_') and va in jb)
            lines.append(f'  largest instruction reductions: ' + ', '.join(f'{va} {d:+d}' for d, va in ranked[:4] if d < 0))
            lines.append(f'  largest instruction increases: ' + ', '.join(f'{va} {d:+d}' for d, va in ranked[::-1][:4] if d > 0))
    report = '\n'.join(lines) + '\n'
    path = LAB/'reports'/f'{a.b}__vs__{a.a}.md'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(report, encoding='utf-8')
    print(report)
    print(f'report: {path.relative_to(ROOT)}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('profile'); s.add_argument('name'); s.add_argument('--perf', required=True)
    s.add_argument('--symbols', required=True); s.add_argument('--tid')
    s = sub.add_parser('snapshot'); s.add_argument('name'); s.add_argument('--profile', required=True); s.add_argument('--top', type=int)
    s = sub.add_parser('build'); s.add_argument('snapshot'); s.add_argument('variant'); s.add_argument('--remarks', action='store_true')
    s = sub.add_parser('static'); s.add_argument('build')
    s = sub.add_parser('bench'); s.add_argument('build'); s.add_argument('run')
    s.add_argument('--reps', type=int); s.add_argument('--calls', type=int)
    s.add_argument('--no-arm64', action='store_true'); s.add_argument('--no-x86', action='store_true')
    s = sub.add_parser('compare'); s.add_argument('a'); s.add_argument('b'); s.add_argument('--aa')
    a = p.parse_args()
    globals()['cmd_' + a.cmd](a)


if __name__ == '__main__':
    main()
