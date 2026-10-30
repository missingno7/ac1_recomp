# Benchmarking CPU work per frame

`tools/bench_gameplay.ps1` measures the fixed successful-Present range 1800–2200
(override with `-StartPresent`/`-EndPresent`). Gameplay follows
`tools/input/to_gameplay.txt`; Present 1800–2200 is the steady WALK segment.
`-Seconds` is only a safety timeout. The harness pins the game process to CCD0
(logical CPUs 0–15, mask `0x0000FFFF`), disables DXVK vsync, and stops its own
game process after EndPresent. Stats flush every eight rows and flush immediately
at EndPresent, preserving the final row without per-frame file writes.

The CSV records wall time, main-thread and process CPU milliseconds, Windows
`QueryThreadCycleTime` deltas, current processor, guest-visible clock, and replay
command index. On Windows, CPU milliseconds are 16-Present rolling averages to
smooth `GetThreadTimes` quantization; cycle percentiles are per-frame. Cycle
counts are an additional per-thread CPU-cycle measure, not retired instructions
and not architecture-independent: frequency, core placement, and hardware
behavior affect them. POSIX/Android leave cycle and processor columns empty.
Hardware retired-instruction/cache-miss counters belong in focused investigations
when available; normal runs avoid that instrumentation.

Workload equivalence is judged from gameplay phase, identical replay command
indices at both boundaries, and guest-visible time elapsed. Present equality
alone does not prove equal simulation work. The harness compares hashes of the
captures at Present 1800 and 2200 as observable-result evidence; these support
the replay and guest-time checks rather than replacing them. Draw calls,
shader uploads, locks, helper/dispatch counts, cycles, and other implementation
costs are optimization outcomes: the bridge records draw calls, shader-constant
uploads, and resource locks per Present and reports their per-run totals and A/B
deltas. Other counters are reported when available; normal benchmarks do not
add retired-instruction or cache-miss instrumentation. Never exclude a pair
because an implementation-cost counter differs.

Comparisons (`-Compare`) take `D:\ac1_bench.lock` exclusively. If another worker
owns it, wait or pass `-Exploratory` to label a non-controlled run. Pairs
alternate AB/BA; the adaptive interval uses paired main-thread CPU ms/frame and
reports the 95% bootstrap CI, spread, valid pair count, and stop reason. Per-frame
cycles/frame distributions are reported alongside CPU time. A/A calibration
uses the same binary on both sides:

```powershell
tools/bench_gameplay.ps1 -Compare -Binary build/host-relwithdebinfo/ac1_startup.exe -BinaryB build/host-relwithdebinfo/ac1_startup.exe -Runs 1
```

To collect runtime CSV without a benchmark, set `AC1_FRAME_STATS` to a writable
path. The Android launcher accepts an app-private destination:

```powershell
adb shell am start -n org.ac1.recomp/.LauncherActivity --es run true --es env 'AC1_FRAME_STATS=private:framestats.csv'
```

The host FPS value is reported last for context; it is not the optimization
target. `tools/run_gameplay.ps1` retains its 45-second smoke default and explicit
`-Seconds` override. A host run does not verify Android hardware behavior.
