# Where the translated code still materializes x86 state (ARM64)

An assessment of what keeps Clang from producing register-resident ARM64 code
for lifted functions, which changes would remove it, and in what order. It is
based on the phone's sampled cycles and backend stalls, the optimized ARM64 of
the hot corpus, and the cost model in `docs/codegen-lab.md`, not on counts of
generated C.

## Evidence

**Dynamic attribution.** `tools/codegen_lab/dynamic_lines.py` maps
every sample of the two city profiles (c33daae, S24 Ultra, main thread, 20 s
each, instructions / cycles / backend-stall events) to its ARM64 instruction
and, through the line table, to the lifted C statement. Translated game code is
78% of the main thread's cycles. Its cycles by instruction class:

| class | cycles | notes |
|---|---|---|
| guest memory loads + stores | 26% | arena accesses: guest stack, heap and globals |
| ALU: 32-bit add/sub immediate | 11% | guest address displacements, `esp -= 4`, counters |
| other ALU | 15% | |
| branches | 11% | |
| CPU struct: GPRs | 9.3% | almost all at call boundaries |
| CPU struct: x87 TOP | 3.5% | block-boundary syncs (below) |
| CPU struct: XMM | 2.7% | call-boundary syncs |
| host stack (prologues, spills) | 5% | low stall share: callee-saved saves per call |
| FP/SIMD arithmetic | 5% | |
| CPU struct: flags | 0.1% | FLAGABI already removed them |

In the 40 hot functions whose source is in the snapshot (21% of main-thread
cycles, natives included), the statements behind the cycles: guest stack
accesses (push, pop, `[esp+x]`, `[ebp-x]`) 19%, lines carrying an x87 TOP sync
15%, other guest accesses 10%, calls with their syncs 8%, flags 4%. Cycles
sampled on a stalled instruction can land a few instructions later, so the
statement view is the more reliable one. Backend stalls are spread evenly
over the classes (0.45-0.55 stalls per cycle), so no single class is a stall
hot spot. The cost comes from dependency chains through memory in general,
which matches the leaf benchmarks: the lifted 0x428540 executes 230
instructions in 110 cycles, the native one 177 in 27.

**Cost model** (phone, `microbench_arm64.c`): a value carried through the
struct across a call costs a 4-5 cycle forwarding hop on the chain that uses
it, and almost nothing in registers. A dead sync store costs issue slots only
(8 stores ≈ 2-4 cycles). Mixed-width traffic to one slot costs 7-16 cycles
when forwarding fails. Materialized flags and predicted indirect calls are
nearly free.

**Already ruled out by measurement:** `CPU *restrict` and LTO leave CPU-state
traffic unchanged, because the struct escapes into every call. Removing the
AC1_CALL fallback path cuts 8.9% of static instructions but 0 cycles. More
flags work has nothing left to remove.

## Opportunities, in order

### 1. x87: keep TOP and the stack slots in locals across blocks

The native-x87 lowering is block-local. Every basic block reloads TOP from
`c->fpu_top`, loads slots lazily from `c->st[(top+r)&7]`, and writes dirty
slots and TOP back before each branch, label, call and return. That is 351,137
sync statements in the 6,563 native-x87 functions. The branch fence on each
`goto` forbids Clang from forwarding across it, so the round trips are real:
3.5% of all game cycles are TOP loads and stores alone, and in FP-heavy hot
functions the TOP-sync lines carry a large share of the cycles.

Only calls, returns and unknown helpers need the struct. Within a function the
x87 stack depth at each block entry is static in compiler-generated code. A
forward dataflow over the CFG (depth in, depth out) proves it per function, and
the slots then become fixed locals across the whole function; functions where
depth differs between predecessors keep today's anchoring. This is contained
in `native_x87_function`, follows the same sync-at-calls rule as the GPR and
XMM locals, and is checked by the existing x87 differential tests.

- Expected gain: most of the 3.5% TOP traffic and the slot reloads in FP
  loops. It removes a memory-carried chain from every block of 6,563 functions.
- Risk: low to moderate. The depth proof is local and falls back safely.
- Engineering cost: small to medium.

### 2. GPRs: call-boundary contracts

GPR struct traffic is 9.3% of game cycles. A typical call site stores six or
seven GPRs and reloads all eight. The XMM contracts now have a sound treatment
of indirect transfers (the struct stays exact for registers read after them,
and each function writes back only what it modifies). The same solver applied
to GPRs removes the syncs of registers the callee neither reads nor modifies.
Under the MSVC ABI that is mostly the callee-saved ebx, esi, edi and ebp.

The gain is smaller than the instruction count suggests: dead syncs are
cheap, and the cycles go where a synced register is live across the call on a
dependency chain. ESP and the argument registers must stay exact for imports,
the dispatcher, scheduler checkpoints and SEH, which read the struct.

