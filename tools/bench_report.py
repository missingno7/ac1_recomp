#!/usr/bin/env python3
"""Summarize AC1 gameplay runs and interleaved comparisons."""
import argparse
import csv
import json
import random
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


def internal_run(run, seconds, workload="gameplay", start_present=1316,
                 scene_present=1300, flush_frames=8):
    log_path = run / "startup.log"
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    captures = list((run / "captures").glob("present_*.bmp"))
    import re
    capture_rows = []
    for line in log.splitlines():
        m = re.search(r"present_(\d+)\.bmp .*draw_calls=(\d+)", line)
        if m:
            capture_rows.append((int(m.group(1)), int(m.group(2))))
    proof = [c for c in captures if int(re.search(r"present_(\d+)", c.name).group(1)) >= scene_present]
    eligible = [row for row in capture_rows if row[0] >= scene_present]
    draws = (eligible[-1][1] - eligible[-2][1]) / max(1, eligible[-1][0] - eligible[-2][0]) if len(eligible) >= 2 else None
    gpu_samples = []
    gpu_path = run / "gpu.csv"
    if gpu_path.exists():
        for line in gpu_path.read_text(errors="replace").splitlines():
            m = re.match(r"\s*(\d+)\s*,\s*(\d+)", line)
            if m:
                gpu_samples.append({"utilization_percent": int(m.group(1)), "graphics_clock_mhz": int(m.group(2))})
    stop_lines = [line for line in log.splitlines() if "AC1 guest stop" in line]
    frame_stats = run / "frame-stats.csv"
    sampled = []
    if frame_stats.exists():
        with frame_stats.open(newline="", encoding="utf-8-sig") as source:
            sampled = list(csv.DictReader(source))
    sampled = [{key: float(row[key]) for key in ("present_index", "wall_ms", "main_thread_cpu_ms", "process_cpu_ms")}
               for row in sampled if all(row.get(key) not in (None, "") for key in
                                        ("present_index", "wall_ms", "main_thread_cpu_ms", "process_cpu_ms"))]
    runtime_rows = []
    runtime_coverage_ms = 0.0
    for row in sampled:
        if row["present_index"] < start_present:
            continue
        if runtime_coverage_ms + row["wall_ms"] > seconds * 1000:
            break
        runtime_rows.append(row)
        runtime_coverage_ms += row["wall_ms"]
    wall_times = sorted(row["wall_ms"] for row in runtime_rows)
    main_cpu = [row["main_thread_cpu_ms"] for row in runtime_rows]
    process_cpu = [row["process_cpu_ms"] for row in runtime_rows]
    flush_tolerance_ms = max(wall_times, default=0.0) * flush_frames
    runtime_full_window = bool(wall_times) and runtime_coverage_ms >= seconds * 1000 - flush_tolerance_ms
    frame_dropped = 0
    reached = runtime_full_window and bool(proof) and any(draw > 0 for _, draw in capture_rows) and not stop_lines
    if stop_lines:
        reason = "guest stopped"
    elif not runtime_full_window:
        reason = f"incomplete {seconds}-second runtime frame window: retained {runtime_coverage_ms / 1000:.3f} seconds"
    elif not proof or not any(draw > 0 for _, draw in capture_rows):
        reason = f"{workload} scene proof missing at/after Present {scene_present}"
    else:
        reason = None
    frame_times = wall_times
    return {
        "workload": workload, "reached_workload": reached,
        "reached_gameplay": reached and workload == "gameplay",
        "scene_start_present_index": scene_present, "measurement_start_present_index": start_present,
        "full_frame_window": runtime_full_window, "frame_capture_coverage_seconds": runtime_coverage_ms / 1000,
        "frame_stats_flush_frames": flush_frames, "frame_dropped": frame_dropped,
        "frames_in_window": len(runtime_rows), "fps": 1000 / statistics.mean(frame_times) if frame_times else None,
        "measurement_elapsed_seconds": runtime_coverage_ms / 1000,
        "frametime_mean_ms": statistics.mean(frame_times) if frame_times else None,
        "frametime_p50_ms": percentile(frame_times, .5), "frametime_p95_ms": percentile(frame_times, .95),
        "frametime_p99_ms": percentile(frame_times, .99), "frametime_min_ms": min(frame_times) if frame_times else None,
        "frametime_max_ms": max(frame_times) if frame_times else None, "draws_per_frame": draws,
        "runtime_frame_stats_coverage_seconds": runtime_coverage_ms / 1000,
        "main_thread_cpu_ms_per_frame": statistics.median(main_cpu) if main_cpu else None,
        "main_thread_cpu_p95_ms_per_frame": percentile(sorted(main_cpu), .95),
        "main_thread_cpu_p99_ms_per_frame": percentile(sorted(main_cpu), .99),
        "process_cpu_ms_per_frame": statistics.median(process_cpu) if process_cpu else None,
        "process_cpu_p95_ms_per_frame": percentile(sorted(process_cpu), .95),
        "process_cpu_p99_ms_per_frame": percentile(sorted(process_cpu), .99),
        "process_cpu_seconds": sum(process_cpu) / 1000 if process_cpu else None,
        "gpu_utilization_percent": statistics.mean(s["utilization_percent"] for s in gpu_samples) if gpu_samples else None,
        "gpu_samples": gpu_samples, "scene_proof": [str(c.relative_to(run)) for c in proof],
        "profile_snapshots": 0, "present_trace_points": [],
        "failure": None if reached else {"reason": reason, "guest_stop_lines": stop_lines,
                                         "startup_log_tail": log.splitlines()[-12:]}
    }


