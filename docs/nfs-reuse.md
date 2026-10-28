# NFSU2 reference findings

Inspected `codepdbh/nfsu2-recompiled` at shallow commit `927333d` in `third_party/nfsu2-reference`. The checkout is clean and contains no game executable or generated game C. Its pcrecomp dependency is a sibling checkout at base `35548b8` with this repository's patches applied.

## Reuse decision

Keep the DukeMP lifter as AC1's initial generator, as planned. NFSU2 validates useful additions to compare or port when a concrete AC1 blocker appears: PE/basic-block catalog caching and function splitting in `scripts/run_lift.py`, x86 local-register and non-volatile memory options in pcrecomp patch 0012, host hooks in patch 0013, and the sliding x87 stack in patch 0014. NFSU2's `scripts/normalize_exe.py` repairs one NFSU2-specific relocation directory; use its analysis-copy pattern, not that patch's assumptions.

The Android target is a separate native CMake/Gradle build in `android/`, not the Windows host target. It compiles generated C directly for ARM64, maps 32-bit guest addresses into a reserved 4-GiB host virtual region, binds IAT slots to synthetic 32-bit import tokens, preserves guest CPU state, and implements POSIX-backed guest threads and services. See `android/app/src/main/cpp/{CMakeLists.txt,guest_runtime.cpp,guest_memory.h,guest_heap.h,guest_threads.inc,guest_kernel.inc,guest_files.inc,guest_input.inc,guest_audio.inc,arm64_guest.h}`. These files provide concrete designs; the HLE APIs, game hooks, launch paths and state are title-specific.

Its D3D9 bridge in `android/app/src/main/cpp/d3d9_bridge.cpp` maps guest COM objects to host DXVK objects and uses synthetic guest vtable tokens. This is the best reference for 32/64-bit COM marshalling. It contains NFSU2 assumptions and partial edge-case coverage, so first compare AC1's D3D9 call trace before porting it. `ports/dxvk_android/prepare.py` adapts pinned DXVK Native 1.9.2b for Android and its README records the DXVK, Wine-derived header, and Vulkan-Headers notices. It is a practical renderer foundation if AC1 uses D3D9, subject to confirming runtime/device compatibility.

## Evidence and limits

The project reports a race prototype on Android ARM64. `docs/ANDROID_GAMEPLAY_CONTROLS_TEST.md` (2026-10-08) records owner-confirmed Resort Loop gameplay with HUD, opponents, track, and accelerator input at roughly 35–51 FPS on a Galaxy S24 Ultra. This is stronger and later evidence than `docs/ANDROID_RUNTIME.md` (2026-10-07), which accurately describes its earlier splash-only milestone. The reports do not establish a complete race, full progression, save/load, or compatibility across devices. Treat it as a strong bring-up reference, not proof AC1's substantially different engine will work unchanged.

## Licensing

The repository README explicitly releases repository-authored lifter tooling, Android bridge, WSI wrappers, launcher and scripts under MIT. There is no separate top-level `LICENSE` file in this checkout; preserve the README grant and original attribution when adapting it. DXVK Native is separately zlib-licensed, Wine-derived headers are LGPL-2.1-or-later, Vulkan headers retain Apache-2.0 notices, and the Gradle wrapper is Apache-2.0, as documented in `ports/dxvk_android/README.md`. No proprietary game or generated code is present in this reference checkout.
