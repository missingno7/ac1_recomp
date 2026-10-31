#!/usr/bin/env python3
"""Summarize AC1 gameplay runs and interleaved comparisons."""
import argparse
import csv
import hashlib
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


def internal_run(run, seconds=0, workload="gameplay", start_present=1800,
                 scene_present=1800, flush_frames=1, end_present=2200):
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
    start_draw = next((draw for index, draw in capture_rows if index == start_present), None)
    end_draw = next((draw for index, draw in capture_rows if index == end_present), None)
    draws = end_draw - start_draw if start_draw is not None and end_draw is not None else None
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
    runtime_rows = []
    for row in sampled:
        try:
            parsed = {key: float(row[key]) for key in
                      ("present_index", "wall_ms", "main_thread_cpu_ms", "process_cpu_ms")}
            for key in ("main_thread_cycles", "processor", "guest_time_ns", "input_command_index",
                        "draw_calls", "shader_constant_uploads", "resource_locks"):
                value = row.get(key, "")
                parsed[key] = int(value) if value not in (None, "") else None
            if start_present <= parsed["present_index"] <= end_present:
                runtime_rows.append(parsed)
        except (KeyError, TypeError, ValueError):
            continue
    runtime_coverage_ms = sum(row["wall_ms"] for row in runtime_rows)
    wall_times = sorted(row["wall_ms"] for row in runtime_rows)
    main_cpu = [row["main_thread_cpu_ms"] for row in runtime_rows]
    process_cpu = [row["process_cpu_ms"] for row in runtime_rows]
    runtime_full_window = (bool(runtime_rows) and runtime_rows[0]["present_index"] == start_present
                           and runtime_rows[-1]["present_index"] == end_present
                           and len(runtime_rows) == end_present - start_present + 1)
    frame_dropped = 0
    commands = [row["input_command_index"] for row in runtime_rows if row["input_command_index"] is not None]
    guest_times = [row["guest_time_ns"] for row in runtime_rows if row["guest_time_ns"] is not None]
    cycles = [row["main_thread_cycles"] for row in runtime_rows if row["main_thread_cycles"] is not None]
    draw_rows = [row["draw_calls"] for row in runtime_rows if row["draw_calls"] is not None]
    constant_rows = [row["shader_constant_uploads"] for row in runtime_rows if row["shader_constant_uploads"] is not None]
    lock_rows = [row["resource_locks"] for row in runtime_rows if row["resource_locks"] is not None]
    processors = [row["processor"] for row in runtime_rows if row["processor"] is not None]
    evidence_complete = len(commands) == len(runtime_rows) and len(guest_times) == len(runtime_rows)
    reached = runtime_full_window and evidence_complete and not stop_lines
    if stop_lines:
        reason = "guest stopped"
    elif not runtime_full_window:
        reason = f"incomplete Present range {start_present}-{end_present}: retained {len(runtime_rows)} frames"
    elif not evidence_complete:
        reason = "missing per-frame replay command or guest-clock equivalence evidence"
    else:
        reason = None
    frame_times = wall_times
    boundary_paths = [run / "captures" / f"present_{p:06d}.bmp" for p in (start_present, end_present)]
    boundary_hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in boundary_paths if path.exists()}
    return {
        "workload": workload, "reached_workload": reached,
        "reached_gameplay": reached and workload == "gameplay",
        "scene_start_present_index": scene_present, "measurement_start_present_index": start_present,
        "full_frame_window": runtime_full_window, "frame_capture_coverage_seconds": runtime_coverage_ms / 1000,
        "frame_stats_flush_frames": flush_frames, "frame_dropped": frame_dropped,
        "frames_in_window": len(runtime_rows), "fps": 1000 / statistics.mean(frame_times) if frame_times else None,
        "present_range": [start_present, end_present],
        "measurement_elapsed_seconds": runtime_coverage_ms / 1000,
        "frametime_mean_ms": statistics.mean(frame_times) if frame_times else None,
        "frametime_p50_ms": percentile(frame_times, .5), "frametime_p95_ms": percentile(frame_times, .95),
        "frametime_p99_ms": percentile(frame_times, .99), "frametime_min_ms": min(frame_times) if frame_times else None,
        "frametime_max_ms": max(frame_times) if frame_times else None, "draws_per_frame": draws,
        "runtime_frame_stats_coverage_seconds": runtime_coverage_ms / 1000,
        "main_thread_cpu_ms_per_frame": statistics.mean(main_cpu) if main_cpu else None,
        "draw_calls_in_window": sum(draw_rows) if draw_rows else draws,
        "shader_constant_uploads_in_window": sum(constant_rows) if constant_rows else None,
        "resource_locks_in_window": sum(lock_rows) if lock_rows else None,
        "process_cpu_ms_per_frame": statistics.mean(process_cpu) if process_cpu else None,
        "process_cpu_p95_ms_per_frame": percentile(sorted(process_cpu), .95),
        "process_cpu_p99_ms_per_frame": percentile(sorted(process_cpu), .99),
        "process_cpu_seconds": sum(process_cpu) / 1000 if process_cpu else None,
        # Per-frame cost is multimodal (short and long frames alternate); the
        # window mean is the work per frame, a median can land in either mode.
        "main_thread_cycles_per_frame": statistics.mean(cycles) if cycles else None,
        "main_thread_cycles_p50": percentile(sorted(cycles), .50),
        "main_thread_cycles_p95": percentile(sorted(cycles), .95),
        "main_thread_cycles_p99": percentile(sorted(cycles), .99),
        "processor_migrations": sum(a != b for a, b in zip(processors, processors[1:])),
        "processors_seen": sorted(set(processors)),
        "input_command_index_start": commands[0] if commands else None,
        "input_command_index_end": commands[-1] if commands else None,
        "guest_time_elapsed_ns": guest_times[-1] - guest_times[0] if len(guest_times) > 1 else None,
        "boundary_captures": [f"captures/present_{p:06d}.bmp" for p in (start_present, end_present)
                              if (run / "captures" / f"present_{p:06d}.bmp").exists()],
        "boundary_capture_sha256": boundary_hashes,
        "workload_equivalence_evidence": {
            "command_progression_complete": len(commands) == len(runtime_rows),
            "guest_clock_complete": len(guest_times) == len(runtime_rows),
            "guest_time_elapsed_ns": guest_times[-1] - guest_times[0] if len(guest_times) > 1 else None,
            "boundary_captures": [f"captures/present_{p:06d}.bmp" for p in (start_present, end_present)
                                  if (run / "captures" / f"present_{p:06d}.bmp").exists()],
            "boundary_capture_sha256": boundary_hashes,
        },
        "gpu_utilization_percent": statistics.mean(s["utilization_percent"] for s in gpu_samples) if gpu_samples else None,
        "gpu_samples": gpu_samples, "scene_proof": [str(c.relative_to(run)) for c in proof],
        "profile_snapshots": 0, "present_trace_points": [],
        "failure": None if reached else {"reason": reason, "guest_stop_lines": stop_lines,
                                         "startup_log_tail": log.splitlines()[-12:]}
    }


