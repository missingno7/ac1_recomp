# Code generation: flag liveness and register locals

This experiment uses the fingerprint in `config/ac1_dx9.json`, starting at
`786ecf1`. Original/generated C, object disassembly, captures, executables and
logs stay under ignored `work/codegen/`, `work/gameplay/` and `build/`.
No reference checkout or installed game file is changed.

## Baseline facts

Five functions from `docs/hot-functions.md` cover nonleaf transforms, SSE,
integer search, integer audio, and x87 quantization. These are static sites,
not execution counts. Source counts exclude comments. `CPU refs` counts
explicit `c->` expressions; direct stores are their assignment sites, excluding
macro/helper effects. Thus subtraction is not a complete load count: compound
assignments and subregister macros can read and write the same field.

| VA | Role | CPU refs | Direct CPU stores | Eager arithmetic helper sites | Eager lanes | x87 helper sites |
|---|---|---:|---:|---:|---:|---:|
| `004F5A70` | nonleaf transform/quantization | 838 | 339 | 27 | 158 | 42 |
| `00428540` | SSE quaternion/matrix | 423 | 165 | 2 | 11 | 0 |
| `0051DD60` | integer binary search | 68 | 21 | 10 | 55 | 0 |
| `00841BA0` | integer audio decoder | 126 | 39 | 20 | 113 | 0 |
| `004F2390` | x87 quantization | 115 | 51 | 4 | 23 | 18 |

The helper-site column covers `flags_add/sub/adc/sbb/logicz/incs/decs`.
Logical helpers write five lanes and preserve AF; arithmetic writes six;
INC/DEC preserve CF. Shift helpers/direct flag assignments are additional.
Every listed eager helper computes PF. Additional producers are four shifts
in `004F5A70` (16 potential lanes, including four PF), two SARs in `0051DD60`
(eight lanes, two PF), and two SARs plus two inline CF/OF stores in `00841BA0`
(ten lanes, two PF). The other two functions have none. Including those gives
174/11/63/123/23 potential lane computations and 31/2/12/22/4 PF sites in table
order, before compiler elimination. The x87 counts include checks, integer
conversion, `fpush`, `fpop`, and `fst`; they do not mean out-of-line host calls.

Objects are built with GCC 12.2, RelWithDebInfo (`-O2 -g -DNDEBUG`), direct
memory, EIP tracing and forced accessor inlining. Disassembly uses
`C:/msys64/mingw64/bin/objdump.exe -dr -Mintel --insn-width=16` on the real
`CMakeFiles/ac1_runtime.dir/work/lift_initial/initial_NNNN.c.obj` files.
Private source/object inventories and extracted functions are
`work/codegen/{baseline-facts,object-facts}.json` and `baseline-<VA>.{c,asm}`.
CPU-memory counts below are conservative static counts through proven CPU
pointer bases; derived/indirect pointers can add accesses. EIP, flags and FPU
environment stores are included. A vector store counts once, even if it writes
multiple fields. Parity counts recognize the byte-wide shift-by-four starting
the XOR-fold recipe, rather than counting arbitrary shifts or host PF use.

| VA | Host instructions | CPU loads/stores (proved) | Flag store sites / lanes | Parity folds | x87 helper calls |
|---|---:|---:|---:|---:|---:|
| `004F5A70` | 5,320 | 648 / 861 | 102 / 105 | 18 | 0 |
| `00428540` | 1,397 | 255 / 200 | 6 / 6 | 1 | 0 |
| `0051DD60` | 435 | 26 / 78 | 34 / 34 | 6 | 0 |
| `00841BA0` | 911 | 71 / 143 | 59 / 60 | 10 | 0 |
| `004F2390` | 961 | 121 / 182 | 18 / 18 | 3 | 0 |

The first two functions retain their CPU pointer in RBX throughout their
bodies; the quantizer also derives a pointer into the FP state. x87 helpers
inline into environment/tag checks and stores. GCC already eliminates some
overwritten flags: the two source helpers in `00428540` leave only six lane
stores and one parity fold in the object. Source helper counts alone therefore
overstate the remaining opportunity.

