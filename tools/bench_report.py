#!/usr/bin/env python3
"""Summarize AC1 gameplay runs and interleaved comparisons."""
import argparse
import json
import statistics
import sys
from pathlib import Path


def read(path):
    p = Path(path) / "result.json"
    if not p.is_file():
        raise SystemExit(f"missing result: {p}")
    return json.loads(p.read_text(encoding="utf-8-sig"))


def percentile(values, q):
    if not values:
        return None
    x = (len(values) - 1) * q
    lo = int(x)
    return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (x - lo)


def internal_run(run, seconds):
    log_path = run / "startup.log"
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    captures = list((run / "captures").glob("present_*.bmp"))
    profile = run / "profile"
    snapshots = []
    if profile.exists():
        for jf in profile.glob("raw_*.json"):
            try:
                data = json.loads(jf.read_text())
                if data.get("mode") == "frames":
                    snapshots.append(data)
            except (OSError, ValueError):
                pass
    snapshots.sort(key=lambda d: d.get("start", 0))
    frames = sorted((int(t), int(d["frequency"])) for d in snapshots for t in d.get("frames", []))
    start_index = 2047  # zero-based index; first measured Present is frame 2048
    start_tick = frames[start_index][0] if len(frames) > start_index else None
    freq = frames[start_index][1] if start_tick is not None else 1
    window = [t for t, f in frames if start_tick is not None and start_tick <= t <= start_tick + seconds * freq]
    intervals = sorted((window[i] - window[i - 1]) / freq * 1000 for i in range(1, len(window)))
    traces = []
    import re
    for a, b in re.findall(r"present_calls=(\d+) draw_calls=(\d+)", log):
        traces.append((int(a), int(b)))
    proof = [c for c in captures if int(re.search(r"present_(\d+)", c.name).group(1)) >= start_index]
    reached = len(window) >= 30 and bool(proof) and any(draw > 0 for _, draw in traces)
    capture_rows = []
    for line in log.splitlines():
        m = re.search(r"present_(\d+)\.bmp .*draw_calls=(\d+)", line)
        if m:
            capture_rows.append((int(m.group(1)), int(m.group(2))))
    eligible = [row for row in capture_rows if row[0] >= start_index]
    draws = (eligible[-1][1] - eligible[-2][1]) / max(1, eligible[-1][0] - eligible[-2][0]) if len(eligible) >= 2 else None
    gpu_samples = []
    gpu_path = run / "gpu.csv"
    if gpu_path.exists():
        for line in gpu_path.read_text(errors="replace").splitlines():
            m = re.match(r"\s*(\d+)\s*,\s*(\d+)", line)
            if m:
                gpu_samples.append({"utilization_percent": int(m.group(1)), "graphics_clock_mhz": int(m.group(2))})
    stop_lines = [line for line in log.splitlines() if "AC1 guest stop" in line]
    coverage = (frames[-1][0] - start_tick) / freq if start_tick is not None else 0.0
    full_window = coverage >= seconds
    frame_dropped = max((d.get("frame_dropped", 0) for d in snapshots), default=0)
    reached = reached and full_window and not stop_lines and not frame_dropped
    reason = ("guest stopped" if stop_lines else
              f"incomplete {seconds}-second frame window: retained {coverage:.3f} seconds" if not full_window else
              f"frame capture dropped {frame_dropped} frames" if frame_dropped else "gameplay start not reached")
    frame_times = intervals
    return {
        "reached_gameplay": reached, "gameplay_start_present_index": start_index + 1 if start_tick is not None else None,
        "full_frame_window": full_window, "frame_capture_coverage_seconds": coverage, "frame_dropped": frame_dropped,
        "frames_in_window": len(window), "fps": 1000 / statistics.mean(frame_times) if frame_times else None,
        "measurement_elapsed_seconds": (window[-1] - window[0]) / freq if len(window) > 1 else 0.0,
        "frametime_mean_ms": statistics.mean(frame_times) if frame_times else None,
        "frametime_p50_ms": percentile(frame_times, .5), "frametime_p95_ms": percentile(frame_times, .95),
        "frametime_p99_ms": percentile(frame_times, .99), "frametime_min_ms": min(frame_times) if frame_times else None,
        "frametime_max_ms": max(frame_times) if frame_times else None, "draws_per_frame": draws,
        "process_cpu_seconds": None, "cpu_utilization_cores": None, "top_threads": [],
        "gpu_utilization_percent": statistics.mean(s["utilization_percent"] for s in gpu_samples) if gpu_samples else None,
        "gpu_samples": gpu_samples, "scene_proof": [str(c.relative_to(run)) for c in proof],
        "profile_snapshots": len(snapshots), "present_trace_points": traces,
        "failure": None if reached else {"reason": reason, "guest_stop_lines": stop_lines,
                                         "startup_log_tail": log.splitlines()[-12:]}
    }


def successful(data, key):
    return [r[key] for r in data.get("runs", []) if r.get("reached_gameplay") and isinstance(r.get(key), (int, float))]


def stats(values):
    values = sorted(values)
    return {"n": len(values), "median": statistics.median(values) if values else None,
            "min": min(values) if values else None, "max": max(values) if values else None,
            "p95": percentile(values, .95), "p99": percentile(values, .99)}


