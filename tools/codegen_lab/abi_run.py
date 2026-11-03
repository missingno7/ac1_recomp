"""Build abi_chain.c under every protocol for ARM64, check them on the phone, then time them.

usage: python tools/codegen_lab/abi_run.py [CALLS]     (phone connected, game not in the foreground)
"""
import os, re, statistics, subprocess, sys, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]; OUT = ROOT/'work/codegen_lab/experiments/abi'; OUT.mkdir(parents=True, exist_ok=True)
NDK = pathlib.Path(os.environ['LOCALAPPDATA'])/'Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64'
ADB = str(pathlib.Path(os.environ['LOCALAPPDATA'])/'Android/Sdk/platform-tools/adb.exe'); DEV = 'R5CX41MRV6V'
FLAGS = ['--target=aarch64-none-linux-android29', f'--sysroot={NDK.as_posix()}/sysroot', '-O2', '-march=armv8.2-a', '-fwrapv',
         '-fno-strict-aliasing', '-ffp-contract=off', '-fPIE']
fixed = lambda mask: [f'-ffixed-x{20 + i}' for i in range(8) if mask >> i & 1]
VARIANTS = {'p0_struct': (0, 0, 0), 'p1_esp': (1, 0x10, 0), 'p1_esp_esi': (1, 0x50, 0), 'p1_all': (1, 0xFF, 0),
            'p1_broken': (1, 0x10, 1), 'p2_esp': (2, 0x10, 0), 'p2_esp_esi': (2, 0x50, 0),
            'p2_four_args': (2, 0x53, 0), 'p2_four_saved': (2, 0xD8, 0), 'p2_all': (2, 0xFF, 0)}
CALLS = int(sys.argv[1]) if len(sys.argv) > 1 else 20_000_000
EVENTS = 'instructions:u,cpu-cycles:u,raw-stall-backend:u'
cc = lambda *a: subprocess.run([str(NDK/'bin/clang.exe'), *FLAGS, *a], check=True)
objs = []
for name, (proto, mask, broken) in VARIANTS.items():
    d = [f'-DPROTO={proto}', f'-DMASK={mask}', f'-DBROKEN_DISPATCH={broken}', f'-DPFX={name}_']
    cc(*d, *(fixed(mask) if proto == 1 else []), '-c', str(ROOT/'tools/codegen_lab/abi_chain.c'), '-o', str(OUT/f'{name}_guest.o'))
    cc(*d, '-c', str(ROOT/'tools/codegen_lab/abi_host.c'), '-o', str(OUT/f'{name}_host.o'))
    objs += [OUT/f'{name}_guest.o', OUT/f'{name}_host.o']
cc('-pie', str(ROOT/'tools/codegen_lab/abi_main.c'), *map(str, objs), '-o', str(OUT/'abi_main'))
with open(OUT/'abi_main.s', 'w') as f:
    subprocess.run([str(NDK/'bin/llvm-objdump.exe'), '-d', '--no-show-raw-insn', str(OUT/'abi_main')], stdout=f, check=True)
sh = lambda c: subprocess.run([ADB, '-s', DEV, 'shell', c], capture_output=True, text=True)
if 'org.ac1.recomp' in sh('dumpsys window | grep mCurrentFocus').stdout: sys.exit('game in foreground; not running')
subprocess.run([ADB, '-s', DEV, 'push', str(OUT/'abi_main'), '/data/local/tmp/abi_main'], check=True, capture_output=True)
sh('chmod 755 /data/local/tmp/abi_main')
check = sh('/data/local/tmp/abi_main check 4000'); print(check.stdout + check.stderr)
results = {}
for core, mask in {'X4 cpu7': '80', 'A720 cpu2': '04'}.items():
    for name in VARIANTS:
        if name == 'p1_broken': continue
        runs = []
        for _ in range(7):
            t = sh(f'taskset {mask} simpleperf stat -e {EVENTS} /data/local/tmp/abi_main time {name} {CALLS}')
            runs.append({m[2]: int(m[1].replace(',', '')) / CALLS for m in re.finditer(r'^\s*([\d,]+)\s+(\S+)', t.stdout + t.stderr, re.M)})
        results[(core, name)] = runs
        cyc = [r['cpu-cycles:u'] for r in runs]
        print(f'{core:10s} {name:12s} insns {statistics.median(r["instructions:u"] for r in runs):6.1f}  cycles {statistics.median(cyc):6.2f} '
              f'(range {min(cyc):.2f}-{max(cyc):.2f})  stalls {statistics.median(r["raw-stall-backend:u"] for r in runs):6.2f}  per inner call')
sh('rm -f /data/local/tmp/abi_main')
(OUT/'results.txt').write_text(repr(results))
