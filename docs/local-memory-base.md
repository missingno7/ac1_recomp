# Captured guest-memory arena

Accepted 2026-10-30: in the calibrated fixed-clock gameplay comparison the
captured arena cut main-thread cycles/frame by 15.3% (95% CI -22.2% to
-8.5%; 23.6 -> 27.8 FPS); see [perf-architecture.md](perf-architecture.md).
The candidate notes below are kept as written (base main `2eacb46`).

## Implementation and correctness

Every generated CPU function captures `ac1_direct_arena` once. The prepared
CPU header passes this value through the transitive closure of 42 memory
helpers; guest byte stores cannot alias the local pointer. Direct accesses
become plain loads/stores without the redundant nullable-arena fallback.
Unaligned atomics retain the original checked fallback and diagnostic.
Checked-memory builds retain their existing implementation.

The process run lock and guest-thread teardown keep the reservation stable
through guest execution. A later run captures its new reservation. Startup
already rejects a failed reservation; its diagnostic now includes bytes,
guard size and the native allocation error. This works with ASLR on Windows
and Android ARM64 without choosing a fixed virtual address or making a false
`const` accessor promise.

An empty inline-assembly input observes only `c->eip` at each direct access.
It emits no instructions and preserves the existing tracing store for fault
diagnostics after opaque fallback calls disappear. No EIP optimization or
tracing mode is included. Region continuations capture their own base;
journal verification adapters retain their existing intercepted memory API.

Validation: 97 Python tests and lifter self-tests passed. New contracts cover
4096 full CPU/memory cases with two real arenas, scalar/vector/ext80/stack
accesses, FPU save/restore, aligned atomics, matching unaligned/cross-span
diagnostics, forced reservation failure, region continuation/journal C
compilation, and exact read/write fault EIPs with and without register locals.
The existing native instruction/audio/Bink oracles also passed.
No gameplay replay or Android hardware verification is claimed for this patch.

## Assembly and isolated measurement

Actual MinGW x64 objects and separately compiled actual generated bodies with
Android NDK 28.2 Clang, API 28, `-O2` were disassembled. The ARM64 check covers
these bodies and their real prepared headers, not a complete APK build.
Counts are static instruction sites, not dynamic retired instructions.

| Guest function | x64 arena-value loads | ARM64 arena-value loads | x64 instructions | ARM64 instructions |
|---|---:|---:|---:|---:|
| 004F5A70 | 93 → 1 | 193 → 1 | 5113 → 2850 | 4635 → 3031 |
| 00428540 | 23 → 1 | 68 → 1 | 1397 → 566 | 1418 → 698 |
| 0051DD60 | 4 → 1 | 11 → 1 | 371 → 242 | 353 → 197 |

The single entry load feeds plain memory accesses across stores and calls;
the nullable-arena branch and its checked fallback disappear. Relocation/GOT
address formation is separate from the arena-value load counted above.

A private two-TU microbenchmark executes five million dependent read/write
iterations with setup and full CPU/64 KiB comparisons outside timing, no LTO,
and seven balanced pairs. Global/local medians: 15.1614/14.4018 ms; median
paired elapsed change −5.156%, spread [−6.696%, −2.296%], identical checksum
`c104c915`. These shared-host numbers are exploratory and isolate memory
access cost; they do not establish a gameplay win.

Stripped PE size: 250,700,658 → 157,614,428 bytes; `.text`:
240,724,936 → 147,912,584 bytes. Baseline native build: 712.80 s plus 361.71 s
preparation. The first candidate build compiled the ordinary shards but found
two region scope errors; after their generator fixes, the successful cached
retry took 33.60 s native plus 353.56 s preparation. This is not a clean
candidate compile-time comparison.

Private evidence is under `work/global/agile-objects/`, `micro/`, and
`agile-{default,memory}-binary.json`; generated code and binaries are excluded
from the patch. Next: one controlled interleaved comparison after calibration,
including workload equivalence, CPU ms/frame and pairing spread.
