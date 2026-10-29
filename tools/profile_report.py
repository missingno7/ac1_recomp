#!/usr/bin/env python3
"""Report private AC1 sampler captures (stdlib only).

Example: python -B tools/profile_report.py work/profile --from-seconds 30
The default selects the latest run, keeping earlier runs available for comparison.
Percentages use CPU-active observations, not cycle counts (CPU frequency varies).
Guest helper attribution uses the nearest emitted guest entry to CPU.eip; generated
host symbols identify their actual entry. This is not a stack/inlining profiler.
"""
from __future__ import annotations
import argparse
import bisect
from collections import Counter, defaultdict
import json
from functools import lru_cache
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SAMPLE = struct.Struct("<QQQIIIIII")
HANDOFF_CAUSES = ("return-slice", "import", "sleep", "yield/Sleep(0)", "wait/block",
                  "critical-section", "event/semaphore/mutex", "I/O", "thread-start", "thread-exit")
GUEST = re.compile(r"(?:^|\b)(?:L|ac1|bink)_([0-9A-Fa-f]{8})(?:\b|\.)")


@lru_cache(maxsize=16)
def pe_exports(path: str):
    """Export starts, excluding forwarded exports. Nearest export is approximate."""
    try:
        data = Path(path).read_bytes()
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        sections, optional_size = struct.unpack_from("<H", data, pe+6)[0], struct.unpack_from("<H", data, pe+20)[0]
        optional = pe+24
        magic = struct.unpack_from("<H", data, optional)[0]
        export, export_size = struct.unpack_from("<II", data, optional+(112 if magic == 0x20b else 96))
        def offset(rva):
            for n in range(sections):
                size, va, raw_size, raw = struct.unpack_from("<IIII", data, optional+optional_size+40*n+8)
                if va <= rva < va+max(size, raw_size): return raw+rva-va
            raise ValueError("unmapped RVA")
        fields = struct.unpack_from("<IIHHIIIIIII", data, offset(export))
        count, functions, names, ordinals = fields[7], fields[8], fields[9], fields[10]
        found = []
        for i in range(count):
            name_rva = struct.unpack_from("<I", data, offset(names)+4*i)[0]
            begin = offset(name_rva)
            name = data[begin:data.index(b"\0", begin)].decode("ascii", errors="replace")
            ordinal = struct.unpack_from("<H", data, offset(ordinals)+2*i)[0]
            rva = struct.unpack_from("<I", data, offset(functions)+4*ordinal)[0]
            if rva and not export <= rva < export+export_size: found.append((rva, name))
        return tuple(sorted(found))
    except (OSError, ValueError, struct.error, IndexError):
        return ()


@lru_cache(maxsize=16)
def wait_exports(path: str):
    return tuple(sorted({rva for rva, name in pe_exports(path)
                         if name.startswith(("Nt", "Zw")) and
                         re.search(r"Wait|DelayExecution|GetMessage|RemoveIoCompletion|ListenPort", name)}))


def nearest_export(module, rip):
    exports = pe_exports(module["path"])
    rva = rip-module["base"]
    index = bisect.bisect_right(exports, (rva, "\uffff"))-1
    if index < 0: return "[no export]"
    start, name = exports[index]
    return f"{name}+0x{rva-start:x}"


def syscall_parked(module, rip):
    if not module: return False
    if Path(module["path"].replace("\\", "/")).name.lower() not in ("ntdll.dll", "win32u.dll"): return False
    stubs = wait_exports(module["path"])
    rva = rip-module["base"]
    index = bisect.bisect_right(stubs, rva)-1
    return index >= 0 and rva-stubs[index] < 32


def category(name: str) -> str:
    if GUEST.search(name): return "generated guest code"
    if re.search(r"guest_(?:ptr|execution|memory)|scalar_|\b(?:rd|wr)(?:8|16|32|64|f)", name): return "guest memory access"
    if re.search(r"dispatch|lifted_lookup|lookup_cache", name): return "dispatch/lookup"
    if re.search(r"x87|mmx|sse|ac1_fp|ac1_mm_|ac1_rcpps|ac1_rsqrt|fpush|fpop|\bfst\b|ac1_simd", name): return "x87/MMX/SSE helpers"
    if re.search(r"scheduler|sync_|critical_section|guest_thread|profile_wait", name): return "scheduler/sync"
    if re.search(r"d3d9|D3D|present_device|trace_frame_success|draw_|shader_|texture_|surface_", name, re.I): return "D3D9 bridge"
    if re.search(r"ds8|bink|ac1_files|file_|audio_|sound|waveOut", name, re.I): return "DS8/Bink/files"
    return "other runtime"


