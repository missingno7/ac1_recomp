# Gameplay guest hotspots and native leaves

This experiment replaces original guest functions in the static recompilation.
It does not introduce a gameplay engine. All addresses and instruction hashes
apply only to the executable fingerprint in `config/ac1_dx9.json`.

## Binding and ABI

`native_overrides` in the lifter config names an address, a C implementation,
a write-plan function and an instruction SHA-256. The hash covers the sorted,
deduplicated recovered instructions: little-endian VA (`uint32`), instruction
size (`uint16`), then original instruction bytes, for each instruction. Sparse
basic-block gaps are excluded. Generation rejects missing translated bodies,
changed hashes, duplicate addresses, invalid C names, calls, interrupts, locked
instructions and external/indirect branches in verification oracles.

The lifter retains the translated body as `ac1_translated_<VA>` and emits one
public `L_<VA>` wrapper. Both `AC1_CALL` direct transfers and the dispatch table
resolve that wrapper. The registry is generated in private `initial_native.c`;
project-owned implementations are in `src/runtime/ac1_native_leaf.c`. The CPU
layout and pinned donor inputs in `prepare_runtime.py` are unchanged.

These bindings use the guest stack and registers directly; they do not call a
host function with the guest's calling convention. Each implementation performs
the guest return itself. These three functions are cdecl: ESP advances by four,
arguments remain on the stack, EBP/EBX/ESI/EDI are preserved, and DF is preserved.
Scratch GPRs, every modified XMM lane, arithmetic flags, final traced EIP and
saved-register/scratch memory are reproduced, including values the C ABI would
normally leave unspecified. Thus flag preservation does not depend on assuming
that a caller ignores returned flags.

| Guest entry | Arguments | Guest writes | Returned state |
|---|---|---|---|
| `0x01176DE0` | plane4, low4, high4 | aligned 48-byte scratch frame; saved EBP | EAX classification 1/2/3; ECX/EDX corner pointers; XMM0-6; CMP flags |
| `0x00653400` | out64, A64, B64 | out64; aligned 32-byte scratch frame; saved EBP | EAX=out, ECX=B; XMM0-7; stack-allocation SUB flags |
| `0x0042EE70` | out4, quaternion4, vector4, scale4, unit4 | intermediate cross product and final out4; saved EBP | EAX=out, ECX=scale, EDX=unit; XMM0-5; stack-alignment AND flags |

The matrix kernel retains the original multiplication and addition order and
supports outputs overlapping either input. The quaternion kernel preserves its
intermediate output store before reading the scale/unit vectors, including when
either aliases the output. The AABB kernel retains both dot-product operand
orders in dead upper lanes. Host SSE2 evaluates four lanes together; ARM64 and
`AC1_NATIVE_PORTABLE` use C lane operations. Contraction is disabled by the
existing `-ffp-contract=off` build flag.

## Runtime switches and verification

- `AC1_NATIVE_OVERRIDES=0`: translated bodies only, in the same binary.
- `AC1_NATIVE_OVERRIDES=1` (default): all configured overrides.
- `AC1_NATIVE_OVERRIDES=00653400,0042EE70`: only those hexadecimal VAs.
- `AC1_NATIVE_VERIFY=1000`: check the first 1000 eligible calls **per binding**.
- `AC1_NATIVE_STATS=1`: print counts at orderly runtime shutdown. Forced process
  termination cannot produce shutdown totals; verifier milestones flush promptly.

Plans decline NaN, infinity, subnormal inputs, magnitudes above 2^20, wrapping ranges and stack aliases
outside the audited contract. Declined calls execute the original translated
body, with a first-use diagnostic. Malformed selections and unknown VAs stop with
a precise diagnostic. Checked mode never silently reduces its comparison budget.

For a checked call, the verifier snapshots the audited write set and copies the
complete CPU. It runs the translated oracle, saves its output, restores original
memory, then runs the native body against identical inputs at identical guest
addresses. Overlapping ranges merge before snapshots, preserving aliases. It
compares all CPU bytes (including flags, ESP, XMM, FP environment and padding) and
every byte in the declared write set. The first mismatch names the binding, call
number and exact CPU byte offset or guest memory address, with expected/actual
bytes. Snapshot sets above 16 MiB fail explicitly.

