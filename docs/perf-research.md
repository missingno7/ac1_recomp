# Performance research: what other static recompilers did, and what fits AC1

Research only. Nothing was built or run for this report. Numbers quoted from other projects are their own
measurements, on their own games and hosts. "Expected gain" for AC1 is an estimate, and its confidence is stated.
Re-profile after each change. The last AC1 profile was taken before direct memory, so the shares below have moved.

## 0. AC1 baseline and what reading the code adds

Measured facts (given): the direct 4 GiB arena made menu frames ~3.5x faster. D3D9 persistent shadows took the
menu from 20 to 65 FPS. ~2,900 baton hand-offs/s happen between two busy guest threads. ntdll + winpthread were
~27 % of CPU-active samples, and the process used ~1.1 cores. Generated code was ~8.5 % of CPU (before direct
memory).

Observations from reading the current tree. Each is a cost that comes from the implementation, not from x86:

| # | Observation | Where |
|---|---|---|
| A | Every handled import ends in `scheduler_checkpoint()`. That takes the scheduler mutex, runs `scheduler_refresh_waits`, scans every thread, and hands the baton to any runnable peer at once, with no quantum. Returns are rate-limited (64 returns + 1 ms); imports are not. | `ac1_runtime.c` `dispatch()` ~L6285, `scheduler_checkpoint` L805 |
| B | A hand-off wakes every waiter (`WakeAllConditionVariable` on one CV). Each waiter re-checks `owner` and goes back to sleep. With 5+ workers, one hand-off costs several kernel wakes. | `scheduler_notify` L720 |
| C | Import resolution is a linear chain of ~25 `handle_*_import` functions. Each does `equal_ascii_fold(dll, ...)` + `strcmp(name, ...)`, so `EnterCriticalSection` passes ~15 handlers before it matches. | `dispatch()` L6250-6284, e.g. `handle_critical_section_import` L2469 |
| D | `_Thread_local` is emutls on **both** targets: MinGW GCC always uses it, and Android uses it because `ANDROID_PLATFORM=android-26` (ELF TLS needs API ≥ 29). `dispatch()` touches `dispatch_count`, `dispatch_depth` (x3), `last_guest_target` (x2) and `active_guest_thread`, ~7 `__emutls_get_address` calls per guest call. Each goes through libgcc → winpthread `pthread_getspecific` → `TlsGetValue`. **Hypothesis:** much of the "winpthread" share is this, not locking. Confirm with call stacks. | `ac1_runtime.c` L94-214, `guest_memory.c` L38-40 |
| E | Flags are eager, and all six are computed: 190k `flags_logicz`, 133k `flags_add`, 130k `flags_sub` in `work/lift_initial`. Only 270k jcc sites read flags; AF is read in 398 places, PF in 1,975. `add esp,0xC` after a call computes six flags. | generated `work/lift_initial/*.c` |
| F | All 139,633 x87 instructions carry `ac1_x87_check` plus `fpu_ip/fpu_cs/fpu_opcode/fpu_dp` stores and tag-word updates in `fpush`. Only `fnstenv/fnsave/fxsave` can observe these. | `lift_ac1.py` L3359-3373, `cpu.h` L517 |
| G | 31,296 `lock xadd` and 147 `lock cmpxchg` sites call out-of-line `guest_atomic_*`, which goes through `guest_ptr` (an emutls `selected_memory` lookup, then a region-cache probe) instead of the arena. | `guest_memory.c` L385, L427 |
| H | `ac1_direct_read` reloads the global `ac1_direct_arena` and tests it for NULL on every access. Because `-fno-strict-aliasing` is on and the arena is an opaque pointer, GCC must assume every guest store may hit `*c`. Registers and flags are then reloaded and stored around every guest store. | `cpu.h` L147-162 |
| I | `dispatch()` runs ~6 trace/diagnostic tests (`trace_object_types`, `trace_imports` with specific EIPs, `dispatch_limit`) on every call. `dispatch_jmp` is a nested call, not a tail call, so a fall-through or jump adds host stack and depth. | `dispatch()` L6107-6237, L6430 |

## 1. Project by project

### 1.1 NFSU2 Web: [th0mk/nfsu2-web](https://github.com/th0mk/nfsu2-web) (MIT; wasm, also macOS ARM64)

