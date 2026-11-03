# Code-generation lab: measuring the lifter as a compiler

Full-game A/B runs on the phone are too slow and too noisy to judge single
lifter changes: thermal limits moved the prime core between 1.6 and 2.6 GHz
within one series, the GPU between 366 and 457 MHz, and frame work includes a
variable amount of waiting (occlusion-query polling, presents). A 5-8% change
in translated-code cycles was visible only through sampling, and FPS showed
nothing usable. This lab evaluates code generation on the host and with
isolated phone benchmarks first; gameplay runs become smoke tests and
milestone checks.

Everything the lab produces from the game (generated C, objects, images,
inputs, reports) stays under ignored `work/codegen_lab/`. The corpus lists
guest addresses only.

## Stages

| Stage | Question | Where | Cost |
|---|---|---|---|
| 1. Machine code | What does Clang emit, and what blocks it? | host | seconds-minutes |
| 2. Correctness | Same observable behaviour as the original x86? | host | minutes |
| 3. Isolated cycles | What does it cost on the Cortex-X4? | phone, no game install | ~1 minute |
| 4. Gameplay | Does the game still run; what matters next? | phone | 5-30 minutes |

### 1. Machine code (host)

For each corpus function and code-generation variant, compile the function's
generated translation unit with the Android build's exact flags (NDK Clang,
ARM64) and with the same Clang for i686 Windows, then record:

- instructions, guest/CPU-struct/host-stack loads and stores, calls, branches;
- block-local redundancy that survived: repeated loads, missed store-to-load
  forwards, dead stores, with what sits between the pair;
- LLVM's own explanation: GVN `LoadClobbered` remarks
  (`-fsave-optimization-record`), grouped by the clobbering construct
  (guest call, asm fence, x87 TOP store, accessor, ...).

Each function's numbers are weighted by its share of main-thread cycles in a
pinned gameplay profile, so a change is scored by the time it can affect, not
by statement counts.

### 2. Correctness (host)

- Round trip on x86: the original function's bytes run in a 32-bit process
  from a copy at arena+VA, with absolute data operands patched (Capstone);
  the lifted C runs on the same arena as its direct-memory base. Both read the
  same constants and write the same buffers. Today outputs and return values
  are compared; the next step is a thunk that loads all registers, flags and
  XMM from a CPU struct before the original call and stores them after it,
  with ESP on an arena stack, so the comparison covers the full CPU state and
  every written byte including scratch below ESP.
- Native leaves: `tests/native_leaf_oracle.py` compares native and translated
  bodies byte for byte (CPU and write set), with and without EIP tracing.
- Regression corpus: a second, broader function set (one per code shape:
  x87, MMX, string ops, SEH frames, switches, indirect calls) compiled and
  checked by the existing unit tests, so a change aimed at hot SSE code cannot
  silently break unrelated shapes.

### 3. Isolated cycles (phone)

The corpus leaves are compiled per variant into one small static ARM64
benchmark binary (no game, no APK), pushed with `adb` and run on the prime
core under `simpleperf stat -e instructions:u,cpu-cycles:u,raw-stall-backend:u`.
Per call it reports executed instructions, cycles, backend stalls and IPC;
repeated runs agree within 0.1%. A benchmark-only call measures harness cost,
which is subtracted. Inputs start synthetic; the plan is recorded inputs:
the runtime's existing bound-function hook can dump a call's CPU state and
its declared input ranges (as the native-leaf plans already know them).

### 4. Gameplay (phone)

- Smoke: one city replay (`city_run.sh`) after a significant change; pass is
  no guest stop, the replay reaching its last tap, and screenshots.
- Milestone: interleaved A/B of a build combining several validated changes,
  scored by translated-code work per frame from samples of the retired
  instructions event (clock- and wait-independent), with cycles and FPS
  reported alongside, never as the deciding number.
- Profiling (crowd and city scenes) chooses what to work on: which overhead
  categories carry time, and the weights stage 1 uses.

## Variants and reproducibility

Each optimisation is an independent switch:

| Kind | Examples | Rebuild needed |
|---|---|---|
| Lifter option | `--xmm-abi`, `--flag-liveness`, `--x87`, `--local-regs` | full generation (15-25 min) |
| Prepared-header macro | `AC1_TRACE_EIP` (EIP observer) | recompile only |
| Runtime environment | `AC1_NATIVE_OVERRIDES`, `AC1_NATIVE_REGIONS` | none |

A snapshot is immutable: the generated units holding the corpus functions,
the prepared headers, the lifter commit and options, and the compiler
version. Header and environment variants reuse one snapshot; a lifter variant
gets a new snapshot from one generation.

## Metrics

Kept separate, because they disagree in practice:

- Static instructions: compiled code, per function, weighted by profile share.
- Executed instructions: `instructions:u` per call (stage 3) or sampled per
  function in gameplay.
- Cycles: `cpu-cycles:u` per call, with backend stalls; the deciding number.

Examples from the EIP-observer change: 0x428540 executed 5% fewer instructions
but took 41% fewer cycles; flag computation removed 16-34% of a function's
x86 instructions and saved no measurable cycles.

