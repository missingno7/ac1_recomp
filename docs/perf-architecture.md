# Generated-code efficiency: diagnosis, experiments, next step (2026-10-30)

Private evidence (generated C, captures, binaries, benchmark runs) is under
ignored `work/perf/`. The isolated harness reuses the architecture study's
capture of `004F5A70` (packed int16 pose propagation, a 7-function cone,
19,202 real records from gameplay) with the real generated bodies and the
game's compiler flags; every variant is checked against the shipped
translation on all records with two poison fills.

## 1. Reliable comparisons first

The fixed-window harness accepted **0 of 8** A/A pairs: boundary screenshots
are not pixel-deterministic, and a 1% guest-time tolerance rejects ordinary
speed variation (and any real speedup), because the guest clock follows frame
time. CPU ms/frame is quantized to ~0.98 ms. Now:

* equivalence = complete window + identical replay command indices + a gross
  (33%) guest-time bound; screenshots are evidence only;
* the decision uses per-run median main-thread cycles/frame and a Student-t
  CI of the mean paired delta (a bootstrap of three same-sign deltas always
  "excluded zero" in an A/A); mean frame time is reported beside it;
* `AC1_FIXED_FRAME_HZ=30` (harness default) is a timedemo clock:
  GetTickCount, timeGetTime, QPC and GetSystemTimeAsFileTime advance exactly
  1/30 s per Present. Four of six A/A runs then rendered bit-identical frames;
  draw-count CV fell 5.5% -> 0.56%, cycles CV 2.0% -> 1.0%. Binaries of
  different speed still diverge somewhat (real-time scheduler slices and
  loading), so frame time and draw counts are reported with cycles.

## 2. Diagnosis: the largest remaining avoidable cost

Execution is moderately concentrated (architecture study): 66 functions are
50% of main-thread CPU, 328 are 80%; the best three cohesive regions cover
30-46%. Generated code is ~68% of the main thread, which is the critical
path (~97% busy).

Inside generated code the dominant cost is **guest CPU state kept in memory
at instruction granularity**, worst for x87: GPRs, XMM lanes and the x87
register stack live in the CPU struct, and every x87 instruction also paid an
MMX/init check, five FPU-environment stores, tag classification, dirty
bookkeeping and a full GPR spill around each helper call. Evidence:

* `restrict` on the CPU and the arena (alias information alone) removed 17%
  of CPU-struct loads and changed nothing in time: GCC never keeps
  pointer-accessed fields in registers. Only true locals do.
* With GPR/XMM locals the cone's profile was led by x87 machinery (MinGW
  `__fpclassify` 8.2%, `ac1_fp_round` 5.4%, tag/dirty/check helpers ~4%).
* Code size is first order in the game: memory-base shrank `.text` 39% and
  gained 15% in-game against 6% in the isolated cone; native x87 shrank the
  executable another 11%.

## 3. Isolated results (ticks per bone; functionally identical to shipped)

| variant | ticks/bone | vs shipped | x64 instructions (cone) |
|---|---:|---:|---:|
| shipped translation | 477 | 1.00x | 8,312 |
| + captured arena (memory-base) | 447 | 1.07x | 4,700 |
| + `restrict` CPU/arena instead of locals | 452 | 1.05x | 4,587 |
| + GPR/XMM register locals | 383 | 1.24x | 4,628 |
| + x87 builtin classification, chop fast path | 355 | 1.34x | 4,495 |
| + native x87 lowering | **274** | **1.74x** | **3,564** |
| native C reconstruction (architecture study) | 60 | 7.9x | ~150 per bone path |

`-march=x86-64-v2` added 2%, `-v3` nothing. AArch64 (NDK Clang 19): the cone
shrinks 7,750 -> 5,049 -> 4,699 instructions with memory-base and locals.

## 4. Controlled gameplay comparisons (fixed clock, CCD0, lock)

| comparison | valid pairs | main-thread cycles/frame | mean frame time | FPS (median) |
|---|---:|---|---|---|
| main -> memory-base | 3 | **-15.3%** [-22.2%, -8.5%] | | 23.6 -> 27.8 |
| memory-base -> + locals + x87 fast paths | 6 | -1.8% [-8.9%, +5.3%] (bimodal) | **-4.0%** [-7.0%, -1.0%] | 26.8 -> 28.4 |
| -> + native x87 | 4 | **-10.6%** [-20.3%, -0.9%] | **-11.3%** [-17.6%, -4.9%] | 28.2 -> 31.0 |

Together: about **+31% FPS** over main in this window (23.6 -> ~31), p95
frame time 53.6 -> 44.8 ms in the last step. Executable `.text`: 240.7 MB ->
~148 MB with memory-base; stripped 250.7 -> 144.8 MB with everything.

## 5. Native x87 (`build.ps1 -X87 native`, now the default; `-X87 exact` restores)

Within a basic block the x87 depth is static, so `st(i)` is a fixed `double`
local; blocks anchor on `c->fpu_top`, slots load lazily, and dirty slots plus
TOP are written back before branches, labels, calls and returns, so callers,
callees and dispatch still see the architectural stack. Arithmetic is
unchanged (binary64 as before; the existing emulation never applied the
game's chop rounding to arithmetic either). `fist(p)` in chop mode is an
inline C conversion with exact IE/PE flags; other rounding modes take a cold
path. Dropped: per-instruction MMX/init checks, FPU IP/DP/opcode, tag word,
dirty mask and unmasked-exception diagnostics. C0-C3, TOP and the control
word stay exact. Functions that call MMX or FPU save/environment/reset
helpers keep the exact lowering (133 x87 checks of ~46,500 remain).

Breakage evidence: the cone is functionally identical on all real records; a
lifted fixture with an x87-unbalanced branch join matches the exact lowering
in 4,096 random cases; eight A/B runs and a run to Present 5000 (menus,
loading, the tutorial crowd, Animus transitions, the Abstergo lab) show no
guest stop or visual breakage. Not yet covered: Bink/MMX video paths beyond
the hidden intros, long gameplay, and AArch64 hardware.

## 6. Better lifting or native reconstruction?

Both, in this order. Automated lifting improved the representative math
kernel 1.74x and the whole game ~31% in one session, applies to all ~102,000
functions and to AArch64, and each step was a small generator change. Native
reconstruction reaches 7.9x on the same kernel, but no small set of
subsystems dominates (top three regions 30-46%), and every region costs days
of contract design and verification. The remaining gap on math kernels
(274 vs 60 ticks/bone) is mostly the original program's own guest-stack
traffic (73% of the cone's memory accesses are esp/ebp-relative), lane-wise
SSE emulation, flags materialized at public exits and call-boundary state
synchronization.

## 7. Recommendation

1. Keep native x87 as the default and harden it (Bink/MMX videos, longer
   replays, AArch64 on the phone); fix lowering bugs rather than retreating.
2. Next lifter step: promote proven-private guest stack slots to C locals
   (the remaining guest-memory traffic) and keep x87 slots in locals across
   basic blocks within a function; both reuse the CFG machinery of the
   register-locals pass.
3. Measure code-size levers explicitly: per-instruction EIP stores and inline
   call accounting are diagnostics in the hot path, and code size has proven
   worth more in-game than isolated timings suggest.
4. Native reconstruction for kernels that remain >5x off after (2), starting
   with pose propagation (prototype exists, 7.35x verified in game).
