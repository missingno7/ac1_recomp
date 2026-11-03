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
