"""Summarize interleaved bench_native pairs; retain individual runs and spreads."""
import argparse
import json
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--label", default="native")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    groups, hashes, rows = {}, set(), []
    for path in args.root.glob("*/result.json"):
        data = json.loads(path.read_text(encoding="utf-8-sig"))
        label = data["identity"].get("label", "")
        if not label.startswith(args.label+"-"): continue
        variant, round_, side = label[len(args.label)+1:].rsplit("-", 2)
        hashes.add(data["identity"]["binary_sha256"])
        for run in data["runs"]:
            if not run.get("reached_gameplay") or run.get("measurement_elapsed_seconds", 0) < 59.5:
                raise SystemExit(f"incomplete gameplay window: {path}")
            groups.setdefault(variant, {}).setdefault(side, []).append(run)
            rows.append({"variant": variant, "round": int(round_), "side": side,
                         "result": str(path), **run})
    if len(hashes) != 1: raise SystemExit(f"expected one immutable binary, found {len(hashes)}")
    lines = [f"Binary SHA-256: `{next(iter(hashes))}`", "",
             "Medians [min, max]; each side has at least three 60-second gameplay windows.", "",
             "| Overrides | Side | n | FPS | p95 ms | p99 ms |", "|---|---|---:|---:|---:|---:|"]
    summary = {}
    for variant, sides in sorted(groups.items()):
        summary[variant] = {}
        for side in ("off", "on"):
            runs = sides.get(side, [])
            if len(runs) < 3: raise SystemExit(f"need >=3 runs per side: {variant} {side} has {len(runs)}")
            stats = {}
            for key in ("fps", "frametime_p95_ms", "frametime_p99_ms"):
                values = [r[key] for r in runs]
                stats[key] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
            summary[variant][side] = stats
            def fmt(key):
                s = stats[key]
                return f"{s['median']:.2f} [{s['min']:.2f}, {s['max']:.2f}]"
            lines.append(f"| {variant} | {side} | {len(runs)} | {fmt('fps')} | {fmt('frametime_p95_ms')} | {fmt('frametime_p99_ms')} |")
    report = "\n".join(lines)+"\n"
    if args.output:
        args.output.write_text(report, encoding="utf-8")
        args.output.with_suffix(".json").write_text(json.dumps({"sha256": next(iter(hashes)), "summary": summary, "runs": rows}, indent=2)+"\n", encoding="utf-8")
    print(report, end="")


if __name__ == "__main__":
    main()
