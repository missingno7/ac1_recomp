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
* the decision uses per-run main-thread cycles/frame and a Student-t CI of
  the mean paired delta (a bootstrap of three same-sign deltas always
  "excluded zero" in an A/A); mean frame time is reported beside it. The
  per-run statistic is the window mean since section 7 (the median can land
  in different modes of the multimodal frame cost);
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

(Cycles in this table are per-run medians, the decision statistic at the
time; section 7 re-evaluates them with window means: same conclusions.)

Direct comparison, previous main `b4f4f8f` against the new default build
(3 pairs): main-thread cycles/frame **-27.7%** [-41.4%, -14.0%], mean frame
time **-28.0%** [-31.1%, -24.9%], **23.3 -> 32.9 FPS (+41.5%)**, p95 frame
time ~61 -> ~48 ms. Executable `.text`: 240.7 MB -> ~148 MB with
memory-base; stripped 250.7 -> 144.8 MB with everything.

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
after this step (274 vs 60 ticks/bone) was diagnosed in section 7: most of
it was the representation of XMM registers, not the guest stack.

## 7. Second step: XMM values in host vector registers

The stack-slot hypothesis (73% of the cone's memory accesses are
esp/ebp-relative) was checked against the assembly before building
anything. On the first-step default (275 ticks/bone) the cone's profile was
led by packed-SSE lines (addps 15%, mulps 10%, pslldq/psrldq 10%, sqrtps 4%),
not by guest-stack accesses.

**Cause 1: the XMM union of arrays.** GCC keeps an XMM local in a host
register only while every access to it is vector-typed. The donor writes a
packed op as four lane statements, `psrldq`/`pslldq` as 16-iteration byte
loops, and scalar ops as in-place lane writes (`x.f64[0] = ...`). Any of
these sends the local to the host stack, where lane stores followed by a
16-byte reload fail store forwarding. The quaternion multiply `0042EE70`
executed ~350 instructions for ~25 instructions of SSE work. Clang handles
the lane form better (34 vs 14 AArch64 instructions in a test) but still
pays for it.

**Cause 2: EIP stores as alias barriers.** Every guest instruction stores
its EIP through the CPU pointer, which under `-fno-strict-aliasing` may
alias guest memory. GCC therefore cannot forward a guest-stack store to its
reload in the next instruction (`movss [esp+58h],xmm1; fld [esp+58h]`,
`fistp [esp+2Ch]; movzx eax,word [esp+2Ch]`, the original compiler's own
spill/reload pairs); without the store it forwards them itself. `restrict`
on the CPU pointer changes nothing: the pointer is passed to every callee,
so GCC treats the CPU struct as escaped memory. (NFSU2 Web solved the same
barrier by making guest memory a named object; AC1's arena is allocated at
run time, so that is not available on PE.)

Change (`tools/lift_ac1.py`, `tools/prepare_runtime.py`):

* XMM has vector members (`vf32`, `vf64`, `vu32`, ...; `aligned(8)`, so the
  CPU layout is unchanged). Packed arithmetic, sqrt/min/max, bitwise ops,
  shuffles, unpacks, `movhlps`/`movlhps`, `pslldq`/`psrldq`, packed compares,
  conversions and packed-integer add/sub/compare/shift/unpack are single
  vector expressions. Every other lane access goes through the vector
  members, and a lane write is a whole-vector assignment
  (`x.vf64 = ac1_lane_vf64(x.vf64, 0, r)`).
* Instructions whose only memory accesses are ESP- or frame-EBP-relative
  (push/pop included) no longer record their EIP. Heap accesses, transfers,
  `div`, function entries and the instruction after each call still do.
  EIP stores fall from 3.36M to 2.42M. The fault handler now also prints the
  host PC, which the build's symbols resolve to the exact generated line.

Correctness: the cone is byte-identical to the first-step default on all
19,202 records and both poison fills. A differential test
(`tests/test_sse_vector_lowering.py`) runs 769 instruction forms x 4,096
random and special-value states against the donor's lane lowering and
compares the whole CPU. The only differences are NaN payloads in lanes of
commutative ops where both inputs are NaN: C lets the compiler commute `+`
and `*`, so neither lowering fixes the x86 rule (destination NaN wins). On
100,000 such states the vector form matched the hardware and the lane form
did not. All lowered forms also compile for AArch64 (NDK Clang 19). 108
tests and the lifter self-test pass.

Isolated cone (ticks per bone, mean of three rounds of per-record minima):