Read: `tools/lregs.py`, `tools/flagabi.py`, `tools/inline_pick.py`, `runtime/cpu.h`, `runtime/dispatch.c`,
`runtime/win32_ext.c`, `runtime/web/ticketlock.h`, `docs/web/DESIGN.md`.

- **LREGS** (`tools/lregs.py`): a text post-pass over each lifted function. It rewrites `c->eax`/`c->zf` into the
  locals `_r_eax`/`_r_zf`. A forward dataflow over labels/gotos tracks which registers and flags may differ from the
  struct. It stores only those before a direct call, an indirect call, a tail jump or a return, and before the
  runtime helpers that take the CPU (`cpuid_shim`, `guest_hook`, `unsupported_insn`). It reloads after calls. Flags
  are stored before a direct call only if the callee reads them at entry, and on return only if the function returns
  flags (FLAGABI).
- **FLAGABI** (`tools/flagabi.py` + `lift.py abi_cfg/abi_liveness`): a whole-program fixed point. It computes
  `live_in[f]` (flags read at entry), `flags_out` (callees whose returned flags are read) and `indirect_rd`. An
  x86-undefined flag counts as defined. On NFSU2: 23,274 functions, 14 return flags, 210 are entry-live, and 12
  indirect sites read flags (from the upstream doc).
- **Inline indirect fast path** (`cpu.h` `DISPATCH_CALL`/`dispatch_fast`, `dispatch.c` `g_dtab`): a pointer table the
  size of `.text`, indexed by `addr - TEXT_BASE`. A hit is one load and one call; a NULL falls back to
  `dispatch_indirect`. Direct calls are plain `sub_X(c)`.
- **Alias trick**: guest memory goes through `g_guest`, a weak undefined symbol at address 0, combined with a
  `restrict` CPU parameter. Clang then knows guest stores cannot touch `*c`, so registers stay in locals.
- **JTSWITCH**: MSVC jump tables become C `switch` statements inside the function, and each case checks the table
  entry. Before this there were 3,821 table targets lifted as separate functions.
- **Profile-guided inlining** (`tools/inline_pick.py`): call counts from a coverage run decide what is inlined. It
  ranks by calls / (sites × body size) up to a 1 MB C budget: 110 functions = 36 % of calls.
- **Native overrides** (`runtime/overrides.c`): `_ftol2`, the frustum box test and `D3DXMatrixMultiply`. Any NaN,
  infinity or subnormal goes to the lifted body, and tests compare each override with its lifted body.
- **GIL policy** (`win32_ext.c`): there is no time-slice preemption in lifted code.
  - `guest_fair_yield` hands over at most once every 1,024 Win32 calls, only if someone is queued, and never inside
    a guest critical section.
  - Guest CSs are real host mutexes, and the GIL is released while waiting on them.
  - `WaitForSingleObject` takes a free mutex without handing over unless the holder has had the GIL ≥ 2 ms.
- **Measured** (DESIGN.md):
  - The `g_guest` alias + fused x87 rounding + `DISPATCH_CALL` together: race game-thread time "p50 −25 %, p95
    and p99 about −20 %".
  - LREGS + inlining + runtime fixes: cycles/frame 16.45 M → 15.48 M (−5.9 %), of which LREGS −3.2 %, inlining
    −1.3 %.
  - An `nanosleep` in the fair yield had made a cut scene run at 4.6 fps ("70 % of the guest thread in
    __semwait_signal").
- **Fit for AC1:**
  - LREGS is a text pass over C that matches ours: `c->` fields, `return;`, `dispatch(c, …)`, labels `L_XXXXXXXX:`.
    Porting means a new `_SITES` table and a re-check of `goto`/`switch` handling. Medium cost.
  - The risk is in runtime code that reads the CPU mid-function: the `stop_guest` diagnostics, SEH/`longjmp` paths,
    and `AC1_SET_GUEST_EIP` tracing.
  - A full `g_dtab` would be ~100 MB of pointers on x64 for ~12.6 MB of AC1 `.text`. Use a 16-byte-line index table
    instead (§4, item 4).
- **License:** MIT. The LICENSE also credits "Portions copyright (c) 2026 Talha Müderrisoğlu (the original recompiler
  and macOS runtime)", whose upstream is now proprietary (§1.2). Prefer reimplementing over copying, and keep the MIT
  notice for anything copied.

