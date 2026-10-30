# Optional Android Turnip provider

This builds an official upstream Mesa KGSL Vulkan ICD and libadrenotools for
Android arm64-v8a/API 29+. It does not modify AC1, its installation, or the
Android app. App integration is delivered separately as an unapplied patch in
`work/turnip/0002-android-optional-turnip.patch`.

## Sources and licenses

`sources.json` pins the complete revisions, and the builder refuses a different
HEAD or modified tracked source. All clones are under ignored `work/deps/`:

| Source | Pin | License |
| --- | --- | --- |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) | `mesa-26.2.5`, `ddaf85e353442d9d7f192fdcf1823e652e3f2ee3` | MIT, with per-file notices |
| [libadrenotools](https://github.com/bylaws/libadrenotools) | `8fae8ce254dfc1344527e05301e43f37dea2df80` | BSD-2-Clause |
| [liblinkernsbypass](https://github.com/bylaws/liblinkernsbypass) | `aa3975893d83ef1bc84c321ec60c65fbf1287887` | BSD-2-Clause |

Mesa 26.2.5 includes upstream A7xx/A750 device support. Compilation does not
establish correctness on a particular phone. See Mesa `docs/license.rst` and
each source file's SPDX/copyright notice. The included license files retain
the upstream MIT text and both BSD notices. Redistributed binaries also need
the relevant per-file notices from the pinned source tree, including Mesa's
third-party utility code; the license summary is not an exhaustive notice bundle.

## Build on Windows

Install Python 3.10+, the pinned Python packages in `requirements-build.txt`,
NDK **28.2.13676358**, SDK CMake **3.22.1**, and native Windows host tools:
MinGW GCC/G++, pkgconf, winflexbison **2.5.25**, and glslangValidator >=12.2.
The tested glslangValidator is the SDK emulator's **14.3.0**. Put these on PATH;
avoid MSYS/Git `usr/bin` programs when their runtime cannot start in the sandbox.
No Bash process is needed by this build.

```powershell
python -m pip install -r ports/turnip/requirements-build.txt
powershell -NoProfile -ExecutionPolicy Bypass -File ports/turnip/build.ps1 `
  -Python python.exe -SupportBuilder Make -DirectCommands
```

`-SupportBuilder Make` uses `mingw32-make.exe`. The default support builder is
Ninja. Normal environments can omit `-DirectCommands` to use Ninja for Mesa.
In the restricted Windows token used for this work, Ninja 1.10.2 and 1.13.1
stalled when starting child processes. `-DirectCommands` consumes Ninja's
expanded command database and Meson's dependency graph, then executes the
complete driver target closure with Python subprocesses. It always rebuilds
the closure and does not implement incremental Ninja behavior. `-Jobs` defaults
to 8. `-ConfigureOnly` stops after Meson setup, and `-SupportOnly` builds only
adrenotools and the probe.

Mesa uses release/O3, no LTO, freedreno only, KGSL only, no Gallium/OpenGL,
`platforms=android`, `android-stub=true`, and `android-strict=false`. The last
option allows capabilities such as BC compression required by DXVK to be
exposed on Android. Zlib is the NDK's system `libz`, selected through a generated
target-only pkg-config file. Shader caching is enabled; the app must set
`MESA_SHADER_CACHE_DIR` to private writable data (the current Android worker does).
C++ is linked statically, and ELF LOAD alignment is 16 KiB.

`prepare_mesa.py` copies the pinned tree into ignored `work/turnip/mesa-source`,
materializes Git symlinks for Windows, and changes the Meson win_flex selection
to use the target's POSIX skeleton for Android. It never edits the source clone.
`stage.py` removes the build-only Android-stub RPATH with Meson's ELF installer
code. Do **not** ship the stub libraries; Android supplies their implementations.

Outputs are under ignored `work/build/turnip-arm64/`:

- `package/arm64-v8a/`: stripped ICD, libadrenotools, and four hook libraries.
- `ac1_vulkan_probe`: standalone PIE executable.
- `freedreno_icd.armv8-a.json`: relocatable reference manifest. Android/adrenotools
  uses the HAL library directly and does not consume this JSON.
- `mesa-release/` and `support-make/` or `support-ninja/`: private build trees.

## Standalone device probe

```powershell
python -B ports/turnip/probe/run_probe.py --serial R5CX41MRV6V --direct
```

The script only creates/pushes/runs files in `/data/local/tmp/ac1-turnip-probe`.
It never starts, stops, installs, reads, or writes `org.ac1.recomp`. Each loader
runs in a separate process. Outputs and PID-filtered logcat are saved as
`work/turnip/probe-*.txt`; global logcat is not cleared. `direct` is an optional
diagnostic path that loads the ICD without the Android loader. It is not a
replacement for an adrenotools success.

The probe requests Vulkan 1.3, Android surface instance extensions, and reports
driver identity/API, all 46 mandatory core feature bits from official DXVK
**v3.1**, commit `70d7508c01201ed3d4bfb33da42ba834eafe3857`, plus required
depthClipEnable, robustness2 robustBufferAccess2/nullDescriptor, maintenance5,
maintenance6, load_store_op_none, swapchain, graphics/compute queue, and the
256-byte push-constant limit. EDS1/2 and DXVK's six optional EDS3 fields are
reported separately. The checklist comes from `getFeatureList()` and
`checkDeviceCompatibility()` in `src/dxvk/dxvk_device_info.cpp`, with Vulkan 1.3
from `dxvk_instance.h`. `required_core.inc` records the source revision.

If requirements are missing it still creates a minimal logical device, then
returns 7. If present it enables the queried core/required extension feature
chain and creates a device. Exit 3 means loader failure, 4 instance failure,
5 enumeration failure, 6 silent fallback instead of Turnip, and 8 device failure.
The shell may lack the app classloader namespace needed by adrenotools; a NULL
return or zero devices is a measured shell limitation, not a Turnip success.
SIGSEGV in upstream linker hooks cannot be recovered by an in-process fallback.

## Integration patch contract

The Android/DXVK worker concurrently added `vulkan_driver.h`, included by
`native_bridge.c`, and the DXVK 3.1 port retains
`dxvkAndroidSetInstanceProcAddr`. The integration patch reuses that bootstrap
instead of adding another loader. It is based on that worker's live files;
exact normalized base hashes are in `work/turnip/integration-base-sha256.txt`.
Review/rebase it after the Android worker lands those files.

The patch makes `AC1_VULKAN_DRIVER=system` the default and `turnip` explicit.
It logs a precise custom-loader failure, attempts the system driver, and
passes the chosen `vkGetInstanceProcAddr` to DXVK. It detects adrenotools'
silent fallback by checking `VK_DRIVER_ID_MESA_TURNIP`. If system Vulkan also
fails the DXVK bootstrap checks, startup stops with that error. A system
fallback cannot supply features missing from the proprietary driver.

Package the optional libraries with the Gradle property
`-Pac1TurnipDir=D:/Prog/ac1_recomp/work/build/turnip-arm64/package`. Both the
CMake and Make/prebuilt packaging paths append this directory. The patch
enables `useLegacyPackaging=true` when selected, because hook lookup requires
real extracted files in `ApplicationInfo.nativeLibraryDir`; the existing
bootstrap finds that directory through `dladdr`. The official adrenotools
header documents this extraction requirement.

The launcher already accepts `--es env "AC1_VULKAN_DRIVER=turnip"` or `system`.
Restart the app process between A/B runs; all loader/hook handles stay alive
for the process lifetime. This task does not run the app. The patch receives
an ARM64/API29 C syntax check and `git apply --check`; APK packaging, actual
DXVK execution, surface rendering, correctness/performance, and app-private
namespace behavior require the owning worker's integration run.

## Recorded execution evidence (2026-10-30)

The complete 706-edge driver target closure and NDK support build passed.
The staged ICD is 14,865,040 bytes; adrenotools plus all four hooks total
1,160,488 bytes. All six libraries are AArch64, use 16 KiB LOAD alignment,
and have no RPATH/RUNPATH. SHA256 values are in `work/turnip/artifacts.json`.

The initial standalone system probe ran on serial `R5CX41MRV6V`: Adreno 750,
Qualcomm 512.762.1 (`0x802fa001`), device API 1.3.128, successful instance and
minimal logical device creation. All 46 required core bits passed. Missing
requirements were maintenance5 (extension and feature), maintenance6 (extension
and feature), and `VK_KHR_load_store_op_none`. Depth clipping, robustness2,
nullDescriptor, EDS1/2, and the 256-byte push limit passed; the six queried EDS3
bits were absent. See `work/turnip/probe-system.txt`.

The phone disconnected before the completed ICD could be pushed. Turnip through
adrenotools and direct ICD execution are **not verified**. The final probe was
rebuilt after the initial system run to add PID/surface-extension reporting and
clearer version formatting; the recorded system run used the earlier 158,000-byte
probe. The checklist is unchanged. No AC1 app or game data was touched by this
task, and no rendering or performance result is claimed.