## Flag pass

`tools/lift_ac1.py --flag-liveness off|straight|cfg` controls an emitted-C
instruction-group pass. It runs after raw-body caching and direct-call
specialization, before native-oracle wrapping. `tools/build.ps1 -FlagLiveness`
passes the selection and checks it in the generation report, so switching it
invalidates preparation correctly. Bink retains its original emission defaults.

The straight mode stops propagation across labels/branches. CFG mode takes the
union of successor live sets to a fixed point, including backedges. Only the
seven audited arithmetic helpers kill lanes. Unmodeled/conditional flag writes
remain conservative. The definitions follow emitted helper semantics rather
than treating x86 undefined flags as disposable. Every call, unknown CPU
helper, dispatch, return, explicit full-flag observation and diagnostic escape
requires all six arithmetic flags. FS/GS/SEH functions are excluded. Repeated
string operations and instruction-internal control flow are not rewritten.
Sparse entry guards stay outside the lowest-address instruction's CFG node.

`tools/prepare_runtime.py` retains pinned donor hashes and generates masked
clones of those helpers, preserving their algorithms. Each lane assignment is
guarded by a constant mask. INC/DEC delegate with CF excluded. The originals
remain available to runtime/Bink code. Accessor inlining makes the constant
masks available to GCC. Even a zero mask retains operand evaluation in emitted
C; memory/error behavior remains subject to the compiler's usual dead-code
rules. Functions with segmented/SEH paths keep eager flags.

| VA | Straight: emitted lanes / PF sites | CFG: emitted lanes / PF sites | CFG zero-mask sites |
|---|---:|---:|---:|
| `004F5A70` | 101 / 17 | 67 / 11 | 14 |
| `00428540` | 6 / 1 | 6 / 1 | 1 |
| `0051DD60` | 44 / 8 | 27 / 4 | 2 |
| `00841BA0` | 82 / 14 | 37 / 3 | 8 |
| `004F2390` | 18 / 3 | 18 / 3 | 1 |

These are pass outputs, before GCC's existing elimination, and are not FPS or
cycle results. `work/codegen/flag-facts.json` records the counts.

Across the full catalog, 302,024 arithmetic sites use constant masks and 159,285
retain conservative helpers. Of the masked sites, 85,250 have zero masks.
Potential arithmetic-helper lane writes fall from 2,570,497 to 1,884,124 (26.7%)
and PF-producing sites from 461,309 to 326,877 (29.1%). Shifts/direct assignments
are excluded from those whole-catalog totals. This is a static emission bound,
not measured executed instructions or cycles. Private inventory:
`work/codegen/flags-inventory.json`.

The real flag-only objects confirm that source reductions are selective:

| VA | Host instructions: baseline → flags | CPU loads/stores: flags (proved) | Flag store sites: baseline → flags | Parity folds: baseline → flags |
|---|---:|---:|---:|---:|
| `004F5A70` | 5,320 → 5,113 | 640 / 831 | 102 → 72 | 18 → 12 |
| `00428540` | 1,397 → 1,397 | 255 / 200 | 6 → 6 | 1 → 1 |
| `0051DD60` | 435 → 371 | 26 / 66 | 34 → 22 | 6 → 3 |
| `00841BA0` | 911 → 778 | 70 / 129 | 59 → 38 | 10 → 4 |
| `004F2390` | 961 → 961 | 121 / 182 | 18 → 18 | 3 → 3 |

The two unchanged numerical leaves illustrate GCC's existing elimination.
The nonleaf transform, search and decoder shrink by 688, 224 and 384 bytes,
respectively (function extents including alignment). x87 helper calls stay
inlined; this pass does not remove x87 environment/check/tag work. Private
counterpart: `work/codegen/flags-{facts,object-facts}.json` and `flags-<VA>.asm`.

## Earlier register-local failure

The preserved prototype was re-executed on the original `007BFD40` RNG body in
an isolated fixture. Its register state matched the original at every guest
instruction, yet its LeaveCriticalSection tail dispatch received a stale stack
argument. The prototype formed its code-only view with
`line.split("/*", 1)[0]`. The donor emits a **leading** `/* indirect jmp */`
comment before `dispatch_jmp(c, ...)`. That removed the real dispatch from the
boundary classifier, so it spilled locals only at the subsequent `return`,
after the import had already read the CPU.

