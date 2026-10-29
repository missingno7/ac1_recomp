#!/usr/bin/env python3
"""Compare immutable AC1 gameplay benchmark result directories."""
import json
import argparse
import math
import re
import statistics
import sys
from pathlib import Path


def read(path):
    p = Path(path) / "result.json"
    if not p.is_file():
        raise SystemExit(f"missing result: {p}")
    return json.loads(p.read_text(encoding="utf-8-sig"))


def vals(data, key):
    return [r[key] for r in data.get("runs", []) if r.get("reached_gameplay") and isinstance(r.get(key), (int, float))]


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--internal-run":
        p = argparse.ArgumentParser()
        p.add_argument("--internal-run", type=Path, required=True)
        p.add_argument("--seconds", type=int, required=True)
        args = p.parse_args()
        run = args.internal_run
        log = (run / "startup.log").read_text(errors="replace") if (run / "startup.log").exists() else ""
        traces = [(int(a), int(b)) for a, b in re.findall(r"present_calls=(\d+) draw_calls=(\d+)", log)]
        caps = list((run / "captures").glob("present_*.bmp"))
        pg = run / "profile"
        jsons = sorted(pg.glob("raw_*.json"), key=lambda x: x.stat().st_mtime) if pg.exists() else []
        data = []
        for jf in jsons:
            try:
                d = json.loads(jf.read_text())
                if d.get("mode") == "frames": data.append(d)
            except (OSError, ValueError): pass
        # Profile emits 10-second snapshots. Each frame timestamp is a successful Present.
        data.sort(key=lambda d: d["start"])
        frames = []
        for d in data:
            frames.extend((int(t), int(d["frequency"])) for t in d.get("frames", []))
        frames.sort()
        # Replay actions end at successful present 1500; 128 frames lets the next scene settle.
        start_index = 1628
        start_tick = frames[start_index][0] if len(frames) > start_index else None
        window = []
        if start_tick is not None:
            window = [t for t, f in frames if start_tick <= t <= start_tick + args.seconds * f]
        proof = [c for c in caps if int(re.search(r"present_(\d+)", c.name).group(1)) >= start_index]
        reached = len(window) >= 30 and bool(proof) and any(draw > 0 for _, draw in traces)
        freq = frames[start_index][1] if start_tick is not None else 1
        intervals = [(window[i]-window[i-1])/freq*1000 for i in range(1, len(window))]
        measurement_elapsed = (window[-1]-window[0])/freq if len(window)>1 else 0.0
        intervals.sort()
        def percentile(q):
            if not intervals: return None
            x=(len(intervals)-1)*q; lo=int(x); return intervals[lo]+(intervals[min(lo+1,len(intervals)-1)]-intervals[lo])*(x-lo)
        run_data = {"reached_gameplay": reached, "gameplay_start_present_index": start_index,
                    "frames_in_window": len(window), "fps": 1000/statistics.mean(intervals) if intervals else None,
                    "measurement_elapsed_seconds": measurement_elapsed,
                    "frametime_mean_ms": statistics.mean(intervals) if intervals else None,
                    "frametime_p50_ms": percentile(.5), "frametime_p95_ms": percentile(.95),
                    "frametime_p99_ms": percentile(.99), "frametime_max_ms": max(intervals) if intervals else None,
                    "draws_per_frame": None, "process_cpu_seconds": None, "cpu_utilization_cores": None,
                    "top_threads": [], "gpu_utilization_percent": None,
                    "scene_proof": [str(c.relative_to(run)) for c in proof],
                    "profile_snapshots": len(data), "present_trace_points": traces}
        capture_rows=[]
        for line in log.splitlines():
            m=re.search(r"present_(\d+)\.bmp .*draw_calls=(\d+)",line)
            if m: capture_rows.append((int(m.group(1)),int(m.group(2))))
        eligible=[row for row in capture_rows if row[0]>=start_index]
        if len(eligible)>=2:
            x,y=eligible[-2:]; run_data["draws_per_frame"]=(y[1]-x[1])/max(1,y[0]-x[0])
        gpu = []; gpu_samples=[]
        if (run/"gpu.csv").exists():
            for line in (run/"gpu.csv").read_text(errors="replace").splitlines():
                m=re.match(r"\s*(\d+)\s*,\s*(\d+)", line)
                if m:
                    sample={"utilization_percent":int(m.group(1)),"graphics_clock_mhz":int(m.group(2))}
                    gpu.append(sample["utilization_percent"]); gpu_samples.append(sample)
        if gpu: run_data["gpu_utilization_percent"] = statistics.mean(gpu)
        run_data["gpu_samples"] = gpu_samples
        print(json.dumps(run_data))
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--summary":
        d = read(sys.argv[2])
        lines=["# AC1 gameplay benchmark", "", f"- Identity: `{d['identity']['sha']}` / `{d['identity']['config_id']}`; branch `{d['identity']['branch']}`; dirty={d['identity']['dirty']}", f"- Binary SHA-256: `{d['identity']['binary_sha256']}`", f"- Machine: {d['machine']['cpu_model']}; {d['machine']['logical_cores']} logical cores; {d['machine']['physical_memory_bytes']} bytes RAM", f"- Build generator: {d.get('environment',{}).get('actual_build_generator','unknown')}", ""]
        for r in d['runs']:
            tops=", ".join(f"{t['tid']}:{t['cpu_seconds']:.2f}s" for t in r.get("top_threads",[])[:5]) or "unavailable"
            lines.append(f"- Run {r['run']}: reached_gameplay={r.get('reached_gameplay',False)}, FPS={r.get('fps')}, mean/p50/p95/p99/max ms={r.get('frametime_mean_ms')}/{r.get('frametime_p50_ms')}/{r.get('frametime_p95_ms')}/{r.get('frametime_p99_ms')}/{r.get('frametime_max_ms')}, draws/frame={r.get('draws_per_frame')}, CPU={r.get('process_cpu_seconds')}s ({r.get('cpu_utilization_cores')} cores), GPU={r.get('gpu_utilization_percent')}%, top threads={tops}, scene captures={len(r.get('scene_proof',[]))}.")
        (Path(sys.argv[2]) / "summary.md").write_text("\n".join(lines)+"\n", encoding="utf-8")
        print("\n".join(lines))
        return
    if len(sys.argv) != 3:
        raise SystemExit("usage: python -B tools/bench_report.py RESULT_DIR_A RESULT_DIR_B")
    a, b = map(read, sys.argv[1:])
    print(f"A: {a['identity']['sha']} ({a['identity']['branch']}) {a['identity']['config_id']} dirty={a['identity']['dirty']}")
    print(f"B: {b['identity']['sha']} ({b['identity']['branch']}) {b['identity']['config_id']} dirty={b['identity']['dirty']}")
    for key, label, lower in (("fps", "FPS", False), ("frametime_mean_ms", "mean ms", True),
                              ("frametime_p95_ms", "p95 ms", True), ("frametime_p99_ms", "p99 ms", True),
                              ("draws_per_frame", "draws/frame", False), ("cpu_utilization_cores", "CPU cores", True),
                              ("gpu_utilization_percent", "GPU %", True)):
        x, y = vals(a, key), vals(b, key)
        if not x or not y:
            print(f"{label}: unavailable (A n={len(x)}, B n={len(y)})")
            continue
        ma, mb = statistics.mean(x), statistics.mean(y)
        sa = statistics.stdev(x) if len(x) > 1 else 0.0
        sb = statistics.stdev(y) if len(y) > 1 else 0.0
        overlap = None if len(x) < 2 or len(y) < 2 else not (ma + sa < mb - sb or mb + sb < ma - sa)
        delta = mb - ma
        pct = (delta / ma * 100) if ma else 0.0
        spread_a = f"±{sa:.3f}" if len(x)>1 else "spread unavailable"
        spread_b = f"±{sb:.3f}" if len(y)>1 else "spread unavailable"
        overlap_text = str(overlap).lower() if overlap is not None else "unknown (need >=2 runs per side)"
        print(f"{label}: A {ma:.3f} {spread_a} (n={len(x)}), B {mb:.3f} {spread_b} (n={len(y)}), delta {delta:+.3f} ({pct:+.2f}%), spread_overlap={overlap_text}")


if __name__ == "__main__":
    main()
