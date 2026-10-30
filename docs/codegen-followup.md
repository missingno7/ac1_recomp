# FLAGABI and register-local diagnosis after c57847f

This round starts from the accepted CFG-flag experiment at `c57847f`.
The recreated worktree regenerated its complete catalog on the first standard
build. Private FFmpeg and DXVK dependencies are real copies. No junction or
symlink to a reference checkout is created; those checkouts and GOG remain
read-only. Generated C, objects, images, profiles, executables and detailed
reports remain under ignored `work/codegen2/` and `build/`.

## Fresh CFG baseline

Command: `tools/build.ps1 -BuildType RelWithDebInfo -Jobs 10 -Generator 'MinGW Makefiles' -FlagLiveness cfg -LocalRegs off`.
Input preparation took **421.22 seconds** and configure/compile/link took
**1,308.04 seconds**. Stripped PE size is **249,463,680 bytes**; `.text` is
**239,521,720 bytes**, identical to the accepted CFG build's code size.
SHA-256: `7a9cd343e3ea703f21c4188e687af24770782b919f5915c45e6ba94488dcb3cc`.
Its immutable private executable is `work/codegen2/bin/baseline/ac1_startup.exe`.

The full suite initially passes **74 tests**, including seven new FLAGABI
contracts and two userdata-seed tests. The 200,000-case partial-alias and
10,000-case portable native differential budgets complete for every binding,
without fallback. Critical-section, scheduler and Win32 contracts pass,
including host/ARM64 syntax and import audits. These checks do not establish
Android hardware execution.

## Repeatability controls

`-UserdataSeed` is an optional interleaved-harness parameter. Before each A/B
side it restores a selected private save/profile tree to workspace userdata,
archives the preceding state and records per-file SHA-256 inputs. It refuses
linked paths and paths outside private `work/`. The original harness behavior
is retained when this option is absent. No runtime or installation write
policy changes are needed.

A warmup creates a private seed; five 120-second CFG runs restore that same
seed with native overrides explicitly disabled, matching the timing setting.
The replay uses successful-Present indices while the guest clock uses actual
monotonic time. Therefore equal Present counts alone do not prove equal
simulation state. Compare the captured stage and the input/clock trace.
The three seed files have identical input manifests across all five starts.
The Present-2048 boundaries show the tiled sprint area but have different
NPC/prompt animation timing. Variant comparisons are recorded below.

All five normal CFG replays pass without a guest stop, through final captures
at Presents **4600, 5000, 4800, 5000 and 4600**. All five boundary images show
the tiled sprint area; NPC and prompt animation differ. Thus normal runs are
not pixel-deterministic, while this controlled set has a stable boundary stage.

