# What separates lifted ARM64 from native ARM64

The question: what makes code from our x86 → C → ARM64 pipeline less efficient
than ARM64 compiled from source, and which lost information is worth recovering?

This uses only local compilation with the production NDK Clang flags, decoded
x86, and cycle profiles already on disk (as function and instruction weights).
No game, APK or phone run was involved. Static counts are evidence about code
shape and never cycle claims. Concrete x86 → C → LLVM → AArch64 examples quote
game code, so they live in `work/codegen_lab/audit/examples.md`.

## Tools (all in the codegen lab, compiling one function at a time)

| Tool | Answers |
|---|---|
| `tools/codegen_lab/audit.py` | Ties each ARM64 instruction to its C line, guest instruction and sampled cycles; grades redundant loads as proven or suspected |
| `tools/codegen_lab/gap.py` | Lifted against native dependency structure |
| `tools/codegen_lab/mini.py` | One-function compile with exact production flags under named C variants, plus corpus runs; a compile takes about a second |
| `tools/codegen_lab/stack_census.py` | What guest stack loads are, from decoded x86 |
| `tools/codegen_lab/idioms.py` | Whole-program census of stack idioms (all 102,014 functions) |
| `function_facts.stack_accesses` | Frame slots and frame escapes from decoded x86 |

`llvm-mca`, `opt`, `llc` and Alive2 are not in the NDK or MSYS, so there is no
scheduling simulation or formal IR equivalence here. Correctness below means
full-state differential testing.

## Main results

1. **The reference gap comes from memory traffic, not a longer data chain, but
   its cycle cause is not established.** For 0x428540 the longest dependency
   chains are nearly equal (44 lifted, 40 native, nominal weights), while the
   phone measured 110 against 27 cycles. Lifted code has 11 store-to-load hops
   against 5. A static model cannot say why it stalls, and no explanation here
   should be read as accounting for those 83 cycles.
2. **Two small information fixes beat the hand-written native code statically
   on 0x428540, but this does not generalize.** Across the hot corpus the same
   fixes leave instruction counts flat and raise host spills. 0x428540 is an
   outlier built almost entirely from one idiom.
3. **The native reconstructions do not run by default.** `AC1_NATIVE_OVERRIDES`
   is opt-in, and every profile so far ran the lifted fallbacks
   (`ac1_translated_*`). In the crowd capture those take 4.6% of the main
   thread. `docs/hot-functions.md` still says the default is on.

## Findings, ranked by evidence

Corpus figures are 74 hot functions (47% of profiled cycles), weighted by those
cycles. Differential results compare a variant with the lifted baseline on 2,000
random states, across the full CPU struct and guest memory; they found no
differences on the three leaves tested, and they are evidence rather than proof.

### 1. 16-byte guest accesses go through `memcpy` (our artifact)

- **Native has:** vector values in vector registers, with lane reads as lane reads.
- **Why LLVM cannot recover it:** `wrxm` is a 16-byte `memcpy`. GVN reports nearly
  all float loads in 0x428540 as "clobbered by call": the `llvm.memcpy` intrinsic
  is still present when GVN runs and only becomes two `store i64` later. SROA
  also turns the `XMM` union into an `i64` pair, so values travel through GPRs.
  A reduced case forwards correctly, so LLVM is not at fault.
- **Recoverable safely:** yes, it is a pure accessor change and needs no analysis
  (exact, no assumption).
- **Frequency:** improves 17 of 74 hot functions, none regress. Corpus-wide
  effect is small: loads -1%, `fcvt` -3%, instructions about -0.3%.
- **Improvement:** one unaligned vector access for `rdxm`/`wrxm`. Two 8-byte
  accesses were tried and are worse (0x653400: 102 → 157 instructions).

### 2. Guest addresses hide their structure (exact semantics, costly form)

- **Native has:** `sp + 16` and `sp + 20` as known offsets of one pointer.
- **Why LLVM cannot recover it:** every address passes through `uint32_t`, so it
  is `zext(i32 add)`. Because the add may wrap, LLVM cannot prove a constant
  difference, so it cannot relate two slots.