| variant | ticks/bone | vs first step | x64 instructions | host-stack refs | AArch64 instructions |
|---|---:|---:|---:|---:|---:|
| first-step default | 275.3 | 1.00x | 3,564 | 307 | 3,228 |
| + vector ops, in-place lane writes, stack-only EIP | 245.2 | 1.12x | 2,935 | 153 | 2,672 |
| + whole-vector lane writes, every EIP kept | 177.2 | 1.55x | 3,060 | 100 | |
| **+ stack-only EIP rule (new default)** | **159.6** | **1.72x** | **2,899** | **96** | **2,672** |
| bound: no EIP stores at all | 140.1 | 1.96x | 2,753 | 113 | |
| native C reconstruction | 60 | 4.6x | | | |

Against the shipped translation (478) the cone is now 3.0x faster. Game
build: `.text` 134.9 -> 127.3 MB (-5.7%), stripped executable 144.8 ->
137.4 MB, generated C 630 -> 587 MB.

Controlled gameplay comparison (first-step default `6a9ef73` against this
step, eight interleaved pairs, fixed clock, CCD0, lock): main-thread
cycles/frame **-5.0%** [-6.9%, -3.1%], mean frame time **-5.0%** [-6.9%,
-3.1%], p95 frame time -9.1% [-14.9%, -3.3%], 32.4 -> 34.3 FPS (+5.3%
[+3.2%, +7.5%]); B was faster in all eight pairs. No guest stop in the 16
runs, and a run to Present 5062 (tutorial crowd, Animus loading) shows no
stop or visual breakage; with the intro videos visible, Bink decodes clean
frames to Present 1150. The kernel's 1.72x is ~5% of the game because
SSE-heavy code was ~22% of main-thread CPU in the architecture study, and a
quarter of the SSE instructions sit in functions without XMM locals (item 3
below).

This comparison exposed a harness defect: the adaptive decision used the
per-run *median* of per-frame cycles. Per-frame cost here is multimodal
(short and long frames alternate), and the median landed in different modes
on the two sides: it reported +12.5% [+2.1%, +23.0%] while the window mean
and the frame time both fell 5.0% in every pair (the presenting thread is
100% busy, ~4.03 GHz of cycles per wall millisecond on both sides). The
harness now decides on the window mean (total cycles / frames).
Re-evaluated with it, the earlier comparisons keep their conclusions with
tighter intervals: memory-base -14.1% [-19.1%, -9.1%], locals -3.5% [-6.4%,
-0.5%] (previously inconclusive), native x87 -10.4% [-15.9%, -4.9%], old main
to the first-step default -27.5% [-29.0%, -25.9%].

What is left in the cone (exploratory profile): guest memory accessors
(stack and heap) ~25%, `cvtsi2sd` merging into the previous register value
~12% (a false dependency the original code has too), flags materialized
only for function exits ~10% (`parity8` alone 4.5%), the x87 integer store
~9%, calls and returns ~4%.

## 8. Third step: guest stack traffic

Stack-slot promotion was the planned next step. Scope first: weighting the
architecture study's main-thread samples by function, 47% of main-thread CPU
is in functions whose frame never escapes (no `lea r,[esp/ebp+k]`, no ESP
copies), 32% in functions that pass frame addresses to callees (the pose
kernel among them), 12% in functions without register locals (`fs:`
exception frames). The kernel's own profile then showed three concrete
stack costs, each removable without a frame model:

* **push/pop through `c->esp`.** The register-locals pass kept ESP in a
  local but routed every push and pop through the CPU struct: store the
  local, load, adjust and store `c->esp`, store the value, reload the local,
  a memory round trip on the ESP dependency chain of each of ~1M pushes and
  ~146k pops in the generated code. The pass now lowers the common forms to
  arithmetic on the local ESP plus one guest store or load (the pushed value
  is read first; `pop esp` takes the loaded value); other forms keep the
  helpers.