A separate causal timing control runs the **unchanged CFG executable**, with
the same seed, using DXVK's `d3d9.maxFrameRate = 12` via a private config file.
The copied DLL contains this option and its log confirms the effective value;
the option is documented in [DXVK's configuration](https://github.com/doitsujin/dxvk/blob/master/dxvk.conf).
At Present 2048 this control shows **cobblestones and the CAMERA prompt**, the
same broad difference previously observed with slow LREGS. The 240-second
control reaches Present 2800 without a guest stop. This demonstrates that
changing render cadence alone can produce the stage difference without any
local-register code. It does not prove every LREGS function correct, but that
image difference alone is not a unique semantic-divergence signal. The capped
control is excluded from performance comparisons. Evidence:
`work/codegen2/cfg-slow-control/` and `cfg-boundary.png`.

The completed interleaved sets provide a stronger uncapped control: the
**same CFG executable and seed** naturally show CAMERA / GRAB AND THROW /
CAMERA at Present 2048 in the three locals-cohort A runs, and TACKLE / fade /
TACKLE in the x87-cohort A runs. Thus flags-only stage progression also varies
between ordinary runs, without a limiter or locals. Paired boundary sheets
are `bench-{abi,locals,x87}-boundaries.png`; all were inspected. This rules out
using those stage labels alone as a locals-only divergence signal.

## FLAGABI contracts

The [NFSU2 precedent](https://github.com/th0mk/nfsu2-web/blob/main/tools/flagabi.py)
computes entry flag needs and caller observations of returned flags. This
implementation analyzes AC1's emitted-C instruction groups and actual helper
semantics: logical operations preserve AF and INC/DEC preserve CF. It does not
assume every direct call defines every arithmetic flag.

Every current lifted AC1 function is in the public dispatch table. Consequently
every public `L_<VA>` retains all-live return flags. The opt-in `abi` mode uses
separate `ac1_abi_<VA>` C bodies only at direct callsites whose successor flag
demand is zero. These bodies never enter guest dispatch or callback tables;
other callers retain the full public entry. Direct-call diagnostic guards and
slow paths retain incoming flag observations. Native bindings, segmented/SEH
functions, nonlocal resumption and unsupported-instruction bodies are excluded
from private copies. Callee definitions reach a monotone call-graph fixed point.

Tests cover mixed consuming/discarding callers, partial definitions, recursion,
opaque observers and exclusions. A 4,096-case compiled fixture compares full
CPU, call accounting and stack writes through direct and dispatch paths. A
synthetic x86 emitter test compiles separate real shards and verifies that
private symbols are absent from the dispatch table. Diagnostic counts distinguish
public entries from private functions/returns and rewritten callsites.

Generation admits **5,988 private functions**, **28,152 private C returns** and
**32,733 direct callsites**, from 101,995 public functions. **No public return
contract is narrowed.** There are 39,880 proved defining callees; 13,650 unsafe
functions and three native bindings remain conservative. Generation retains the
same two precisely diagnosed unsupported instructions.

ABI preparation takes **646.63 s**, native configure/compile/link **1,498.00 s**.
Its stripped size is **276,301,112 bytes**, `.text` **265,959,832 bytes**
(+11.04% text versus CFG). SHA-256:
`de7886a768ed867421f714330aa6c9072524b75f202d51b61dc28a6467260698`.
Five seeded 120-second ABI replays complete with no guest stop. At Present
2048 they remain in the tiled area with CAMERA/SPRINT prompt variation.

The completed interleaved comparison uses three 60-second pairs, immutable
executables, identical three-file userdata inputs and native overrides disabled.
No task-owned compilation runs during measurement. All six windows pass the
no-stop/no-drop audit; minimum coverage is 64.059 s and first-to-last sample
span 59.968 s. Evidence: `work/codegen2/bench-abi/` and `bench-abi-audit.json`.

| Metric | CFG median [min, max] | ABI median [min, max] |
|---|---:|---:|
| FPS | 24.539 [23.734, 24.617] | 25.469 [24.630, 29.134] |
| p95 frame ms | 47.162 [46.888, 48.430] | 46.191 [45.810, 98.234] |
| p99 frame ms | 50.732 [49.416, 77.582] | 49.973 [49.102, 114.394] |
| CPU cores | 1.100 [1.067, 1.117] | 1.100 [1.033, 1.100] |

Paired FPS changes are **+3.773%, +18.723%, +3.461%**; their median is
**+3.773% / +0.896 FPS**. Paired median p95/p99 changes are −1.351/+0.557 ms.
Pair two has a phase/tail outlier (B shows cobblestones/GRAB AND THROW while A
shows the tiled CAMERA area), so this is a replay result on a shared PC,
not a general cycle-cost estimate. The backend-interrupted cohort is retained
separately as `bench-abi-interrupted/`: its only complete pair was
26.551→23.151 FPS (−12.81%), and the next pair was incomplete. It is excluded
from the completed three-pair statistic but limits confidence in repeatability.
CFG remains the default; the ABI experiment is selectable with
`-FlagLiveness abi` / `--flag-liveness abi` for review and further workloads.

## LREGS object evidence before changes

The original generated bodies and their existing LREGS transformation were
compiled as paired isolated objects with the host build's optimization and
arithmetic flags. They retain each real body's generated declarations and CPU
header. Whole-shard counts can differ slightly through alignment and compiler
interprocedural analysis; these are paired isolated measurements, not claimed
as the whole-program object extents.

| VA | Instructions CFG / old locals | Stack-memory instructions CFG / old locals | Vector-memory instructions CFG / old locals | Emitted full spills / reloads |
|---|---:|---:|---:|---:|
| `004F5A70` | 5,113 / 5,644 | 305 / 813 | 147 / 457 | 55 / 54 |
| `00428540` | 1,406 / 1,497 | 135 / 301 | 59 / 93 | 4 / 3 |
| `0051DD60` | 375 / 495 | 25 / 97 | 0 / 54 | 10 / 9 |

In the search function, GCC materializes the local register aggregate at host
stack offsets `rsp+0x40..0x58` and repeatedly copies its 32-byte prefix. Full
synchronization surrounds PUSH/POP and even `op_sar` in the search loop. The
actual pinned shift helper only reads/writes flag fields; it does not use GPRs.
The larger transform also synchronizes around x87 helpers. These observations
establish a concrete synchronization/host-spill cost; they do not by themselves
prove absence of a semantic divergence or assign the whole FPS loss to it.
Private sources, objects, disassemblies and counts are under
`work/codegen2/objects/`.

## Systematic LREGS changes

The revised pass never takes the local aggregate's address. It emits scalar
assignments only for referenced GPRs, permitting GCC's scalar replacement.
Unreferenced GPRs stay CPU-authoritative, including writes by opaque helpers.
An explicit allowlist of audited flag/shift helpers needs no GPR synchronization.
PUSH32/POP32 use CPU-authoritative ESP throughout each instruction group; only
dirty ESP is spilled and the result is reloaded on ordinary and taken edges.
This preserves PUSH ESP, POP ESP, PUSHAD/POPAD and PUSHFD evaluation order.
A compile-time assertion rejects a non-flat stack-base configuration.

Dirty GPR sets reach a CFG fixed point through joins and loops. Opaque CPU
helpers, calls, imports, diagnostics and exits receive all dirty registers;
post-helper reloads preserve writes to localized registers. Unknown edges stay
conservative. Segmented/SEH/nonlocal-resumption bodies and address-exposed
registers retain their original bodies. Labels and indirect entry guards are
included in the CFG. The existing per-function selection and default `off`
remain in place.

| VA | CFG / revised instructions | CFG / revised stack-memory instructions | CFG / revised vector-memory instructions |
|---|---:|---:|---:|
| `004F5A70` | 5,113 / 5,204 | 305 / 617 | 147 / 174 |
| `00428540` | 1,406 / 1,487 | 135 / 289 | 59 / 79 |
| `0051DD60` | 375 / 397 | 25 / 46 | 0 / 3 |

These paired isolated objects show substantially less copying than old locals,
but still more instructions and stack traffic than CFG. A private named-scalar
experiment produces nearly identical stack traffic, so changing C spelling
alone does not solve the remaining register pressure and helper alias barriers.

The complete suite passes **77 tests** before the gameplay comparison. Added
4,096-case full-CPU/stack fixtures cover PUSH/POP ESP, PUSHFD, dynamic shifts,
dirty joins/backedges and an opaque observer that writes both localized and
unreferenced registers. The FLAGABI fixture also compares ABI+locals through
fast and dispatch paths. The explicit `tests/run_mmx_general.py
--flag-liveness cfg --local-regs all` run passes 669,000 MMX cases/202 forms,
all MXCSR modes, 2,000 PUSHAD/POPAD cases, x87 environment/ext80/tag/TOP cases,
11,200 actual audio and 15,420 actual Bink cases. This tests emitted local code,
rather than merely rerunning the default-off oracle.

The lifter self-test and 200,000-case partial-alias / 10,000-case portable
native-binding budgets also pass. Five seeded 120-second locals replays have
no guest stop, reaching captures **4096, 4200, 4200, 4200, 4000**. Boundary
images all show the tiled area; one has SPRINT and four CAMERA, with varying
NPC/tutorial animation. No distinct locals-only divergence signal was found.
Together with the unchanged-CFG cadence control, this does not justify using
the previous stage mismatch alone to bisect a supposed synchronization error.
The original critical-section span regression remains covered and passing;
this round found synchronization cost rather than a new critical-section miss.

Generation admits **86,994** of 101,995 functions with locals (`all` selection).
Preparation takes **444.65 s**, native configure/compile/link **1,221.50 s**.
Stripped PE is **255,916,171 bytes**, `.text` **245,331,464 bytes** (+2.43%).
SHA-256: `164d32aaf2145e3f1c702b12d3789bf091613f1d85b1404d7ae68dfa969a19e4`.

| Metric | CFG median [min, max] | Revised locals median [min, max] |
|---|---:|---:|
| FPS | 21.465 [19.644, 22.000] | 22.967 [21.287, 23.617] |
| p95 frame ms | 60.395 [58.958, 64.582] | 54.919 [51.550, 58.270] |
| p99 frame ms | 69.401 [65.329, 75.770] | 64.279 [59.944, 65.231] |
| CPU cores | 1.117 [1.100, 1.117] | 1.117 [1.100, 1.117] |

Three interleaved 60-second pairs give **+8.367%, +6.996%, +7.348%** FPS;
median **+7.348% / +1.617 FPS**. Paired median p95/p99 changes are
**−6.313/−5.385 ms**, CPU **0.000 cores**. All six windows pass, with
minimum coverage 62.996 s and sample span 59.942 s, identical seed manifests,
no dropped samples or guest stops, and unchanged hashes for 186 frozen source
files. No own build or compiled test ran during measurement. Evidence:
`work/codegen2/bench-locals/`, `bench-locals-audit.json`, and
`locals-source-freeze.json`.

This is an aggregate win for the revised `all` set in this replay, despite
remaining costs in the three inspected objects. It is not a per-function win
claim or a comparison with the earlier ABI cohort under different host load.
Locals remain default-off and per-function selectable for review; no unmeasured
function selection is enabled by default. ABI+locals composition has synthetic
correctness coverage, but its combined gameplay performance is unmeasured.
The paired boundary sheet also shows CAMERA/GRAB AND THROW variation on both
sides; pair three shows CAMERA on both. These are shared-PC replay deltas,
not proof of a fixed simulation workload or per-function cycle savings.

## x87 integer-helper experiment

The original exact-name disassembly counter missed GCC helper suffixes. A
fresh linked-CFG audit finds **seven** `ac1_x87_integer.constprop.0` calls in
`004F5A70` and **three** in `004F2390`. The baseline also contains 176 integer
helper symbols across its generated shards. Symbol counts are static evidence,
not a new dynamic profile. Private evidence is
`work/codegen2/baseline-linked-x87-calls.json` and `baseline-linked-*.asm`.

`tools/prepare_runtime.py` now gates only this helper's declaration with
`AC1_FORCE_INLINE_X87`; its complete body is unchanged, including dynamic guest
rounding, invalid/precision status bits and precise unmasked-exception messages.
Pinned donor inputs and hashes are unchanged. CMake defaults the option off;
`tools/build.ps1 -X87Inline` explicitly sets it on, while omitting the switch
explicitly resets a previously enabled cache. The option requires GCC/Clang's
`always_inline` support and diagnoses unsupported opt-in compilers precisely.

The new native oracle compares baseline and forced-inline translation units
both at normal `-O2` and with `-fno-inline` (the explicit helper boundary).
Each configuration passes **412,416** cases, **824,832 total**, comparing
the returned 64 bits, complete CPU bytes, diagnostic count/VA/reason/CPU at the
observer, and host floating-point exception bits. Inputs cover all four host
and guest rounding modes, 16/32/64-bit outputs, truncation on/off, invalid and
precision exception masks, signed zeros, subnormals, quiet/signaling NaNs,
infinities, half-way values, signed-width boundaries and random binary64 bits.
Object checks verify that the forced variant has no remaining integer helper
body or branch to one. The transform test rejects missing/duplicate signatures
and verifies that only the declaration changes.

The full suite passes **79 tests**. The separate
`tests/run_mmx_general.py --flag-liveness cfg --x87-inline` run compiles the
generated and native units with the macro enabled and passes all existing
MMX/x87-state/audio/Bink budgets. Detailed evidence is under
`work/x87-inline-tests/` and `work/codegen2/x87-{unit,mmx-oracles}.log`.

The separate CFG/locals-off/x87-inline build takes **254.03 s** preparation and
**1,248.00 s** native configure/compile/link. Stripped PE is **249,691,015
bytes**, `.text` **239,755,672 bytes** (+0.098% versus CFG). SHA-256:
`4f20bb6547e56163769d42b4056e88a491ef1aba353ca38d50dfe13bdaab754e`.
Preparation timings include discovery/emission and reuse available private
caches; neither these nor shared-PC compile timings are claimed as controlled
compiler-speed improvements.

The actual linked executable has **zero** remaining integer-helper symbols.
In `004F5A70`, host instructions grow **5,113→6,420**, integer calls **7→0**,
with seven static `fmod` sites in the expanded rounding branches. In `004F2390`,
instructions grow **961→1,482**, integer calls **3→0**, with three `fmod`, one
`ceil` and one `trunc` site. Those libm operations already form part of the
conversion algorithm; static site counts do not establish their dynamic cost.
This confirms removal of the helper boundary but also exposes body growth,
so inlining alone is not treated as a demonstrated speedup. Evidence:
`work/codegen2/x87-linked-comparison.json` and `x87-linked-*.asm`.

The refined declaration-only test passes again after the build, followed by
the full native-leaf budgets and critical-section/scheduler/Win32 contracts.
Five seeded 120-second x87 replays pass without a guest stop, all reaching
Present **3200**. All five executable hashes and three-file seed inputs match;
the inputs also match CFG's seed. Boundary captures retain the tiled area,
with TACKLE/prompt/fade variation rather than pixel determinism. Evidence:
`work/codegen2/x87/`, `x87-boundary.png`, and `final-*-contract.log`.

| Metric | CFG median [min, max] | x87 inline median [min, max] |
|---|---:|---:|
| FPS | 19.909 [19.822, 20.504] | 19.896 [18.843, 24.286] |
| p95 frame ms | 61.354 [60.340, 62.153] | 61.668 [47.328, 63.420] |
| p99 frame ms | 67.384 [66.651, 68.587] | 69.662 [67.137, 99.176] |
| CPU cores | 1.133 [1.100, 1.133] | 1.133 [1.050, 1.133] |

Three interleaved 60-second pairs give **−0.069%, −8.099%, +22.520%** FPS;
median **−0.069% / −0.014 FPS**, with paired median p95/p99 changes
**−0.484/+3.012 ms** and CPU **0.000 cores**. Pair three has a phase/tail
outlier: B has SPRINT while A has TACKLE, and B's p99 rises to 99.176 ms.
All six windows remain valid and are retained. Minimum coverage is 63.552 s,
sample span 59.963 s; hashes for 186 source files are unchanged, all seed
manifests match and there are no dropped samples or guest stops. Evidence:
`work/codegen2/bench-x87/`, `bench-x87-audit.json`, `x87-source-freeze.json`,
and `bench-x87-boundaries.png`.

**No x87 inlining win is established; it stays default-off.** RSP-relative
memory-instruction sites grow 305→318 and 53→62 in the two linked bodies.
The body/stack growth and remaining libm operations explain why merely
removing helper symbols is insufficient evidence of a speedup; assigning
the measured variance to a particular cause would require another profile.

| Variant | Stripped PE bytes | `.text` bytes | Preparation s | Native configure/compile/link s |
|---|---:|---:|---:|---:|
| CFG | 249,463,680 | 239,521,720 | 421.22 | 1,308.04 |
| FLAGABI | 276,301,112 | 265,959,832 | 646.63 | 1,498.00 |
| Revised locals + CFG | 255,916,171 | 245,331,464 | 444.65 | 1,221.50 |
| x87 inline + CFG | 249,691,015 | 239,755,672 | 254.03 | 1,248.00 |

## Reproduction and evidence boundaries

Build each variant once, retaining the same `ac1_startup.exe` basename in
separate A/B folders. Preserve A before building B, finish its comparison, then
replace B for the next experiment. Keep at most these two stripped variants.
All four builds use `RelWithDebInfo` and ten jobs; no builds or compiled tests
run during a replay/timing set. A selected private seed is restored before
every side. Its proprietary contents are not published.

```powershell
tools/build.ps1 -BuildType RelWithDebInfo -Jobs 10 -FlagLiveness cfg -LocalRegs off
New-Item -ItemType Directory -Force work/codegen-review/A, work/codegen-review/B
C:/msys64/mingw64/bin/objcopy.exe --strip-debug build/host-relwithdebinfo/ac1_startup.exe work/codegen-review/A/ac1_startup.exe
# Copy a chosen workspace userdata tree once; keep it immutable thereafter.
Copy-Item -LiteralPath work/userdata -Destination work/codegen-review/seed -Recurse

# Build one B at a time, then strip to B/ac1_startup.exe and run the comparison below.
tools/build.ps1 -BuildType RelWithDebInfo -Jobs 10 -FlagLiveness abi -LocalRegs off
# Next experiment: cfg + locals all; then cfg + locals off + X87Inline.
# tools/build.ps1 -BuildType RelWithDebInfo -Jobs 10 -FlagLiveness cfg -LocalRegs all
# tools/build.ps1 -BuildType RelWithDebInfo -Jobs 10 -FlagLiveness cfg -LocalRegs off -X87Inline
C:/msys64/mingw64/bin/objcopy.exe --strip-debug build/host-relwithdebinfo/ac1_startup.exe work/codegen-review/B/ac1_startup.exe
tools/bench_gameplay.ps1 -Compare -Runs 3 -Seconds 60 -Binary work/codegen-review/A/ac1_startup.exe -BinaryB work/codegen-review/B/ac1_startup.exe -AllowBusy -UserdataSeed work/codegen-review/seed -Env @{AC1_NATIVE_OVERRIDES='0'} -EnvB @{AC1_NATIVE_OVERRIDES='0'}
```

The local measurements also use `-AllowDirty` because the shared worktree Git
metadata is read-only. Source and executable hashes, rather than that shared
HEAD alone, identify each measured build. Private commits use
`work/codegen-commits.git` with an explicit work tree and a plain alternates
file; no Git junction/symlink or reference checkout modification is needed.
The exported `work/codegen.bundle` is based on **c57847f**, unlike the first
round's historical bundle base. Generated CPU headers, game/generated C,
builds, captures, logs and task briefs are excluded from all commits.

The CFG, ABI, locals and x87 comparisons are separate cohorts on a shared PC.
Use each cohort's paired deltas; do not compare raw medians between cohorts or
add their percentages. No combined ABI+locals+x87 FPS result is claimed.
Android syntax checks are separate from hardware execution, which is not
claimed here. Capture pruning records SHA-256 inventories and retains the
Present-2048 boundary, first gameplay and final images.

After validation, obsolete ABI/locals stripped variants and redundant captures
were removed. Only CFG and x87 stripped variants remain. The completed native
executable, runtime/graphics archives and **1,039 object files** were removed
individually. Automatic approval review rejected recursive build-directory
removal; the safer artifact cleanup succeeded, leaving **9,322,578 bytes** of
build configuration, dependency and compiler-probe files. Free space was about **34 GB**.
No task-owned game, benchmark, GPU sampler or compiler remains; unrelated
build shells and older processes were left untouched.

All five focused commits carry the Codex coauthor trailer. The bundle contains
only source, synthetic tests and documentation. The next step is review and
broader workload measurement of the opt-in ABI and revised locals modes;
repeat the ABI result given its interrupted negative pair and phase outlier,
and measure combinations separately before enabling any by default. x87
integer-helper inlining has no demonstrated win and remains off.
