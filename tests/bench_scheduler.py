"""Unattended scheduler evidence. Private logs/metrics stay under work/.

Run sequentially; every child is joined, including timeout/error paths.
Example: python -B tests/bench_scheduler.py --count 10 --tag fixed
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=Path, default=ROOT/"build/host-sched/ac1_startup.exe")
parser.add_argument("--count", type=int, default=1)
parser.add_argument("--tag", default="scheduler")
parser.add_argument("--timeout", type=float, default=90)
parser.add_argument("--presents", type=int, default=512)
parser.add_argument("--waits", type=int, default=3)
parser.add_argument("--profile", action="store_true")
parser.add_argument("--imports", action="store_true", help="trace imports and native Bink progress")
parser.add_argument("--desktop-focus", action="store_true", help="use real focus, including original guest pause on deactivation")
parser.add_argument("--stable-activation", action="store_true", help="use an empty replay to hold activation for performance measurements")
parser.add_argument("--input-script", type=Path, help="explicit activation/input probe instead of idle activation")
parser.add_argument("--activation-probe", action="store_true", help="post deactivation after intro probes and reactivation at 512 to this child only (requires --imports)")
parser.add_argument("--resume-black", action="store_true", help="post activation to a naturally black child at present 512")
args = parser.parse_args()
out = ROOT/"work"/args.tag
out.mkdir(parents=True, exist_ok=True)
(out/"dxvk").mkdir(exist_ok=True)
env = dict(os.environ)
env.update(PATH="C:/msys64/mingw64/bin;"+env["PATH"],
           AC1_D3D9_LIBRARY=str(ROOT/"work/d3d9_dxvk_probe/d3d9.dll"),
           AC1_D3D9_FORCE_WINDOWED="1", AC1_DISPATCH_LIMIT="0",
           DXVK_LOG_PATH=str(out/"dxvk"),
           AC1_D3D9_TRACE_FRAME="1", AC1_TRACE_WAITS=str(args.waits),
           AC1_HIDE_FILES=";".join("videos\\"+x+".bik" for x in
               ("UBI_LOGO", "Ubisoft_Presents", "Warning_Disclaimer", "DemoIntro")))
for key in ("AC1_TRACE_IMPORTS", "AC1_D3D9_CAPTURE_DIR", "AC1_PROFILE", "AC1_PROFILE_FRAMES"):
    env.pop(key, None)
if args.imports: env["AC1_TRACE_IMPORTS"] = "1"
env.pop("AC1_INPUT_SCRIPT", None)
env.pop("AC1_INPUT_RECORD", None)
if args.input_script:
    env["AC1_INPUT_SCRIPT"] = str(args.input_script.resolve())
elif args.stable_activation and not args.desktop_focus and not args.activation_probe and not args.resume_black:
    # Existing replay support owns guest activation independently of the
    # desktop. An empty stream supplies no keys/mouse and stays active forever.
    idle=out/"idle_input.txt"
    idle.write_text("# No input. Keep guest activation stable for unattended measurements.\n")
    env["AC1_INPUT_SCRIPT"] = str(idle)
if args.profile:
    env.update(AC1_PROFILE="1", AC1_PROFILE_INTERVAL="5")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.GetProcessTimes.argtypes = [ctypes.c_void_p]+[ctypes.c_void_p]*4
u32=ctypes.WinDLL("user32",use_last_error=True)
u32.GetForegroundWindow.restype=ctypes.c_void_p
u32.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
u32.IsWindowVisible.argtypes=[ctypes.c_void_p]
u32.PostMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t]
enum_callback=ctypes.WINFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_ssize_t)
u32.EnumWindows.argtypes=[enum_callback,ctypes.c_ssize_t]

def foreground_pid():
    pid=ctypes.c_ulong()
    u32.GetWindowThreadProcessId(u32.GetForegroundWindow(),ctypes.byref(pid))
    return pid.value

def post_activation(pid, active):
    windows=[]
    @enum_callback
    def collect(hwnd,unused):
        found=ctypes.c_ulong()
        u32.GetWindowThreadProcessId(hwnd,ctypes.byref(found))
        if found.value==pid and u32.IsWindowVisible(hwnd): windows.append(hwnd)
        return 1
    u32.EnumWindows(collect,0)
    if not windows: raise RuntimeError(f"no visible window for child {pid}")
    for hwnd in windows:
        for message in (0x1c,0x6):
            if not u32.PostMessageW(hwnd,message,int(active),0): raise ctypes.WinError(ctypes.get_last_error())
    return len(windows)

def cpu_seconds(process):
    values = [ctypes.c_uint64() for _ in range(4)]
    if not k32.GetProcessTimes(int(process._handle), *(ctypes.byref(v) for v in values)):
        raise ctypes.WinError(ctypes.get_last_error())
    return (values[2].value+values[3].value)/1e7

rows = []
for run in range(1, args.count+1):
    log = out/f"run_{run:02d}.log"
    if args.profile:
        capture = out/f"profile_{run:02d}"
        capture.mkdir(exist_ok=True)
        env["AC1_PROFILE_DIR"] = str(capture)
    with log.open("wb") as err, log.with_suffix(".out").open("wb") as stdout:
        start = time.monotonic()
        process = subprocess.Popen([str(args.exe), r"C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe"],
                                   cwd=ROOT, env=env, stdout=stdout, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        frames, draws, cpu = {}, {}, 0
        activation=[]
        foreground128=None
        try:
            while process.poll() is None:
                text = log.read_text(errors="replace")
                for p, d, tick in re.findall(r"present_calls=(\d+) draw_calls=(\d+)(?: t_ms=(\d+))?", text):
                    draws[int(p)] = int(d)
                    if tick: frames[int(p)] = int(tick)
                cpu = cpu_seconds(process)
                present=max(draws,default=0)
                if present>=128 and foreground128 is None: foreground128=foreground_pid()
                if args.activation_probe:
                    if "hidden by AC1_HIDE_FILES:" in text and "DemoIntro.bik" in text and not activation:
                        activation.append(dict(present=present,active=False,windows=post_activation(process.pid,False)))
                    if present>=512 and len(activation)==1:
                        activation.append(dict(present=present,active=True,windows=post_activation(process.pid,True)))
                if args.resume_black and present>=512 and draws.get(128,0)<1000 and not activation:
                    activation.append(dict(present=present,active=True,windows=post_activation(process.pid,True)))
                if max(draws, default=0) >= args.presents or time.monotonic()-start >= args.timeout:
                    break
                time.sleep(.2)
        finally:
            if process.poll() is None: process.terminate()
            process.wait()
        elapsed = time.monotonic()-start
    row = dict(run=run, pid=process.pid, title=draws.get(128, 0)>1000,
               draws128=draws.get(128), draws=draws, frames_ms=frames,
               wall_s=round(elapsed, 3), cpu_s=round(cpu, 3),
               cpu_one_core_percent=round(100*cpu/elapsed, 2),
               foreground_pid128=foreground128, activation_probe=activation)
    if 2 in frames and 128 in frames: row["loading_s"] = (frames[128]-frames[2])/1000
    for a,b in ((128,256), (256,512)):
        if a in frames and b in frames: row[f"fps{a}_{b}"] = (b-a)*1000/(frames[b]-frames[a])
    rows.append(row)
    print(json.dumps(row), flush=True)
    (out/"summary.json").write_text(json.dumps(rows, indent=2))
print(f"Title: {sum(r['title'] for r in rows)}/{len(rows)}", flush=True)