Recompilation efficiency, per function and corpus-weighted: original x86
cycles divided by recompiled-x86 cycles from the same lifted C and the same
Clang. It isolates the lifter from x86/ARM64 differences. The original is a
reference, not a target: 0x1176DE0's original stalls on a store-forwarding
pattern that the recompiled code can avoid. ARM64 has no original, so it is
judged by cycles per call, and by the native leaves where one exists.

## Report

One markdown and JSON file per run, comparing a variant with its baseline:

- corpus-weighted static instructions, memory operations, surviving
  redundancies and remark blockers, with the largest per-function changes;
- x86 round trip: correctness counts and efficiency per function;
- ARM64: instructions, cycles and stalls per call, with regressions above a
  threshold listed first;
- the variant's switches, snapshot identity and tool versions.

## Running it

`tools/codegen_lab.py` implements stages 1-3 (`config/codegen_lab.json`
lists the leaves, the hot-corpus size, variants and benchmark settings).
Results go to `work/codegen_lab/{profiles,snapshots,builds,runs,reports}`;
each snapshot, build and run directory is written once, with a manifest of
hashes, flags, compiler versions and switches, and raw tool output is kept.

```powershell
$py = 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe'
# Exclusive main-thread cycles per guest function from a sampled profile.
& $py -B tools/codegen_lab.py profile crowd2 --perf work/phone/live/crowd2/flat.data --symbols work/phone/sym-prof1.so
# Inputs from the current generation: the hot corpus's units, prepared
# runtime headers, the original image and its patched leaf operands.
& $py -B tools/codegen_lab.py snapshot c33daae --profile crowd2
# Compile a variant for ARM64 and i686 with the Android build's exact flags
# (taken from its compile_commands.json); --remarks adds LLVM GVN remarks.
& $py -B tools/codegen_lab.py build c33daae base --remarks
& $py -B tools/codegen_lab.py build c33daae eip_observer --remarks
& $py -B tools/codegen_lab.py static c33daae/base
# Gates first (ARM64: lifted vs exact native leaf, whole CPU struct, output
# and stack scratch; x86: original vs lifted outputs), then counters/timing.
& $py -B tools/codegen_lab.py bench c33daae/base a1
& $py -B tools/codegen_lab.py bench c33daae/eip_observer a1
& $py -B tools/codegen_lab.py bench c33daae/base a2              # A/A
& $py -B tools/codegen_lab.py compare c33daae__eip_observer__a1 c33daae__base__a1 --aa c33daae__base__a2
```

Details that matter for trusting a number:

- Weights are exclusive (flat-report) samples, so inclusive time is never
  counted twice; a function's `L_` wrapper and translated body are summed.
- Benchmarks run the unit build's own code. ARM64 cuts each leaf's function
  section, the unit's read-only data and their relocations out of the unit
  object (`llvm-objcopy`); x86 links the whole unit object with the leaf
  renamed and traps for every symbol nobody defines (GNU `objcopy` corrupts
  COFF relocation symbols when sections are dropped).
- The phone run asks the PMU which of `instructions:u`, `cpu-cycles:u`,
  `raw-stall-backend:u`, `raw-stall-backend-mem:u` it can count before using
  them, refuses to run while the game is in the foreground, records a
  background game process, and marks the run if the game reached the
  foreground before it ended. Nothing is installed: one binary is pushed to
  `/data/local/tmp/codegen_lab/<hash>/`.
- Every metric is net of a harness-only call measured the same way. Phone
  counters and timings use the median of the repetitions; host cycles use the
  minimum (a shared PC only adds cycles). Noise is the relative interquartile
  range, raised to the A/A difference when an A/A run is given; a change is a
  verdict only beyond three times the noise and at least 2%.
