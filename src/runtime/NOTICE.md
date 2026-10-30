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

The portable Windows/system directory queries and Android virtual drive
enumeration in `ac1_runtime.c` follow recomp-kit `runtime/kernel32.cpp`'s
`GetSystemDirectoryA` and `logical_drive_strings` buffer-size contracts,
copyright 2026 veritr1x, MIT; see `LICENSE.recomp-kit.txt`.

Android single-monitor enumeration in `src/platform/ac1_window_android.c`
adapts recomp-kit `runtime/user32_vcl.cpp`'s `enum_monitors` clipping and
callback approach, copyright 2026 veritr1x, MIT; see `LICENSE.recomp-kit.txt`.

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

`tools/lift_ac1.py` lowers MMX through the CPU-owned eight-register file generated
by `tools/prepare_runtime.py`. AC1 and Bink share this lowering, including packed
integer saturation, full-width shift counts, shuffles, unpacking and SSE-era
MMX conversions. Registers persist across translated calls. The original Havok
interval proofs and native oracle remain regression tests; those routines now
use the same CPU state as audio and Bink.

The transformation preserves every pinned donor hash and records the prepared
header hash in runtime provenance. Its project-owned helpers set TOP to zero
and all tags valid on MMX entry; EMMS empties the tags without changing TOP or
register payloads. Raw significands and exponent words survive environment
saves, including after EMMS. x87 writes synchronize their physical-register
MMX views when MMX next enters. An x87 data instruction while MMX is active
produces an exact-address diagnostic and stops the translated function.
Because environment images have no separate MMX-mode bit, FRSTOR conservatively
recognizes a full, TOP-zero stack containing an MMX-shaped `0xffff` exponent;
an unusual full x87 stack containing negative NaNs can trigger that diagnostic.

FNSTENV/FLDENV use the 28-byte protected-mode environment; FNSAVE/FRSTOR add
eight logical ST registers in 80-bit format for 108 bytes. Control/status words,
physical tags, TOP, instruction/opcode and data pointers/selectors are retained;
FNSTENV masks exceptions after storing and FNSAVE initializes the FPU afterward.
The underlying x87 arithmetic remains binary64: extended precision/range and
NaN payloads can be lost when values participate in x87 computation. Precision
control bits are recorded but do not create 80-bit arithmetic. Unmodified raw
restored register payloads retain their saved representation.

MMX floating conversions honor MXCSR rounding, DAZ and masked invalid/precision
flags; unmasked exceptions receive a diagnostic rather than a guest exception.
x87 integer conversions honor the saved rounding control. Other x87/SSE
arithmetic retains the existing model's precision and exception limitations,
including incomplete empty-stack/FXAM behavior.
MOVNTQ uses the ordinary checked guest store; the cache hint has no modeled
effect. Sixteen-bit environment layouts and segmented MMX memory operands
remain explicit diagnostics.

Instruction/layout references: Intel's [instruction-set manual](https://cdrdv2-public.intel.com/774492/325383-sdm-vol-2abcd.pdf)
and [MMX state-management description](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366821.pdf).
Native and generated-code checks are in `tests/run_mmx_general.py` and
`tests/run_mmx_havok.py`; generated functions and oracle binaries stay private
under `work/`.

`src/platform/ac1_gamepad.c` reuses the XInput wire layouts, button masks,
packet-state and disconnected-device design from the local recomp-kit
`dx/xinput.cpp`, copyright 2026 veritr1x, MIT. The full license is retained in
`LICENSE.recomp-kit.txt`. The AC1 discovery node and controller-selection policy
follow the pinned executable's existing DirectInput-to-PadXenon branch; actual
controller gameplay continues to execute the original translated game functions.