* **EIP stores as alias barriers between stack accesses.** With stack-only
  EIP stores gone, the remaining store before every heap access still
  stopped GCC forwarding stack values across it. EIP is now recorded at
  function entries, control transfers, division and after calls (EIP stores
  2.42M -> 0.97M; 214 -> 24 in the kernel's main function). Crash reports
  carry the host PC for exact attribution. Recording it at conditional
  branches too measured ~2% faster in the kernel than omitting them, with
  identical C apart from those stores (scheduling or layout), and keeps the
  rule "every control transfer".
* **Vectors built through the stack.** MSVC builds `__m128` values with
  4-byte stores (`movss`, `mov dword`) and then one 16-byte load. A CPU cannot
  forward several narrow stores into one wide load, so the load waits for the
  stores to commit; these stalls were the hottest remaining samples (8.4% on
  the instruction after one such load, 5.5% after another). Within a block,
  with ESP followed through push/pop/add/sub, the lifter now assembles such a
  load from the stored lane values (`StackLaneForwarding`); unknown lanes are
  4-byte loads, which forward. The stores still happen; any memory write it
  cannot place, a call, a label or an ESP change it cannot follow ends the
  tracking. Static census: 5,137 such loads in 2,689 functions (3,870 with
  all four lanes just written), plus 733 8-byte loads after 4-byte stores
  (not handled yet).

Correctness: the cone is byte-identical to the default on all 19,202
records and both poison fills after each step. New tests compare the
push/pop lowering and the forwarding with the plain forms on 4,096 random
states each (whole CPU and stack memory), including `push esp`, `pop esp`,
`push [esp+4]`, a heap store that hits a tracked slot, an overlapping
narrow store, a misaligned wide load and a packed op with a memory source.
110 tests and the lifter self-test pass.

| step (cumulative) | ticks/bone | vs first-step default | x64 instructions | host-stack refs | AArch64 instructions |
|---|---:|---:|---:|---:|---:|
| second-step default (`0d57d16`) | 159.2 | 1.73x | 2,899 | 96 | 2,672 |
| + push/pop on the local ESP | 152.6 | 1.80x | 2,825 | 102 | 2,580 |
| + EIP only at transfers | 135.7 | 2.03x | 2,718 | 127 | |
| **+ stack lane forwarding** | **117.8** | **2.34x** | **2,714** | **102** | **2,277** |
| native C reconstruction | 60 | 4.6x | | | |

Against the shipped translation (478) the cone is 4.1x faster; the native
version is now 2.0x away. Game build: 5,060 forwarded loads, `.text` 127.3
-> 120.9 MB (-5.0%), stripped executable 137.4 -> 130.6 MB.

Controlled gameplay comparison (second-step default `0d57d16` against the
three stack steps, window-mean statistic, fixed clock, CCD0, lock; the
interval excluded zero after three pairs): main-thread cycles/frame
**-4.7%** [-8.7%, -0.7%], mean frame time **-4.8%** [-8.6%, -1.0%], 33.7 ->
35.1 FPS (+5.0% [+0.8%, +9.3%]); B was faster in every pair. No guest stop
or memory fault in the six runs; a run to Present 5010 and the intro videos
show no stop or visual breakage.

What is left in the cone (exploratory profile, harness excluded): x87
integer stores ~15%, guest memory accessors ~23%, flags kept alive only for
function exits ~15% (`parity8` 6.5%), calls ~8%.

## 9. Fourth step: flags across calls

**Choice.** The cone's remaining costs (x87 integer stores ~15%, flags kept
for function exits ~15%, calls ~8%) are one kernel's mix. Game-wide, on the
main thread of the `e634876` profile (Presents 1800 + 15 s), flag computation
was the largest avoidable cost: 21.5% (`parity8` 8.7%, masked helpers 10.5%,
unmasked helpers 2.3%) against 1.5% for x87 integer stores and 8.6% + 3.0% for
calls and the return checkpoint. Per-function liveness kept every flag alive at
each call and each return, and the 13,472 functions with `fs:` exception frames
stayed fully unmasked: 325,865 of ~461k flag computations produced all six
flags (~2.0M live lanes).

