"""Focused gamepad contracts; private products in work/gamepad/. No game build."""
import os
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
OUT = ROOT / "work/gamepad"
OUT.mkdir(parents=True, exist_ok=True)
BIN = Path("C:/msys64/mingw64/bin")
env = dict(os.environ, PATH=str(BIN)+os.pathsep+os.environ["PATH"])
def run(name, args):
    p = subprocess.run([str(a) for a in args], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / (name+".log")).write_text(p.stdout, encoding="utf-8", newline="\n")
    print(p.stdout, end="", flush=True)
    if p.returncode: raise SystemExit(f"FAIL: {name} ({p.returncode})")
run("pad-build", [BIN/"gcc.exe", "-std=c11", "-Wall", "-Wextra", "-Werror", "-c", "src/platform/ac1_gamepad.c", "-o", OUT/"pad.o"])
run("contract-build", [BIN/"g++.exe", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DAC1_HOST_D3D9=1",
    "tests/gamepad_contract.cpp", "src/platform/ac1_input_script.cpp", OUT/"pad.o", "-o", OUT/"contract.exe", "-luser32"])
run("contract", [OUT/"contract.exe"])
run("existing-script-build", [BIN/"g++.exe", "-std=c++17", "-Isrc/platform", "tests/input_script_contract.cpp", "src/platform/ac1_input_script.cpp", "-o", OUT/"input-script.exe", "-luser32"])
run("existing-script", [OUT/"input-script.exe"])

for name, source in [("win-discovery", "src/platform/ac1_dinput8_bridge.cpp"),
                     ("portable-discovery", "src/platform/ac1_dinput8_portable.cpp")]:
    run(name+"-build", [BIN/"g++.exe", "-std=c++17", "-Wall", "-Wextra", "tests/pad_discovery_contract.cpp",
        source, "src/platform/ac1_input_script.cpp", OUT/"pad.o", "-o", OUT/(name+".exe"), "-luser32"])
    run(name, [OUT/(name+".exe")])
