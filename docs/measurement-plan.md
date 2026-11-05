# Isolating what to optimize

Measuring live gameplay mixes too many variables to separate a code change from
everything else:

| Variable | Why it breaks a comparison |
|---|---|
| Scene | Busy spots are hard to reach twice; the city replay ended in a cutscene |
| Clocks | All big cores were capped at 2.38 GHz at 9-11% battery; the cap moves |
| GPU | A faster CPU changes nothing when the frame is waiting for the GPU |
| Host work | 49% of the main thread in a crowd is not game code |
| Timing | Threads, clock reads and streaming change the work done per frame |
| Link | USB drops and installs interrupt runs and can stop on Play Protect |

The fix is to stop asking one measurement to answer every question. Three
separate instruments each answer one question, and only the last step uses
live play.

## 1. Is this scene limited by the CPU? (causal test, live)

Busy percentages cannot answer this, so test it directly. An opt-in runtime
switch adds a fixed amount of spinning to the main thread, proportional to its
work, in alternating windows of a few seconds during normal play:

- If FPS falls in step with the added work, the CPU limits this scene and CPU
  savings will raise FPS.
- If FPS barely moves, something else is limiting the frame.

The on and off windows alternate inside the same scene, so scene changes and
clock drift affect both alike. It needs one debug install and no reachable
scene, and it says how much a CPU saving is worth before any work is done.

## 2. How much CPU work does this code take? (capture and replay, isolated)

This is the core of the plan. It replaces gameplay A/B for code generation.

Translated code touches the outside world in very few places, all of them
already in the runtime:

- all guest memory is one arena (`ac1_direct_arena`);
- imports go through `dispatch` and `run_import_handlers`;
- D3D9 COM calls are tokens handled by `dispatch_d3d9_method`;
- the baton scheduler runs one guest thread at a time;
- guest time can be fixed per Present (`AC1_FIXED_FRAME_HZ`).

So a slice of real gameplay can be recorded at that boundary and replayed
without the game, the GPU or the Android app.

**Capture.** You play to a crowd and press a trigger. At the next Present the
runtime dumps the arena and every guest thread's CPU state, then logs N frames
(about 120): each import and COM call with its arguments, its results, and the
guest memory it wrote. Host writes are found by making the arena read-only while
host code or another guest thread runs. Capture may be slow, because guest time
is fixed per Present and the guest cannot tell. Other guest threads are treated
as outside the boundary in the first version: their memory writes are logged
and applied at the same points.

**Replay.** A plain command-line ARM64 program linked with the translated code.
It loads the dump and runs the main guest thread. Each import returns its
logged result and applies its logged memory writes, after checking that the
call and its arguments match the log. There is no APK, install, GPU, DXVK,
audio or save file, and the game on your phone is untouched. It runs from
`/data/local/tmp` for 1-2 seconds per variant, and also on the PC.

What this fixes:

- **Scene:** a crowd is captured once and reused indefinitely. Captures form a
  corpus (crowd, combat, rooftops), replacing the replay of one scene.
- **GPU and timing:** absent by construction.
- **Clocks:** executed instructions are exact whatever the clock. Cycles and
  stalls come from short runs, alternating A B A B on one core, with A/A
  calibration. A run is rejected if `scaling_max_freq` or battery state changed.
- **Correctness:** every variant must make the same import calls with the same
  arguments and end with the same memory hash. Each comparison is a differential
  test on real gameplay, with the first divergence located exactly.
- **Weights:** the log gives exact per-frame counts of imports, and an
  instrumented replay gives exact call counts for every function, which sampled
  profiles cannot.

Limits, stated up front:

- Replay measures game code only: 51% of the main thread in the crowd sample.
- Other guest threads run outside it. They used about 5% as many cycles as the
  main thread in that sample.
- Host costs need instrument 3.

## 3. What does host code cost? (import log)

The capture log gives each import's exact count per frame, so host work becomes
a list of counts multiplied by unit costs. That suits the 41% of the crowd frame
in the runtime, DXVK and libc. It is also how to find out what calls `strcmp`
(4.3% in quiet gameplay) and `advance`/`consume`, and whether they are avoidable.

## 4. Confirmation (live, rare)

A candidate reaches gameplay only after replay shows a real gain and the causal
test says the CPU matters in that scene. The check uses a charged phone, the
same spot and short on/off comparisons. It confirms a result; it does not decide
one. Battery level and `scaling_max_freq` are recorded with every phone
measurement.

## Options considered

- **Deterministic full-game timedemo with a null renderer:** it keeps host code
  in the run, but every run needs a save near the scene and a long load, and any
  asynchronous streaming difference breaks determinism.
- **Replaying single hot functions:** too narrow. No function exceeds 2.2% of
  the crowd frame, and call chains are where the cost interacts.
- **More careful gameplay A/B:** it keeps every variable in the table above.

## Feasibility risks

- **Host writes the read-only trick misses:** they would show up as a replay
  divergence, not as a silent error.
- **Dump size:** the first version dumps the whole committed arena. If that is
  too large, it records only pages touched in the slice.
- **State outside the arena:** CPU structs and `fs_base` are dumped. Runtime
  caches in native bindings must rebuild themselves or be included.
- **Proprietary data:** captures contain game memory, so they stay under `work/`
  and on the phone, never published.

## Order of work

1. Add the run header (battery, clock caps, temperature) to phone measurements
   and the causal switch. This is small and useful at once.
2. Build capture and replay, first on the PC. Done when two replays execute the
   same instruction count and match the memory hash.
3. Run replay on the phone and calibrate A/A noise.
4. You capture three scenes, each needing one trigger press in normal play.
5. Re-judge the existing opt-ins on that corpus: private ABI, x87 regions, XMM
   contracts, backward-only fences.
