# Runtime sources

The guest memory, PE loader, clock and heap adapters were copied from the user's current local DukeMP working tree. `tools/prepare_runtime.py` records and checks the exact source hashes; `work/runtime-provenance.json` records each input. AC1 startup services live separately in `ac1_runtime.c`.

The checked scalar-copy adaptation in `tools/prepare_runtime.py` follows the existing approach in the user's local Mafia runtime (`src/runtime/guest_memory.c`): validate the entire guest interval before copying across separate host allocations. It preserves the one-region pointer API and the pinned Duke inputs; it does not read or modify the Mafia checkout during builds.

The ordinary-return scheduler slice in `ac1_runtime.c` follows the per-thread counter and deadline strategy in the local DukeMP `src/runtime/guest_scheduler.c`. AC1 checks its deadline every 64 generated-function returns; API checkpoints retain their immediate scheduling behavior. The implementation uses AC1's existing scheduler and keeps guest execution serialized.

`cpu.h` and `pcrecomp_heap.inc` derive from pcrecomp, copyright 2026 sp00nz, MIT; see `LICENSE.pcrecomp.txt`. The CPU header also contains helper fragments from recomp-kit, copyright 2026 veritr1x, MIT; see `LICENSE.recomp-kit.txt`. This applies to those reusable runtime implementations, not the original Assassin's Creed executable or its generated translations.

Generated game functions are private local build products under ignored `work/`. APKs and host executables that include those translations are likewise local build products. No game executable, assets, or translated game functions belong in the source repository.

`tools/lift_ac1.py` has a bounded MMX lowering for the two validated Havok record routines at `0x01023410` and `0x01023520`. It uses function-local MMX registers because both intervals are call-free, initialize every MMX read on all paths, and execute `EMMS` before every exit. This does not model general MMX/x87 register aliasing or x87 tag state; MMX instructions outside these pinned intervals remain unsupported diagnostics.
