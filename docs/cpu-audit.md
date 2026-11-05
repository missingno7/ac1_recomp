# Where the main thread's cycles go

Phase 0 of the consolidation: a read-only audit of profiles already on disk. No
build, phone run or production change was involved.

## Evidence

| Capture | Build | Scene | Stacks | Use here |
|---|---|---|---|---|
| `gameplay2` | c33daae, symbols match by build ID | Crowd, 570 frames in 20 s | No | Costs |
| `gameplay1` | c33daae, symbols match by build ID | Quiet, 772 frames in 20 s | No | Comparison |
| `busy1-cg` | d99a16a, older | City scene, 6 s | Yes, 2,252 samples | Call paths only |

All figures are exclusive sampled cycles of the main thread (`cpu-cycles`), so a
sample belongs to one symbol and one category and nothing is counted twice.

Limits that apply to every number below:

- **Sampled attribution is not causal.** A cost here is not a saving. Removing a
  function can move cycles elsewhere or shift a wait.
- **User-space only.** No kernel samples are present, so futex and GPU driver
  syscalls are missing from these totals.
- **One capture of each scene,** at 9-11% battery with big cores capped at 2.38 GHz.
- **The call paths come from another build and a city scene.** They show which
  paths exist, never how much each costs in gameplay.
- **Skid.** Hot instructions are neighbourhoods, not exact culprits.

## Exclusive categories

Mcycles per frame. This replaces the "runtime" and "other host" split in
`docs/cpu-state-assessment.md`, which was a regular-expression split that put
`strcmp`, audio and D3D9 lookups under "other".

| Category | Crowd | Quiet | Share of crowd |
|---|---|---|---|
| Translated game code | 37.23 | 25.10 | 51.3% |
| Import handlers, including `strcmp` | 5.86 | 6.99 | 8.1% |
| Clock reads | 5.75 | 6.96 | 7.9% |
| Audio mixer and service | 5.35 | 4.63 | 7.4% |
| D3D9 bridge | 4.60 | 3.75 | 6.3% |
| DXVK and driver | 4.41 | 3.24 | 6.1% |
| Dispatch core | 2.67 | 2.39 | 3.7% |
| DXVK atomics | 2.04 | 1.74 | 2.8% |
| Locks and other atomics | 1.58 | 1.70 | 2.2% |
| Guest memory helpers | 0.89 | 0.65 | 1.2% |
| Scheduler | 0.51 | 0.37 | 0.7% |
| TLS resolver | 0.48 | 0.41 | 0.7% |
| Native binding wrapper | 0.44 | 0.20 | 0.6% |
| Everything else | 0.70 | 0.56 | 1.0% |
| **Total** | **72.5** | **58.7** | |

The crowd adds 12.1 Mcycles of game code per frame and only 1.7 of everything
else. Non-game cost is nearly flat per frame between the scenes.

## Ranked findings

"Ceiling" is the sampled cost of the category: the most that could vanish, not an
expected saving.

### 1. Imports are resolved at run time by comparing strings

- **Measured:** `strcmp` 2.09 Mc (2.9%), and handlers 3.77 Mc more. The route
  cache in `run_import_handlers` picks a handler once, but the handler then
  compares `import->name` against its own list on every call
  (`src/runtime/ac1_runtime.c:2673` and the rest of the chain).
- **Call paths:** `strcmp < handle_heap_import < dispatch < game` and the
  `wc_*_import` chain under `handle_win32_compat_import`.
- **Static fix:** the import table is fixed, so each import can resolve at
  generation time to one direct handler. No string compare, chain or token decode
  remains. A runtime `GetProcAddress` can keep using the name path.
- **Ceiling:** 5.86 Mc (8.1%). `strcmp` itself is pure overhead, so that 2.9% is
  the firm part. Real handler work, such as allocation, remains.
- **Confidence:** high that the string work is unnecessary. Which imports are
  hot is unknown, because no profile records import counts per name.

### 2. Clock reads survive the earlier sampling fix

- **Measured:** 5.75 Mc (7.9%), in a build that already has
  `AC1_IMPORT_CLOCK_INTERVAL`. The earlier label "polling" is misleading.
- **Call paths, older build:** 44% return checkpoint, 25% `scheduler_refresh_waits`,
  17% audio `service()`, 16% import checkpoint. No path was under a game clock
  import in that capture, and not all of this can be polling for the GPU.
- **Fix direction:** the 1 ms slice does not need a precise clock on every check.
  Audio `service()` and `scheduler_refresh_waits` could share one reading.