- Expected gain: part of the 9.3%, plausibly a third to a half.
- Risk: moderate. Many runtime readers of GPRs must be enumerated, and the
  analysis is whole-program.
- Engineering cost: medium. It reuses the XMM v3 machinery.

XMM contracts show the ceiling of this kind of work on a small state class.
XMM struct traffic is 2.7% of game cycles. After the fix that stopped the
indirect-call unions from widening every contract (`20f7378`), functions with
return demand fall from 101,937 to 37,015, and call sites with no sync rise
from 200 to 7,048. On the hot XMM corpus, XMM-state loads fall 16% and stores
27% against the baseline, with no regressions. That is slightly less than the
earlier over-conservative version (-20% / -30%), because indirect callers now
store what they read afterwards. The indirect entry union stays at all eight
registers: with 67,146 possible targets, some target reads each register at
entry. It now only costs indirect call sites stores of the caller's own XMM
locals. The realistic gain is well under 1% of game cycles, so XMM stays
opt-in until it rides along with a larger milestone. GPRs are 3.5 times
larger.

### 3. Exact 64-bit guest addressing via a mirrored arena

Every guest access computes `base + zext32(reg + disp)`: x86 wraps the address
at 4 GB, so the displacement cannot fold into the ARM64 addressing mode. If the
arena is mapped twice, back to back, from one memory object (a memfd on
Android, a section on Windows), then `base + zext32(reg) + disp` is exact for
any displacement up to the mirror size. The add becomes an addressing-mode
offset, and adjacent accesses can pair into LDP/STP. 32-bit add/sub-immediate
is 11% of game cycles; part of it is guest stack pointer arithmetic, which
stays.

This is a runtime and accessor change. It needs no new lifter analysis and
applies to every function. An earlier unmirrored 64-bit experiment helped
0x653400 and hurt 0x428540 through a mixed-width forwarding failure, so it
must go through the lab (static, leaf benchmarks, then the cost model's
hazard check) before any gameplay run.

- Expected gain: a few percent of game cycles, more where the narrower
  addressing currently blocks pairing.
- Risk: low for correctness, since it is exact by construction. The
  platform's memory mapping and the 64-bit guard sizes need care.
- Engineering cost: small to medium.

### 4. Guest stack: promote only what can be proven private