This is a verifier for audited pure leaves, not a sandbox for arbitrary native
code. The write plans were checked against all recovered store instructions;
there is no page-wide detector for undeclared writes. Imports, guest scheduling,
callbacks and irreversible host effects are prohibited in the translated oracle.
Only the guest owner runs either body, so rollback cannot race another guest.
Production calls pass NULL to the plan for eligibility-only validation; they do
not construct or merge write ranges. A rejected call can rerun its pure plan to
collect the first diagnostic's exact guest address and rejection condition.

Reproducible focused checks (private original bodies are read from the generated
catalog, never committed):

```powershell
& D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tests/test_native_bindings.py
& D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tests/native_leaf_oracle.py
& D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tests/native_leaf_oracle.py --portable
& D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tests/native_leaf_oracle.py --cases 200000 --partial-alias
& D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tests/native_leaf_oracle.py --cases 200000 --partial-alias --portable
cmake --build build/host-relwithdebinfo --target ac1_native_contract
build/host-relwithdebinfo/ac1_native_contract.exe
```

## Measurement procedure and evidence

The initial attempt to reuse existing generated object files was rejected:
sampling exposed out-of-line accessor helpers from stale historical compilation
inputs, despite matching current source/header text. Its exploratory capture
`work/hot-baseline/` is **not** the final baseline and is excluded from performance
claims. All 840 copied objects were invalidated for local recompilation.

Final profiling uses successful Present index 1628 as its lower bound and a
60-second gameplay window. Exclusive guest-body samples use all CPU-active
process samples as their denominator. Helper/import attribution by the last
guest EIP is reported separately: it is not a true stack-inclusive measurement.
Per-entry call counts are unavailable from this statistical sampler.

The benchmark uses one debug-stripped immutable binary, frames-only collection,
and off/on pairs interleaved three times for each binding and all together:

The measurements below were taken with a since-removed `tools/bench_native.ps1`
wrapper. On main, repeat them with the interleaved harness, e.g. for all overrides:

```powershell
tools/bench_gameplay.ps1 -Compare -Runs 3 -Seconds 60 -Binary build/host-relwithdebinfo/ac1_startup.exe `
  -BinaryB build/host-relwithdebinfo/ac1_startup.exe -EnvB @{ AC1_NATIVE_OVERRIDES='1' } -AllowBusy
