# Code-generation decisions

The authoritative list of what production uses and why. The goal is one
implementation of each guest function, chosen at generation or build time, with
no experiment left selectable.

A candidate ends in one state:

- **KEEP:** proven correct and beneficial; part of the normal path.
- **REMOVE:** rejected, redundant, superseded or unjustified.
- **BLOCKED:** a named proof or measurement is missing. It is excluded from
  production and not left behind as a switch. Git history keeps the code.

Instruction counts here are diagnostics, never performance results. FPS is not used
for CPU decisions. See `docs/cpu-audit.md` for where the main thread's time goes.

## Decisions

| Mechanism | Was | Decision | Evidence | What would reopen it |
|---|---|---|---|---|
| Native x87 lowering | Default | **KEEP**, as the only x87 path | Production default; project direction | - |
| GPR and XMM locals | Default (`--local-regs all`) | **KEEP**, no option | Production default | - |
| ABI flag liveness | Default (`--flag-liveness abi`) | **KEEP**, no option | Production default | - |
| Direct guest calls | Default | **KEEP**, no option | Production default | - |
| Fences on every branch | Default (`--branch-fences all`) | **KEEP**, no option | The fence protects polling loops; moving it needs a proof about what other threads observe | A proof covering threads and shared memory |
| Whole-function x87 regions | Off | **BLOCKED** | Differentially exact, TOP traffic down, estimated 0.5-1% from static data. No cycle measurement | Cycles for equivalent work with waiting separated |
| GPR speculation | Off | **REMOVE** | Exact, but bimodal on the X4 and no stable gain | - |
| Private ABI | Off | **REMOVE** | Exact, static count neutral, APK +13%. Only A/B was a cutscene replay | A new design with measured gain on real gameplay |
| Whole-program XMM contracts | Off | **BLOCKED** | Sound; below 1% as tested. Whole-program return facts are the right kind of evidence, but this form adds a separate analysis | A shared facts layer with a measured benefit |
| Vector guest accesses | Off | **BLOCKED** | Exact except NaN payload and an access at the very top of the address space. Static count falls, but store-to-load hops on `0x428540` rise from 11 to 19 | ARM64 cycles on the hot leaves |
| Native leaf overrides | Off, wrapper always paid | **REMOVE from production** | `ac1_native_invoke` costs 0.6-0.7% with overrides off, and plans run on every call. `0x428540` measured 110 against 27 cycles in isolation, but that excludes the plan | Static proof of each plan's preconditions, then selection at build time |
| Native regions and journal | Off on ARM64; the palette region is on for the Windows x64 build only | **REMOVE from production** | Same selection and per-call cost, plus rollback machinery. The palette's -2.79% cycles per frame was measured on Windows only, never on ARM64. Removing it gives up that Windows gain | Static proof of the region's preconditions, and ARM64 cycles |
| Native palette reuse | Off | **REMOVE** | Inconclusive in testing | - |
| Modifier fusion | Off | **REMOVE** | Regressed 3.91% over ten Windows pairs | - |
| Runtime guards with fallback bodies | Lab only | **REMOVE** | Per-call dispatch by design | - |
| Separate LLVM IR backend | Lab only | **REMOVE** | Bypassing C showed no visible gain: mean -0.54 instructions over 63 functions | Measured advantage over C with the same facts |
| Semantic IR lowering | Lab only | **BLOCKED** | Fails closed on x87 and lifts 24 of 74 corpus functions. Cycle benefit unknown | Forwarding as a C fact, then ARM64 cycles on `0x428540`, `0xAA83A0`, `0x4E6820`, `0x1176DE0` |
| Dirty-register syncs at calls and returns (`d`) | Lab only | **BLOCKED** | Exact in 61,806 comparisons, with mutants caught, but 202 rewritten sites never ran and it has no cycle measurement | Implementation as a shared fact, full coverage, cycles |
| `restrict` on the CPU pointer | Lab only | **REMOVE** | Unsound across calls: the runtime also reaches the CPU through its own pointers | - |
| 64-bit guest addressing | Lab only | **REMOVE** | Cannot be proven for pointers; spill loads rose from 4.4 to about 17 | - |
| Moving fences to backward branches | Option | **REMOVE** | No proof; see fences above | - |

The default path therefore stays what production already generates. This
consolidation changes how much code exists, not what the game executes, apart from
removing the native wrappers and the flag opt-outs they forced on six functions.

## Semantic facts

The IR experiment answered the backend question as far as static evidence can.
Recovered information matters more than bypassing C, so facts should reach the
existing C lowering. The lifted result is:

- One pipeline: x86, then proven facts, then one lowering. An unproven fact is
  absent; there are no assumption flags.
- Facts are keyed by guest address, with a proof basis and the condition under which
  they fail closed.
- Local facts: dirty registers, jump-table edges, frame roots and offsets, frame
  escape, read-only addresses.
- Whole-program facts: flag demand, return readers, callers.

This is direction, not yet code. `tools/function_facts.py` already holds the shared
analysis to build it on.

The experiment's code is private and unmerged, on the local branch
`worktree-agent-afdc6961b28db1bce`, with results under its `work/semantic_ir/`.

## Correction: an exact-looking fact was wrong twice

Two rewrites passed early checks and were wrong. A flag rewrite changed control flow
on `0x00AE11A0`, and it inflated a corpus figure. A register-sync rewrite lost jump
table edges on `0x0071E320`, and only 3 of 400 comparisons exposed that. So any
future fact needs comparison of the whole CPU state including flags, runs that reach
a return, and a mutant proving the test can fail. A site that ran is not a site
proven correct.