def adaptive_decision(deltas, baseline, executed_pairs, min_pairs=3, max_pairs=8,
                      practical_percent=1.0, bootstrap_samples=20000):
    deltas = [float(value) for value in deltas if value is not None]
    baseline = [float(value) for value in baseline if value is not None]
    center = statistics.median(deltas) if deltas else None
    ci_low = ci_high = half_width = practical_threshold = None
    verdict = "insufficient valid pairs"
    stop_reason = "continue until minimum valid pair count"
    stop = False
    if baseline:
        practical_threshold = statistics.median(baseline) * practical_percent / 100.0
    if len(deltas) >= min_pairs:
        rng = random.Random(0xAC1B00 + len(deltas))
        medians = [statistics.median(rng.choices(deltas, k=len(deltas)))
                   for _ in range(bootstrap_samples)]
        medians.sort()
        ci_low = percentile(medians, 0.025)
        ci_high = percentile(medians, 0.975)
        half_width = (ci_high - ci_low) / 2.0
        if ci_low > 0 or ci_high < 0:
            stop = True
            verdict = "B regression" if center > 0 else "B improvement"
            stop_reason = "95% bootstrap CI excludes zero"
        elif practical_threshold is not None and half_width < practical_threshold:
            stop = True
            verdict = "no meaningful difference"
            stop_reason = f"95% CI half-width is below {practical_percent:g}% of baseline"
        else:
            verdict = "inconclusive"
            stop_reason = "CI still overlaps zero and is wider than the practical threshold"
    if not stop and executed_pairs >= max_pairs:
        stop = True
        if len(deltas) < min_pairs:
            verdict = "inconclusive"
            stop_reason = "maximum pairs reached before enough valid pairs"
        else:
            stop_reason = "maximum pairs reached"
    return {
        "stop": stop, "stop_reason": stop_reason, "verdict": verdict,
        "executed_pairs": int(executed_pairs), "valid_pairs": len(deltas),
        "paired_delta_median_ms": center,
        "paired_delta_min_ms": min(deltas) if deltas else None,
        "paired_delta_max_ms": max(deltas) if deltas else None,
        "paired_delta_spread_ms": max(deltas) - min(deltas) if deltas else None,
        "ci95_low_ms": ci_low, "ci95_high_ms": ci_high,
        "ci95_half_width_ms": half_width,
        "practical_threshold_ms": practical_threshold,
        "practical_percent": practical_percent,
    }


def successful(data, key):
    return [r[key] for r in data.get("runs", []) if r.get("reached_workload", r.get("reached_gameplay")) and isinstance(r.get(key), (int, float))]


def stats(values):
    values = sorted(values)
    return {"n": len(values), "median": statistics.median(values) if values else None,
            "min": min(values) if values else None, "max": max(values) if values else None,
            "p95": percentile(values, .95), "p99": percentile(values, .99)}


