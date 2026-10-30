# Benchmarking CPU work per frame

Use `tools/bench_gameplay.ps1` to collect host gameplay samples. The default
window is 30 seconds after the replay reaches gameplay. The replay and workload
boundary are explicit: `tools/input/to_gameplay.txt` drives the title/profile
menus into gameplay, with the scene boundary at Present 1300 by default. The
first complete Windows CPU sample (Present 1316, after the 16-Present smoothing
window) starts the measured window. Override the boundary with `-StartPresent`.
The optimization metric is guest main-thread CPU milliseconds per frame, not FPS. At each
successful D3D9 Present the runtime samples wall time, the Present thread's CPU
time, and total process CPU time. It buffers the CSV and flushes every 64 frames
by default; the benchmark sets `AC1_FRAME_STATS_FLUSH_FRAMES=8` so it can stop
the game immediately after its measurement window without losing most recent
samples.
Set `AC1_FRAME_STATS` to a writable CSV path to enable collection. With the
Android launcher, pass an app-private filename as a diagnostic environment
setting:

```powershell
adb shell am start -n org.ac1.recomp/.LauncherActivity --es run true --es env 'AC1_FRAME_STATS=private:framestats.csv'
```

The file is written to the app's private files directory. No device-side output
is copied into the repository automatically.

The host benchmark turns vsync off in DXVK with a per-run
`d3d9.presentInterval = 0` config and records that setting in `result.json`.
Each run and A/B comparison reports main-thread and process CPU ms/frame
median/p95/p99, wall frame-time percentiles, then FPS. The main-thread reading
is sampled on the Present thread using `GetThreadTimes` on Windows or
`CLOCK_THREAD_CPUTIME_ID` on POSIX/Android; process CPU uses the matching
process clock. Windows CPU values are 16-Present rolling averages to smooth
the OS thread-time accounting granularity; POSIX/Android values use one frame.
CSV rows are aligned to the measured workload Present window. The benchmark
uses these rows as its wall-time source and stops the game process at the window
deadline; it does not wait for the periodic profiler to flush.

To sample the idle main menu, run `tools/bench_gameplay.ps1 -Workload menu`; it
leaves replay input unset and uses the same Present-aligned measurement window.
The default `gameplay` workload follows `tools/input/to_gameplay.txt`.

Comparisons default to 3 minimum and 8 maximum pairs. Pair order alternates
AB/BA. After each pair, a bootstrap 95% confidence interval is calculated for
the paired median difference in main-thread CPU ms/frame (B minus A). The run
stops when that interval excludes zero, or when its half-width is below 1% of
the A baseline median; otherwise it continues to the maximum. The report records
the verdict, stop reason, executed and valid pair counts, delta spread, CI, and
practical threshold. Override the limits with `-MinPairs` and `-MaxPairs`, or
the equivalence threshold with `-PracticalPercent`.

For example, `-Compare -Binary <A.exe> -BinaryB <B.exe>` uses these adaptive
defaults. A known-different metric check can pass
`-EnvB @{ AC1_FRAME_STATS_TEST_SLOWDOWN_US = '10000' }`; that diagnostic knob
busy-spins on the Present thread and should only be used for metric validation.
`tools/run_gameplay.ps1` defaults to a 45-second smoke run and accepts explicit
`-Seconds` overrides. Host measurements are not Android hardware verification.