The unmodified prototype fails this fixture at `007BFEC4`. Removing complete
comments while preserving following code makes all 99 seeds pass complete CPU
and RNG-object comparison, including instruction-by-instruction GPR checks.
Evidence: `work/codegen/probe-rng{,-fixed}.log`, generated fixture
`work/codegen/rng.c`. This failure does not require a critical section crossing
two mappings; the span repair does not address this synchronization miss.
The flag parser uses the corrected comment handling as well.

## Validation and measurements

The flag step passes 59 unit tests (including three benchmark regressions), including all-mask differential
helper execution (16,859,136 operation cases), CFG joins/backedges, sparse
entry guards, explicit flag observers, leading-comment tail dispatches,
generated direct calls, and generated register switches. The existing MMX,
audio, Bink, PUSHAD/POPAD and x87 state oracles pass in the same suite.
Private output: `work/codegen/flags-unit-final.log`.

Python's hardened `tempfile` directory permissions are incompatible with this
Windows sandbox's token. The private test launcher changes only creation mode
for temporary directories beneath this worktree's `work/` to inherit its ACLs;
it does not skip tests. The initial run also caught/fixed generated-helper
`-Wmisleading-indentation` failures by bracing each conditional store.

The first Ninja compiler probe stalled. Only this worktree's probe was stopped;
the partial CMake directory was moved to `work/codegen/baseline-probe` and the
same standard build script restarted with `-Generator 'MinGW Makefiles'`.
The reference FFmpeg native archives/source disappeared after configuration.
Command-line networking was unavailable. The GitHub connector recovered 484
source blobs at the bootstrap's pinned official FFmpeg commit
`9047fa1b084f76b1b4d065af2d743df1b40dfb56`; each local Git blob SHA-1 was checked
against that tree. The existing scalar bootstrap compiled 191 sources into
private archives under `work/ffmpeg-recovery/`. Its private launcher adds only
the disabled `HAVE_NEON` and `HAVE_LASX` names needed by macro concatenation in
the partial source set. No reference directory was written. Manifest,
verification/bootstrap driver and log remain private. DXVK's probe DLL is
copied into private `work/`. Updating the CMake include prefix required a clean
recompile; the five object inventories remained identical.

The successful baseline standard build completed with 0.97 seconds of input
preparation and **1,292.35 seconds** of configure/compile/link. The earlier
failed dependency build is excluded from that time. Baseline stripped PE size
is **254,480,143 bytes**, with **244,335,512 bytes** in `.text`, SHA-256
`3597666ff57e43715098cb6328dcb3d1d8232580d5249a0ec71b0b634e5ff285`.
The unstripped replay hash is
`a56a60a7bd244487bd6a5c1899757f4fe8cde3b571663f7941b9bd0947f680c2`.

One 75-second baseline replay passed through capture Present 3000 with no
guest stop. That capture visibly contains the character, tutorial camera UI,
world and NPCs. All three native verifier budgets completed 1,000 full CPU and
write-set comparisons. The standalone native-leaf oracle also passed. Evidence
is `work/codegen/baseline/run-01/` and `baseline-native-oracle.log`. The replay
ended at the harness deadline, not by natural guest exit.

The saved register-local prototype was also re-run against the **actual repaired
CS import adapter**, not just a stub. At `007BFEC4`, the old pass supplied ESP
`000107F8`, reading argument zero. Corrected output supplied ESP `00010800` and
the valid CS `00030000`. The old pass gets the real unmapped-CS diagnostic;
corrected output returns normally. Evidence:
`work/codegen/probe-rng-adapter.log`, fixture/driver `rng-adapter.c` and
`probe_rng_adapter.py`. This confirms an eight-byte synchronization miss before
LeaveCriticalSection, independently of the adjacent-mapping repair.