# Two-sided 95% Student-t quantiles by degrees of freedom. A bootstrap of the
# median over three same-sign A/A deltas always "excluded zero" (a 25% false
# positive rate); the t interval on the mean is conservative for small n.
T975 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
        8: 2.306, 9: 2.262, 10: 2.228, 12: 2.179, 15: 2.131, 20: 2.086, 30: 2.042}


def t975(df):
    return T975[max(k for k in T975 if k <= df)] if df < 30 else 1.96


def adaptive_decision(deltas, baseline, executed_pairs, min_pairs=3, max_pairs=8,
                      practical_percent=1.0):
    deltas = [float(value) for value in deltas if value is not None]
    baseline = [float(value) for value in baseline if value is not None]
    center = statistics.median(deltas) if deltas else None
    mean = statistics.mean(deltas) if deltas else None
    ci_low = ci_high = half_width = practical_threshold = None
    verdict = "insufficient valid pairs"
    stop_reason = "continue until minimum valid pair count"
    stop = False
    if baseline:
        practical_threshold = statistics.median(baseline) * practical_percent / 100.0
    if len(deltas) >= max(2, min_pairs):
        half_width = t975(len(deltas) - 1) * statistics.stdev(deltas) / len(deltas) ** 0.5
        ci_low, ci_high = mean - half_width, mean + half_width
        if ci_low > 0 or ci_high < 0:
            stop = True
            verdict = "B regression" if mean > 0 else "B improvement"
            stop_reason = "95% t CI of the mean paired delta excludes zero"
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
        "paired_delta_median": center, "paired_delta_mean": mean,
        "paired_delta_min": min(deltas) if deltas else None,
        "paired_delta_max": max(deltas) if deltas else None,
        "paired_delta_spread": max(deltas) - min(deltas) if deltas else None,
        "ci95_low": ci_low, "ci95_high": ci_high,
        "ci95_half_width": half_width,
        "practical_threshold": practical_threshold,
        "practical_percent": practical_percent,
    }


