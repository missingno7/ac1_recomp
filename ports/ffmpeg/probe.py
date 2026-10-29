"""Compile the standalone API test and compare private movies against FFmpeg CLI.
Run with python -B. The CLI is used only to produce independent reference data.
"""
import argparse
import array
import hashlib
import json
import math
import os
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "work/bink_native"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bootstrap", action="store_true", help="use build_mingw_probe.py scalar archives")
parser.add_argument("--prefix", type=pathlib.Path, help="normal FFmpeg install prefix (headers + import libs)")
parser.add_argument("--frames", type=int, default=30)
parser.add_argument("--video-dir", type=pathlib.Path,
                    default=pathlib.Path("C:/Program Files (x86)/GOG.com/Assassins Creed/Videos"))
parser.add_argument("--ffmpeg", default=str(pathlib.Path(shutil.which("ffprobe") or "ffprobe").with_name("ffmpeg.exe")))
args = parser.parse_args()
if args.bootstrap and args.prefix:
    parser.error("choose --bootstrap or --prefix")
OUT.mkdir(parents=True, exist_ok=True)
mingw = pathlib.Path(os.environ.get("MINGW_BIN", "C:/msys64/mingw64/bin"))
prefix = args.prefix or OUT / ("ffmpeg-native" if args.bootstrap else "ffmpeg-mingw64/install")
includes = ([prefix, OUT / "ffmpeg-src"] if args.bootstrap else [prefix / "include"])
lib = prefix if args.bootstrap else prefix / "lib"
env = os.environ.copy()
env["PATH"] = str(prefix / "bin") + os.pathsep + str(mingw) + os.pathsep + env["PATH"]
commands = []


def run(command, name):
    command = [str(arg) for arg in command]
    commands.append(command)
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True)
    (OUT / (name + ".stdout.log")).write_bytes(result.stdout)
    (OUT / (name + ".stderr.log")).write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(f"{name} failed ({result.returncode}); see work/bink_native/{name}.*.log")
    return result.stdout.decode(errors="replace")


exe = OUT / "bink_native_contract.exe"
run([mingw / "g++.exe", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"] +
    ["-I" + str(p) for p in includes] +
    [ROOT / "src/platform/ac1_bink_native.cpp", ROOT / "src/platform/tests/bink_native_contract.cpp",
     "-L" + str(lib), "-Wl,--start-group", "-lavformat", "-lavcodec", "-lswscale", "-lavutil",
     "-Wl,--end-group", "-lm", "-lbcrypt", "-lole32", "-lws2_32", "-lpsapi", "-o", exe], "compile")
# A real C translation unit validates the public C interface, independently of C++.
c = OUT / "header_contract.c"
c.write_text('#include "ac1_bink_native.h"\nint main(void) { ac1_bink_provider *p = 0; ac1_bink_destroy(p); return 0; }\n')
run([mingw / "gcc.exe", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src/platform"),
     "-c", c, "-o", OUT / "header_contract.o"], "c-header")
report = {"ffmpeg_cli": run([args.ffmpeg, "-version"], "ffmpeg-version").splitlines()[0],
          "bootstrap": args.bootstrap, "frames": args.frames, "videos": {},
          "test_sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
for name in ("LoadingScreen", "UBI_LOGO"):
    video = args.video_dir / (name + ".bik")
    raw = OUT / (name + ".reference.bgra")
    run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", video,
         "-map", "0:v:0", "-frames:v", args.frames, "-pix_fmt", "bgra", "-f", "rawvideo", raw], name + ".reference")
    stdout = run([exe, video, raw, args.frames, OUT / name], name + ".test")
    summary = next(line for line in stdout.splitlines() if line.startswith("PASS "))
    report["videos"][name] = {"result": summary, "input_sha256": hashlib.sha256(video.read_bytes()).hexdigest()}
    print(name + ": " + summary)
    if name == "UBI_LOGO":
        pcmref = OUT / (name + ".reference.s16")
        run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", video,
             "-map", "0:a:0", "-acodec", "pcm_s16le", "-f", "s16le", pcmref], name + ".pcm-reference")
        actual = array.array("h", (OUT / (name + ".track0.s16")).read_bytes())
        reference = array.array("h", pcmref.read_bytes())
        if not actual or len(reference) < len(actual):
            raise RuntimeError("missing/truncated PCM evidence")
        delta = [int(a) - int(b) for a, b in zip(actual, reference)]
        peak = max(map(abs, delta))
        rms = math.sqrt(sum(d*d for d in delta) / len(delta))
        if peak > 2 or rms > 1:
            raise RuntimeError(f"PCM comparison failed: peak={peak} rms={rms}")
        report["pcm"] = {"samples": len(actual), "peak_error": peak, "rms_error": rms}
        print(f"PCM prefix: samples={len(actual)} peak_error={peak} rms_error={rms:.8f}")
report["commands"] = commands
(OUT / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