```

Other workers share the machine; report medians and min/max spread, and retain
each pair rather than treating one run as evidence of an optimization gain.
Sampling runs are separate from frame-timing benchmarks.

## Final 60-second gameplay profile

The baseline window is 23.054-83.054 seconds after runtime profiling starts, beginning at successful Present index 1628. It has 49,372 active observations. Percentages below are exclusive body time estimates weighted by each thread's measured OS CPU time, divided by all measured thread CPU. They include no inferred callees. Inlined helpers remain inside their containing body.

Roles marked inferred or unproven are intentionally limited to the observed instruction/call pattern; no engine-level behavior is invented. The saving column is an **exclusive-only 2x body-speed scenario**, before helper savings or override overhead, not a promised FPS gain. Stateful or unproven bodies have no defensible native saving forecast.

| VA | CPU % | Classification from disassembly/generated C | Native proposal | 2x scenario |
|---|---:|---|---|---:|
| `0x004F5A70` | 1.911 | Math: nonleaf transform evaluation/quantization | Retain coordinator; optimize its quaternion/packing callees | unestimated |
| `0x00428540` | 1.688 | Math: quaternion to matrix, mixed float/double SSE | Native formula with exact mixed precision and scratch lanes | 0.844% CPU |
| `0x00653400` | 1.580 | Math: four-by-four matrix product | Implemented SSE2/C kernel | 0.790% CPU |
| `0x01176DE0` | 1.481 | Math: plane/AABB support-point classification | Implemented SSE2/C kernel | 0.741% CPU |
| `0x00761210` | 1.255 | Object/container update with virtual calls and SEH | Retain traversal; audit leaf callees | unestimated |
| `0x0051DD60` | 0.962 | Container: pointer-array binary search, 16-bit key at +0x70 | Native lower-bound loop preserving NULL cases and flags | 0.481% CPU |
| `0x00516B00` | 0.907 | Math/container: vector interpolation via object data | Recover ownership/lookup ABI before a native interpolation | unestimated |
| `0x00841BA0` | 0.817 | Audio: IMA-style ADPCM nibble decoder, clamped predictor/step index | Native integer decoder, PCM/state and all scratch effects | 0.409% CPU |
| `0x004F2390` | 0.789 | Math: scale XYZ, x87 round to three int16 components, zero fourth | Native quantization with guest rounding and FP environment | 0.395% CPU |
| `0x00AE11A0` | 0.786 | Math/container: transform traversal and matrix products | Keep traversal; benefits from matrix child override | unestimated |
| `0x0042EE70` | 0.733 | Math: quaternion/vector transform and component scale | Implemented SSE2/C kernel | 0.366% CPU |
| `0x00933450` | 0.690 | Container/schema: tagged-value dispatch (inferred) | Recover tag/ownership semantics before native lookup | unestimated |
| `0x00F9FB80` | 0.690 | CRT: overlap-aware bulk copy, SSE and helper paths | Native memmove with exact branch-dependent register/flag effects | unestimated |
| `0x0051AF40` | 0.598 | Math: integer quotient/remainder plus x87 interpolation | Native interpolation after FP/return-state audit | 0.299% CPU |
| `0x00AA83A0` | 0.589 | Math: four-component bitwise vector equality | Native comparison retaining first differing-lane flags/scratch | 0.294% CPU |
| `0x01037A60` | 0.517 | Math: large mixed x87/SSE geometry kernel; formula unproven | Recover formula first; retain FP-sensitive paths | unestimated |
| `0x0048F310` | 0.514 | Math: shortest-arc quaternion interpolation and normalization | Native nlerp with sqrt/div and lane-state equivalence | 0.257% CPU |
| `0x00A84FF0` | 0.488 | Math: large x87 geometry routine; high-level role unproven | Defer whole-body replacement; target verified subkernels | unestimated |
| `0x00FCA502` | 0.475 | CRT: math-error/FP-environment dispatch | Retain exception/rounding behavior | unestimated |
| `0x005167E0` | 0.457 | Container: object/animation traversal with SEH (inferred) | Retain stateful coordinator; audit numerical leaves | unestimated |
| `0x0075D0A0` | 0.445 | Math: XYZ distance, SSE sqrt then x87 return | Native distance with exact ST0/tags/FP environment | 0.223% CPU |
| `0x006C8260` | 0.419 | Math/container: nonleaf geometry/transform evaluation | Optimize audited callees rather than replacing traversal | unestimated |
| `0x004F3420` | 0.382 | Math: vector quantization wrapper, calls 0x004F2390 | Optimize the quantization leaf first | unestimated |
| `0x01035EB0` | 0.361 | Math: vector dot/projection arithmetic, x87/SSE | Recover geometric formula and preserve FP state | 0.181% CPU |
| `0x004BF460` | 0.342 | Math: quaternion product with intermediate stores | Native Hamilton product preserving output/input aliases | 0.171% CPU |
| `0x0075D600` | 0.324 | Container: binary search of eight-byte records by uint16 key | Native search loop after exact ordering/flag audit | 0.162% CPU |
| `0x0051ECA0` | 0.318 | Container: lookup wrapper around 0x0051DD60 | Optimize the binary-search leaf first | unestimated |
| `0x00A228D0` | 0.293 | Geometry/container: bounded object traversal with virtual callbacks | Retain traversal; audit bounds/culling leaves | unestimated |
| `0x0102FF30` | 0.291 | Container: recursive object traversal and virtual callbacks | Keep recursion/ownership behavior; audit leaf operations | unestimated |
| `0x00AE0E90` | 0.262 | Math: adapter calling quaternion-to-matrix 0x00428540 | Optimize 0x00428540 first | unestimated |

These were chosen for a small, well-understood floating-point contract and reproducible ABI oracle. The next strong candidates are `0x0051DD60`, `0x00428540` and `0x00841BA0`. `strlen`/`memset` and lock/atomic wrappers did not dominate the exclusive top 30. Import-heavy wait wrappers such as `0x007A3030` must not be ranked by last-EIP attribution as if their time were numerical guest code.

Private per-function disassemblies are `work/disasm-<VA>.txt`; generated originals are under `work/lift_initial/`. Per-entry counts are absent from the sampler. A preliminary whole-run native counter snapshot before the CS repair observed 32,672,714 eligible matrix calls, 24,504,652 AABB calls, and 30,685,971 quaternion calls plus 710,912 translated fallbacks over 117.876 seconds **including startup**. Those are not counts for the final 60-second window.

### Guest code versus runtime overhead

| Exclusive host category | Baseline weighted CPU % |
|---|---:|
| generated guest code | 56.477 |
| x87/MMX/SSE helpers | 4.778 |
| guest memory access | 1.714 |
| dispatch/lookup | 2.897 |
| import adapters | 0.126 |
| scheduler/sync | 1.615 |
| other runtime | 4.797 |
| native override machinery | 0.048 |
| External DLLs (all) | 18.776 |
| CPU with no active observation | 8.753 |

Directly identified dispatch/import/scheduler overhead is 4.639% of measured CPU. This is a lower bound: external ntdll (5.291%), winpthread (2.026%) and other DLL samples cannot be split reliably into imports, scheduling and TLS without stacks. Import adapter code inlined into dispatch is charged to dispatch. Parked wait-stub observations are excluded; sampler CPU is excluded from thread-weighted totals.

### Replaced-body profile shares

| VA | Overrides off CPU % | All on CPU % |
|---|---:|---:|
| `0x00653400` | 1.580 | 0.493 |
| `0x01176DE0` | 1.481 | 0.598 |
| `0x0042EE70` | 0.733 | 0.552 |
| Combined | 3.794 | 1.643 |
| Shared override machinery | 0.048 | 0.346 |

The after column includes native bodies, retained translated fallbacks and VA wrappers, but separates shared eligibility/verification machinery. The profiles are separate statistical captures; their shares are not per-call cycle measurements or standalone FPS evidence. Both final 150-second replays reached gameplay and ended only at the harness deadline. First 10,000 live calls per override passed full CPU/write-set comparison, with all check budgets complete before the 1628-Present measurement boundary.

### Stabilization before final measurements

An earlier all-native sampler run stopped at 117.9 seconds at `InitializeCriticalSectionAndSpinCount`, guest `0x007A568E`, object `0x1478FFF0`. No native verifier mismatch occurred. Read-only inspection of the parallel `D:/Prog/ac1_wt_cs` source and `docs/critical-section-span.md` identified its existing adjacent-mapping CS fix. This branch reuses only that narrow validation/word-write change and its regression. The local regression reproduces the old false rejection in both direct and checked memory (25 assertions), then passes all six CS APIs, native Windows comparison, holes and wrap checks after the fix (163 assertions). No other worktree was modified. **All final profiles and A/B timing runs use the same repaired adapter.**

Final profile evidence: `work/hot-final-profile-off/{run.json,report.txt,hot.json}` and `work/hot-final-profile-all/{run.json,report.txt,hot.json}`. Earlier exploratory/pre-fix captures are excluded. The final unstripped executable SHA-256 is `764c1ba3405dfa890a0c367c409a14b607d419aee7824a5f4ab7e089de537ce3`. Final native/verifier objects compile for ELF64 AArch64/API28 with warnings treated as errors; no Android device or APK execution is claimed.

## Interleaved A/B timing results

All 24 runs passed the gameplay/window/scene-proof audit, with zero guest stops and one immutable binary. Three A/B pairs per variant ran in order all, AABB, matrix, quaternion, repeated three times. Each nominal window was 60 seconds after successful Present index 1628; shortest first-to-last Present span was 59.956864 seconds. Sampling and checked execution were disabled for these timing runs.

Binary SHA-256: `9c1c1292e8c261218dd17897358248499cb109d99ee87655678fa26765869718`

Medians [min, max]; each side has at least three 60-second gameplay windows.

| Overrides | Side | n | FPS | p95 ms | p99 ms |
|---|---|---:|---:|---:|---:|
| 0042EE70 | off | 3 | 25.96 [24.64, 26.57] | 46.52 [46.34, 48.94] | 52.81 [49.71, 54.44] |
| 0042EE70 | on | 3 | 25.84 [25.72, 25.86] | 47.25 [45.41, 47.66] | 51.30 [49.76, 51.87] |
| 00653400 | off | 3 | 25.47 [24.21, 26.91] | 51.06 [43.43, 55.39] | 59.47 [46.77, 64.20] |
| 00653400 | on | 3 | 25.78 [24.05, 26.55] | 50.45 [45.91, 50.66] | 55.52 [48.60, 56.03] |
| 01176DE0 | off | 3 | 25.78 [25.04, 27.18] | 45.46 [44.79, 51.58] | 49.41 [47.05, 55.81] |
| 01176DE0 | on | 3 | 26.14 [25.78, 27.69] | 45.67 [44.16, 48.33] | 49.07 [48.16, 57.31] |
| all | off | 3 | 26.77 [25.39, 26.86] | 46.51 [43.30, 48.35] | 51.66 [46.47, 52.59] |
| all | on | 3 | 25.96 [25.59, 27.11] | 47.12 [42.83, 50.95] | 49.55 [46.82, 55.00] |

Machine: Ryzen 9 7950X3D, 32 logical CPUs, 64 GiB RAM, Windows x64; GCC 12.2, `RelWithDebInfo` (`-O2 -g`), direct memory, forced accessor inlining and EIP tracing on; MinGW Makefiles. Concurrent work was permitted. These absolute rates describe this machine state and must not be compared directly with the earlier 65-75 FPS context.

- All together: median-FPS change -3.03%; paired FPS changes median +0.96% [min -4.42%, max +2.22%].
- AABB only: median-FPS change +1.39%; paired FPS changes median +4.39% [min -5.16%, max +7.40%].
- Matrix only: median-FPS change +1.21%; paired FPS changes median -0.66% [min -1.34%, max +1.21%].
- Quaternion only: median-FPS change -0.45%; paired FPS changes median -0.92% [min -2.68%, max +4.87%].

**No overall FPS improvement is demonstrated.** All-on has a lower observed median FPS, mixed tail-time changes, and overlapping run ranges. Matrix/AABB individual medians improve slightly, but their ranges also overlap. Replaced-body CPU share fell by 2.151 percentage points, while shared machinery rose by 0.298 points; these statistical CPU shares do not establish a wall-frame speedup. Keep the overrides experimental and use `AC1_NATIVE_OVERRIDES=0` to retain the measured baseline or select a VA explicitly. More controlled trials and the next ranked leaves are needed before recommending an all-on performance preset.

Raw per-run FPS, p95/p99, timestamps, frame snapshots, input copy, GPU samples and identity are in `work/bench/3ae63f85d766-38f462ac2fe6-hot-final-*/`. Aggregate Markdown/JSON are `work/hot-bench.{md,json}`; audit is `work/hot-audit.json`. The harness process CPU values are usable, but its C# per-thread deltas were zero in this environment and are not used as evidence; per-thread sampled breakdowns are in the separate final profile reports.

## Final verification and commit delivery

The final contract/leaf checks pass, including 10,000 normal alias cases per leaf and a supplemental 200,000 comparisons per leaf in each SSE2/portable build. Supplemental quaternion cases overlap both vectors read after the intermediate store, down to byte offsets; matrix cases include exact and partial output aliases. One additional call tests the production eligibility-only path after each check budget. Logs: `work/native-{contract-final,sse2-final,portable-final,partial-alias,partial-alias-portable}.log`. The final live run passed 10,000 calls per binding before Presents 125/175/325 respectively. All 24 timing games were stopped by the harness and no owned game remains running. Menu/probe captures were pruned; final sampler data and scene proofs referenced by benchmark results are retained privately.

The shared worktree is on `hot-native`, but its Git metadata is outside the writable root: `D:/Prog/ac1_recomp/.git/worktrees/ac1_wt_hot/index.lock` creation was denied. The shared branch ref therefore remains at `3ae63f85d76679f2584bae49b5808cc3c431ee49`. Focused commits with the requested trailer are prepared in private Git directory `work/hot-commits.git`, branch `hot-native`, and exported as `work/hot-native.bundle`. The bundle contains source/tests/docs only, never `BRIEF_hot.md`, generated translations, captures or game files. Timing identity records a dirty shared worktree and the immutable binary hash; it does not mislabel those measurements as a shared-branch commit.

Once shared Git metadata is writable, from this worktree the already-present working files can be preserved while installing the prepared fast-forward history:

```powershell
git fetch work/hot-native.bundle hot-native
git update-ref refs/heads/hot-native FETCH_HEAD 3ae63f85d76679f2584bae49b5808cc3c431ee49
git read-tree HEAD
git status --short
```

The expected-old-ref guard prevents replacing a branch that changed in the meantime. `read-tree` updates the index only; it does not discard working files or the untracked brief. These installation commands were not executed successfully inside the restricted sandbox.