def segment_deltas(comparison, lo, hi):
    """Paired B-A main-thread cycles/frame (% of A) over Presents [lo, hi) of
    each pair's per-Present CSVs, e.g. the part of the window before a scene
    change that a faster side reaches later."""
    deltas = []
    for a_path in sorted(Path(comparison, "A").glob("pair-*/run-*/frame-stats.csv")):
        b_path = Path(comparison, "B", *a_path.parts[-3:])
        means = []
        for path in (a_path, b_path):
            with open(path, newline="") as f:
                cycles = [float(r["main_thread_cycles"]) for r in csv.DictReader(f)
                          if r["present_index"].isdigit() and lo <= int(r["present_index"]) < hi
                          and r["main_thread_cycles"]]
            means.append(statistics.mean(cycles) if cycles else None)
        if None not in means:
            deltas.append((means[1] - means[0]) / means[0] * 100)
    if len(deltas) < 2:
        return deltas, None, None
    mean = statistics.mean(deltas)
    half = t975(len(deltas) - 1) * statistics.stdev(deltas) / len(deltas) ** 0.5
    return deltas, mean, (mean - half, mean + half)


def successful(data, key):
    return [r[key] for r in data.get("runs", []) if r.get("reached_workload", r.get("reached_gameplay")) and isinstance(r.get(key), (int, float))]


def stats(values):
    values = sorted(values)
    return {"n": len(values), "median": statistics.median(values) if values else None,
            "min": min(values) if values else None, "max": max(values) if values else None,
            "p95": percentile(values, .95), "p99": percentile(values, .99)}