**Design** (`-FlagLiveness abi`, now the default; `cfg` restores the per-function
masks). One body per function, no copies. `E(F)` is what F reads at entry
before defining it, `R(F)` what callers read after F returns. A direct call
reads `E(callee)` and then defines every flag; the demand after it (for a tail
call, the caller's own `R`) joins `R(callee)`; a return reads `R(F)`. The fixed
point over all 101,994 bodies starts from zero and only grows.

* Indirect calls and imports (host code may call back) read the entry demand
  of every possible target, and the demand after them joins `R` of every
  possible target; indirect jumps likewise. Possible targets are entries whose
  address the image stores (data sections at any offset, code sections aligned:
  jump tables) or that lifted code uses as a value, excluding a call's own
  operand and pushed return address: 67,612 entries. Indirect calls never enter
  another function's interior (486 such entries are indirect-jump targets only).
* Imports and native bindings return with the incoming flags (host code never
  writes them), so demand after them also reads through them. Native entries
  and region oracles keep the per-function masks.
* Logical operations clear AF. x86 leaves AF undefined after AND/OR/XOR/TEST;
  the donor helper preserved it, which carried AF demand backwards through
  every `test` (from the CRT crash reporter's `pushfd` into every indirect
  target). The prepared helper now clears it, as QEMU and Bochs do.
* Shifts and rotates with a constant nonzero count define their flags.
* A diagnostic stop (`int3`, `RECOMP_TODO`, reserved MXCSR bits) never returns
  to guest code: it observes nothing and ends its path. Every exact x87
  instruction begins with `if (!ac1_x87_check(c, va)) return;`; a failing check
  stops the guest, so that return is dead and `fcomi`/`fucomi` (1,555 sites)
  define flags unconditionally. The first game lift modeled both as
  observers: `0x00404040`, an address-taken entry that is a single `int3`, and
  `fucomi; lahf` at a function entry gave every indirect target an all-flag
  demand (756k live lanes instead of 236k).
* Exception-frame functions get contracts too: the runtime never dispatches
  guest SEH (`RaiseException` accepts only the thread-name protocol; a memory
  fault stops with a diagnostic), so no handler observes a flag.
* Any other helper given the CPU, `lahf`/`pushfd` and unknown goto targets
  observe every flag. Tests audit the helper tables against the prepared
  header: transparent helpers transitively never name a flag field, and each
  declared write is a function of the inputs, never of the prior flag (2,442
  compiled cases).

One vtable-scan seed, `0x00ABABCD`, is the word after a five-slot vtable and
points at a `je` inside `0x00ABAA40`; as an indirect-jump target its ZF entry
demand reached every indirect transfer. It is removed from `startup_seeds`
(a dispatch there would stop with the unknown-target diagnostic).

**Result of the analysis** (game build, lift report `flag_abi`): no flag
crosses an indirect call or jump in either direction. Nine CRT x87 helpers read
ZF at entry and two `_fload_withFB` variants return it, all through direct
calls. Of 453,272 flag computations 250,376 (55%) are dead, 155,947 (34%) keep
only ZF, 40 keep all six: 235,861 live lanes (-88%). The native bindings need
no flags either way. `.text` 126.9 -> 105.2 MB (-17%), stripped executable
130.6 -> 105.2 MB.

| cone | ticks/bone | x64 instructions | stores | AArch64 instructions |
|---|---:|---:|---:|---:|
| per-function masks (third-step default) | 116-121 | 2,706 | 507 | 2,272 |
| **flag contracts** | **98.4-98.8** | **2,145** | **427** | **1,969** |

The cone's 1,950,603 output bones are byte-identical under both fills. A
compiled whole-program differential (per-function masks, contracts, contracts
with local registers; direct and dispatch paths, 4,096 random states) compares
registers, memory, call accounting and the flags the program observes;
mutations that drop tail-call or import demand fail it. 118 tests and the lifter
self-test pass.

**Controlled gameplay comparison** (`e634876` against the contract build, fixed
clock, CCD0, lock; every pair workload-equivalent, B faster in every pair):

| window | pairs | main-thread cycles/frame | mean frame time | FPS |
|---|---:|---:|---:|---:|
| 1800-2200 (standard) | 3 | **-3.7%** [-5.3%, -2.2%] | -4.5% [-5.9%, -3.1%] | 35.6 -> 37.2 |
| 2400-2800 (after the scene change) | 5 | **-10.7%** [-18.4%, -2.9%] | -10.6% [-18.3%, -2.8%] | 56.7 -> 63.3 |

The standard window understates the gain: the scene change near Present 2040
(cost ~130M -> ~65M cycles/frame, with a white flash) follows real time, so the
faster build reaches it ~110-160 Presents later and keeps more heavy frames in
the window (before the change, Presents 1800-2000: -19.9% [-24.2%, -15.5%]; see
[benchmarking.md](benchmarking.md)). A run to Present 5010 and the intro
videos show no stop or visual breakage.

## 10. Recommendation

Re-profile the contract build before choosing; the order below is from the
`e634876` profile with the flag share removed.

1. **Call boundaries** (Pinyon Shift's ABI sync: -11-12% guest
   instructions; here call/ret 8.6%, return checkpoint 3.0%, dispatch 3.5%).
   A guest call is ~70 host instructions: accounting, an out-of-line scheduler
   checkpoint after every return, full GPR/XMM syncs. An inline countdown
   before the checkpoint is a small runtime change; skipping callee-saved
   reloads needs an exception list (CRT EH/SEH prologs, `_chkstk`, `longjmp`)
   or a preservation proof.
2. **XMM and GPR locals in exception-frame functions** (12% of main-thread
   CPU, 25% of SSE instructions). The argument used for flags applies: the
   runtime never dispatches guest SEH or C++ exceptions, so no handler reads a
   register at a fault.
3. **x87 integer stores.** Every `fist(p)` reloads the control word for the
   rounding mode, range-checks, converts, re-converts to test exactness and
   read-modify-writes `c->fpu_sw` for PE (~15% of the cone, 1.5% of the
   game's main thread). Keep RC and the sticky status bits in locals synced at
   the existing TOP sync points.
4. **Frame-slot promotion proper** (store elimination, values across calls)
   for the 47% of main-thread CPU in private-frame functions, plus the 733
   8-byte loads after 4-byte stores. Forwarding already removed the stalls;
   what remains is the stores themselves and reloads across calls.
5. **Masked shifts.** Constant-count shifts now define flags, but the shift
   helpers still compute all four (parity included) even when dead.
6. Keep native x87, the vector lowering, the stack changes and the flag
   contracts under validation on AArch64 hardware, videos and long play.