### 1.2 NFSU2Recomp: [talhamudo/nfsu2recomp](https://github.com/talhamudo/nfsu2recomp) (proprietary, all rights reserved)

Read: `LIFTER_EFFICIENCY.md`, `PROJECT.md`, `runtime/mem.c`, `LICENSE`.

- **Memory**: 4 GB reserved `PROT_NONE`, used regions committed. This is the same as what AC1 now has.
- **Dispatch**: an O(1) direct index table the size of `.text` ("14 MB of `uint32`"; 23,278 functions, no
  collisions).
- **Measured lifter steps** (ARM instructions/cycles per race frame):

| Step | Instructions | Cycles |
|---|---|---|
| FLAGABI | −6.9 % | −4.5 % |
| + X87OPT | −10.9 % total | −10.1 % total |
| RETEIP (`ret` → `esp += 4+n; return`) | −0.7 % | −2.7 % |
| MEMARG (guest base as a second `restrict` parameter `sub_X_m(CPU *restrict c, uint8_t *restrict _mb)`) | −9.7 % | −11 % |
| `-fno-strict-aliasing` (needed for correctness) | +2.8–6.2 % | +1.1–5.6 % |

  - X87OPT keeps x87 slots in locals with a compile-time top inside a block.
  - The `-fno-strict-aliasing` cost was found through a GVN miscompile: an i32 store followed by a float load at
    the same guest address.
  - Overall, v1 → v2: CPU 5.71 → 4.44 ms/frame (−22.3 %), cycles −27.7 %.
  - **Negative result:** leaf inlining (INLINE=50, 4,703 functions) cost +3.4 % cycles. ThinLTO was noise.
  - COM dispatch by method name → numeric id: 12.60 → 11.06 ms/frame. `strcmp` was 11 % of the guest thread.
- **GIL**: released only in blocking calls (`Sleep`, `WaitFor*`), with real pthreads and a CPU struct per thread.
  There was a GIL-starvation bug: a wait loop that never blocked ran "188 million empty" iterations.
- **Fit for AC1:**
  - AC1 already has RETEIP (`c->esp += 4; return;` in the generated code) and `-fno-strict-aliasing`.
  - MEMARG is the measured fix for observation H. FLAGABI and X87OPT target E and F.
  - The name→id COM lesson applies directly to observation C.
- **License: blocker for code.** Its LICENSE forbids distributing "any work derived from it". Use the published
  measurements and ideas only. Do not copy code or the generator.

### 1.3 Pinyon Shift runtime: [site](https://arcanite24.github.io/pinyon-shift/runtime/), [repo](https://github.com/arcanite24/pinyon-shift) (BSD-3-Clause; Xbox 360/PPC on rexglue)

- **Direct memory**: one file-backed 4.5 GB mapping, "a guest address plus the base pointer is the host address".
  - A view mapped 4 KB short forced `+0x1000` on ~70 % of accesses (~14 % of host instructions). They fixed it
    with `VirtualAlloc2`/`MapViewOfFile3`.
  - Lesson: inspect the disassembly of the access macro, not only the C.
- **Threads run truly concurrently** (~42 host threads in a race). A process-wide recursive mutex is used only for
  interrupts-off sections, events and vblank.
  - Load-reserve/store-conditional sections (1,001 of 1,062) skip the lock.
  - Contentions fell from 132–160 to 22–25 per frame, lock wait p99 from 39 ms to 8–9 ms, and the worst frame from
    186 ms to 45–48 ms.
  - Guest `RtlEnterCriticalSection` is a host function: 2,718 → 19 lookups per frame.
  - CS spin capped at 1,024 tries.
- **Wakeups**:
  - GPU fence waits spin 20 µs and then block for at most 1 ms: process CPU −15 %.
  - A render-thread `SleepEx(0)` spin became a 200 µs sleep: render CPU 477–839 → 242–253 ms/s.
  - POSIX waits wake on signal instead of 1 ms polling: process CPU −6 to −9 %.
- **Native replacements** (`src/guest_native/guest_native.cpp`):
  - Each one checks an FNV-1a hash of the guest code it replaces before installing (`CheckGuestCode`).
  - `pinyon_shift_guest_native_verify=N` runs the original every Nth call and compares the results.
  - Native `memcpy/memmove/memset` cut "11–14 % of simulation-thread cycles". Native `pow` and LZX are
    bit-identical and ~2x faster.
