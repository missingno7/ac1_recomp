"""Exercise the actual CS imports and native Windows boundary oracle, without game code.

Run after tools/build.ps1 (prepared lookup headers are required).
--expect-rejection verifies the original failure before applying the fix.
"""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
OUT = ROOT / "work/cs/contract"
OUT.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PATH="C:/msys64/mingw64/bin;" + os.environ["PATH"])
gcc = "C:/msys64/mingw64/bin/gcc.exe"
common = [gcc, "-O2", "-g", "-ffunction-sections", "-fdata-sections", "-Isrc/runtime"]
commands = [
    common + ["-fwhole-program", "-Iwork/lift_initial", "-Iwork/lift_bink", "-c",
              "tests/critical_section_span.c", "-o", str(OUT / "fixture.obj")],
    common + [str(OUT / "fixture.obj"), "src/runtime/guest_memory.c", "src/runtime/guest_clock.c", "src/runtime/ac1_guest_time.c",
              "src/runtime/ac1_files.c", "src/runtime/ac1_profile.c", "-Wl,--gc-sections",
              "-luser32", "-o", str(OUT / "fixture.exe")],
    [str(OUT / "fixture.exe"), *sys.argv[1:]],
]
for index, command in enumerate(commands):
    result = subprocess.run(command, env=env, capture_output=True, text=True, errors="replace")
    output = result.stdout + result.stderr
    (OUT / f"step_{index}.log").write_text(subprocess.list2cmdline(command) + "\n" + output)
    print(output, end="", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