- **Recoverable safely:** not from x86 alone. x86 addresses do wrap at 4 GB, so
  dropping that needs a mirrored arena, or a guard like the native plans use.
- **Frequency:** 47 of 74 functions improve, 7 worsen (0xA84FF0 by 334
  instructions). Together with finding 1: stores -5%, `fcvt` -11%, instructions
  flat, spill loads +73%, spill stores +56%. Total memory operations rise from
  about 138 to 143 per weighted function, so traffic partly moves from guest
  memory to the host stack. The cause of the spills is not yet established.
- **Improvement:** not ready. It needs a version that does not raise register
  pressure before it is worth anything.

### 3. Guest stack temporaries (compiler artifact, partly recoverable)

- **Native has:** values that never needed memory.
- **Why LLVM cannot recover it:** beyond findings 1 and 2, a guest store through
  an unknown pointer may alias any frame slot, and that is the correct assumption
  for arbitrary x86.
- **Recoverable safely:** only with proof. 40% of hot weight is in functions where
  a frame address leaves esp/ebp. The native plans discharge aliasing at run time
  with overlap checks, which is a sound model for leaf functions.
- **Frequency** (weighted guest stack loads, decoded x86):

  | Load | Share | Forwardable locally? |
  |---|---|---|
  | same block, nothing between | 34.6% | yes, given findings 1 and 2 |
  | past frame stores only | 3.8% | yes |
  | arguments | 21.1% | no, the caller wrote them |
  | no prior store in the function | 16.6% | no |
  | saved register pops | 10.9% | not exactly; relies on an ABI assumption |
  | other block | 4.5% | needs CFG dataflow |
  | across a call | 4.5% | no |
  | past an unknown store | 3.9% | needs alias proof or a guard |

  Whole program: the lane-read idiom (16-byte store, narrower load) has 2,429
  sites in 974 functions, 12 of them hot with 15.2% of profiled cycles. The wide
  reload idiom has 5,203 sites in 2,720 functions, which `StackLaneForwarding`
  already targets.
- **Improvement:** none needed beyond findings 1 and 2 for the same-block share.
  A general promotion pass has a low ceiling: most remaining loads are arguments,
  saved registers or slots without a local store.

### 4. Calls materialize CPU state (our representation)

Measured earlier and recorded in `docs/cpu-state-assessment.md`: guest calls are
the largest GVN blocker in the hot corpus. The private call ABI is unproven: its
only dynamic test was the cutscene replay. Argument loads (21% above) are the
same boundary seen from the callee side.

### 5. x87 and flags bookkeeping (small)

TOP traffic is about 3.5% of cycles in the replay profile, and region tracking
removes roughly a third of that in hot code. Flag stores are 0.46% of sampled
corpus cycles. `sub esp` at function entry still materializes flags, because no
later write kills them before the return.

### Essential, not artifacts

- **`fcvt` chains.** MSVC computed in double and narrowed; rounding points are
  observable, so native keeps them too. Removing redundant ones through
  forwarding is exact; removing the widening is not.
- **`fmov` to integer compares.** In 0xAA83A0 these feed integer `cmp`: the x86
  compares float bits as integers. Keeping forwarded lanes as floats was tested
  and rejected: it removed 2 moves and added 3 instructions in 0x4F5A70.
- **Guest stores.** They remain observable. Only overwritten ones may go.

## What is worth recovering

1. **Vector typing of 16-byte accesses:** exact, cheap, no regressions seen.
   Worth an opt-in lifter switch.
2. **Whether to enable the native reconstructions:** already built; it needs one
   measurement, not new work.
3. **Address structure:** large on idiom-heavy leaves, but a net loss in memory
   operations across the corpus as tested. Investigate the spills first.
4. **Not worth pursuing now:** general stack promotion, given its ceiling and
   proof burden, and float-typed lanes.

## Open questions

- What actually causes the 83-cycle gap on 0x428540. A phone microbenchmark of
  the `stack64+vec` lowering would answer it directly, when phone tests resume.
- Why 64-bit stack addressing raises spills.
- All percentages rest on the current hot corpus and on weights from crowded
  gameplay; they are static apart from those weights.