The initial nominal timing cohort is excluded: the default ten-second profiler
flush interval left only 53.819 seconds of retained frames in its first run.
Evidence is retained as `work/codegen/bench-flags-incomplete`. The repaired
harness freezes CPU/GPU accounting at the 60-second deadline, then allows one
flush interval plus two seconds before stopping the game. Frame statistics
still select only the requested window. The reporter rejects incomplete sample
horizons, dropped frames and guest stops. Three synthetic regressions cover
truncation, post-deadline flushes, stops and dropped frames. Read-only Git diffs
also suppress line-ending warnings so PowerShell does not treat them as errors.

The corrected harness starts measurement at **Present 2048**, rather than the
historical 1628 boundary in the earlier hotspot report. All six corrected runs
pass the complete-window, scene-proof, no-stop and no-drop audit. Minimum
first-to-last measured frame span is 59.958230 seconds; minimum retained sample
horizon is 67.381200 seconds. Images at the measurement boundary show the
tutorial character/world. Five separate 75-second flag replays passed through
captures at Presents 3000, 3200, 3200, 2600 and 3000, with no guest stop and all
15 native-verifier budgets complete. The contact sheet agrees with the baseline
scene; NPC/animation/tutorial timing varies between starts. Each replay retained
its first gameplay and final image, with a hash inventory for pruned captures.

The 200,000-case partial-alias native-leaf oracle, 10,000-case portable oracle,
163-assertion CS/native-Windows contract, 73-assertion scheduler contract,
Win32's 280 calls/1,427 assertions, file/profile contracts, host and ARM64 syntax,
and import audit pass. Two pre-existing Win32 scaffolding assumptions needed
updating for the current line-index table and import-handler chain; runtime
behavior was unchanged. Android hardware execution is not claimed.

### Baseline versus flags: corrected interleaved result

Three A/B pairs, each with a 60-second measurement window; native overrides
retain their default enabled configuration and verification/sampling are off.
Both immutable debug-stripped executables are named `ac1_startup.exe` in separate
private directories. Host: Ryzen 9 7950X3D, 32 logical CPUs, 64 GiB RAM. Other
work shares the host; `-AllowBusy` is recorded. Medians [minimum, maximum]:

| Variant | FPS | p95 ms | p99 ms | CPU cores |
|---|---:|---:|---:|---:|
| Baseline | 24.97 [24.88, 25.49] | 46.97 [46.27, 47.42] | 50.53 [48.98, 50.54] | 1.083 [1.083, 1.100] |
| Flags CFG | 26.96 [25.67, 27.01] | 43.45 [43.08, 44.40] | 47.40 [46.33, 48.18] | 1.100 [1.083, 1.100] |

| Pair | Baseline FPS | Flags FPS | Paired change |
|---|---:|---:|---:|
| 1 | 24.88 | 25.67 | +3.15% |
| 2 | 24.97 | 26.96 | +7.97% |
| 3 | 25.49 | 27.01 | +5.97% |

Median paired change: **+1.52 FPS / +5.97%**. Paired p95/p99 changes are
−3.19/−2.35 ms; paired CPU-core change is zero. This is evidence for this replay
and shared host, not a cycles-per-frame measurement or another level's result.
Do not substitute the ratio of independently calculated FPS medians for the
paired statistic. Raw results/audit:
`work/codegen/bench-flags/{result.json,A,B}` and `bench-flags-audit.json`.

| Variant | Stripped PE bytes | `.text` bytes | Configure/compile/link seconds |
|---|---:|---:|---:|
| Baseline | 254,480,143 | 244,335,512 | 1,292.35 |
| Flags CFG | 249,464,192 | 239,521,720 | 1,259.76 |

Flags reduce `.text` by 1.97%. Compile times are observed wall times, with the
same ten-job build and shared-host limitations; they are not paired timing
claims. Flags input preparation was 437.59 seconds, including 160.553 seconds
discovery and 251.947 seconds emission. Both variants emitted the same 101,995
functions with no discovery/lift failures and the same two known instruction
TODOs. Flags stripped SHA-256:
`86d6a9ab1a14b24b0fffb4de97f6a7941d596d3e7878b8528be034a4e72c0d12`.

Production LREGS remains the next separately measured step.
