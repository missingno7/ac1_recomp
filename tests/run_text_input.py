"""Host text contracts, including the actual Android window adapter. No game build."""
import os
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
os.chdir(ROOT)
OUT=ROOT/"work/gamepad/text"
OUT.mkdir(parents=True,exist_ok=True)
BIN=Path("C:/msys64/mingw64/bin")
env=dict(os.environ,PATH=str(BIN)+os.pathsep+os.environ["PATH"])
def run(name,args):
    p=subprocess.run([str(x) for x in args],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (OUT/(name+".log")).write_text(p.stdout,encoding="utf-8",newline="\n")
    print(p.stdout,end="",flush=True)
    if p.returncode:raise SystemExit(f"FAIL {name}: {p.returncode}")
run("text-contract-build",[BIN/"g++.exe","-std=c++17","-DAC1_HOST_D3D9=1","-Wall","-Wextra","-Werror","tests/text_input_contract.cpp",
    "src/platform/ac1_input_script.cpp","-o",OUT/"text.exe","-luser32"])
run("text-contract",[OUT/"text.exe"])
for name,source,defines in [("win-window","src/platform/ac1_window_win32.c",[]),
                            ("android-window","src/platform/ac1_window_android.c",["-DTEXT_PORTABLE=1"])]:
    objects=[]
    for i,file in enumerate(["tests/text_window_contract.c",source,"src/runtime/guest_memory.c"]):
        obj=OUT/f"{name}-{i}.o";objects.append(obj)
        run(f"{name}-compile-{i}",[BIN/"gcc.exe","-std=c11","-Isrc/platform","-Isrc/runtime",*defines,"-c",file,"-o",obj])
    run(name+"-build",[BIN/"g++.exe","-std=c++17","-DAC1_HOST_D3D9=1",*objects,"src/platform/ac1_input_script.cpp","-o",OUT/(name+".exe"),"-luser32","-lgdi32"])
    run(name,[OUT/(name+".exe")])