def comparison_text(d):
    workload = d.get("workload", "gameplay")
    lines = [f"# AC1 interleaved {workload} comparison", "", f"- Pair count: {d['pair_count']}; A failures: {d['A']['failures']}; B failures: {d['B']['failures']}", f"- Vsync: {d.get('vsync', 'unknown')}", f"- Busy-host override: {d.get('host_load_override', False)}", ""]
    decision = d.get("adaptive_decision")
    if decision:
        lines.extend([
            f"- Adaptive verdict: {decision.get('verdict')}; stop reason: {decision.get('stop_reason')}",
            f"- Valid paired main-CPU deltas (B - A): n={decision.get('valid_pairs')}, median={decision.get('paired_delta_median_ms')} ms/frame, spread={decision.get('paired_delta_min_ms')}..{decision.get('paired_delta_max_ms')} (range {decision.get('paired_delta_spread_ms')})",
            f"- 95% bootstrap CI: [{decision.get('ci95_low_ms')}, {decision.get('ci95_high_ms')}] ms/frame; half-width={decision.get('ci95_half_width_ms')}; practical threshold={decision.get('practical_threshold_ms')} ms/frame",
            "",
        ])
    side_metrics = (("frametime_mean_ms", "mean frame time ms"),
                    ("frametime_p50_ms", "wall frame ms p50"),
                    ("frametime_p95_ms", "wall frame ms p95"),
                    ("frametime_p99_ms", "wall frame ms p99"),
                    ("main_thread_cpu_ms_per_frame", "main CPU ms/frame median"),
                    ("main_thread_cpu_p95_ms_per_frame", "main CPU ms/frame p95"),
                    ("main_thread_cpu_p99_ms_per_frame", "main CPU ms/frame p99"),
                    ("process_cpu_ms_per_frame", "process CPU ms/frame median"),
                    ("process_cpu_p95_ms_per_frame", "process CPU ms/frame p95"),
                    ("process_cpu_p99_ms_per_frame", "process CPU ms/frame p99"),
                    ("gpu_utilization_percent", "GPU %"), ("fps", "FPS"))
    for side in ("A", "B"):
        lines.append(f"## {side}: {d[side]['label']}")
        for key, label in side_metrics:
            s = d[side].get("stats", {}).get(key) or stats(successful(d[side], key))
            lines.append(f"- {label}: median={s['median']}, min={s['min']}, max={s['max']}, p95={s['p95']}, p99={s['p99']} (n={s['n']})")
        for run in d[side].get("runs", []):
            if not run.get("reached_workload", run.get("reached_gameplay")):
                failure = run.get("failure") or {}
                stop = failure.get("guest_stop_lines") or []
                lines.append(f"- Failed run {run.get('run')}: {stop[-1] if stop else failure.get('reason', 'gameplay not reached')}")
        lines.append("")
    lines.append("## Paired median deltas (B - A)")
    paired_metrics = (("frametime_mean_ms", "wall frame mean ms"),
                      ("frametime_p50_ms", "wall frame p50 ms"),
                      ("frametime_p95_ms", "wall frame p95 ms"),
                      ("frametime_p99_ms", "wall frame p99 ms"),
                      ("main_thread_cpu_ms_per_frame", "main CPU median ms/frame"),
                      ("main_thread_cpu_p95_ms_per_frame", "main CPU p95 ms/frame"),
                      ("main_thread_cpu_p99_ms_per_frame", "main CPU p99 ms/frame"),
                      ("process_cpu_ms_per_frame", "process CPU median ms/frame"),
                      ("process_cpu_p95_ms_per_frame", "process CPU p95 ms/frame"),
                      ("process_cpu_p99_ms_per_frame", "process CPU p99 ms/frame"), ("fps", "FPS"))
    for key, label in paired_metrics:
        delta = d.get("paired_deltas", {}).get(key)
        if delta is None:
            pairs = [(a.get(key), b.get(key)) for a, b in zip(d["A"].get("runs", []), d["B"].get("runs", []))
                     if a.get("reached_workload", a.get("reached_gameplay")) and b.get("reached_workload", b.get("reached_gameplay")) and isinstance(a.get(key), (int, float)) and isinstance(b.get(key), (int, float))]
            delta = statistics.median([y - x for x, y in pairs]) if pairs else None
        lines.append(f"- {label}: {delta} (valid pairs={sum(1 for a, b in zip(d['A'].get('runs', []), d['B'].get('runs', [])) if a.get('reached_workload', a.get('reached_gameplay')) and b.get('reached_workload', b.get('reached_gameplay')) )})")
    lines.append("")
    return "\n".join(lines)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--internal-run":
        p = argparse.ArgumentParser()
        p.add_argument("--internal-run", type=Path, required=True)
        p.add_argument("--seconds", type=int, required=True)
        p.add_argument("--workload", choices=("gameplay", "menu"), default="gameplay")
        p.add_argument("--start-present", type=int, default=1316)
        p.add_argument("--scene-present", type=int, default=1300)
        p.add_argument("--flush-frames", type=int, default=8)
        args = p.parse_args()
        print(json.dumps(internal_run(args.internal_run, args.seconds, args.workload,
                                      args.start_present, args.scene_present, args.flush_frames)))
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--decision-json":
        p = argparse.ArgumentParser()
        p.add_argument("--decision-json", type=Path, required=True)
        p.add_argument("--min-pairs", type=int, default=3)
        p.add_argument("--max-pairs", type=int, default=8)
        p.add_argument("--practical-percent", type=float, default=1.0)
        args = p.parse_args()
        state = json.loads(args.decision_json.read_text(encoding="utf-8-sig"))
        print(json.dumps(adaptive_decision(state.get("deltas", []), state.get("baseline", []),
                                           state.get("executed_pairs", 0), args.min_pairs,
                                           args.max_pairs, args.practical_percent)))
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--summary":
        d = read(sys.argv[2])
        if d.get("comparison"):
            report = comparison_text(d)
        else:
            workload = d.get("environment", {}).get("workload", "gameplay")
            failed = sum(not r.get("reached_workload", r.get("reached_gameplay")) for r in d.get("runs", []))
            env = d.get("environment", {})
            lines = [f"# AC1 {workload} benchmark", "", f"- Identity: `{d['identity']['sha']}` / `{d['identity']['config_id']}`; branch `{d['identity']['branch']}`; dirty={d['identity']['dirty']}", f"- Binary SHA-256: `{d['identity']['binary_sha256']}`", f"- Machine: {d['machine']['cpu_model']}; {d['machine']['logical_cores']} logical cores; {d['machine']['physical_memory_bytes']} bytes RAM", f"- Vsync: {env.get('vsync', 'unknown')}", f"- Start: scene Present {env.get('start_present', 'unknown')}; measured from Present {env.get('measurement_start_present', 'unknown')} after {env.get('cpu_sample_window_frames', 'unknown')}-Present CPU smoothing window", f"- Build generator: {env.get('actual_build_generator', 'unknown')}", f"- Failed runs: {failed}", ""]
            for r in d["runs"]:
                lines.append(f"- Run {r['run']}: reached_workload={r.get('reached_workload', False)}, main CPU ms/frame median/p95/p99={r.get('main_thread_cpu_ms_per_frame')}/{r.get('main_thread_cpu_p95_ms_per_frame')}/{r.get('main_thread_cpu_p99_ms_per_frame')}, process CPU ms/frame median/p95/p99={r.get('process_cpu_ms_per_frame')}/{r.get('process_cpu_p95_ms_per_frame')}/{r.get('process_cpu_p99_ms_per_frame')}, wall ms p50/p95/p99={r.get('frametime_p50_ms')}/{r.get('frametime_p95_ms')}/{r.get('frametime_p99_ms')}, FPS={r.get('fps')}, GPU={r.get('gpu_utilization_percent')}%, failure={r.get('failure')}")
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
        for key in ("main_thread_cpu_ms_per_frame", "main_thread_cpu_p95_ms_per_frame", "main_thread_cpu_p99_ms_per_frame", "process_cpu_ms_per_frame", "frametime_p95_ms", "frametime_p99_ms", "fps", "gpu_utilization_percent"):
            print(f"  {key}: {stats(successful(data, key))}")


if __name__ == "__main__":
    main()