def comparison_text(d):
    workload = d.get("workload", "gameplay")
    equivalence = d.get("pair_equivalence", [])
    valid_pair_indices = {item.get("pair", 0) - 1 for item in equivalence if item.get("equivalent")}
    lines = [f"# AC1 interleaved {workload} comparison", "", f"- Pair count: {d['pair_count']}; A failures: {d['A']['failures']}; B failures: {d['B']['failures']}", f"- Vsync: {d.get('vsync', 'unknown')}", f"- CCD affinity: {d.get('pin_mask', 'unknown')}; exploratory={d.get('exploratory', False)}; lock acquired={d.get('lock_acquired', False)}", f"- Workload-equivalent pairs: {len(valid_pair_indices)}; excluded pairs: {sum(1 for item in equivalence if not item.get('equivalent'))}", ""]
    decision = d.get("adaptive_decision")
    for item in equivalence:
        lines.append(f"- Pair {item.get('pair')}: equivalent={item.get('equivalent')}; replay commands match={item.get('command_indices_match')}; guest elapsed ns A/B={item.get('guest_time_A_ns')}/{item.get('guest_time_B_ns')}; boundary hashes match={item.get('observable_boundary_hashes_match')}; captures A/B={item.get('captures_A')}/{item.get('captures_B')}")
    if equivalence:
        lines.append("")
    if decision:
        lines.extend([
            f"- Adaptive verdict: {decision.get('verdict')}; stop reason: {decision.get('stop_reason')}",
            f"- Valid paired main-thread cycles/frame deltas (B - A): n={decision.get('valid_pairs')}, median={decision.get('paired_delta_median')}, spread={decision.get('paired_delta_min')}..{decision.get('paired_delta_max')} (range {decision.get('paired_delta_spread')})",
            f"- Mean paired delta {decision.get('paired_delta_mean')}; 95% t CI: [{decision.get('ci95_low')}, {decision.get('ci95_high')}] cycles/frame; half-width={decision.get('ci95_half_width')}; practical threshold={decision.get('practical_threshold')} cycles/frame",
            "",
        ])
    side_metrics = (("frametime_mean_ms", "mean frame time ms"),
                    ("frametime_p50_ms", "wall frame ms p50"),
                    ("frametime_p95_ms", "wall frame ms p95"),
                    ("frametime_p99_ms", "wall frame ms p99"),
                    ("main_thread_cpu_ms_per_frame", "main CPU ms/frame mean"),
                    ("process_cpu_ms_per_frame", "process CPU ms/frame mean"),
                    ("process_cpu_p95_ms_per_frame", "process CPU ms/frame p95"),
                    ("process_cpu_p99_ms_per_frame", "process CPU ms/frame p99"),
                    ("main_thread_cycles_per_frame", "main-thread cycles/frame mean"),
                    ("main_thread_cycles_p50", "main-thread cycles/frame p50"),
                    ("main_thread_cycles_p95", "main-thread cycles/frame p95"),
                    ("main_thread_cycles_p99", "main-thread cycles/frame p99"),
                    ("draw_calls_in_window", "draw calls in window (optimization effect)"),
                    ("shader_constant_uploads_in_window", "shader constant uploads in window"),
                    ("resource_locks_in_window", "resource locks in window"),
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
                      ("main_thread_cpu_ms_per_frame", "main CPU mean ms/frame"),
                      ("process_cpu_ms_per_frame", "process CPU mean ms/frame"),
                      ("process_cpu_p95_ms_per_frame", "process CPU p95 ms/frame"),
                      ("process_cpu_p99_ms_per_frame", "process CPU p99 ms/frame"),
                      ("main_thread_cycles_per_frame", "main-thread cycles/frame mean"),
                      ("main_thread_cycles_p50", "main-thread cycles/frame p50"),
                      ("main_thread_cycles_p95", "main-thread cycles/frame p95"),
                      ("main_thread_cycles_p99", "main-thread cycles/frame p99"),
                      ("draw_calls_in_window", "draw calls in window"),
                      ("shader_constant_uploads_in_window", "shader constant uploads in window"),
                      ("resource_locks_in_window", "resource locks in window"), ("fps", "FPS"))
    for key, label in paired_metrics:
        delta = d.get("paired_deltas", {}).get(key)
        valid_count = 0
        if delta is None:
            pairs = [(a.get(key), b.get(key)) for index, (a, b) in enumerate(zip(d["A"].get("runs", []), d["B"].get("runs", [])))
                     if (not equivalence or index in valid_pair_indices) and
                     a.get("reached_workload", a.get("reached_gameplay")) and b.get("reached_workload", b.get("reached_gameplay")) and
                     isinstance(a.get(key), (int, float)) and isinstance(b.get(key), (int, float))]
            valid_count = len(pairs)
            delta = statistics.median([y - x for x, y in pairs]) if pairs else None
        else:
            valid_count = len(valid_pair_indices) if equivalence else sum(
                1 for a, b in zip(d["A"].get("runs", []), d["B"].get("runs", []))
                if a.get("reached_workload", a.get("reached_gameplay")) and b.get("reached_workload", b.get("reached_gameplay")))
        lines.append(f"- {label}: {delta} (workload-equivalent pairs={valid_count})")
    lines.append("")
    return "\n".join(lines)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--internal-run":
        p = argparse.ArgumentParser()
        p.add_argument("--internal-run", type=Path, required=True)
        p.add_argument("--seconds", type=int, default=0)
        p.add_argument("--workload", choices=("gameplay", "menu"), default="gameplay")
        p.add_argument("--start-present", type=int, default=1800)
        p.add_argument("--end-present", type=int, default=2200)
        p.add_argument("--scene-present", type=int, default=1800)
        p.add_argument("--flush-frames", type=int, default=8)
        args = p.parse_args()
        print(json.dumps(internal_run(args.internal_run, args.seconds, args.workload,
                                      args.start_present, args.scene_present, args.flush_frames,
                                      args.end_present)))
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
            lines = [f"# AC1 {workload} benchmark", "", f"- Identity: `{d['identity']['sha']}` / `{d['identity']['config_id']}`; branch `{d['identity']['branch']}`; dirty={d['identity']['dirty']}", f"- Binary SHA-256: `{d['identity']['binary_sha256']}`", f"- Machine: {d['machine']['cpu_model']}; {d['machine']['logical_cores']} logical cores; {d['machine']['physical_memory_bytes']} bytes RAM", f"- Vsync: {env.get('vsync', 'unknown')}", f"- Present window: {env.get('start_present', 'unknown')}-{env.get('end_present', 'unknown')}; pin mask={env.get('pin_mask', 'unknown')}", f"- Build generator: {env.get('actual_build_generator', 'unknown')}", f"- Failed runs: {failed}", ""]
            for r in d["runs"]:
                lines.append(f"- Run {r['run']}: reached={r.get('reached_workload')}, main CPU ms/frame median={r.get('main_thread_cpu_ms_per_frame')}, cycles/frame p50/p95/p99={r.get('main_thread_cycles_p50')}/{r.get('main_thread_cycles_p95')}/{r.get('main_thread_cycles_p99')}, migrations={r.get('processor_migrations')}, guest ns={r.get('guest_time_elapsed_ns')}, command indices={r.get('input_command_index_start')} to {r.get('input_command_index_end')}, other CPU={r.get('other_process_cpu_percent')}%, frequency MHz={r.get('cpu_frequency_start_mhz')} to {r.get('cpu_frequency_end_mhz')}, draws/uploads/locks={r.get('draw_calls_in_window')}/{r.get('shader_constant_uploads_in_window')}/{r.get('resource_locks_in_window')}, wall p50/p95/p99={r.get('frametime_p50_ms')}/{r.get('frametime_p95_ms')}/{r.get('frametime_p99_ms')}, FPS={r.get('fps')}, failure={r.get('failure')}")
            report = "\n".join(lines) + "\n"
        (Path(sys.argv[2]) / "summary.md").write_text(report, encoding="utf-8")
        print(report, end="")
        return
    if len(sys.argv) >= 4 and sys.argv[1] == "--segment":
        for span in sys.argv[3:]:
            lo, hi = map(int, span.split("-"))
            deltas, mean, ci = segment_deltas(sys.argv[2], lo, hi)
            shown = ", ".join(f"{d:+.1f}%" for d in deltas)
            print(f"Presents {lo}-{hi}: main-thread cycles/frame B-A per pair [{shown}]" +
                  (f"; mean {mean:+.1f}%, 95% t CI [{ci[0]:+.1f}%, {ci[1]:+.1f}%]" if ci else ""))
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
        for key in ("main_thread_cpu_ms_per_frame", "process_cpu_ms_per_frame", "main_thread_cycles_per_frame", "main_thread_cycles_p95", "main_thread_cycles_p99", "frametime_p95_ms", "frametime_p99_ms", "fps", "gpu_utilization_percent"):
            print(f"  {key}: {stats(successful(data, key))}")


if __name__ == "__main__":
    main()
