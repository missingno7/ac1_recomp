# Runtime sources

The guest memory, PE loader, clock and heap adapters were copied from the user's current local DukeMP working tree. `tools/prepare_runtime.py` records and checks the exact source hashes; `work/runtime-provenance.json` records each input. AC1 startup services live separately in `ac1_runtime.c`.

The checked scalar-copy adaptation in `tools/prepare_runtime.py` follows the existing approach in the user's local Mafia runtime (`src/runtime/guest_memory.c`): validate the entire guest interval before copying across separate host allocations. It preserves the one-region pointer API and the pinned Duke inputs; it does not read or modify the Mafia checkout during builds.

The ordinary-return scheduler slice in `ac1_runtime.c` follows the per-thread counter and deadline strategy in the local DukeMP `src/runtime/guest_scheduler.c`. AC1 checks its deadline every 64 generated-function returns; API checkpoints retain their immediate scheduling behavior. The implementation uses AC1's existing scheduler and keeps guest execution serialized.

The main-module exact-address lookup cache in `ac1_lookup_cache.h` follows the 1024-slot direct-map lookup strategy in the user's local DukeMP `src/runtime/lifted_lookup.h`; it verifies full guest addresses and falls back to AC1's original binary search on collisions.

`cpu.h` and `pcrecomp_heap.inc` derive from pcrecomp, copyright 2026 sp00nz, MIT; see `LICENSE.pcrecomp.txt`. The CPU header also contains helper fragments from recomp-kit, copyright 2026 veritr1x, MIT; see `LICENSE.recomp-kit.txt`. This applies to those reusable runtime implementations, not the original Assassin's Creed executable or its generated translations.

`src/platform/ac1_bink_native.cpp` adapts recomp-kit `dx/bink.cpp`, copyright
2026 veritr1x, MIT. Its full MIT permission and warranty notice is retained in
the provider source and `LICENSE.recomp-kit.txt`.

`ac1_win32_compat.inc` adapts the portable locale table, environment isolation
and FILETIME approach from recomp-kit `runtime/kernel32.cpp` and its Win32
service interfaces (`runtime/kernel32_internal.h`, `runtime/user32.cpp`,
`runtime/misc.cpp`, `dx/xinput.cpp`), copyright 2026 veritr1x, MIT.
The full permission and warranty notice is in `LICENSE.recomp-kit.txt`.

Native Bink decoding links FFmpeg 8.1 (official n8.1 commit
9047fa1b084f76b1b4d065af2d743df1b40dfb56), copyright the FFmpeg developers and
individual source authors, under LGPL version 2.1 or later. The selected build
enables no GPL or nonfree components. The unmodified upstream sources and
`COPYING.LGPLv2.1`, generated configuration and private static archives live
under ignored `work/bink_native/`; build recipes are in `ports/ffmpeg/`.
Any distribution of a linked runtime must provide the applicable notices,
corresponding FFmpeg source/build configuration and the LGPL-required means to
relink a modified FFmpeg. This notice does not authorize redistribution of
original AC1 assets, translated game code or their linked private executables.

Generated game functions are private local build products under ignored `work/`. APKs and host executables that include those translations are likewise local build products. No game executable, assets, or translated game functions belong in the source repository.

`tools/lift_ac1.py` has a bounded MMX lowering for the two validated Havok record routines at `0x01023410` and `0x01023520`. It uses function-local MMX registers because both intervals are call-free, initialize every MMX read on all paths, and execute `EMMS` before every exit. This does not model general MMX/x87 register aliasing or x87 tag state; MMX instructions outside these pinned intervals remain unsupported diagnostics.