- **Fit for AC1**: PPC guests are weakly ordered and the code was written for 6 hardware threads, so true
  concurrency is natural there. For AC1 the transferable parts are:
  - native CS/wait implementations with direct wake,
  - wake-on-signal instead of polling,
  - hash-checked native overrides with a verify mode.
  The x86-TSO question (§3) does not arise for them.
- **License**: BSD-3-Clause. Code is reusable with the notice.

### 1.4 XboxRecomp: [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (MIT; LGPL-2.1 only for the xemu-derived APU/nv2a files)

Read: `src/kernel/kernel_guest_cpu.c`, `src/kernel/kernel_thread.c`, `docs/technical/register-model.md`,
`docs/technical/indirect-calls.md`.

- **Default: guest threads unlocked.** They run in parallel on host threads. `RECOMP_GUEST_CPUS=one` pins title
  threads to one core, which restores the console's interleaving for racy title code.
- **Opt-in guest CPU lock** (`RECOMP_GUEST_LOCK=1`, `kernel_guest_cpu.c`), with measured evidence for the policy:
  "32 flips/s with the lock off, 28.5 with a critical section, 12.7 with a fair ticket lock that switched at every
  kernel call". The policy:
  - **Soft release**: a kernel call that returns at once switches nobody. The caller marks the CPU free and barges
    back if nobody took it.
  - **Blocking calls** wake waiters. A waiter also re-polls every 2 ms.
  - **Quantum** `RECOMP_GUEST_QUANTUM` (default 4 ms). The *waiter* sets `g_xbox_guest_cpu_preempt` once it has
    waited a quantum. The holder only does one relaxed load of that flag at kernel calls and loop back edges, and
    never reads a clock.
  - **Tickets**: a waking thread barges (like a dispatcher priority boost); a preempted thread queues behind every
    waiter.
- **Dispatch**: binary search over 22K entries ("negligible at ~120 ICALLs per second"). Their register model uses
  globals, not a CPU struct.
- **Fit for AC1**: this is the most directly applicable scheduler design for observations A and B. The runtime-only
  change replaces `scheduler_return_checkpoint`'s 64-return/clock check with a flag load.
- **License**: MIT (`kernel_guest_cpu.c` is MIT).

### 1.5 Recomp Kit: [veritr1x/recomp-kit](https://github.com/veritr1x/recomp-kit) (MIT; local copy `D:/Prog/dukemp_recomp/.deps/recomp-kit` @ 487b94a)

- **Memory**: a flat `g_mem` arena (256 MB default). `rd32` is `g_mem + a` with `always_inline`
  (`RECOMP_HOT_INLINE`, `runtime/x86.h`).
- **Direct calls**: `CALL_FN(a)` calls `FN(a)(c)` directly (`translate.py` L3177-3192, macro L5702-5715).
  - The only overhead is an acquire load of `recomp_hooked[i]` for run-time hooks, plus optional frame-watch and
    profile checks.
  - Indirect calls use `recomp_call`, with a 16K-entry atomic direct-mapped cache (`(target<<32)|index` in one word)
    in front of a binary search (L5886-5905).
  - Import calls go to trampolines indexed by `(target - TRAMP_BASE) / STRIDE`: O(1), no strings.
- **Flags**: per-function straight-line liveness (`Translator.liveness`, L2110). All flags are live at
  CALL/RET/JMP/Jcc, and any flag write that is overwritten before being read is removed. `--check-flags` measures
  CFG-wide liveness.
- **Threads**: one baton under `g_sched_m` plus one `condition_variable_any` with `notify_all`, the same shape as
  AC1's.
  - `sched_checkpoint` (`kernel32.cpp` L2816) runs only at import trampolines (`imports.cpp` L415), rate-limited to
    one yield per ms per thread.
  - Atomic imports and `sched_atomic_enter` stretches never yield.
  - `k_GetTickCount` detects a spinning frame limiter and sleeps 1 ms instead of burning the core (L1550-1580).
- **Measured**: no performance numbers were found in its docs.
- **Fit**: lowest integration cost. Its x86 semantics are close to `cpu.h`, and it is MIT. Borrow the direct-call
  macro, the straight-line flag liveness and the import trampoline indexing.

