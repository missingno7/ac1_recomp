# Benchmarking CPU work per frame

Use `tools/bench_gameplay.ps1` to collect host gameplay samples. The optimization
metric is guest main-thread CPU milliseconds per frame, not FPS. At each
successful D3D9 Present the runtime samples wall time, the Present thread's CPU
time, and total process CPU time. It buffers the CSV and flushes every 64 frames.
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
CSV rows are aligned to the measured workload Present window.

To sample the idle main menu, run `tools/bench_gameplay.ps1 -Workload menu`; it
leaves replay input unset and uses the same Present-aligned measurement window.
The default `gameplay` workload follows `tools/input/to_gameplay.txt`.

For repeatability, compare runs of the same binary under both quiet and busy
host conditions. Then use an interleaved `-Compare` run with a deliberate
runtime difference, such as `-EnvB @{ AC1_FRAME_STATS_TEST_SLOWDOWN_US = '2000' }`,
to confirm the CPU metric responds. That diagnostic knob busy-spins on the
Present thread and should only be used for metric validation. Host measurements
are not Android hardware verification.
