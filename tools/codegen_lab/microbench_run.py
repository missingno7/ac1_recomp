"""usage: python tools/codegen_lab/microbench_run.py   (phone connected, game not in the foreground)

Build the cost-model microbenchmarks, run each kernel on the phone under simpleperf stat, per core."""
import json, os, re, statistics, subprocess, sys, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]; OUT = ROOT/'work/codegen_lab/experiments/microbench'
OUT.mkdir(parents=True, exist_ok=True)
NDK = pathlib.Path(os.environ['LOCALAPPDATA'])/'Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64'
ADB = str(pathlib.Path(os.environ['LOCALAPPDATA'])/'Android/Sdk/platform-tools/adb.exe'); DEV = 'R5CX41MRV6V'
FLAGS = ['--target=aarch64-none-linux-android29', f'--sysroot={NDK.as_posix()}/sysroot', '-O2', '-march=armv8.2-a', '-fwrapv',
         '-fno-strict-aliasing', '-ffp-contract=off', '-fPIE', '-pie']
EVENTS = 'instructions:u,cpu-cycles:u,raw-stall-backend:u'
N, REPS = 20_000_000, 5
CORES = {'X4 cpu7': '80', 'A720 cpu2': '04'}
sh = lambda c: subprocess.run([ADB, '-s', DEV, 'shell', c], capture_output=True, text=True)
src = ROOT/'tools/codegen_lab/microbench_arm64.c'
subprocess.run([str(NDK/'bin/clang.exe'), *FLAGS, str(src), '-o', str(OUT/'microbench')], check=True)
subprocess.run([str(NDK/'bin/llvm-objdump.exe'), '-d', '--no-show-raw-insn', str(OUT/'microbench')], stdout=open(OUT/'microbench.s', 'w'), check=True)
if 'org.ac1.recomp' in sh('dumpsys window | grep mCurrentFocus').stdout: sys.exit('game in foreground; not running')
subprocess.run([ADB, '-s', DEV, 'push', str(OUT/'microbench'), '/data/local/tmp/microbench'], check=True, capture_output=True)
sh('chmod 755 /data/local/tmp/microbench')
kernels = sh('/data/local/tmp/microbench list').stdout.split()
raw = {}
for core, mask in CORES.items():
    for k in kernels:
        for r in range(REPS):
            t = sh(f'taskset {mask} simpleperf stat -e {EVENTS} /data/local/tmp/microbench {k} {N}')
            for m in re.finditer(r'^\s*([\d,]+)\s+(\S+)', t.stdout + t.stderr, re.M):
                raw.setdefault(core, {}).setdefault(k, {}).setdefault(m[2], []).append(int(m[1].replace(',', '')) / N)
(OUT/'raw.json').write_text(json.dumps(dict(n=N, reps=REPS, temp=sh('cat /sys/class/thermal/thermal_zone0/temp').stdout.strip(), raw=raw), indent=1))
for core, ks in raw.items():
    e0 = {e: statistics.median(v) for e, v in ks['empty'].items()}
    print(f'\n{core}: per iteration, loop overhead (empty) subtracted; median of {REPS}, spread = (max-min)/median')
    for k, ev in ks.items():
        cols = []
        for e in ('instructions:u', 'cpu-cycles:u', 'raw-stall-backend:u'):
            v = ev.get(e, [0]); med = statistics.median(v)
            cols.append(f'{e.split(":")[0][:6]} {med - e0.get(e, 0):7.2f} ({100*(max(v)-min(v))/med if med else 0:4.1f}%)')
        print(f'  {k:24s} ' + '  '.join(cols))