def load_symbols(exe: Path, nm: str):
    with exe.open("rb") as stream:
        data = stream.read(4096)
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe+4] != b"PE\0\0": raise ValueError("host executable is not PE")
    magic = struct.unpack_from("<H", data, pe+24)[0]
    base = struct.unpack_from("<Q" if magic == 0x20b else "<I", data, pe+48 if magic == 0x20b else pe+52)[0]
    output = subprocess.check_output([nm, "-C", "--defined-only", str(exe)], text=True, errors="replace")
    symbols = []
    for line in output.splitlines():
        match = re.match(r"^([0-9a-fA-F]+)\s+([tTwW])\s+(.+)$", line)
        if match:
            address, _, name = match.groups()
            symbols.append((int(address, 16)-base, name))
    symbols.sort()
    if not symbols: raise ValueError("nm found no text symbols")
    return [x[0] for x in symbols], [x[1] for x in symbols]


def percentile(values, q):
    if not values: return 0.0
    pos = (len(values)-1)*q
    lo = int(pos)
    return values[lo] + (values[min(lo+1, len(values)-1)]-values[lo])*(pos-lo)


def thread_cpu(meta):
    totals = Counter()
    for thread in meta.get("threads", []): totals[thread["tid"]] += thread["cpu_100ns"]
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", type=Path, default=ROOT/"work/profile")
    parser.add_argument("--exe", type=Path, default=ROOT/"build/host-relwithdebinfo/ac1_startup.exe")
    parser.add_argument("--nm", default=r"C:\msys64\mingw64\bin\nm.exe")
    parser.add_argument("--from-seconds", type=float, default=0)
    parser.add_argument("--to-seconds", type=float, default=float("inf"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--pid", type=int, help="select a specific run's process ID instead of the latest run")
    parser.add_argument("--run-start", type=int, help="select the run_start QPC value in raw metadata (disambiguates reused PIDs)")
    parser.add_argument("--top", type=int, default=15)
    args = parser.parse_args()
    single = None if args.capture.is_dir() else args.capture.with_suffix(".json")
    paths = list((args.capture if single is None else single.parent).glob("raw_*.json"))
    if not paths: parser.error("no raw_*.json captures found")
    captures = [(p, json.loads(p.read_text())) for p in paths]
    if args.run_start is not None:
        captures = [(p, d) for p, d in captures if d["run_start"] == args.run_start]
        if not captures: parser.error(f"no captures for run_start {args.run_start}")
    if args.pid is not None:
        captures = [(p, d) for p, d in captures if d["pid"] == args.pid]
        if not captures: parser.error(f"no captures for PID {args.pid}")
    latest = json.loads(single.read_text()) if single else max(captures, key=lambda x: x[1]["run_start"])[1]
    captures = sorted(((p, d) for p, d in captures if (d["pid"], d["run_start"]) == (latest["pid"], latest["run_start"])), key=lambda x: x[1]["end"])
    hz, origin = latest["frequency"], latest["run_start"]
    low = max(captures[0][1]["start"], origin+args.from_seconds*hz, latest["start"] if single else origin)
    high = min(captures[-1][1]["end"], origin+args.to_seconds*hz, latest["end"] if single else captures[-1][1]["end"])
    if high <= low: parser.error("empty requested time window")
    elapsed = (high-low)/hz
    rvas, names = load_symbols(args.exe, args.nm)
    guest_entries = sorted({int(m.group(1), 16) for n in names if (m := GUEST.search(n))})
    modules, symbols, categories, guest_functions = Counter(), Counter(), Counter(), Counter()
    external_exports = Counter()
    by_thread = defaultdict(Counter)
    module_threads, symbol_threads, category_threads, guest_threads = (defaultdict(Counter) for _ in range(4))
    all_frames, ticks = [], set()
    guest_unknown = 0
    entry_detail, entry_symbols = defaultdict(Counter), defaultdict(Counter)
    before, after = {}, {}
    cpu_before, cpu_after = {}, {}
    previous_guest, previous_cpu = {}, {}
    sampler_before = sampler_after = previous_sampler = 0
    bounds = []
    for path, meta in captures:
        if meta["end"] <= low:
            before = {g["tid"]: g for g in meta["guests"]}
            cpu_before = thread_cpu(meta)
        # Counters are only snapshotted at interval boundaries. Use complete
        # intervals inside the requested window, never prorate counter totals.
        if meta["start"] >= low and meta["end"] <= high:
            if not bounds:
                before = previous_guest.copy()
                cpu_before = previous_cpu.copy()
                sampler_before = previous_sampler
            bounds.append((meta["start"], meta["end"]))
            after = {g["tid"]: g for g in meta["guests"]}
            cpu_after = thread_cpu(meta)
            sampler_after = meta.get("sampler_cpu_100ns", 0)
        previous_guest = {g["tid"]: g for g in meta["guests"]}
        previous_cpu = thread_cpu(meta)
        previous_sampler = meta.get("sampler_cpu_100ns", 0)
        all_frames.extend(f for f in meta["frames"] if low <= f <= high)
        if meta["end"] < low or meta["start"] > high: continue
        raw = path.with_suffix(".bin").read_bytes()
        if len(raw) != meta["samples"]*SAMPLE.size: raise ValueError(f"truncated/invalid capture: {path}")
        for tick, rip, cycles, tid, guest, eip, flags, module, reserved in SAMPLE.iter_unpack(raw):
            if not low <= tick <= high: continue
            ticks.add(tick)
            t = by_thread[tid]; t["total"] += 1; t["guest"] = guest
            mod = meta["modules"][module-1] if module else None
            if flags & 2: t["baton"] += 1
            if flags & 4: t["baseline"] += 1
            parked = flags & 8 or syscall_parked(mod, rip)
            if parked: t["parked"] += 1
            if not flags & 1 or parked: continue
            t["running"] += 1; t["cycles"] += cycles
            modname = Path(mod["path"].replace("\\", "/")).name if mod else "[unknown module]"
            modules[modname] += 1
            module_threads[tid][modname] += 1
            name = "[external]"
            if mod and modname.lower() == args.exe.name.lower():
                index = bisect.bisect_right(rvas, rip-mod["base"])-1
                name = names[index] if index >= 0 else "[unknown exe address]"
                symbols[name] += 1; categories[category(name)] += 1
                symbol_threads[tid][name] += 1; category_threads[tid][category(name)] += 1
            else:
                categories["external: "+modname] += 1
                category_threads[tid]["external: "+modname] += 1
                if mod: external_exports[modname+"!"+nearest_export(mod, rip)] += 1
            if guest:
                match = GUEST.search(name)
                if match:
                    address = int(match.group(1), 16)
                else:
                    # EIP is the last traced guest instruction. Imports/helpers
                    # retain their caller's EIP. No synthetic call stack implied.
                    index = bisect.bisect_right(guest_entries, eip)-1
                    address = guest_entries[index] if index >= 0 and eip-guest_entries[index] < 0x10000 else 0
                if address:
                    guest_functions[address] += 1
                    guest_threads[tid][address] += 1
                    entry_detail[address][category(name) if name != "[external]" else "external: "+modname] += 1
                    entry_symbols[address][name if name != "[external]" else "external: "+modname] += 1
                else: guest_unknown += 1
    total = sum(modules.values())
    lines = [f"AC1 profile: PID {latest['pid']}, {latest['mode']}, window {(low-origin)/hz:.3f}..{(high-origin)/hz:.3f}s ({elapsed:.3f}s)",
             f"CPU-active samples {total:,}; thread observations {sum(t['total'] for t in by_thread.values()):,}; cumulative errors {captures[-1][1]['errors']}; dropped samples/frames {captures[-1][1]['dropped']}/{captures[-1][1]['frame_dropped']}"]
    def add_top(title, counter, denominator, count=args.top, render=str):
        lines.append(title)
        for key, n in counter.most_common(count): lines.append(f"  {100*n/max(denominator,1):6.2f}% {n:9,d}  {render(key)}")
    def cpu_estimates(mixes):
        estimates = Counter()
        for tid, mix in mixes.items():
            running = by_thread[tid]["running"]
            cpu_ms = max(0, cpu_after.get(tid, 0)-cpu_before.get(tid, 0))/10000
            if running:
                for name, n in mix.items(): estimates[name] += cpu_ms*n/running
        return estimates
    def add_cpu_top(title, mixes, denominator=None, count=args.top, render=str):
        estimates = cpu_estimates(mixes)
        denominator = sum(estimates.values()) if denominator is None else denominator
        lines.append(title)
        for name, ms in estimates.most_common(count): lines.append(f"  {100*ms/max(denominator,1):6.2f}% {ms:10.2f}ms estimated  {render(name)}")
    lines.append("Threads: CPU-active/observed, CPU-active share, OS CPU ms, parked share, baton-held share, observations/s, guest ID")
    lines.append(f"  samplers (coordinator tid={latest['sampler_tid']}): {(sampler_after-sampler_before)/10000:.2f}ms combined OS CPU over complete counter intervals (not suspended)")
    for tid, t in sorted(by_thread.items(), key=lambda item: -item[1]["running"]):
        cpu = (cpu_after.get(tid, 0)-cpu_before.get(tid, 0))/10000
        lines.append(f"  {tid:6d}: {t['running']:7,d}/{t['total']:7,d} {100*t['running']/max(total,1):6.2f}% {cpu:10.2f}ms parked={100*t['parked']/t['total']:6.2f}% baton={100*t['baton']/t['total']:6.2f}% {t['total']/elapsed:.1f}/s guest={t['guest']:#x}")
    add_top("Host modules (% of all CPU-active samples)", modules, total)
    add_top("Host categories (% of all CPU-active samples)", categories, total, count=30)
    add_top("External RIPs by nearest local PE export (approximate, no call stack; % of all active samples)", external_exports, total, count=25)
    if bounds and total:
        all_cpu = sum(max(0, cpu_after.get(tid, 0)-cpu_before.get(tid, 0)) for tid in cpu_after)/10000
        add_cpu_top("Host modules (OS CPU-time weighted sample mix; compensates for different thread sampling rates)", module_threads, all_cpu)
        add_cpu_top("Host categories (OS CPU-time weighted sample mix)", category_threads, all_cpu, count=30)
        missing_cpu = all_cpu-sum(cpu_estimates(module_threads).values())
        lines.append(f"  CPU without an active observation: {missing_cpu:.2f}ms ({100*missing_cpu/max(all_cpu,1):.2f}%)")
    add_top("Exe symbols (% of exe CPU-active samples)", symbols, sum(symbols.values()))
    add_top("Guest entries including helper/import samples (% of attributed guest-thread CPU-active samples)", guest_functions, sum(guest_functions.values()), render=lambda a: f"0x{a:08X} L_{a:08X}")
    if bounds and total:
        add_cpu_top("Guest entries including helpers (OS CPU-time weighted sample mix)", guest_threads, render=lambda a: f"0x{a:08X} L_{a:08X}")
    lines.append(f"  Guest samples with unknown EIP attribution: {guest_unknown}")
    # Where does each hot guest entry's time actually go (generated code vs
    # memory/dispatch helpers vs imports/bridges/baton waits)?
    lines.append("Host breakdown under the top guest entries (% of that entry's samples)")
    for address, n in guest_functions.most_common(min(args.top, 10)):
        detail = ", ".join(f"{name} {100*k/n:.0f}%" for name, k in entry_detail[address].most_common(5))
        top_symbols = ", ".join(f"{name} {100*k/n:.0f}%" for name, k in entry_symbols[address].most_common(4))
        lines.append(f"  0x{address:08X} ({n:,}): {detail}")
        lines.append(f"      symbols: {top_symbols}")
    lines.append("Top guest entries per busy guest thread (% of that thread's attributed samples)")
    for tid, mix in sorted(guest_threads.items(), key=lambda item: -sum(item[1].values()))[:3]:
        n = sum(mix.values())
        top = ", ".join(f"0x{a:08X} {100*k/n:.1f}%" for a, k in mix.most_common(8))
        lines.append(f"  tid={tid} guest={by_thread[tid]['guest']:#x} ({n:,}): {top}")
    all_frames.sort()
    intervals = sorted((b-a)*1000/hz for a, b in zip(all_frames, all_frames[1:]))
    fps = (len(all_frames)-1)*hz/(all_frames[-1]-all_frames[0]) if len(all_frames)>1 else 0
    lines.append(f"Frames: {len(all_frames):,} Presents, {fps:.3f} FPS over first..last Present; {len(all_frames)/elapsed:.3f} Presents/s in window; p50/p95/p99 {percentile(intervals,.5):.3f}/{percentile(intervals,.95):.3f}/{percentile(intervals,.99):.3f} ms")
    if bounds and latest["mode"] == "sampling":
        duration = (bounds[-1][1]-bounds[0][0])/hz
        totals = Counter()
        causes = Counter()
        lines.append(f"Counters: complete intervals {(bounds[0][0]-origin)/hz:.3f}..{(bounds[-1][1]-origin)/hz:.3f}s ({duration:.3f}s)")
        for tid, g in after.items():
            delta = {k: g[k]-before.get(tid, {}).get(k, 0) for k in ("dispatches", "misses", "slow", "scanned", "handoffs", "wait_ticks")}
            totals.update(delta)
            for key in ("transfer_count", "transfer_ns", "io_wait_completions"):
                totals[key] += g.get(key, 0)-before.get(tid, {}).get(key, 0)
            old_causes = before.get(tid, {}).get("handoff_causes", [0]*len(HANDOFF_CAUSES))
            for label, value, old in zip(HANDOFF_CAUSES, g.get("handoff_causes", []), old_causes):
                causes[label] += value-old
            lines.append(f"  guest {g['guest']:#x} tid={tid}: baton wait {delta['wait_ticks']/hz:.3f}s ({100*delta['wait_ticks']/hz/duration:.2f}% of wall interval), handoffs={delta['handoffs']:,}")
        lines.append(f"  dispatches={totals['dispatches']:,} ({totals['dispatches']/duration:,.1f}/s), lookup misses={totals['misses']:,} ({100*totals['misses']/max(totals['dispatches'],1):.2f}% of dispatches)")
        lines.append(f"  guest_ptr/try_ptr slow scans={totals['slow']:,} ({totals['slow']/duration:,.1f}/s), regions scanned={totals['scanned']:,}, average scan={totals['scanned']/max(totals['slow'],1):.2f}")
        lines.append(f"  handoffs={totals['handoffs']:,} ({totals['handoffs']/duration:,.1f}/s); causes: "+", ".join(f"{k}={v:,}" for k,v in causes.items()))
        if totals['transfer_count']:
            lines.append(f"  baton transfer (publish owner -> recipient holding lock; excludes peer guest execution): mean {totals['transfer_ns']/totals['transfer_count']/1000:.3f} us over {totals['transfer_count']:,} transfers")
        else:
            lines.append("  baton transfer latency: unavailable in this capture")
        lines.append(f"  waits granted on I/O-backed completion events: {totals['io_wait_completions']:,} (not a count of ReadFile submissions)")
        all_cpu = sum(max(0, cpu_after.get(tid, 0)-cpu_before.get(tid, 0)) for tid in cpu_after)/1e7
        lines.append(f"  process CPU excluding sampler: {all_cpu:.3f}s, {100*all_cpu/duration:.2f}% of one core")
    elif latest["mode"] == "frames": lines.append("Counters: disabled in frame-only baseline.")
    else: lines.append("Counters: no complete intervals in selected window; widen window to interval boundaries.")
    lines.append("Notes: sampler threads exclude themselves. CPU-active means nonzero QueryThreadCycleTime delta and RIP outside known syscall wait stubs (capture flags plus local ntdll/win32u PE exports); raw deltas are retained. Suspension can itself charge sleeping threads kernel cycles. This is not exact scheduled CPU residency. OS CPU times cover complete counter intervals. Inlined helpers remain charged to generated code. Helper guest-entry attribution via EIP is approximate where discovery entries overlap. Baton wait counts condition sleeps entered without ownership, including guest API blocking after relinquishing ownership. Enumeration is every 250ms; short-lived threads can be missed. Guest/native samplers are separate so a slow graphics GetThreadContext cannot stall guest observations.")
    report = "\n".join(lines)+"\n"
    if args.output: args.output.write_text(report, encoding="utf-8")
    print(report, end="")


if __name__ == "__main__":
    main()