Guest stack accesses are the largest single statement class (19% of the hot
corpus's cycles). But the stack is memory the program can observe. Callees
read their arguments and can address the caller's frame through their own
ESP (varargs, `_chkstk`, SEH handlers). Frames leak through `lea` of locals,
and exact semantics include any pointer into the stack. Clang cannot forward
these accesses because `-fno-strict-aliasing` guest stores through other base
registers may alias them, and that is correct.

A sound subset exists: slots of a frame whose address never escapes (no
`lea`/copy of an ESP- or EBP-derived value except as a plain base), promoted
between calls only, with every store still performed. The promotion has to
prove that no other access in the region can alias. Doing that soundly needs
a per-function value analysis of which registers hold frame addresses. It is
the most error-prone item here and should start with a measurement: how many
stack loads are fed by a same-function store with no intervening call.

- Expected gain: uncertain until measured. It is bounded by the call-free
  share of the 19%.
- Risk: high.
- Engineering cost: high.

### 5. Selective inlining of tiny hot direct callees

Each translated call costs a host prologue and epilogue (5% of cycles go to
host-stack stores, mostly callee-saved saves), the return-address push, and a
full CPU-state sync. Inlining small leaf callees in the lifter (keeping the
guest-visible return-address store) would remove all of these at once and
give Clang one register-allocation scope. It needs call counts to choose
targets, and the phone has no cheap source for them yet. The host-side
`AC1_PROFILE_COUNTS` would need an Android equivalent. Contracts (items 1-2)
remove much of the sync part first, so this is measured after them.

### Rejected or deferred

- **`restrict`, LTO:** measured, no change in CPU-state traffic.
- **Further flags or EIP work:** already near zero.
- **Dispatch-path tuning:** predicted indirect calls cost about 1 cycle; the
  fallback path costs 0 cycles.
- **Manual register assignment:** unnecessary. Every item above works by
  letting Clang's allocator see values that are currently forced to memory.

## Architecture: a shared function-facts layer, not a rewrite

Today each pass (flags, GPR locals, XMM locals and contracts, native x87,
fences) re-derives instruction groups and the CFG from generated C with
regular expressions. Each new analysis repeats that work, and the regexes
have been the main source of bugs. A whole new IR would stall optimization
work for a long time.

The proportionate step is a small shared layer built once per function from
the decoded x86, not from C:

- the CFG;
- per instruction, its defs and uses of GPRs, flags, XMM and x87 stack depth;
- its transfer kind.

The passes then query it. The x87 depth analysis (item 1) is easiest on the
x86 instruction stream and is the natural first user; GPR contracts (item 2)
would be the second. C stays the output format, and the existing passes
migrate one at a time behind their current tests.

## Order of work

1. x87 whole-function depth: lab snapshot, static compare on the x87-heavy
   hot functions, x87 differential tests, then leaf benchmarks.
2. Mirrored-arena addressing: a runtime and header switch, so it reuses a
   snapshot. Static compare, leaf benchmarks on the phone, and a hazard check
   for mixed-width forwarding.
3. GPR contracts on the XMM v3 machinery, with the runtime's GPR readers
   enumerated first.
4. A measurement of call-free guest-stack forwarding, then a decision on item 4.
5. One gameplay A/B after 1-3, as the integration milestone.

## Follow-up: whole-function x87 TOP, measured (item 1)

**Proof.** `tools/function_facts.py` builds per-function facts from the
recovered CFG (`functions.json`) and the decoded x86, not from generated C:

- blocks and successors;
- call and tail-transfer kinds (direct, import, indirect);
- an explicit x87 effects table: pops, pushes, and status, control, rotate,
  tag, reset, environment and MMX classes.

`x87_region` propagates (anchor, depth) over the CFG, including loops. An
anchor is the function entry, a call of unknown stack effect, or a join whose
predecessors disagree. Within one anchor, st(i) is the fixed physical slot
(i - depth) mod 8, so value identity across blocks follows from consistent
depth, and FXCH is a swap of two slots. Direct calls use whole-program
net-depth summaries (`x87_deltas`, a worklist over the call graph).
Environment, reset, tag, rotate and MMX instructions keep today's lowering.
Census over the current generation:

- 5,935 of the 6,563 native-x87 functions are provable: 31% of profiled
  cycles, out of 34% in native-x87 functions.
- Only 1,664 of them need a single anchor. 4,280 call something of unknown
  stack effect, mostly virtual calls and imports.
- 3,165 have a join whose predecessors disagree.
- No native-lowered function contains fincstp, fdecstp, ffree or finit. The
  block-local lowering does not model their direct writes to `c->fpu_top`, so
  this was worth confirming.

**Lowering.** `native_x87_function(body, regions)` keeps TOP in its local
across blocks. It stores TOP only where it can be observed: calls, returns,
fnstsw, and edges into a disagreeing join. It reloads TOP only after a call of
unknown effect or at such a join, and adds a call's proven net depth to the
local instead of reloading. Dirty slots are still written back at every block
boundary. Where the generated C disagrees with the proof, the block-anchored
lowering is used for that function. It is opt-in: `lift_ac1.py --x87-regions
on`, `build.ps1 -X87Regions on`.

**Correctness.** `tests/test_x87_native.py` compares region mode with the
exact lowering over 4,096 random states each, on the full CPU struct (minus
the fields native mode drops by design) and 512 bytes of guest memory. The
fixtures cover:

- a loop;
- consistent joins;
- FXCH;
- fnstsw at depth 1;
- a direct call with a proven +1 effect;
- an indirect call of unknown effect;
- a join whose predecessors disagree;
- the unbalanced-join fixture, where region mode must equal today's code.

A mutant that ignores a call's net depth fails the test.

**Small-scale result** (`tools/codegen_lab/x87_regions_experiment.py`). Both
lowerings are built from the same lifter input. The block-anchored rebuild
must equal the snapshot's code exactly, and the two are compiled with the
game's ARM64 flags. 27 of the 39 native-x87 hot-corpus functions change
(14.8% of profiled cycles). Weighted ARM64 changes:

| metric | change |
|---|---|
| instructions | -3.5% |
| x87 TOP loads | -48% |
| x87 TOP stores | -29% |
| CPU-struct loads | -13% |
| CPU-struct stores | -16% |
| missed store-to-load forwards | -30% |

One function grows: 0x762980, +12 instructions.

A full generation with `-X87Regions on` (lab snapshot `x87r`) puts 5,101
functions in region mode. On the whole 74-function hot corpus (47% of
profiled cycles), against the current default:

| metric | change |
|---|---|
| instructions | -3.1% |
| x87 TOP loads | -48% |
| x87 TOP stores | -29% |
| missed forwards | -25% |
| dead stores | -19% |
| GPR and XMM state traffic | -3% to -6% |

The three leaf controls are unchanged. 17 of the 18 flagged functions
execute fewer instructions but save one more register pair, because TOP now
stays live across calls in a callee-saved register.

Against the phone's city profile (same x87 lowering), the TOP loads and
stores of these 27 functions take 1.56% of game-code cycles, and region mode
removes about a third of them: **about 0.5% of game-code cycles**, plus part
of the 3.5% fewer instructions. TOP traffic in all game code is 3.5% of
game-code cycles. The rest stays because calls must store TOP for the callee.
Only 84 of the 277 call sites in those functions reach callees that never
touch x87, even transitively, so an x87-transparency summary would remove
roughly another third of what remains.

**Verdict.** The proof requirements turned out modest and the implementation
is small, but the removable cost is too. Realistically 0.5-1% of game-code
cycles, with a hard ceiling of 3.5%. It stays opt-in, to be measured with an
isolated phone benchmark and then enabled at the next integration milestone
if confirmed. It does not justify further x87 work ahead of GPR contracts
(9.3% of cycles) or guest addressing (11%).

**The shared facts layer was worth it.** `function_facts.py` (decode, CFG,
transfer kinds, an effects table, worklist summaries over the call graph)
replaced what would have been another regex pass over generated C. GPR
contracts need the same pieces, with Capstone's per-instruction register
reads and writes in place of the x87 table.

## Follow-up: GPR call boundaries, measured (item 2)

**Exact footprint contracts do not pay.** `tools/codegen_lab/gpr_census.py`
gives each function its transitive register footprint from the decoded x86
(`function_facts.gpr_effect`, Capstone's implicit and explicit operands; an
indirect call, import or unknown target counts as all eight registers). A
register can stay in the caller's local across a call only if it is outside
the callee's footprint. 72,995 of the 102,014 functions reach an indirect call
or import and have all eight registers in their footprint. In the hot corpus,
190 of 343 direct call sites have a register outside it: 31% of the live
reloads and 13% of the dirty spills at call sites, weighted by function
cycles. Against the phone city profile that is about 6% of all GPR-state
cycles, or **0.5% of game-code cycles**, because most traffic is at calls into
callees that use the register or call through vtables. That gain does not
carry the whole-program invariants that contracts need.

**Where the GPR cost is.** Splitting the profile's GPR-state loads and stores
by the lifted statement they belong to (hot corpus):

| sync site | share of GPR-state cycles | stall share |
|---|---|---|
| reloads after a call | 59% | 0.55 |
| spills before a call | ~16% | |
| entry loads | 9% | |
| return stores | 9% | |

About three quarters of the reload cycles are esi and edi, mostly one `ldp` of
the pair, with stall share 0.62. These are callee-saved registers. The callee
pushes them, pops them back and stores them to the struct at return; the caller
reloads them and uses them at once (esi is usually `this`). That makes a memory
chain through two struct hops and a guest-stack round trip on every call. The
cost is the calling protocol, not missing analysis. esp is about 1.6%.

**Value speculation (opt-in, exact).** With `--gpr-speculation on`
(`build.ps1 -GprSpeculation on`), each live callee-saved local (ebx, ebp, esi,
edi) is reloaded after a guest call as:

```c
v = c->esi;
if (v != local) local = v;   /* behind __builtin_expect and a volatile asm */
```

On the predicted path the code after the call uses the caller's own register,
so it no longer waits for the callee's stores. The result is always the
struct's value, so the state is exact whatever the callee does. Liveness comes
from `function_facts.gpr_live_after_calls`. Dead registers are left to Clang,
which would otherwise have to keep the load for the compare.

Checks:
- The branch form survives in real lifted functions; no `csel` puts the load
  back on the chain.
- `tests/test_codegen_reload_speculation.py` runs plain and speculative code on
  callees that keep or change esi and compares the full state and memory.
- A mutant that ignores a mismatch fails that test.

On the hot corpus (snapshot `gs1`, everything else default) the static price is:

| metric | change |
|---|---|
| instructions | +8.2% |
| branches | +40% |
| CPU-struct GPR loads | +25% (one `ldp` of a pair becomes two compared loads) |
| host stack spills | +15-17% (the old values stay live across the call) |

`microbench_reload.c` reproduces the protocol. On the PC it shows no
difference (1.56 vs 1.54 ns per call), as expected: x86 cores rename memory and
forward stores nearly for free. The ARM64 cost model measured a 4-5 cycle hop
per struct round trip. The phone run (`microbench_run.py
reload_plain,reload_speculative`, then a lab benchmark of hot non-leaf
functions) decides whether the removed chain outweighs the extra
instructions. Until then it stays off.

**If speculation wins on the phone**, the same chain is the case for a private
register calling convention: callee-saved values and `this` passed in host
registers instead of through the struct. That is the larger structural change,
since every signature, the dispatch table and native bindings would change, so
the speculation result is the cheap way to learn how much it could gain first.
