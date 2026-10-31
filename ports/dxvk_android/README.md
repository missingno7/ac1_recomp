# Official DXVK 3.1 for AC1 Android

The Android D3D9 provider builds official upstream DXVK `v3.1`, commit
`70d7508c01201ed3d4bfb33da42ba834eafe3857`, matching the host release.
The standalone DXVK Native 1.9.2b recipe has been replaced. The pinned
submodules, including `dxbc-spirv` and its SPIR-V headers, are checked by
`prepare.py`. See [NOTICE.md](NOTICE.md) for pins and licenses.

Downloaded source stays under `work/deps/dxvk-3.1-turnip/`; platform changes
are applied to `work/dxvk/prepared-3.1/`. Normal builds use the official Git
checkout and recursive submodules. In restricted environments, a connected
GitHub snapshot may populate a submodule with `AC1_SOURCE_SNAPSHOT.json`;
preparation checks the revision and each file's pinned Git blob SHA. No
downloaded source, generated shaders, binaries or game files belong in commits.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ports/dxvk_android/build.ps1 -Abi arm64-v8a
# Optional cross-build, without launching an emulator:
powershell -NoProfile -ExecutionPolicy Bypass -File ports/dxvk_android/build.ps1 -Abi x86_64
```

The default builder is MinGW Makefiles (`C:/msys64/mingw64/bin/mingw32-make.exe`);
`-Builder Ninja` is also supported. Both use NDK 28.2.13676358. The build compiles
and validates 28 built-in shaders for Vulkan 1.3 with scalar block layout.
Build trees are `work/build/dxvk31-android-<abi>-<builder>/`; the stripped provider
is published as `work/build/dxvk-android-<abi>/libac1_dxvk_android.so` for the app.
The ARM64 and x86_64 providers have compiled successfully. Compilation alone
does not establish phone rendering.

## WSI, loader and cache

`android_wsi.cpp` implements upstream `WsiDriver`/`WsiBootstrap`, registered as
`Android`, with ANativeWindow surfaces, monitor/mode enumeration, window
lifetime and compositor buffer sizing. It preserves `Direct3DCreate9`,
`dxvkAndroidSetWindow`, `dxvkAndroidGetWindowHandle`, `dxvkAndroidSetRenderSize`,
`dxvkAndroidSetInstanceProcAddr` and `dxvkAndroidGetInstanceProcAddr`.
The loader must be selected before the first DXVK instance. All instance,
device and surface entry points use that loader; the provider has no dynamic
dependency on the system `libvulkan.so`.

Android returns `VK_SUBOPTIMAL_KHR` for every present while the swapchain's
identity pre-transform differs from the rotated landscape surface, and upstream
DXVK rebuilt the swapchain after each such frame (about once per present on
the S24 Ultra). `prepare.py` patches the presenter to keep the swapchain while
the surface's current extent still equals its images and let the compositor
rotate; extent changes, `VK_ERROR_OUT_OF_DATE_KHR` and surface loss still
rebuild it. The patched ARM64 provider builds (2026-10-31); its phone run is
pending.

JNI sets `DXVK_WSI_DRIVER=Android`. ARM64 defaults to
`AC1_VULKAN_DRIVER=turnip`, loading the pinned `ports/turnip/` package in-process
through libadrenotools. It verifies the actual driver is Mesa Turnip and rejects
silent system fallback. An explicit `AC1_VULKAN_DRIVER=system` is available for
diagnostics; this phone's current system driver fails the mandatory-extension
checks. Unknown selections, missing libraries, loader failures and device
enumeration failures produce precise startup/logcat diagnostics.

The APK uses `useLegacyPackaging=true`: both hooks and the driver are extracted
to the app's installed native-library directory. Gradle requires all six ARM64
Turnip package libraries under `work/build/turnip-arm64/package/arm64-v8a/`.
JNI locates its own extracted library directory with `dladdr`, without launcher
changes. Keep the Turnip worker's dependency sources and notices with any
redistribution; its optional integration patch is not needed by this port.

`DXVK_SHADER_CACHE_PATH` and `MESA_SHADER_CACHE_DIR` point to app-private
userdata. DXVK 3.1 uses its own `.dxvk.bin`/`.dxvk.lut` cache; the 1.9.2b custom
pipeline-cache implementation and supplemental Vulkan headers were removed.
The game installation remains read-only.

## Upstream fixes

`prepare.py` backports upstream commit `0847af3` (2026-10-16), which takes the
allocator lock when creating imported and sparse allocations. Without it,
swapchain images imported on the game thread race other DXVK threads on the
allocation pool. A 2026-10-31 phone run of the pinned release crashed in
`DxvkMemoryAllocator::importImageResource` during a swapchain recreation; the
fault address was the first word of a live imported allocation, so the pool
had handed out one slot twice.

## Phone capability checkpoint

The NDK probe ran on the Galaxy S24 Ultra / Adreno 750 / Android 16. The
proprietary driver reports Vulkan 1.3.128, 256-byte push constants and the
queried mandatory core feature bits, but lacks `VK_KHR_maintenance5`,
`VK_KHR_maintenance6` and `VK_KHR_load_store_op_none`. Both maintenance feature
bits are zero. Its EXT load/store variant does not satisfy the KHR check in
the pinned upstream release. No requirements are bypassed by this port.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ports/dxvk_android/probe.ps1 -Serial <adb-serial>
```

That probe queries the system loader, not imported Turnip; exit 2 means no
compatible candidate. The Turnip worker supplies a separate selected-loader
probe under `ports/turnip/probe/`. Its device result and a real APK replay are
required before claiming compatibility, rendering correctness or FPS.

At this checkpoint the complete APK has packaged successfully, but the phone
disconnected from adb before its Turnip run. The 2026-10-31 phone runs later
showed world geometry and shadows with this provider; see
[docs/graphics.md](../../docs/graphics.md). Evidence and review patches are
under ignored `work/render/`.