- `AC1_EIP_OBSERVER` restores the accessor barrier on its own (without EIP
  tracing's stores), so the 5c0308c change stays measurable.

## First results (snapshot c33daae, crowd-scene weights)

The lab reproduces the earlier findings with gates passing (20,000 calls per
leaf on each architecture):

| | without observer (as shipped) | with observer | |
|---|---|---|---|
| ARM64 static instructions, weighted | 243.8 | 254.6 | -4.2% |
| ARM64 missed store-to-load forwards, weighted | 2.8 | 5.1 | -44% |
| GVN loads blocked by the observer asm | 0 | 4,503 | |
| x86 0x428540 cycles (25 reps, min) | 35.0 | 61.4 | improved -43% |
| x86 0x1176DE0 cycles | 85.4 | 44.1 | REGRESSED +94% (store forwarding) |
| x86 originals (controls) | 34.9 / 14.7 / 93.4 | 34.7 / 14.6 / 93.9 | unchanged |

Recompilation efficiency on x86 (original/lifted cycles) as shipped:
0x428540 1.00, 0x653400 0.50, 0x1176DE0 1.09.

ARM64, Cortex-X4 prime core, per call net of the harness (runs q1 with A/A
q2, 7 repetitions; gates passed):

| | instructions | cycles | backend stalls |
|---|---|---|---|
| 0x428540 with / without observer | 232 / 230 (-0.9%) | 155 / 110 (-29%) | 102 / 60 (-42%) |
| 0x653400 | 125 / 116 (-7%) | 33 / 30 (within noise) | 18 / 16 (within noise) |
| 0x1176DE0 | 133 / 130 (-3%) | 77 / 35 (-55%) | 60 / 18 (-70%) |
| native leaves (controls) | unchanged | within 1.5% | within noise |

Executed instructions barely move while cycles drop by up to 55%, nearly all
of it backend stalls; 0x1176DE0 moves in opposite directions on the two
architectures (-55% ARM64, +94% x86). The exact native 0x428540 executes 177
instructions in 27 cycles (3 stalls) against the lifted 230 in 110: the
remaining gap is stalls, not instruction count. The A/A pair also shows why
calibration is mandatory: wall-clock time of identical builds differed by
4-21% from clock changes while cycles and instructions agreed within a few
percent, so on the phone cycles and instructions decide and nanoseconds are
context.

What the hot corpus says to do next (weighted static mix of the shipped
build, and LLVM's reasons for loads it kept):

1. Call boundaries. CPU-struct loads and stores are 24% of the instructions;
   guest calls clobber 60% of the loads GVN could not remove (the callee is
   opaque, and the syncs around it). XMM contracts (`--xmm-abi on`) are the
   first piece; general-register preservation and x87 TOP are next.
2. Branch fences. The `""::: "memory"` on every `goto` blocks 11% of the kept
   loads once the observer is gone; fencing only backward branches keeps the
   polling guarantee.
3. 32-bit address arithmetic: 14% of the instructions are `add/sub w` forming
   guest addresses. 64-bit addressing alone helped 0x653400 and hurt 0x428540
   (narrow loads from wide stores) in the earlier study, so it goes with
   stack-slot forwarding, measured here before any gameplay run.

Lifter variants need one generation each (a new snapshot); header and
runtime switches reuse a snapshot. Still to come: recorded real-game inputs,
a full-state x86 thunk (all registers, flags, XMM and scratch), benchmarks
for non-leaf hot functions, and a corpus-only fast path for lifter variants.

## Cost model of lifted code shapes (phone)

`tools/codegen_lab/microbench_run.py` builds `microbench_arm64.c` with the
game's ARM64 flags, runs every kernel under `simpleperf stat` on the X4
(cpu7) and an A720 (cpu2), and keeps raw counts in
`work/codegen_lab/experiments/microbench/raw.json`. Each kernel repeats one
shape the lifter emits; compiler barriers stand in for the calls and escapes
that keep values in the CPU struct, and the machine code of every kernel was
checked to contain the intended loads and stores. Cycles per iteration with
the empty loop subtracted, median of 5, 20M iterations (spread under 2%
unless noted):

| shape | insns | X4 cycles | A720 cycles |
|---|---|---|---|
| XMM add in a register (reference) | 3 | 10 | 10 |
| XMM 16B store, 16B reload | 2 | 7 | 7 |
| XMM 16B store, 4B lane store, 16B reload | 5 | 23 | 18 |
| four 4B stores, 16B reload | 10 | 14 | 17 |
| 16B store, 4B lane reload | 3 | 9 | 9 |
| call, value in argument registers | 3 | 1 | 1 |
| call, one value through the struct each way | 5 | 5 | 5 |
| call, 4 live values through the struct | 11 | 6 | 6 |
| call, same 4 values in callee-saved registers | 5 | 1 | 1 |
| call, all 8 GPRs synced, callee reads/writes all | 18 | 29 | 37 |
| call, 8 GPR stores only (dead afterwards) | 12 | 3.7 | 2 |
| indirect call through a table (predicted) | 4 | 1 | 1.2 |
| x87 op with TOP and st(i) in the struct | 8 | 10 | 10 |
| x87 op in a register | 1 | 4 | 4 |
| flags stored, reloaded, branched on | 6 | 1.3 | 1.8 |
| flags kept in a register | 3 | 1.3 | 1.3 |

What this says for the lifter:

- A value that crosses a call through the CPU struct costs a store-to-load
  forwarding hop (about 4-5 cycles) on whatever depends on it; in registers
  the call is nearly free. Dead syncs cost only issue slots. GPR
  call-boundary contracts therefore pay where synced registers are live and
  on the critical path, not in proportion to the instructions removed.
- Mixed-width traffic to one slot is the expensive case: a 16B reload after a
  narrower store fails forwarding and costs 7-16 extra cycles (the same shape
  made 0x1176DE0 slower on x86). Lane writes into memory-resident XMM state
  and Clang merging scalar CPU-field reloads into `ldp q` both produce it.
- Materialized flags and predicted indirect calls are cheap; they are not
  worth lifter work for cycles.
- x87 through TOP in memory adds about 6 cycles per dependent operation.

Not yet measured: byte and word partial-register stores followed by a 32-bit
reload (kernels `gpr_byte_chain`, `gpr_byte_merge_chain`, `pair_2x8_v16_chain`
are in the source; the run was stopped when the phone was needed).

Where CPU state is still materialized, measured on the phone, and the resulting
priorities: [cpu-state-assessment.md](cpu-state-assessment.md).