- **Ceiling:** 7.9%, minus readings that guest time needs.
- **Confidence:** medium. The cost is measured, but its split in the crowd is
  unknown. It needs a stack capture on the current build.

### 3. Audio bookkeeping runs on the main thread on each scheduler refresh

- **Measured:** `advance` 3.63 Mc, `consume` 1.36 Mc, 5.35 Mc in all. Two thirds
  of `advance` sits at one point around the `b.frames` store and `b.looping` load
  (`src/platform/ac1_ds8_portable.cpp:217`).
- **Call path:** `scheduler_refresh_waits < ac1_ds8_service <
  ac1_audio_device_service < service < ac1_audio_consume < consume < advance`.
- **Suspected:** `consume` walks every pending buffer on each service even when
  the presented position has not moved, and each `Buffer` is large enough for
  cold-line misses. Whether the audio thread contends for those lines is untested.
- **Fix direction:** return early when `presented` is unchanged, which needs no new
  mechanism, or move the bookkeeping off the main thread.
- **Ceiling:** 7.4%. The work must still happen somewhere.
- **Confidence:** high on the cost, medium on the cause.

### 4. The D3D9 bridge pays a lock and a lookup on each call

- **Measured:** bridge 4.60 Mc, including `find_object` 1.28 Mc and
  `ac1_d3d9_dispatch` 1.86 Mc. Within `find_object`, about 70% is in the
  `g_objects` hash chain walk, meaning the small cache in front of it misses often.
  The bridge's mutex acquire is about 0.5 Mc, and `pthread_mutex_unlock`'s atomic
  swap another 0.8 Mc over all its callers.
- **Fix direction:** keep the host object pointer where the proxy can reach it
  directly. Check whether the lock can go, given that the scheduler already
  serializes guest code.
- **Ceiling:** 6.3%, minus the forwarding itself.
- **Confidence:** high on the cost. Removing the lock needs a thread-safety proof.

### 5. Hot code is scattered through the whole text section

- **Measured layout:** 90% of sampled game cycles sit in 812 functions and 1.37
  MiB of code, spread over 57.6 MiB of a 64.6 MiB `.text`. Half are in 64 functions
  and 161 KiB, spread over 53.8 MiB. 90% of cycles in the library touch 641 4 KiB
  pages; packed, 90% of game cycles would fit in about 350.
- **Missing:** no i-cache, iTLB or fetch counter has ever been recorded, so this
  shows only that pressure is plausible, not that it exists.
- **Static fix:** a linker symbol ordering file, using production's existing
  function sections. It adds no switch and no runtime cost.
- **Ceiling:** unknown.
- **Confidence:** the layout is measured, the cost is a hypothesis. The first step
  is one PMU capture.

### 6. Dispatch core

- **Measured:** 2.67 Mc (3.7%), including the catalog lookup and a per-call
  guest-site comparison (`src/runtime/ac1_runtime.c:7173`).
- **Static fix:** imports and COM calls that bind statically leave this path.
  Whole-program analysis may devirtualize some indirect calls.
- **Ceiling:** 3.7%.
- **Confidence:** medium; how much is import traffic is unknown.

### 7. Certain small removals

- **Native wrapper:** 0.44 Mc (0.6%) in `ac1_native_invoke`, paid with overrides
  disabled. Static selection removes it. Confidence high.
- **TLS resolver:** 0.48 Mc (0.7%) in `tlsdesc_resolver_dynamic`, from runtime
  thread-locals. Many callers already hold `CPU *`, so the guest thread can be
  reached from it. Confidence medium.
- **Atomics:** DXVK's outlined atomics are 2.04 Mc. Direct LSE instructions would
  remove only the call, because the atomic itself stays. Confidence low that this
  matters.

## Work against waiting

The stack capture has 6.83% of its cycles under the game's occlusion wait
(`0x00AA40B0`), led by clock reads, audio service and bridge calls. So some of
findings 2-4 run inside a GPU wait, and removing them there would not shorten the
frame. The crowd capture has no stacks, so this split is unknown for gameplay.

The main thread was 87% busy, but that includes active waiting, so it does not
prove a CPU limit.

## Suggested order

| Order | Action | Why |
|---|---|---|
| 1 | Static import binding | Firmest saving, largest category, and it matches the design rule |
| 2 | Remove the native wrapper | Certain, and part of static selection |
| 3 | Audio early return | Small and local, if the cause is confirmed |
| 4 | One stack capture and one PMU capture on the current build | Settles clocks, waits and instruction fetch |
| 5 | Hot-code ordering file | Only if the PMU capture shows fetch pressure |

The capture in step 4 needs the phone. None of this has been implemented.

Private scripts and data are in `work/phone/audit/`.
