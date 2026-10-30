# Critical-section span investigation

The captured stop at `0x007a568e` is a runtime range-validation failure. The
24-byte x86 critical section begins at `0x2574fff0`: sixteen bytes precede
the `0x25750000` boundary and eight follow it. `guest_memory_try_ptr` requires
one backing region for the entire request, even in direct-memory mode. That
contract is appropriate for callers that dereference the returned host pointer,
but it cannot be used to decide whether all bytes of a guest object are mapped.

The first local unattended replay reproduced the stop with the diagnostic build:
`InitializeCriticalSectionAndSpinCount`, EIP `0x007a568e`, thread `4104`,
CS `0x2628fff4`. All six words were readable. The live allocations
`[0x26280000,0x26290000)` and `[0x26290000,0x262a0000)` had both been allocated
at guest `0x007a2648` (the original VirtualAlloc wrapper), on thread `4104`.
The stop occurred at about present 1425, after 16.895 seconds of native game
execution, exit code 8. Private evidence: `work/cs/before/run-01/startup.log`.
This is a false rejection of a mapped span; coalescing mapping records or
ignoring unmapped memory is unnecessary.

## Object and lifetime

Inspection of the fingerprinted local executable identifies the primary vtable
at `0x016899cc` as `scimitar::ActionComponent`. Its constructor at `0x00505650`
constructs the lock wrapper at object `+0x4c`; wrapper constructor `0x007a5680`
stores its embedded CS address at wrapper `+0` and initializes the CS at
wrapper `+4` with spin count `0x1000`. Thus the captured object `0x2574ffa0`
owns exactly the CS pointer in the stop. The import at this site is
`InitializeCriticalSectionAndSpinCount`.

The registered factory `0x00505d70` supports placement construction and, when
no storage is supplied, requests `0x110` bytes aligned to sixteen through
`0x00924070` and allocator dispatcher `0x007a4510`, then calls placement helper
`0x00505b30` and the constructor. `0x007a4510` forwards to the original game
allocator's virtual allocation method. These are original guest functions;
the runtime's donor heap reserves at or above `0x30000000`, so it did not hand
out the captured `0x2574ffa0` block.

Read-only inspection of the owned fixed process identifies the default pool at
guest `0x01a15e84`, selected by `0x01a1e680`, as
`Gear::MemDefaultAllocator` (vtable `0x016a3ce4`). Its RTTI hierarchy contains
`Gear::MemAllocDl283`, and its allocation slot `0x007c5c30` jumps to
`0x007ad880`. For size `0x110`, selector `0x00924000` returns zero (the small
allocation table only covers sizes through `0x40`); the alignment/boundary test
in `0x00924070` retains the default pool. Backing comes through the original
VirtualAlloc wrapper `0x007a2610`, whose import instruction is `0x007a2648`.
The live inspection is in `work/cs/default-allocator-live.log`; it reads the
owned process without pausing it or changing memory.

Destructor `0x00505340` deletes the CS through wrapper `0x007a2ee0`. The
deleting destructor `0x00505800` calls this destructor before its conditional
release through `0x00f9dc10`. The captured call is initialization during
construction, before that cleanup path.
The selected Gear pool's free slot is `0x007c4d50`, a tail adapter to
`0x007a61d0`, reached through the memory-hook free interface. The original trace
does not contain an observed free of `0x2574ffa0`; the investigation establishes
the owner and release paths, not a fabricated allocation/free history.

The new reproduction is a second owner: `scimitar::SkeletonComponent`,
constructor `0x004e6db0`, wrapper at object `+0x150`, CS at `+0x154`.
Object `0x2628fea0` therefore produces exactly CS `0x2628fff4`; the call returns
to `0x004e6e59`. The shared failure across two original constructors confirms
that this is an adapter issue rather than an ActionComponent-specific path.

The earlier local-register experimental stop is not the same import site:
the executable's `0x007bfec4` tail jump calls **LeaveCriticalSection** through
`0x01a47b94`; its matching Enter call is at `0x007bfd4c`. No shared cause is
established for that experimental pointer corruption.

## Mapping and concurrency audit

The sorted-region transform in `tools/prepare_runtime.py` maintains disjoint
regions. Binary search selects the only region that can contain a whole raw
pointer span; it intentionally rejects a span across adjacent regions. Lookup
cache entries copy region records, so sorted insertion does not invalidate
their backing pointers. Removal invalidates the cache.

Guest execution, imports, VirtualAlloc/VirtualFree and heap operations run under
the scheduler baton. DirectSound guest allocations/frees are synchronous bridge
callbacks within guest imports. Native file completion writes its supplied
buffer and host OVERLAPPED state, without inserting/removing guest mappings.
The installed executable imports no timer/APC scheduling APIs. This audit found
no asynchronous host mapping mutation that would explain this rejection.

