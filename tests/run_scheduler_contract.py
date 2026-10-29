"""Compile the actual runtime wait code in isolation; no game execution.

Requires prepared private lookup headers, but does not link translated game
functions. GCC whole-program analysis discards unused runtime entry points.
"""
from pathlib import Path
import os
import subprocess
import json
import struct

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
out = ROOT/"work/scheduler_contract"
out.mkdir(parents=True, exist_ok=True)
gcc = "C:/msys64/mingw64/bin/gcc.exe"
env = dict(os.environ, PATH="C:/msys64/mingw64/bin;"+os.environ["PATH"])
common = [gcc, "-O2", "-g", "-ffunction-sections", "-fdata-sections", "-Isrc/runtime"]
commands = [
    common+["-fwhole-program", "-Iwork/lift_initial", "-Iwork/lift_bink", "-c",
            "tests/scheduler_contract.c", "-o", str(out/"fixture.obj")],
    common+[str(out/"fixture.obj"), "src/runtime/guest_memory.c", "src/runtime/guest_clock.c",
            "src/runtime/ac1_files.c", "src/runtime/ac1_profile.c", "-Wl,--gc-sections",
            "-luser32", "-o", str(out/"fixture.exe")],
    [str(out/"fixture.exe")],
]
for index, command in enumerate(commands):
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace")
    (out/f"step_{index}.log").write_text(subprocess.list2cmdline(command)+"\n"+result.stdout)
    print(result.stdout, end="", flush=True)
    if result.returncode: raise SystemExit(result.returncode)

subprocess.run([gcc, "-O2", "-g", "-Isrc/runtime", "tests/scheduler_profile_contract.c",
                "src/runtime/ac1_profile.c", "-o", str(out/"profile.exe")], check=True, env=env)
profile_dir=out/"profile"
profile_dir.mkdir(exist_ok=True)
env.update(AC1_PROFILE="1", AC1_PROFILE_INTERVAL=".3", AC1_PROFILE_DIR=str(profile_dir))
result=subprocess.run([str(out/"profile.exe")], check=True, env=env, capture_output=True, text=True)
(out/"profile.log").write_text(result.stdout+result.stderr)
metadata=[(p, json.loads(p.read_text())) for p in profile_dir.glob("raw_*.json")]
latest=max(metadata,key=lambda x:x[1]["end"])[1]
selected=[(p,m) for p,m in metadata if m["run_start"]==latest["run_start"]]
sample=struct.Struct("<QQQIIIIII")
blocked=[r for p,m in selected for r in sample.iter_unpack(p.with_suffix(".bin").read_bytes()) if r[4]==0x100]
active=sum(bool(r[6]&1) and not (r[6]&8) for r in blocked)
assert len(blocked)>10 and active < len(blocked)*.05, (len(blocked),active)
busy=next(g for g in latest["guests"] if g["guest"]==0x101)
assert busy["handoffs"]==busy["transfer_count"]==1000, busy
assert busy["handoff_causes"]==[100]*10, busy
assert busy["transfer_ns"]==1200000 and busy["transfer_max_ns"]==1200, busy
print(f"sampler contract: blocked={len(blocked)}, raw nonzero cycles={sum(bool(r[6]&1) for r in blocked)}, active after wait filter={active}; exact handoff counters passed")
