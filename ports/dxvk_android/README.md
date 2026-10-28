# DXVK Native for AC1 Android

This is AC1's Android D3D9 backend, adapted from the Android WSI and build recipe in `codepdbh/nfsu2-recompiled` (MIT per that repository's README; copyright attribution: codepdbh). It fetches DXVK Native `native-1.9.2b` at `c8dc91fabd00cac11d697ccf07426e798393cd40` into ignored `work/deps`, applies the Android preparation recipe into ignored `work/dxvk/prepared`, generates Vulkan 1.1 built-in shaders with the Android NDK's `glslc` plus `spirv-val`, and builds an ARM64 shared library.

Build with the installed Android SDK/NDK:

```powershell
.\ports\dxvk_android\build.ps1
```

This only builds the renderer dependency. AC1 still needs a guest D3D9 COM bridge and an Android surface/lifecycle connection. The exported native API needed by that bridge is `Direct3DCreate9`; `dxvkAndroidSetWindow`, `dxvkAndroidGetWindowHandle`, and `dxvkAndroidSetRenderSize` connect the host surface. Guest COM pointers must remain 32-bit handles/vtable tokens in guest memory, mapped to 64-bit DXVK interfaces by a separate host-side table.

The NFSU2-authored adaptations retain the MIT notice in `LICENSE.nfsu2.txt`. DXVK Native is zlib-licensed (`DXVK-LICENSE`). Its bundled Windows/DirectX headers include Wine-derived material under LGPL-2.1-or-later; keep their notices with any redistribution. The supplemental Vulkan Android/Xlib headers retain Apache-2.0 notices. This port contains no game assets or generated game code. See `docs/graphics.md` for current integration limits.
