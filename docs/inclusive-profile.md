# Inclusive host profiling

The host sampler has an optional x64 stack capture:

```powershell
tools/run_gameplay.ps1 -SkipBuild -Seconds 150 -OutputDirectory work/engine_prof/gameplay -Env @{
    AC1_PROFILE='1'
    AC1_PROFILE_STACKS='1'
    AC1_PROFILE_HZ='250'
    AC1_PROFILE_INTERVAL='5'
    AC1_PROFILE_DIR='D:/Prog/ac1_wt_engine/work/engine_prof/gameplay'
    AC1_NATIVE_OVERRIDES='0'
}
D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tools/profile_inclusive.py work/engine_prof/gameplay `
    --exe build/host-relwithdebinfo/ac1_startup.exe --from-present 2048 --seconds 60 `
    --output work/engine_prof/gameplay-inclusive
```

With `AC1_PROFILE` unset the extension starts no thread, allocates no buffers,
and inserts no guest instruction/call hooks or additional hot-path enable tests.
`AC1_PROFILE_STACKS=1` requires `AC1_PROFILE=1`. Flat mode retains its original
1,000 Hz default; stack mode defaults to 250 Hz. `AC1_PROFILE_HZ=25..2000`
overrides either rate. Native non-guest threads retain the existing separate
flat sampler and have zero-depth stack records. Sampling captures are not FPS
benchmarks.

Each `stacks_N.bin` record corresponds to the same ordinal in `raw_N.bin`:
little-endian uint32 depth, uint32 status, uint64 suspension ticks, followed by
96 uint64 PCs, leaf first. Unused PCs are zero. Return PCs are adjusted by one
byte. Status bits are 1=depth limit, 2=unknown/unretained image (including dynamic
function tables), 4=invalid/nonprogressing stack. Metadata gives record bytes
and depth. The old flat report reads the unchanged raw format.

The sampler requests full nonvolatile register context and uses the loaded
PE images' static exception tables with `RtlVirtualUnwind(NHANDLER)`. A module
reference acquired outside suspension keeps unwind/code pages live until
shutdown; profiling delays DLL unload. No symbol lookup, heap allocation,
loader call, file I/O or runtime lock occurs while a target is suspended.
Unknown images and truncated walks remain explicit in the analysis. The
native chain represents extant C activations; optimized tail calls and inlined
guest functions have no separate frame. Asynchronous DXVK work is visible as
external CPU but has no defensible synchronous guest ancestor.

Inclusive entry totals overlap. Recursive entries count once per observation;
subtree paths retain calling context. Self-body totals use the actual RIP;
self-with-host totals charge helper/import CPU to the nearest extant guest
frame. CPU estimates weight each thread's observed mix by measured OS CPU
deltas over complete metadata intervals, expanded to the selected window for
ms/frame estimates. Per-thread wall trees retain parked samples separately;
wall shares cannot be added across threads. Missing active observations remain
in the all-process CPU denominator. Inspect stack coverage and suspension
quantiles before interpreting results. GPU execution time is not measured.

For exact selected-entry counts, make a separate count build after sampling:

```powershell
tools/build.ps1 -BuildType RelWithDebInfo -Jobs 10 -ProfileCountEntries '004F75A0,004F5A70'
# Run with AC1_PROFILE_FRAMES=1, AC1_PROFILE_COUNTS='004F75A0,004F5A70',
# AC1_PROFILE unset, and AC1_PROFILE_DIR set to a new private capture directory.
D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tools/profile_counts_report.py work/engine_prof/counts `
    --from-present 2048 --seconds 60 --output work/engine_prof/counts.json
```

The generator injects a counter call only at the selected public definitions
(maximum 32), after native wrapping and before either direct or indirect entry.
`counts_N.bin` stores Present index and cumulative counts in metadata target
order. Differences between Present snapshots give exact counts for those
boundaries, across all guest threads. The atomic count hooks perturb execution;
never use this build for a speed comparison. Restore ordinary generation by
omitting `-ProfileCountEntries`. All modes preserve unknown-target diagnostics.

`tests/test_profile_stacks.py` compiles a real nested x64 fixture and verifies
ancestry, termination status, counts, disabled behavior and generator injection.
Android retains a no-op profiling implementation; compile checks establish
portability only, not Android sampling or hardware execution.