def comparison_text(d):
    lines = ["# AC1 interleaved gameplay comparison", "", f"- Pair count: {d['pair_count']}; A failures: {d['A']['failures']}; B failures: {d['B']['failures']}", f"- Busy-host override: {d.get('host_load_override', False)}", ""]
    for side in ("A", "B"):
        lines.append(f"## {side}: {d[side]['label']}")
        for key, label in (("fps", "FPS"), ("frametime_mean_ms", "mean frame time ms"),
                           ("frametime_p95_ms", "p95 ms"), ("frametime_p99_ms", "p99 ms"),
                           ("cpu_utilization_cores", "CPU cores"), ("gpu_utilization_percent", "GPU %")):
            s = d[side].get("stats", {}).get(key) or stats(successful(d[side], key))
            lines.append(f"- {label}: median={s['median']}, min={s['min']}, max={s['max']}, p95={s['p95']}, p99={s['p99']} (n={s['n']})")
        for run in d[side].get("runs", []):
            if not run.get("reached_gameplay"):
                failure = run.get("failure") or {}
                stop = failure.get("guest_stop_lines") or []
                lines.append(f"- Failed run {run.get('run')}: {stop[-1] if stop else failure.get('reason', 'gameplay not reached')}")
        lines.append("")
    lines.append("## Paired median deltas (B - A)")
    for key, label in (("fps", "FPS"), ("frametime_mean_ms", "mean frame time ms"),
                       ("frametime_p95_ms", "p95 ms"), ("frametime_p99_ms", "p99 ms"),
                       ("cpu_utilization_cores", "CPU cores"), ("gpu_utilization_percent", "GPU %")):
        delta = d.get("paired_deltas", {}).get(key)
        if delta is None:
            pairs = [(a.get(key), b.get(key)) for a, b in zip(d["A"].get("runs", []), d["B"].get("runs", []))
                     if a.get("reached_gameplay") and b.get("reached_gameplay") and isinstance(a.get(key), (int, float)) and isinstance(b.get(key), (int, float))]
            delta = statistics.median([y - x for x, y in pairs]) if pairs else None
        lines.append(f"- {label}: {delta} (valid pairs={sum(1 for a, b in zip(d['A'].get('runs', []), d['B'].get('runs', [])) if a.get('reached_gameplay') and b.get('reached_gameplay'))})")
    lines.append("")
    return "\n".join(lines)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--internal-run":
        p = argparse.ArgumentParser()
        p.add_argument("--internal-run", type=Path, required=True)
        p.add_argument("--seconds", type=int, required=True)
        args = p.parse_args()
        print(json.dumps(internal_run(args.internal_run, args.seconds)))
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--summary":
        d = read(sys.argv[2])
        if d.get("comparison"):
            report = comparison_text(d)
        else:
            failed = sum(not r.get("reached_gameplay") for r in d.get("runs", []))
            lines = ["# AC1 gameplay benchmark", "", f"- Identity: `{d['identity']['sha']}` / `{d['identity']['config_id']}`; branch `{d['identity']['branch']}`; dirty={d['identity']['dirty']}", f"- Binary SHA-256: `{d['identity']['binary_sha256']}`", f"- Machine: {d['machine']['cpu_model']}; {d['machine']['logical_cores']} logical cores; {d['machine']['physical_memory_bytes']} bytes RAM", f"- Build generator: {d.get('environment', {}).get('actual_build_generator', 'unknown')}", f"- Failed runs: {failed}", ""]
            for r in d["runs"]:
                tops = ", ".join(f"{t['tid']}:{t['cpu_seconds']:.2f}s" for t in r.get("top_threads", [])[:5]) or "unavailable"
                lines.append(f"- Run {r['run']}: reached_gameplay={r.get('reached_gameplay', False)}, FPS={r.get('fps')}, mean/p50/p95/p99/max ms={r.get('frametime_mean_ms')}/{r.get('frametime_p50_ms')}/{r.get('frametime_p95_ms')}/{r.get('frametime_p99_ms')}/{r.get('frametime_max_ms')}, CPU={r.get('process_cpu_seconds')}s ({r.get('cpu_utilization_cores')} cores), GPU={r.get('gpu_utilization_percent')}%, top threads={tops}, failure={r.get('failure')}")
            report = "\n".join(lines) + "\n"
        (Path(sys.argv[2]) / "summary.md").write_text(report, encoding="utf-8")
        print(report, end="")
        return
    if len(sys.argv) == 2 and Path(sys.argv[1], "result.json").exists():
        d = read(sys.argv[1])
        print(comparison_text(d) if d.get("comparison") else json.dumps(d, indent=2))
        return
    if len(sys.argv) != 3:
        raise SystemExit("usage: python -B tools/bench_report.py RESULT_DIR_A RESULT_DIR_B")
    a, b = map(read, sys.argv[1:])
    for label, data in (("A", a), ("B", b)):
        print(f"{label}: {data['identity']['sha']} ({data['identity']['branch']}) {data['identity']['config_id']} dirty={data['identity']['dirty']}")
        for key in ("fps", "frametime_mean_ms", "frametime_p95_ms", "frametime_p99_ms", "cpu_utilization_cores", "gpu_utilization_percent"):
            print(f"  {key}: {stats(successful(data, key))}")


if __name__ == "__main__":
    main()