## Deterministic evidence

`tests/critical_section_span.c` uses the actual runtime import adapter and two
live, adjacent 64 KiB regions at the captured addresses, inserted in reverse
order. Before the fix, both direct and checked address spaces reject CS
initialization although all six words can be read. The native Windows oracle
initializes, recursively enters, leaves and deletes a CS crossing committed
memory successfully. The final oracle uses two independent native VirtualAlloc
reservations. Private pre-fix output is `work/cs/contract-before.log`.

The regression also exercises all six supported CS imports, recursive ownership,
deletion, a released tail and uint32 address wrap. Invalid spans must stop before
any write, preserving sentinel words. The scheduler contract repeats contended
ownership handoff with the boundary-spanning CS.

## Fix and verification

The CS adapter keeps its single-region fast check, then validates the complete
24-byte guest span with the existing checked scalar-copy helpers (16 + 8 bytes).
Initialization/deletion writes six guest words, so unrelated host backing in
checked mode works as well as the direct arena. Full validation precedes every
write. Holes and uint32 wrap still stop with the original diagnostic. Raw
pointer contracts, guest allocator behavior and original game code are unchanged.

Unmapped CS objects always emit word checks, neighboring region records and
nearby VirtualAlloc origins. `AC1_TRACE_CS_SPANS=1` also records accepted
cross-region operations, including a checked factory-frame provenance record
when the ActionComponent registered-factory chain is present. The region dumper
is preserved by `tools/prepare_runtime.py`; its pinned donor inputs are unchanged.

After the fix, the CS/native-Windows contract passes **163 assertions**, the
scheduler contract passes **73 assertions** (including cross-region contention),
and the existing scalar-memory/VirtualQuery contract passes. Repeated runtime
preparation produces the same guest-memory source hash. The runtime and memory
source pass ARM64/API28 syntax checks; no Android execution is claimed.

Fixed replay run 04 records CS `0x2688fff4` initialized across
`[0x26880000,0x26890000)` and `[0x26890000,0x268a0000)`, followed by successful
Enter/Leave operations. The sorted map has 4,036 entries and contiguous direct
backing for those distinct allocations. This exercises the repaired path in
the real game, beyond the isolated fixture.

Commands (from the repository root):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -BuildType RelWithDebInfo -Jobs 6
# If the known Ninja compiler probe stalls, use the same build directory with
# a fresh CMake configuration and -Generator 'MinGW Makefiles'.
& 'D:/Prog/dukemp_recomp/.venv/Scripts/python.exe' -B tests/run_critical_section_span.py
& 'D:/Prog/dukemp_recomp/.venv/Scripts/python.exe' -B tests/run_scheduler_contract.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_gameplay.ps1 -SkipBuild -Seconds 45
```

## Replay results and limits

Before: the supplied main-67a86f7 series failed **1/7** runs. The fresh local
diagnostic binary failed **1/1**, with the live mappings and six readable words
described above. After: **10 consecutive 60-second replays** completed without
a CS stop or another guest stop, followed by a successful **75-second extra
replay**. All eleven use executable SHA-256
`4434b8e524cf428357b33b12d3ad63b595a8e5090b623708ae5fabf624619eb0`
and input SHA-256
`364cbec638f686141ef4b55586497533da6da89c112461081a93f291363c3027`.

The ten-run batch is under `work/cs/after/run-01` through `run-10`; per-run
`run.json` records the executable hash, elapsed time, timeout and exit status.
The harness stopped each game at its deadline (`timeout=true`, exit `-1`),
rather than observing natural game exit. `work/cs/after/audit.json` records zero
guest stops in all ten. Run 04 contains 15 accepted cross-region CS operations.
In these measurements the timer included executable hashing before launch;
the first game therefore ran for about 43 seconds of the 60-second harness
limit. The final launcher hashes before starting its timer, preserving the
meaning of `-Seconds` for future runs. The binary and guest replay are unchanged.

Nine of the ten retained final captures visibly show the tutorial character,
world/NPCs, or tutorial UI. Run 09's final capture at present 2400 is white;
the log reports `draw_calls=1087159`, `ms=58020`, and no guest stop. Its earlier
captures were pruned before that visual ambiguity was noticed, so the white
frame's cause is not established. It is not evidence of a CS failure.
The extra replay (`work/cs/after-extra/run-11`) retains tutorial UI at present
2200 and the visible world/character at present 2900, with no guest stop.
No other guest-failure lines occurred after the fix. Extended play, other
levels, the old local-register prototype and Android hardware remain unverified.

Original/generated game code, binaries, screenshots and logs remain private
under ignored `work/` and `build/`. All owned game processes were stopped.
