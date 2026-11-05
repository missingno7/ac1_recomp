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

"Removed" code can be recovered from tag `baseline-consolidation`.

| Mechanism | Decision | Evidence | What would reopen it |
|---|---|---|---|
| Native x87 lowering | **KEEP** | -10.6% main-thread cycles per frame on host, with a differential fixture and captured records | - |
| GPR and XMM locals | **KEEP** | Measured on host gameplay; also shrinks code | - |
| Whole-program flag contracts | **KEEP** | `.text` -17%, cycles per frame -3.7% on host | - |
| Captured guest-memory arena | **KEEP** | -15.3% cycles per frame on host | - |
| Direct guest calls | **KEEP** | Replaces dispatch for known targets | - |
| Fences on every branch | **KEEP** | Protects polling loops; moving them needs a proof about what other threads observe | A proof covering threads and shared memory |
| Native Bink through FFmpeg | **KEEP** | Covers all 14 imports the game uses | - |
| Translated Bink DLL | **REMOVE** | Reachable only through a runtime switch; its `DllMain` ran only the DLL's own C runtime startup | - |
| Checked guest memory | **REMOVE** | Direct memory was 1.7x faster at loading; one memory path | - |
| x87 regions | **BLOCKED** | Exact, but no cycle measurement | Cycles for equivalent work with waiting separated |
| XMM contracts | **BLOCKED** | Sound, under 1% as tested, and a separate analysis | A shared facts layer with a measured benefit |
| Vector guest accesses | **BLOCKED** | Exact except NaN payload and an access at the top of the address space. Store-to-load hops on `0x428540` rise from 11 to 19 | ARM64 cycles on hot leaves |
| Dirty-register syncs (`d`) | **BLOCKED** | Exact in 61,806 comparisons, but 202 rewritten sites never ran and it has no cycle measurement | A shared fact, full coverage, cycles |
| Semantic IR lowering | **BLOCKED** | Lifts 24 of 74 corpus functions, no x87, cycle benefit unknown | Forwarding as a C fact, then ARM64 cycles |
| GPR speculation | **REMOVE** | Exact, but bimodal on the X4 with no stable gain | - |
| Private call ABI | **REMOVE** | Static count neutral, APK +13%, only tested on a cutscene | A new design measured on gameplay |
| Native leaf overrides | **REMOVE** | The wrapper cost 0.6-0.7% with overrides off, and plans ran on each call. `0x428540` was 110 against 27 cycles in isolation, excluding its plan | Static proof of each plan, then selection at build time |
| Native regions, journal, palette reuse | **REMOVE** | Same per-call cost. The palette's -2.79% cycles per frame was on Windows only | Static proof and ARM64 cycles |
| Modifier fusion | **REMOVE** | Regressed 3.91% on Windows | - |
| Runtime guards with fallback bodies | **REMOVE** | Per-call dispatch by design | - |
| Separate LLVM IR backend | **REMOVE** | Bypassing C showed no gain: mean -0.54 instructions over 63 functions | Measured advantage over C with the same facts |
| `restrict` on the CPU pointer | **REMOVE** | Unsound across calls | - |
| 64-bit guest addressing | **REMOVE** | Unprovable for pointers; spill loads rose from 4.4 to about 17 | - |
| Backward-only fences | **REMOVE** | No proof | - |

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

This is direction, not yet code.

## Testing a new fact

Two rewrites passed early checks and were wrong. A flag rewrite changed control flow
on `0x00AE11A0`, and it inflated a corpus figure. A register-sync rewrite lost jump
table edges on `0x0071E320`, and only 3 of 400 comparisons exposed that. So any
future fact needs comparison of the whole CPU state including flags, runs that reach
a return, and a mutant proving the test can fail. A site that ran is not a site
proven correct.
