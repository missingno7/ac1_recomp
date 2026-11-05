"""Focused Windows oracle + host/Android syntax checks. No runtime generation.
Usage: D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tests/run_win32_compat.py
All programs execute synchronously; artifacts/logs remain in work/.
"""
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
OUT = ROOT / "work/win32_compat"
OUT.mkdir(parents=True, exist_ok=True)
MINGW = Path("C:/msys64/mingw64/bin")
NDK = Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin"
STUB = OUT / "stubs"
STUB.mkdir(exist_ok=True)
# Declarations/empty lookup bodies, not generated game functions.
(STUB / "initial_dispatch.h").write_text(
    "typedef void (*ac1_lifted_fn)(CPU *);\n"
    "typedef struct { uint32_t guest_va; ac1_lifted_fn function; } ac1_lifted_entry;\n"
    "static const ac1_lifted_entry ac1_lifted_functions[1]={{0,0}};\n"
    "static const size_t ac1_lifted_function_count=0;\n")
(STUB / "ac1_fingerprint.h").write_text('#define AC1_EXPECTED_EXE_SHA256 "syntax-only"\n')


def run(label, command, env=None):
    command = [str(x) for x in command]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, errors="replace", env=env)
    (OUT / f"{label}.log").write_text(subprocess.list2cmdline(command) + "\n" + completed.stdout)
    print(completed.stdout, end="", flush=True)
    if completed.returncode:
        raise SystemExit(f"FAIL: {label}: exit {completed.returncode}")
    print(f"PASS: {label}", flush=True)


GCC = MINGW / "gcc.exe"
run("resources", [MINGW / "windres.exe", "tests/ac1_win32_compat_fixture.rc", "-o", OUT / "fixture-res.o"])
run("fixture-build", [GCC, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
    "-Isrc/runtime", "tests/ac1_win32_compat_fixture.c", "src/runtime/ac1_files.c", "src/platform/ac1_gamepad.c",
    OUT / "fixture-res.o", "-o", OUT / "fixture.exe", "-luser32", "-lwinmm"])
run("fixture", [OUT / "fixture.exe"])
run("files-build", [GCC, "-std=c11", "-Isrc/runtime", "tests/files_contract.c",
                    "src/runtime/ac1_files.c", "-o", OUT / "files_contract.exe"])
env = dict(os.environ, AC1_FILES_TEST_ROOT=str(OUT))
run("files-contract", [OUT / "files_contract.exe"], env=env)
run("profile-build", [GCC, "-std=c11", "-Isrc/runtime", "tests/ac1_profile_int_fixture.c",
                      "-o", OUT / "profile_fixture.exe"])
run("profile-contract", [OUT / "profile_fixture.exe"])
syntax = ["-fsyntax-only", "-std=c11", "-Wall", "-Wextra", "-Werror=implicit-function-declaration",
          "-Werror=return-type", "-Isrc/runtime", f"-I{STUB}"]
run("host-syntax", [GCC, *syntax, "-DAC1_HOST_D3D9=1",
                     "src/runtime/ac1_runtime.c", "src/runtime/ac1_files.c"])
run("android-syntax", [NDK / "clang.exe", "--target=aarch64-linux-android28", *syntax,
                        "src/runtime/ac1_runtime.c", "src/runtime/ac1_files.c"])
run("import-audit", [sys.executable, "-B", "tests/ac1_win32_import_audit.py"])
print("PASS: all focused checks; no game execution or Android hardware claim")
