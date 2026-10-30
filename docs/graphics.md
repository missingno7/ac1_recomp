# Android D3D9 renderer foundation

## Current rendering and migration checkpoint (2026-10-30)

Host DXVK 3.1 renders the tutorial's ground, buildings, shadows, characters,
HUD and video panel (`work/merge3_run/captures/present_002800.bmp` and
`present_003400.bmp`). The same replay on the physical Galaxy S24 Ultra with
the existing Android DXVK Native 1.9.2b provider renders characters and HUD,
but lacks the environment/shadows and shows a black tutorial video panel
(`work/phone/run6-shot.png`). The x86_64 emulator capture
`work/emu2/gameplay-verified.png` has ground, shadows and a working panel.
This narrows the old-provider failure to ARM64 or the phone's driver/capabilities;
it does not identify a proven rendering root cause. The phone's existing
startup/logcat evidence contains no Vulkan shader/pipeline creation errors.

The requested migration target is upstream DXVK 3.1, pinned to
`70d7508c01201ed3d4bfb33da42ba834eafe3857`. A real-device NDK capability probe
found that the proprietary Adreno 750 driver lacks `VK_KHR_maintenance5`,
`VK_KHR_maintenance6` and `VK_KHR_load_store_op_none`; both maintenance feature
bits are zero. Vulkan 1.3.128, 256-byte push constants and all queried mandatory
core feature bits are present. The EXT load/store variant does not satisfy
upstream's KHR-extension check. These are genuine requirements in the
[pinned device compatibility code](https://github.com/doitsujin/dxvk/blob/70d7508c01201ed3d4bfb33da42ba834eafe3857/src/dxvk/dxvk_device_info.cpp).
Raw evidence is `work/render/vulkan-probe/capabilities.txt`; reproduce with
`ports/dxvk_android/probe.ps1 -Serial <adb-serial>`.

DXVK 3.1 is not yet ported or verified on the phone. App-local Mesa Turnip via
libadrenotools is the proposed alternative; its capabilities must be measured
through the imported loader. See the [renderer migration notes](../ports/dxvk_android/README.md).
The existing 1.9.2b build remains available. World geometry and video are still
open issues; no successful fix or new-provider FPS result is claimed.

Opt-in `AC1_D3D9_TRACE_CAPS=1` bridge diagnostics log CheckDeviceFormat requests
and results, the complete adapter caps payload, and vertex declaration elements.
They do not change reported capabilities or resource creation behavior.

## Historical initial host checkpoint

The current host trace, exact stop, and fingerprint are maintained in the [README checkpoint](../README.md). The host graphics path has passed native device creation, shader and texture initialization, all 460 D3DX constant-table calls, monitor enumeration, and `TestCooperativeLevel`. Original code has made a successful native `Present` with zero recorded draws. No rendered game image or menu is verified; the latest verification run failed earlier at device creation, as recorded in the README.

AC1 now has a reproducible ARM64 DXVK Native build under `ports/dxvk_android/`. It pins DXVK Native `native-1.9.2b` to `c8dc91fabd00cac11d697ccf07426e798393cd40`, applies the Android patch recipe into ignored `work/dxvk/prepared/`, and uses NDK `glslc` plus `spirv-val` to generate and validate all 54 built-in shaders for Vulkan 1.1. Run `ports/dxvk_android/build.ps1` to repeat the build; downloaded source, prepared source and compiler output stay under ignored `work/`.

The build completed with NDK 28.2.13676358 / Clang 19.0.1 and produced `work/build/dxvk-android-arm64/libac1_dxvk_android.so`. `llvm-readelf` verified ELF64 AArch64, and the dynamic symbol table contains `Direct3DCreate9`, `dxvkAndroidSetWindow`, `dxvkAndroidGetWindowHandle`, and `dxvkAndroidSetRenderSize`. The first compile exposed missing DXVK `include/native/{windows,directx}` search paths; adding those paths resolved the failure. One upstream C/C++ compatibility warning remains in a third-party DirectX header; it did not affect the build.

The D3D9 ABI bridge under `src/graphics/` supports the observed host device, state and resource initialization paths. Its smoke test exercises native adapter/device creation and supported resource methods when the selected provider supports them. Android CMake now includes the bridge and links the prepared DXVK shared library. The launcher supplies a `SurfaceView`; JNI retains its `ANativeWindow`, calls `dxvkAndroidSetWindow` on replacement or removal, and provides the bridge's native factory and synthetic-HWND callbacks. Repeated callbacks for the same native window preserve its handle. Guest memory and heap callbacks remain shared with the host bridge. Native COM pointers remain host-side; guest objects and vtables contain 32-bit tokens.

The Android hookup has passed focused ARM64/API-28 object compilation, Java compilation, and CMake configuration; it has not yet been packaged into a new APK or executed on a device. Ordinary USER32 calls still use a Windows-only adapter: `LoadCursorA`, `RegisterClassExA`, `AdjustWindowRect`, and `CreateWindowExA` occur before D3D initialization and remain Android blockers. The native Windows D3DX requirements helper also needs a portable implementation. No Android Vulkan surface, D3D9 device, or game frame has been exercised. The host smoke includes a fake COM fallback, so inspect native-provider output rather than inferring device creation from its exit code alone. DXVK's Android WSI and build recipe are adapted from `codepdbh/nfsu2-recompiled`; its README states repository-authored code is MIT licensed (copyright attribution: codepdbh). DXVK Native remains under its zlib license; bundled Wine-derived native headers retain LGPL-2.1-or-later notices; the added Vulkan Android/Xlib headers retain Apache-2.0 notices. The host bridge's COM-token adapter attribution is recorded in `src/graphics/NOTICE.md`.

## D3D9 bridge integration boundary

Interface coverage: The guest D3D9 COM bridge recognizes all 119 IDirect3DDevice9 slots and covers the wrapped surface, vertex/index buffer, vertex declaration, 2D/cube/volume texture, volume, shader, query, state block, and swap chain interfaces, including inherited resource/base-texture methods. Native interface outputs retain guest proxy identity and acquired references; rectangle/box locks use checked guest shadow buffers with row/slice pitches and READONLY handling. Reset retains native failure semantics and the CreateDevice force-windowed presentation mapping. Surface GetDC/ReleaseDC and shared handles remain explicitly unsupported because the callback API cannot translate them. GetNPatchMode returns float bits for the guest CPU adapter to place in x87 ST(0). Standalone MinGW builds are warning-free and six CTest tests pass with the specified DXVK provider, including real-device round trips, pixel readback, UP draws, volume locks, state blocks, identity/refcounts, and Reset; this does not establish AC1 gameplay or Android hardware verification.

`AC1_D3D9_FORCE_WINDOWED=1` maps the game's exclusive-fullscreen device request (the game ignores `Fullscreen=0`) to a windowed swapchain in its own window, without a display-mode change; the guest still reads back the parameters it requested. DXVK's log showed the intermittent `D3DERR_NOTAVAILABLE` was `EnterFullscreenMode: Failed to change display mode` at 3840x2160.

NFSU2's bridge is a useful object-token design reference, not a drop-in AC1 component. It expects unchecked `guestBase + uint32_address` pointer conversion, guest allocation/free callbacks, a host-side guest-COM-object-to-`IUnknown*` registry, and a second dispatcher to translate a token plus stack arguments into an `HRESULT` and guest return. Its `0x7e000000` method-token family is distinct from AC1's current `0xf1000000` import tokens. The NFS implementation also embeds its own `ANativeWindow` handoff and reset policy, compressed texture fallback and ownership workarounds, plus NFS-specific guest-address behavior; none of those should be transplanted as AC1 behavior.

AC1 currently provides `CPU*` to `dispatch`, checked guest-memory accessors (`guest_memory_try_ptr` / `guest_memory_read`) and a process heap implementation (`pcrecomp_heap_alloc` / `pcrecomp_heap_free`) internally. The active `CPU`, heap, PE import records and import resolver remain private to `ac1_runtime.c`, and only `ac1_run` / `ac1_last_error` are public. The bridge therefore takes callbacks instead of assuming runtime globals: route `d3d9.dll!Direct3DCreate9` to `ac1_d3d9_create9`, then query `ac1_d3d9_argument_count(token)` and route supported tokens in its `0x7e000000` range through `ac1_d3d9_dispatch`. Argument counts include the guest `this` stack word. Dispatch returns 1 for a native COM call (the EAX result is valid even when HRESULT is a failure), 0 for a foreign token, -1 for invalid bridge state, and -2 for an unsupported slot; unsupported scalar/void methods must trap before stack cleanup rather than masquerading as HRESULTs. Its callback table binds checked guest memory and heap allocation to the active process. Android `CreateDevice` additionally needs a native-window callback and surface lifecycle coordination.

Earlier host milestones (historical; superseded by the current checkpoint above): the Windows runtime wired import handling and COM-token dispatch behind `AC1_HOST_D3D9`, with checked guest-memory and heap callbacks, guest-to-native HWND translation, and bridge shutdown after worker joins. The original startup created its native window and called `Direct3DCreate9` and `IDirect3D9::CreateDevice` in `work/startup_9e51f0_window_d3d.log`. A debugger run created a native HAL device and reached `SetRenderState` at `0x00AA6083`; later System32 D3D9 runs rejected device creation with `D3DERR_INVALIDCALL` and original code dereferenced its null device at `0x00AA6078` (`work/startup_windowed_ini_gdb.log`).

Diagnostics enabled by `AC1_D3D9_TRACE_CREATEDEVICE=1` capture complete presentation parameters, native window validity/visibility, and window/caller thread IDs. The failed requests have a valid visible window on the calling thread. Independent probes under `work/d3d9_native_probe.cpp` reproduce unavailable native HAL caps and failed fullscreen/windowed device creation without game code. An isolated official DXVK v3.1 x64 DLL succeeds on the same machine through Vulkan and `VK_KHR_win32_surface`, selecting the RTX 4090; its logs are under `work/d3d9_dxvk_probe`. The release archive SHA-256 is `30f9cc326874be344285582275446968cfa4c069db31ce56df312d6644179154`. This is a host backend experiment: upstream [Windows usage notes](https://github.com/doitsujin/dxvk/wiki/Windows) describe Windows as unsupported, and this DLL is separate from the Android Native build. It does not establish game rendering or Android execution.

The game reads `[Graphics] Fullscreen`, `DisplayWidth`, and `DisplayHeight` from its rooted `Assassin.ini`; a temporary 1280x720 windowed profile changed the native window but not the initial device's fullscreen request. Original helper `0x009D90A0` returns true and `0x00A0FDA0` turns that into `D3DPRESENT_PARAMETERS.Windowed=FALSE`; dimensions/refresh come from the selected adapter-mode row. The test profile under `work/userdata/roaming/Ubisoft/Assassin's Creed/Assassin.ini` has been restored to the original defaults (Fullscreen=1, dimensions/refresh=0). The GOG installation remains unchanged.

Earlier texture-pipeline evidence (historical): DXVK-backed runs passed real device creation, state initialization, buffer/surface/viewport setup, vertex-declaration creation and readback, D3DX texture validation, texture creation, and the first pixel upload. The uploader at `0x00AABB50` locks mip 0 of a 4×4 A8R8G8B8 texture, writes four rows using the returned pitch, and unlocks it; the same path passed a DXVK write/readback smoke. `work/startup_dxvk_texturelock.log` records that upload and its then-current stop at `0x00AA9E90`, which later runs passed. `work/host_dxvk_run/ac1_startup_d3d9.log` confirms Vulkan on the RTX 4090. Set `DXVK_LOG_PATH` to an existing ignored directory to retain provider logs; it is a directory, not a filename prefix.

## Retail D3D9 initialization evidence

Read-only disassembly of the configured PE (`2fa8d50c60398cc249c6d908de46b15f48e445dd0c605b5956e94fa2f445a713`) locates its sole `d3d9.dll!Direct3DCreate9` IAT entry at `0x01A48004`; the linker thunk is `0x00FCE1EE`. `0x00A0FC80` calls that thunk at `0x00A0FD2D` after pushing SDK version `0x20`, then saves EAX to global `0x01A25664` under a null check. The setup routine `0x009DA450` calls `0x00A10420` twice (`0x009DA49A`, `0x009DA4AE`); each wrapper constructs the object and calls device initialization at `0x00A0FDA0`.

The apparent REF device is conditional. At `0x00A0FEBC`, `0x00A0FDA0` tests its second argument (`[esp+0x44]` after its prologue) and jumps over the first `CreateDevice` at `0x00A0FEF7` when it is zero. That branch would request `D3DDEVTYPE_REF` (2) with behavior flags `0x46`; it is not an unconditional retail requirement. The two observed callers of wrapper `0x00A10420` pass `0` to this second argument, so their path skips the REF call and reaches the HAL (`D3DDEVTYPE_HAL` 1) `CreateDevice` at `0x00A0FFB1`, flags `0x42`. A separate wrapper at `0x00A104C0` passes `1` and would enable the REF branch, but no direct near-call to that wrapper was found. That does not rule out an indirect caller, and static evidence alone cannot establish which branch executes at runtime.

Before those calls, the bridge covers `IDirect3D9::GetAdapterDisplayMode` (slot 8, `0x00A0FDE1`) and `CheckDeviceMultiSampleType` (slot 11, through helper `0x00A0EF70`, call site `0x00A0EFCC`). Capability/adapter probing also references `GetAdapterCount` (slot 4, `0x00A0B545`), `CheckDeviceFormat` (slot 10, `0x00A0B1BB` and `0x00A0B2D8`), `GetDeviceCaps` (slot 14, `0x00A0B23D`) and `GetAdapterMonitor` (slot 15, `0x00A0B55C`). These addresses identify original call sites; actual startup evidence is recorded separately above. The bridge's current method coverage and unsupported-method contract are documented in [its README](../src/graphics/README.md).
