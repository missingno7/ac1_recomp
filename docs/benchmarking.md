# Benchmarking CPU work per frame

`tools/bench_gameplay.ps1` measures the fixed successful-Present range 1800–2200
(override with `-StartPresent`/`-EndPresent`). Gameplay follows
`tools/input/to_gameplay.txt`; Present 1800–2200 is the steady WALK segment.
`-Seconds` is only a safety timeout. Runs use a deterministic guest clock
(`-FixedFrameHz 30`, environment `AC1_FIXED_FRAME_HZ`): GetTickCount,
timeGetTime, QueryPerformanceCounter and GetSystemTimeAsFileTime start at a
fixed epoch and advance by exactly 1/30 s per Present, so every run simulates
the same time-step sequence. Host waits and scheduling stay on real time; a
thread polling the frozen clock more than 2^20 times in one frame is released
by 1 us per read (reported once). With this clock Presents 1800-2200 cover
walking through the crowd from the GRAB AND THROW prompt to the cobblestone
area; `-FixedFrameHz 0` restores real time, where the window starts in the
Animus white-out and reaches the CAMERA prompt. The harness pins the game process to CCD0
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

The window is not one steady phase. Near Present 2040 the scene changes (a
white flash): main-thread cost falls from ~130M to ~65M cycles/frame. Its
Present index follows real time, not the fixed guest clock, so a faster binary
reaches it later: `e634876` at Present 2038-2050 in five runs, the flag-contract
build at 2148-2200+ (the stack step moved it from ~2010 to ~2075). The 1800-2200
window then holds more heavy frames on the faster side and understates its gain:
-3.7% there against -10.7% in 2400-2800 for the flag contracts. A window
after the change is no cleaner: frame cost keeps falling for hundreds of
Presents after it, so the side that changed scene later is in a heavier
stage, and runs of one binary spread by +-10% there (flag locals: eight pairs
from -10% to +13%, inconclusive). The most stable comparison is the part of
the standard window before the change in both runs, from the per-Present CSVs
(`python -B tools/bench_report.py --segment <comparison> 1800-2000`); report it
next to the standard window. The white frame at the boundary capture is that flash, not
breakage.

Workload equivalence is judged from gameplay phase and identical replay command
indices at both boundaries, plus a gross guest-time sanity bound (33%). Present
equality alone does not prove equal simulation work. The guest clock follows
real frame time, so over a fixed Present window a faster side legitimately
simulates less guest time: A/A runs of one binary differ by up to 12%. The
harness records hashes of the captures at Present 1800 and 2200 as evidence
only; rendering is not pixel-deterministic, and identical binaries never
produced matching hashes. Draw calls,
shader uploads, locks, helper/dispatch counts, cycles, and other implementation
costs are optimization outcomes: the bridge records draw calls, shader-constant
uploads, and resource locks per Present and reports their per-run totals and A/B
deltas. Other counters are reported when available; normal benchmarks do not
add retired-instruction or cache-miss instrumentation. Never exclude a pair
because an implementation-cost counter differs.

Comparisons (`-Compare`) take `D:\ac1_bench.lock` exclusively. If another worker
owns it, wait or pass `-Exploratory` to label a non-controlled run. Pairs
alternate AB/BA; the adaptive interval uses the paired per-run mean of
main-thread cycles/frame (total cycles over the Present window / frames) and
reports the 95% Student-t CI of the mean paired delta, spread, valid pair
count, and stop reason. (A bootstrap CI of the median over three same-sign A/A
deltas always excluded zero.) Per-frame cost is multimodal (short and long
frames alternate), so a per-run median can land in a different mode on each
side: in the vector-SSE comparison it reported +12.5% cycles while the window
mean and mean frame time both fell 5.0% in all eight pairs. CPU ms/frame is still reported, but it is quantized to
one 15.625 ms scheduler tick per 16 Presents (~0.98 ms/frame, ~3%). A/A
calibration uses the same binary on both sides:

```powershell
tools/bench_gameplay.ps1 -Compare -Binary build/host-relwithdebinfo/ac1_startup.exe -BinaryB build/host-relwithdebinfo/ac1_startup.exe -Runs 1
```

The first A/A calibration (main `92d4a98`, eight pairs, CCD0, other processes
at 1-2% CPU) gave paired cycles/frame deltas with median +0.18%, bootstrap 95%
CI [-2.6%, +2.9%] and per-run CV 2.0% around ~128M cycles/frame (~31 ms of a
~31.5 ms frame; the main thread is the critical path). Eight pairs therefore
resolve effects of roughly 3% or larger; smaller effects need an isolated
region benchmark or more pairs.

With the fixed frame clock, four of six A/A runs rendered a bit-identical
Present-1800 frame with identical draw and constant-upload counts; draw-count
CV fell from 5.5% to 0.56% and run cycles/frame CV from 2.0% to 1.0% (~153M
cycles/frame, ~23.5 FPS in the heavier crowd window). Three A/A pairs gave
-1.2%, 95% t CI [-3.5%, +1.1%]: inconclusive, as it should be.

To collect runtime CSV without a benchmark, set `AC1_FRAME_STATS` to a writable
path. The Android launcher accepts an app-private destination:

```powershell
adb shell am start -n org.ac1.recomp/.LauncherActivity --es run true --es env 'AC1_FRAME_STATS=private:framestats.csv'
```

The host FPS value is reported last for context; it is not the optimization
target. `tools/run_gameplay.ps1` retains its 45-second smoke default and explicit
`-Seconds` override. A host run does not verify Android hardware behavior.
