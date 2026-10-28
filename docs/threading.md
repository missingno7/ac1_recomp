# AC1 guest threads

## Current execution model

`src/runtime/ac1_runtime.c` runs guest workers on native OS threads with a guest CPU, stack, TIB, static TLS block/vector, dynamic TLS values, last-error state, and stop boundary per worker. Workers share the process image, guest address space, heaps, import table, TLS allocation bitmap, and synchronization objects. The primary and worker TIBs carry x86 self pointers and client IDs. The runtime has handlers for `CreateThread`, thread waits/exits, events, semaphores, `Sleep`, `SwitchToThread`, and thread-priority calls.

The traced startup has exercised `CreateThread`, worker `TlsGetValue`/`TlsSetValue`, `SetEvent`, and `WaitForSingleObject`. It now creates two workers; both have started. The current startup boundary is `WS2_32.dll!WSAStartup` (version 1.1, guest `WSADATA` at `0x7FEFF880`). Both workers are blocked there; shutdown wakes and joins them. No natural worker `ExitThread` return has been observed yet.

Guest instructions execute under a serialized baton. A native worker can wait while another guest thread owns the baton, but only one guest CPU executes guest instructions at a time. Native worker creation therefore does not currently provide multicore guest execution. Waits yield cooperatively, and teardown stops and joins workers before shared guest memory is released. `ac1_run` serializes launches so process-global runtime state is not reused concurrently.

## Parallel execution later

Relaxing the baton requires auditing guest shared-memory access, heaps, memory-map changes, handle tables, TLS bookkeeping, graphics callbacks, and process-wide service state. Those structures need explicit synchronization or safe ownership partitioning, followed by profiling to show guest-thread parallelism helps. Do not pin host workers to physical cores by default; guest affinity remains metadata until there is a demonstrated host scheduling need.

## Reference implementations

`D:/Prog/dukemp_recomp/src/runtime/guest_scheduler.c` informed the per-thread CPU and continuation model. `third_party/nfsu2-reference/android/app/src/main/cpp/guest_threads.inc` informed event and wait behavior. Neither is a drop-in module: both depend on title-specific process, memory, allocator, graphics, and stop APIs.

## Regression checks

`tools/build.ps1 -CheckRepeat` runs the pinned executable twice in one host process and checks that both runs reach the same genuine guest boundary. It exercises two-worker teardown and process-state cleanup sufficiently for a second launch. This is an integration check, not an independent scheduler stress test.
