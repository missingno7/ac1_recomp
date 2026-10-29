# AC1 guest threads

## Current execution model

`src/runtime/ac1_runtime.c` runs guest workers on native OS threads with a guest CPU, stack, TIB, static TLS block/vector, dynamic TLS values, last-error state, and stop boundary per worker. Workers share the process image, guest address space, heaps, import table, TLS allocation bitmap, and synchronization objects. The primary and worker TIBs carry x86 self pointers and client IDs. The runtime has handlers for `CreateThread`, thread waits/exits, events, semaphores, `Sleep`, `SwitchToThread`, and thread-priority calls.

The traced startup has exercised `CreateThread`, worker `TlsGetValue`/`TlsSetValue`, `SetEvent`, `WaitForSingleObject`, and contended critical sections. It creates five workers by input initialization. A trace recorded three complete wait, ownership-transfer and resume cycles on critical section `0x01A1609C`, with worker 4108 owning the lock when it resumed. Shutdown wakes and joins the workers before releasing shared memory. The summary distinguishes exits before teardown from blocked workers; diagnostic cancellation does not establish natural completion of game threads.

Guest instructions execute under a serialized baton. A native worker can wait while another guest thread owns the baton, but only one guest CPU executes guest instructions at a time. Native worker creation therefore does not currently provide multicore guest execution. Waits yield cooperatively, and teardown stops and joins workers before shared guest memory is released. `ac1_run` serializes launches so process-global runtime state is not reused concurrently.

The default scheduler slice is 1,000 µs, with a check every 64 ordinary guest returns and immediate yields at API and blocking boundaries. Setting `AC1_SCHEDULER_SLICE_US=0` preserves the previous scheduling path; guest execution remains serialized in either mode. On the same executable, the legacy run took 52.729 s (47,078,418 guest dispatches) and the 1,000 µs run took 25.616 s (45,051,075 dispatches), about twice as fast through startup. The traces are `work/startup_slice_legacy.log` and `work/startup_slice_1000.log`. Their workloads differ slightly because crypto initialization uses random input, so this is a startup comparison, not a gameplay benchmark.

## Parallel execution later

Relaxing the baton requires auditing guest shared-memory access, heaps, memory-map changes, handle tables, TLS bookkeeping, graphics callbacks, and process-wide service state. Those structures need explicit synchronization or safe ownership partitioning, followed by profiling to show guest-thread parallelism helps. Do not pin host workers to physical cores by default; guest affinity remains metadata until there is a demonstrated host scheduling need.

## Reference implementations

`D:/Prog/dukemp_recomp/src/runtime/guest_scheduler.c` informed the per-thread CPU and continuation model. `third_party/nfsu2-reference/android/app/src/main/cpp/guest_threads.inc` informed event and wait behavior. Neither is a drop-in module: both depend on title-specific process, memory, allocator, graphics, and stop APIs.

## Regression checks

`tools/build.ps1 -CheckRepeat` runs the pinned executable twice in one host process and checks that both runs reach the same genuine guest boundary. It exercises worker teardown and process-state cleanup for a second launch. `AC1_TRACE_IMPORTS=1` additionally logs critical-section wait, handoff and resume events so worker progress can be distinguished from main-thread progress. This is an integration check, not an independent scheduler stress test.

The installed MinGW toolchain reproduced a native heap failure during destruction of a `thread_local std::string` in a minimal program without any guest code, using either `CreateThread` or `_beginthreadex`. Bridge diagnostics now use trivial fixed-size TLS character buffers. The minimal replacement and the actual startup both completed worker teardown under GDB without the previous `0xC0000374` failure. Reordering the file service's destruction was not the fix.
