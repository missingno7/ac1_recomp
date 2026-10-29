#!/usr/bin/env python3
"""Build and run native plus generated-code MMX checks for AC1's two Havok routines."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
ADDRESSES = (0x01023410, 0x01023520)
WORK = ROOT / "work/mmx_havok_oracle/reproducible"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python", type=Path, help="Python environment used by tools/lift_ac1.py")
    parser.add_argument("--pcrecomp", type=Path, help="pcrecomp checkout used by tools/lift_ac1.py")
    parser.add_argument("--cc", default="gcc", help="MinGW GCC executable")
    parser.add_argument("--cxx", default="g++", help="MinGW G++ executable")
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def stable_json(path: Path, timeout: float = 5.0) -> dict:
    """Wait for a present JSON file to settle, then parse a stable snapshot."""
    deadline = time.monotonic() + timeout
    previous: tuple[int, int] | None = None
    stable_since: float | None = None
    while time.monotonic() < deadline:
        try:
            stat = path.stat()
            state = (stat.st_size, stat.st_mtime_ns)
        except FileNotFoundError:
            state = None
        now = time.monotonic()
        if state and state[0] > 0 and state == previous:
            if stable_since is None:
                stable_since = now
            if now - stable_since >= 1.0:
                before = path.stat()
                raw = path.read_bytes()
                after = path.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    previous, stable_since = None, None
                    continue
                try:
                    return json.loads(raw)
                except json.JSONDecodeError:
                    previous, stable_since = None, None
        else:
            stable_since = now if state else None
        previous = state
        time.sleep(0.25)
    raise RuntimeError(
        f"file is missing, changing, or invalid after {timeout:g}s: {path}; "
        "complete tools/build.ps1 -PrepareOnly before running this fixture"
    )


def load_lifter():
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    sys.dont_write_bytecode = True
    path = ROOT / "tools/lift_ac1.py"
    spec = importlib.util.spec_from_file_location("ac1_lifter_for_mmx_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import project lifter: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def run() -> None:
    args = parse_args()
    config_path = ROOT / "config/ac1_dx9.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    binary = Path(config["binary"])
    if not binary.is_absolute():
        binary = ROOT / binary
    expected_sha = str(config["sha256"]).lower()
    actual_sha = sha256(binary)
    if actual_sha != expected_sha:
        raise RuntimeError(
            f"installed executable does not match pinned config: expected {expected_sha}, got {actual_sha}"
        )

    report_path = ROOT / "work/lift_initial/lift-report.json"
    report = stable_json(report_path)
    current_driver_sha = sha256(ROOT / "tools/lift_ac1.py")
    report_inputs = report.get("generation_input_sha256", {})
    if (report.get("binary_sha256", "").lower() != actual_sha or
            report_inputs.get("config") != sha256(config_path) or
            report.get("lift_driver_sha256") != current_driver_sha):
        raise RuntimeError("lift success report is stale for the configured binary, config, or current driver")

    catalog_path = ROOT / "work/lift_initial/functions.json"
    catalog = stable_json(catalog_path)
    if catalog.get("binary_sha256", "").lower() != actual_sha:
        raise RuntimeError("catalog binary fingerprint does not match config/ac1_dx9.json")
    entries = catalog.get("functions")
    if not isinstance(entries, list):
        raise RuntimeError(f"catalog has no functions list: {catalog_path}")
    by_address = {entry.get("address"): entry for entry in entries
                  if isinstance(entry, dict)}
    missing = [f"0x{address:08X}" for address in ADDRESSES
               if address not in by_address]
    if missing:
        raise RuntimeError("pinned catalog does not contain required functions: " + ", ".join(missing))
    functions = [by_address[address] for address in ADDRESSES]

    module = load_lifter()
    tooling_args = argparse.Namespace(python=args.python, pcrecomp=args.pcrecomp)
    _python, pcrecomp = module.resolve_tooling(config, tooling_args)
    generated_dir = WORK / "generated"
    generated_dir.mkdir(parents=True, exist_ok=True)
    result = module.emit_lift(binary, pcrecomp, functions, generated_dir, 100, 0, None)
    failures = result.get("lift_failures", [])
    if failures or len(result.get("lifted", [])) != len(ADDRESSES):
        raise RuntimeError(f"failed to emit both MMX routines: {failures}")
    units = result.get("translation_units", [])
    if len(units) != 1:
        raise RuntimeError(f"expected one emitted translation unit for two routines, got {units}")
    generated_c = generated_dir / units[0]
    if not generated_c.is_file():
        raise RuntimeError(f"emitter did not create expected generated C: {generated_c}")

    WORK.mkdir(parents=True, exist_ok=True)
    oracle_exe = WORK / "native_oracle.exe"
    generated_obj = WORK / "generated_functions.o"
    compare_exe = WORK / "generated_compare.exe"
    subprocess.run([
        args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        str(ROOT / "tests/mmx_havok_native_oracle.cpp"), "-o", str(oracle_exe),
    ], cwd=ROOT, check=True)
    subprocess.run([
        args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "src/runtime"), "-c", str(generated_c), "-o", str(generated_obj),
    ], cwd=ROOT, check=True)
    subprocess.run([
        args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "src/runtime"), str(ROOT / "tests/mmx_havok_generated_compare.cpp"),
        str(generated_obj), "-o", str(compare_exe),
    ], cwd=ROOT, check=True)
    subprocess.run([str(oracle_exe)], cwd=ROOT, check=True)
    subprocess.run([str(compare_exe)], cwd=ROOT, check=True)


if __name__ == "__main__":
    try:
        run()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"MMX fixture failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