### 1.6 KillzoneRecomp: [docs/perf_research.md](https://github.com/drakolordx7/KillzoneRecomp/blob/master/docs/perf_research.md) (GPL-3.0; PS2)

- **Profile breakdown**: call + checkpoint overhead ~10–15 %, runtime helpers not inlined ~4 %, heap/string work in
  stubs 3–5 %, generated code ~45–50 %.
- **Fibers** (proven in the DQ8 PS2Recomp fork, commit 8b83b2b): an unwind-and-rebuild thread switch cost "~10 µs;
  a fiber round trip costs 22 ns". Guest dispatches per movie frame went 82,441 → 353.
- **Direct `jal` calls with an inline flag test** instead of a per-call checkpoint. Its sources: N64Recomp, and
  PCSX2's `cycle >= nextEventCycle` test with the full event check at most every 3,072 cycles.
- **Registers as locals.** XenonRecomp credits this as its biggest single gain ("frame times are reduced by several
  milliseconds") and requires Clang.
- **Scheduler fixed costs** (DQ8 commits 2e538a7/20973e8): a 128-queue walk per dispatch was ~7 %, and a mutex was
  taken just to ask "anything queued?".
- **Release flags**: `/Ob1` stopped inlining of hot helpers inside huge generated functions, and stack cookies,
  incremental-link thunks etc. showed in the profile.
- **Fit**: these are mostly confirmations. AC1's `scheduler_checkpoint` per import is the same "mutex to ask
  anything queued" pattern.
  - Check that GCC actually inlines `rd32`/`flags_*` inside the largest generated functions. Use
    `always_inline` as recomp-kit does.
  - Fibers would cut hand-off cost to almost nothing, but only while execution stays serialized (§3).
- **License: GPL-3.0. Ideas only; no code.**

### 1.7 Local: `third_party/nfsu2-reference` ([codepdbh/nfsu2-recompiled](https://github.com/codepdbh/nfsu2-recompiled) @ 927333d; README says MIT tooling, no LICENSE file in the copy)

- `patches/pcrecomp/0012`:
  - `RECOMP_LOCAL_REGS` keeps the eight registers in locals, written back at calls and exits through
    `RECOMP_RETURN()`. It does this by rewriting `return;` on the text, so no emitter can forget it.
  - `RECOMP_MEM_QUAL` drops `volatile` from memory macros.
  - Validated on 1,475 function cases (clang -O2, non-volatile memory, local registers) against MSVC /O1.
- `0014` `RECOMP_X87_WINDOW`: a sliding window over a 256-slot buffer instead of shifting eight registers. AC1
  already rotates `fpu_top`, so this does not apply.
- `0005` `RECOMP_BACKEDGE`: a yield every 65,536 back-edges so spin-waits cannot starve peers.
- `docs/ANDROID_PERFORMANCE_2026_10_16.md`:
  - The render thread spent 56 % of wall time in `MachineMutex::lock`.
  - The slice deadline was a shared static, so the incoming thread "inherited an expired slice and handed the
    machine straight back (ping-pong)".
  - The fix: per-acquisition slices, 3 ms for the presenting thread, and a yield once after each Present.
  - Results: 27.7 → 34.3–36.1 FPS at ~19k calls/frame and 20.3 → 27.8 FPS at ~25k. Wait dropped to 5–7 ms of a
    28–36 ms frame, with 7–9 hand-overs per frame.
  - Also: `-Bsymbolic` removed PLT stubs from the lookups taken on every indirect call (Android).

## 2. Inherent to x86 vs artifacts of the first implementation

| Inherent (keep, make cheap) | Artifact (remove) |
|---|---|
| Flags where something reads them (jcc/setcc/cmov/adc/pushfd/lahf); x87 compare→`fnstsw`→`sahf` chains | All six flags eagerly on every op (E). AF/PF parity is almost never read |
| Guest stack in guest memory: `push`/`call` must store, because code reads `[esp+n]` and the return address | A store to `c->esp` and every register on every instruction (no locals, H) |
| An indirect call needs an address→function map | `dispatch()` for 245k static `call imm` sites; diagnostics + emutls + `find_import` before the table lookup (I, D) |
| `lock` RMW must be a host atomic (31k sites) | Atomics through `guest_ptr` region lookup instead of the arena (G) |
| x87 stack top and 80→64-bit precision policy | Per-instruction FPU env/tag bookkeeping (F); `ST(i)` indexed through `c->fpu_top` at run time |
| Per-thread CPU, stack, TIB and TLS | Reaching them through emutls on both hosts (D) |
| Guest waits and CS semantics; x86-TSO ordering *if* threads run in parallel on ARM64 | Hand-off at every import, broadcast wake, clock read every 64 returns (A, B) |
| Import calls cross into host code | String-matching the import on every call (C) |
| Unknown-target diagnostics | Running every trace check on every call, even with tracing off (I) |

The direct-memory result shows the pattern: the old per-access region scan was an artifact worth 3.5x. Items A–I are
the same kind of cost.

## 3. The safest path toward multicore guest execution

Evidence from the projects:

- **Parallel by default:** Pinyon (PPC: a weakly ordered guest written for 6 hardware threads) and XboxRecomp (an
  x86 guest on an x86 host).
- **Serialized:** recomp-kit, both NFSU2 trees and current AC1.
- **Why serialization was safe for NFSU2:** NFSU2 has "0" `lock`-prefixed instructions (nfsu2-reference
  `docs/THREADING.md`), so it relied on the single CPU.
- **Why AC1 differs:** it has 31,296 `lock xadd` + 147 `lock cmpxchg` sites and Interlocked/CS imports. It was
  written for multicore PCs, so its own synchronization is real.
- **What still blocks parallelism:**
  1. The runtime's own shared state is protected only by the baton.
  2. On ARM64 hosts, plain guest loads and stores lose x86-TSO ordering.
  3. GCC may reorder plain `memcpy` guest stores, even on x64.

Recommended stages, each shippable on its own:

1. **Cheap serialization first** (§4 items 1–3).
   - Hand-offs only when someone waits a quantum, at a blocking call, or at a soft release.
   - Targeted wake: a per-thread `CONDITION_VARIABLE`/event, or `WakeConditionVariable` on a per-slot CV.
   - Slices counted from acquisition.
   - This should take the 2,900/s hand-offs to the order of hundreds, without semantic change.
2. **Release the baton around host work.**
   - Covers file I/O, `Present`/D3D9 waits, audio and sleeps: nfsu2recomp's "GIL released only in blocking calls",
     and nfsu2-web's real CS mutexes with the GIL released while waiting.
   - The host must not touch guest-owned state without the baton, or must use its own locks.
   - This alone can lift core usage above ~1.1 without any guest code running in parallel.
3. **Make runtime services baton-independent.** Per-service locks or ownership for:
   - heap (`pcrecomp_heap.c`), handle/event/semaphore tables, TLS bitmap, file service, the D3D9/DS8/DInput bridges,
     the lookup caches (make slots single-word atomic as recomp-kit does), and the profile counters.
   - Guest CS and waits as native primitives with direct wake (Pinyon: 2,718 → 19 lookups/frame, wake-on-signal
     −6 to −9 % CPU).
   - Keep the baton as an outer lock while this happens. Each service gets a focused stress test.
4. **Opt-in parallel workers, x64 host first.** Run a whitelisted set of guest threads (identified by start
   address, e.g. job/streaming workers) without the baton, while the main/render thread keeps it.
   - Follow XboxRecomp: a default-off environment switch plus a one-core pinning escape hatch for racy title code.
   - Also make guest stores compiler-ordered: every guest memory access a relaxed atomic, or at least
     `asm volatile("" ::: "memory")` at calls and back edges. On x64 the hardware is TSO, so ordering only needs to
     survive the compiler.
5. **ARM64 parallelism last.** x86 code assumes store→store and load→load order. Android cores do not offer a TSO
   mode, so guest accesses would need `ldapr`/`stlr` (RCpc) or barriers.
   - Its cost is unmeasured and could erase the parallel gain. Measure on hardware before committing.
   - Until then, Android stays serialized at stage 3.

Fibers (DQ8's 10 µs → 22 ns hand-off) are the alternative to stages 2–5. They make serialized switching nearly free,
but they rule out parallelism and need all `_Thread_local` guest state moved into per-fiber structs. Because AC1 is
written for multicore, prefer stages 1–3. Consider fibers only if stage 1 leaves hand-off cost dominant and
parallelism is abandoned.

## 4. Ranked experiments (top 8)

Gains are estimates for AC1 unless a source is named. "Cost" is engineering effort. "Risk" is correctness risk.

| # | Experiment | Bottleneck | Borrow from | Expected gain | Cost | Risk |
|---|---|---|---|---|---|---|
| 1 | **Baton policy.** (a) No checkpoint after non-blocking imports; use XboxRecomp's soft release/barge-back. (b) A 2–4 ms quantum counted from acquisition. (c) The waiter sets an atomic `preempt` flag; the holder tests it (one load) instead of counting returns and reading the clock. (d) Wake only the chosen thread, not `WakeAll`. Keep `AC1_SCHEDULER_SLICE_US` as the A/B switch. | handoffs | XboxRecomp `kernel_guest_cpu.c`; nfsu2-reference Android doc; nfsu2-web `guest_fair_yield` | High confidence the hand-off rate drops ≥ 5x. NFSU2-Android went from 56 % lock wait to ~5–7 ms/frame and +24–37 % FPS; XboxRecomp saw 12.7 vs 28.5 flips/s from policy alone. Plausibly 10–25 % CPU here. | Low (runtime only, ~200 lines) | Low–med: starvation of the audio/streaming threads, CS ordering. Watch per-thread wait p99 and audio underruns. |
| 2 | **Remove emutls from hot paths.** Embed `CPU` in the per-thread struct and reach `dispatch_depth`, `last_guest_target`, `dispatch_count` and the active thread through `c` (`container_of`); pass the thread to `scheduler_*`. Alternatively build Windows with clang/llvm-mingw (native TLS) and raise Android minSdk to 29 (ELF TLS). Verify first with call stacks: is "winpthread" `pthread_getspecific` called from `__emutls_get_address`? | dispatch, handoffs | none (fixes our toolchain issue; Pinyon reads the current thread from the PCR) | Medium confidence that much of the 27 % "ntdll+winpthread" is TLS, not locks. If confirmed, 5–15 %. | Low–med | Low |
| 3 | **O(1) import and atomic paths.** Resolve each `import_record` to a handler function pointer + small id once at load (keep the unknown-import diagnostic). Inline `lock xadd/cmpxchg/xchg` as `__atomic` on `ac1_direct_arena + a`. | dispatch | nfsu2recomp COM name→id (12.60 → 11.06 ms, strcmp was 11 %); recomp-kit trampolines | 2–10 %, depending on import call rate (CS/Tls/Interlocked/QPC are hot in such engines) | Low | Low (same handlers; cover with a test that every pinned import resolves to the same handler as before) |
| 4 | **Direct calls + slim dispatch** (a worker is implementing this). (a) Emit `L_XXXXXXXX(c)` for static targets, with an inline `if (unlikely(ac1_poll))` check instead of `scheduler_return_checkpoint` per return. (b) Move all trace checks behind one `unlikely(ac1_dispatch_diag)` global. (c) For indirect targets, an O(1) table: `idx = line_tab[(a-TEXT_BASE)>>4]` (~3 MB uint32 for ~12.6 MB `.text`) checked against `func_addrs[idx]`, falling back to binary search when two entries share a 16-byte line. (d) `dispatch_jmp` as a real tail call (`musttail` on clang; check that GCC emits `jmp`). | dispatch | recomp-kit `CALL_FN`; nfsu2-web `DISPATCH_CALL`/`g_dtab`; Killzone 2.2 | Medium–high. nfsu2-web credits the inline indirect path as part of −25 % p50; Killzone's call/checkpoint bucket was 10–15 %. | Medium (lift_ac1 + prepare_runtime) | Medium: host stack depth (keep a depth guard per N calls), call-site semantics for `guest_callsite`, hook points, the `retpast` pattern (a callee that returns past its caller: nfsu2-web `tools/retpast.txt`) |
| 5 | **Break the alias barrier (MEMARG).** Generated functions take `(CPU *restrict c, uint8_t *restrict m)` with a thin `L_X(CPU*)` wrapper for tables; `rd32` uses `m`. Drop the per-access NULL test (direct builds always have an arena). Audit runtime code that touches the running CPU through another pointer during a lifted call; that would be UB. | reg/flag sync | nfsu2recomp MEMARG (−9.7 % instr, −11 % cycles); nfsu2-web `g_guest` | 5–15 % of generated-code time. Large if generated code's share grew after direct memory. | Medium (post-pass in `lift_ac1.py`, adapter in `prepare_runtime.py`) | Medium: `restrict` violations miscompile silently. Run differential checks on traced runs. |
| 6 | **Flag liveness.** Step 1: recomp-kit's straight-line liveness (kill flag writes overwritten before any read in the same run). Step 2: CFG liveness with a mask argument (`flags_add_m(c,a,b,sz,MASK)`, a constant mask so GCC deletes dead lanes, as nfsu2-web `FLAGS_*_M`). Step 3: FLAGABI proof across calls. | reg/flag sync | recomp-kit `liveness`; nfsu2-web `flagabi.py` | Our baseline has none, so more than NFSU2's −4.5 % cycles is likely. Once #5 lets flags live in registers, most `parity8` work disappears. | Medium (lift_ac1 post-pass over pcrecomp output, or a pcrecomp patch) | Low–med: `pushfd`, `lahf`, SEH filters and `int3` paths must see full flags. Treat indirect calls as F_ALL until proven. |
| 7 | **Hash-checked native overrides with a verify mode.** Targets: CRT `memcpy/memmove/memset/strlen`, `_ftol2`/`_ftol`, `_CIpow`-family math. Install only if the guest bytes' FNV-1a matches. `AC1_NATIVE_VERIFY=N` runs the original every Nth call and compares results and memory. Pick targets from the profiler's top generated functions. | generated code | Pinyon `guest_native.cpp` (memcpy/memset −11–14 %); nfsu2-web `overrides.c` (8.5 % → 2.9 %) | 3–15 %, workload dependent | Low–med per function | Low with verify mode (x87 precision differences: follow nfsu2-web and fall back to the lifted body for NaN/inf/subnormal) |
| 8 | **Register locals (LREGS) + x87 cleanup.** Port `lregs.py`'s dataflow to our `c->` output. Separately: emit FPU env stores only in functions that can reach `fnstenv/fnsave/fxsave`, or keep them lazily as a single `fpu_ip` store per block. Later, X87OPT block-local slots. | reg/flag sync | nfsu2-web `lregs.py` (−3.2 % on top of the alias fix); pcrecomp patch 0012; nfsu2recomp X87OPT (≈ −4 % instr, −6 % cycles alone) | 5–10 % of generated-code time after #5/#6 | Med–high | Medium: SEH/`longjmp` resumption, callbacks reading the CPU, hooks. Gate per function and A/B on traces. |

Not recommended now:
- **Blanket leaf inlining.** NFSU2Recomp measured +3.4 % cycles. Profile-guided inlining with a ~1 MB budget (nfsu2-web,
  −1.3 %) is worth trying only after #4–#6.
- **ThinLTO over generated code.** Noise or slower in both NFSU2 trees.
- **Fibers.** See §3.

Measurement discipline, from all three NFSU2/Killzone reports:
- Use instruction/cycle counts per frame on a deterministic replay (`beb549d` record/replay); single runs were ±7 %.
- Measure with the frame limiter off, and always with `-BuildType RelWithDebInfo` (the build default is Debug).
- Change one switch at a time, and keep each optimization behind an environment or CMake option for A/B.

## 5. License summary

| Project | License | Use |
|---|---|---|
| nfsu2-web | MIT (with an upstream-provenance note) | Ideas freely; copy code only with the MIT notice; prefer reimplementation |
| NFSU2Recomp | Proprietary, all rights reserved | **Ideas and published numbers only** |
| Pinyon Shift | BSD-3-Clause | Reusable with notice |
| XboxRecomp | MIT (LGPL-2.1 only for xemu-derived APU/nv2a files) | Reusable; scheduler file is MIT |
| recomp-kit | MIT | Reusable; closest code fit |
| KillzoneRecomp | GPL-3.0 | **Ideas only** |
| nfsu2-reference (codepdbh) | README states MIT tooling; no LICENSE file in local copy | Confirm before copying; patches 0012/0014 are pcrecomp-derived |

The AC1 repository itself has no top-level LICENSE file. Choose one before importing MIT/BSD code, so the notices have
somewhere to go.
