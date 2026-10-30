# DXVK Native for AC1 Android

## Upstream 3.1 migration feasibility (2026-10-30)

The migration target is official upstream [`v3.1`](https://github.com/doitsujin/dxvk/tree/v3.1),
commit `70d7508c01201ed3d4bfb33da42ba834eafe3857` (annotated tag object
`a1596f766f236244686c0c0fffba4416f0e185b1`). The existing build below still uses
1.9.2b; it has **not** been replaced by 3.1.

An NDK-built probe ran on the Samsung Galaxy S24 Ultra / Adreno 750 / Android 16.
The proprietary driver advertises Vulkan 1.3.128 and 256-byte push constants.
All queried mandatory core feature bits pass, but `maintenance5` and
`maintenance6` are false, and these required extensions are absent:

- `VK_KHR_maintenance5`
- `VK_KHR_maintenance6`
- `VK_KHR_load_store_op_none` (the driver exposes only `VK_EXT_load_store_op_none`)

The requirements were checked against the pinned release's
[`getFeatureList` and compatibility checks](https://github.com/doitsujin/dxvk/blob/70d7508c01201ed3d4bfb33da42ba834eafe3857/src/dxvk/dxvk_device_info.cpp),
as well as its Vulkan profile. The release's API minimum is Vulkan 1.3;
the missing extensions prevent this driver from qualifying. Do not bypass
the checks or advertise unsupported functionality.

Reproduce the read-only query (no emulator or game data upload):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ports/dxvk_android/probe.ps1 -Serial <adb-serial>
```

Without `-Serial`, this only compiles the ARM64 executable. Output goes to
ignored `work/render/vulkan-probe/`; exit 2 means the query succeeded but found
no compatible candidate. It creates no Vulkan device or surface and therefore
does not establish renderer operation.

The alternative under evaluation is app-local Mesa Turnip through
[`libadrenotools`](https://github.com/bylaws/libadrenotools). Its documented API
supports rootless ARM64 driver loading. That requires packaged hook libraries,
`useLegacyPackaging = true`, and an internal app-private driver directory;
external game storage cannot hold executable driver libraries. A selected
Turnip build must be pinned, licensed, and probed through its loader before
claiming compatibility. No Turnip integration or DXVK 3.1 phone run is verified.

The implementation work includes the new upstream WSI platform interface,
NDK/C++ build changes and pinned submodules (including `dxbc-spirv`), retaining
the existing surface/render-size exports, and moving 3.1's shader cache to
app-private userdata. The old state/pipeline-cache implementation must not be
assumed compatible with the new release.

## Existing 1.9.2b build

This is AC1's Android D3D9 backend, adapted from the Android WSI and build recipe in `codepdbh/nfsu2-recompiled` (MIT per that repository's README; copyright attribution: codepdbh). It fetches DXVK Native `native-1.9.2b` at `c8dc91fabd00cac11d697ccf07426e798393cd40` into ignored `work/deps`, applies the Android preparation recipe into ignored `work/dxvk/prepared`, generates Vulkan 1.1 built-in shaders with the Android NDK's `glslc` plus `spirv-val`, and builds an ARM64 shared library.

Build with the installed Android SDK/NDK:

```powershell
.\ports\dxvk_android\build.ps1
```

This only builds the renderer dependency. AC1 still needs a guest D3D9 COM bridge and an Android surface/lifecycle connection. The exported native API needed by that bridge is `Direct3DCreate9`; `dxvkAndroidSetWindow`, `dxvkAndroidGetWindowHandle`, and `dxvkAndroidSetRenderSize` connect the host surface. Guest COM pointers must remain 32-bit handles/vtable tokens in guest memory, mapped to 64-bit DXVK interfaces by a separate host-side table.

The NFSU2-authored adaptations retain the MIT notice in `LICENSE.nfsu2.txt`. DXVK Native is zlib-licensed (`DXVK-LICENSE`). Its bundled Windows/DirectX headers include Wine-derived material under LGPL-2.1-or-later; keep their notices with any redistribution. The supplemental Vulkan Android/Xlib headers retain Apache-2.0 notices. This port contains no game assets or generated game code. See `docs/graphics.md` for current integration limits.
