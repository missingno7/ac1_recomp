# Assassin's Creed 1 native recompilation

This repository builds and executes a statically translated portion of the original GOG DX9 executable on a 64-bit Windows host. The same runtime renders the original title screen and a 3D tutorial gameplay frame with the WALK prompt in the Android x86_64 emulator. See the [verified emulator evidence and commands](docs/android.md#optimized-native-apk-2026-10-30). The optimized native APK reaches the title prompt at the 591.747-second input-clock checkpoint (clear capture at 609.734 seconds), versus 907.573 seconds with Debug native code. The APK is 258.4 MB with symbols retained separately. The replay passes the former 4,096-mapping allocation limit; its gameplay capture is paired with `present=1175 ms=4940141`. Emulator runs predate the AAudio merge and were silent.

## Gamepad and Android touch controller (2026-10-30)

The runtime now feeds AC1's original Xbox controller path from Windows XInput,
Android physical pads, or a multitouch AC1 overlay. A DirectInput discovery node
and the original controller-selection setting enable the game's own prompts.
Host contracts and a tutorial replay verify raw state, packets, movement,
camera and RT+A, with captures at `work/gamepad/replay2/`. ARM64 APK packaging
passes; this input change has no phone/emulator verification. The 3120x1440
layout preview is `work/gamepad/overlay.png`. See [bindings, settings and
verification commands](docs/gamepad.md).

## Android ARM64 hardware run (2026-10-30)

On a Galaxy S24 Ultra (SM-S928B, Snapdragon 8 Gen 3, Adreno 750, Android 16) the ARM64 APK runs the original executable through the replay into the 3D tutorial: first present at ~4.4 s, present 1175 at ~34 s (host: 3.5 s / 7.1 s), full-screen 1560x720 render at the display aspect, AAudio output with 0 xruns. Rendering is incomplete: with the inherited DXVK-native 1.9.2b backend, characters and HUD draw but world geometry, shadows and the HUD Bink panel are missing (the host renders them correctly). The stock Qualcomm driver lacks `VK_KHR_maintenance5/6` and `VK_KHR_load_store_op_none` required by upstream DXVK 3.1 (`ports/dxvk_android/vulkan_probe.c`), so the Android backend is moving to DXVK 3.1 with Mesa Turnip loaded in-process via libadrenotools (Snapdragon/Adreno only for now). Gameplay runs at ~8 FPS at 1560x720 with 1.9.2b; the GPU is ~14% busy and the main guest thread is CPU-bound yet averaged 1.77 GHz (prime core 3.4 GHz), which `AC1_CPU_AFFINITY` (default: high-capacity cores) targets. Phone diagnostics: `--es env "AC1_X=..,DXVK_Y=.."` sets runtime settings; `AC1_RENDER_SIZE=WxH` overrides the render size.

Android preparation on `android-prep`: current runtime/platform/JNI ARM64 syntax checks and Java compilation pass; portable silent DS8 and keyboard/gamepad/touch/replay adapters are wired. Portable D3DX texture requirements and SM3 constant tables now pass native-DLL oracle contracts (80,425 matches, zero mismatches) and ARM64 bridge syntax. Texture tests used deterministic caps/format fixtures because native HAL caps were unavailable. No game-code build or device execution was performed. ARM64 FFmpeg archives and actual graphics/device validation remain required; see the [current Android audit and post-benchmark commands](docs/android.md#current-preparation-android-prep-2026-10-29) and [focused D3DX check command and limits](docs/android.md#portable-d3dx-2026-10-30).

## Critical-section span investigation (2026-10-30)

The intermittent loading stop at `0x007a568e` is a runtime bounds-check bug:
the 24-byte critical section can cross two live adjacent guest allocations.
The first local diagnostic replay reproduced it with all six words readable.
The fix validates the entire guest span and initializes/deletes it with guest
word writes. Native Windows, direct/checked-memory and scheduler contracts pass.
Ten consecutive 60-second replays of one fixed RelWithDebInfo/direct-memory
binary completed without a guest stop; tutorial-scene captures and a real
cross-region Initialize/Enter/Leave sequence are retained under `work/cs/`.
One final capture is white; the detailed evidence and validation limits are in
[the investigation record](docs/critical-section-span.md).

## Native leaf overrides and gameplay hotspots (2026-10-30)

Gameplay CPU is flat: generated guest code is ~56% of sampled CPU, but the hottest single guest function is under 2%, and dispatch/import/scheduler overhead is a ~4.6% lower bound ([full breakdown](docs/hot-functions.md)). Three fingerprint-pinned native leaves (matrix product `0x00653400`, plane/AABB classification `0x01176DE0`, quaternion/vector transform `0x0042EE70`) preserve the guest CPU state and write sets; each passed 200,000 synthetic and 10,000 live differential checks. Interleaved 60-second A/B runs showed no overall FPS win, so overrides are **opt-in**: `AC1_NATIVE_OVERRIDES=1` (all) or a comma-separated hex VA list; unset or `0` runs the translated code. `AC1_NATIVE_VERIFY=N` compares the first N calls against the translated body. Compare with `tools/bench_gameplay.ps1 -Compare ... -EnvB @{ AC1_NATIVE_OVERRIDES='1' }`.

## Earlier host checkpoint: first original image (loading screen)

Original code now renders AC1's loading screen on Windows. The translated Bink DLL decodes `videos\LoadingScreen.bik`, and the game draws its progress bar over it through the D3D9 bridge on DXVK. Back-buffer captures show loading progress from 4% at present 4 to 81% at present 64, with 81 original draw calls by then. Evidence: `work/startup_first_image_loading.log` (SHA-256 `77EBD97228EEF25CD839F887B44240EFFD52C690A353E51DF7CCE4D3A6BE0412`) and `work/captures/present_000064.bmp`. This verifies sustained original rendering of the loading screen. Menu, input, audio playback and gameplay are not yet verified.

The catalog grew to about 83,900 main-executable functions and 292 Bink functions, from these systematic discovery sources:
- decoded `MOV`/`PUSH` imm32 code pointers at function boundaries (`code_immediate` edges);
- every RTTI-located vtable slot, primary and secondary, including `.?AU` struct bases and single-slot tables (`work/scan_all_rtti_vtables.py`);
- Bink's relocation-proven function tables.

The D3D9 bridge covers every `IDirect3DDevice9` slot and the resource interfaces. The DirectSound bridge covers every slot of the DS8, buffer, 3D buffer, 3D listener and notify interfaces. The newly covered but not yet reached MMX code produces 13 main-executable lift failures and 71 explicit `RECOMP_TODO`s. Bink has 165 MMX sites that its loading-screen decode has not reached so far. A native FFmpeg Bink provider adapted from recomp-kit is being integrated (see [architecture](docs/architecture.md)).

Development run, windowed and skipping the intro videos as players do by deleting them:

```powershell
$env:AC1_D3D9_LIBRARY = (Resolve-Path work\d3d9_dxvk_probe\d3d9.dll).Path
$env:AC1_D3D9_FORCE_WINDOWED = '1'   # fullscreen request presented windowed; no mode switch
$env:AC1_D3D9_TRACE_FRAME = '1'      # present/draw counts at power-of-two presents
$env:AC1_D3D9_CAPTURE_DIR = (Resolve-Path work\captures).Path   # BMP back buffers
$env:AC1_HIDE_FILES = 'videos\UBI_LOGO.bik;videos\Ubisoft_Presents.bik;videos\Warning_Disclaimer.bik;videos\DemoIntro.bik'
.\tools\build.ps1 -BuildType RelWithDebInfo -Jobs 16 -Run
```

Without `AC1_HIDE_FILES`, the game plays all four intro videos in real time (about eight minutes) before loading.

## Earlier saved host checkpoint (64,409 main + 65 Bink functions; historical)

The generated catalog contains 64,409 main-executable functions and 65 Bink functions, with zero generation failures and an empty queue. Two `RECOMP_TODO` instructions remain: `FNSAVE` and `FRSTOR`. Work stopped at the user's request after the current callback change and one final verification run.

The latest build passed in 126.05 seconds. Its run, [`work/startup_final.log`](work/startup_final.log), SHA-256 `CAD8DFC46715A44370B4DDE55ADAA7C9FBE43C6A8CDFE2F7B34BB719644C9733`, failed earlier than the preceding successful run: DXVK `CreateDevice` returned `D3DERR_NOTAVAILABLE` (`0x8876086A`), then original code attempted a scalar read at address zero. The new observed callback root `0x00415600` is compiled in commit `a06ee1f`, but this run did not reach it. No retry or further blocker work was performed after this final attempt.

The furthest verified execution is [`work/startup_clipcursor.log`](work/startup_clipcursor.log), SHA-256 `FE3BCBB9FA5FCB526C9C688D6AC294D82AB0B85F7BADF573D3F39C0B0B835CC7`, using the preceding 64,402-function catalog. Original code made its first successful native `Present` call, with `present_calls=1` and `draw_calls=0`, then stopped at missing callback `0x00415600` through `0x00415CA0` at `0x00415CAA`. It took 32.854 seconds and 53,792,544 total guest dispatches; all five workers joined. This verifies presentation, not a rendered game image. No original draw, menu, interactive control, audio playback, gameplay, or Android device execution is verified.

Graphics integration in `fde142c` covers vertex/index-buffer staging, frame boundaries, typed bindings and draws, render-target/depth-stencil selection, and occlusion queries. Native synthetic tests and four isolated CTest checks passed. The canonical graphics bridge also compiles for ARM64/API28 (553,904-byte object; source SHA-256 `DEED49C6D0E085EB602D787EE0BAD61AE8FB201DC2082C2098E8B24A0BF6668D`), with five unused-helper warnings. On resumption, first reproduce device creation and the saved presentation checkpoint, then validate execution through the newly translated callback.

Generated functions are native C compiled ahead of time. Guest registers and 32-bit addresses preserve the original x86 program's semantics; there is no instruction decoder, JIT, Wine, or x86 interpreter in the runtime. Untranslated function targets and unimplemented imports stop with the original instruction address. The executable and its adjacent `binkw32.dll` must match the pinned fingerprints.

Host dispatch uses a 1,024-entry exact-address cache for successful main-executable function lookups, adapted from Duke's `lifted_lookup.h`. It resets each run; Bink and import resolution remain independent, and unknown or interior addresses still go through exact binary search and fail normally. Set `AC1_LIFTED_LOOKUP_CACHE=0` to disable it. Collision/miss fixtures and ARM64 compilation pass. On the same executable (SHA-256 `E94C399AC7BB00BC1BD907D9B66B3844B25356338B6EAD10BCE4A05B9A1F9205`), cache-off took 47.028 s / 100,833,053 total calls and cache-on 31.515 s / 58,286,304, but main crypto work differed (59.7M vs 17.2M calls), so these runs do not establish a speedup.

## Build and execute

The current configuration uses the actual local DukeMP pcrecomp checkout and its Python environment, recorded in `config/ac1_dx9.json`. Runtime inputs copied from that working tree are pinned by content in `tools/prepare_runtime.py`. The GOG directory is read-only.

```powershell
# Analyze, generate, compile and run the real startup path.
.\tools\build.ps1 -Generate -Run

# Runtime edits only: incremental compile and execute.
.\tools\build.ps1 -Run

# Optional optimized host build with symbols, in build/host-relwithdebinfo.
# Generated instructions still retain their guest-address diagnostics.
.\tools\build.ps1 -BuildType RelWithDebInfo -Jobs 16 -Run

# Run twice in one process to check repeatability and worker cleanup.
.\tools\build.ps1 -CheckRepeat

# Build the local Android ARM64 diagnostic APK.
.\tools\build_android.ps1
```

The current Windows Vulkan test uses the separately staged official DXVK x64 DLL through an explicit absolute path. With that local dependency present:

```powershell
$env:AC1_D3D9_LIBRARY = (Resolve-Path work\d3d9_dxvk_probe\d3d9.dll).Path
New-Item -ItemType Directory -Force work\host_dxvk_run | Out-Null
$env:DXVK_LOG_PATH = (Resolve-Path work\host_dxvk_run).Path
.\tools\build.ps1 -BuildType RelWithDebInfo -Jobs 16 -Run
```

An explicit DLL load/export failure stops with its diagnostic; it does not fall back. Without `AC1_D3D9_LIBRARY`, the bridge uses System32 D3D9. This host DLL is separate from the Android DXVK Native build.

Host tools: MSYS2 MinGW64 GCC and the Android SDK's CMake 3.22.1/Ninja. Python dependencies are in `requirements.txt`. Android uses NDK 28.2.13676358, SDK 35 and JDK 17 or 21. See [Android instructions](docs/android.md) for installation and user-supplied game data.

`-Jobs` controls native compilation parallelism and defaults to 8; the optimized example uses 16 for this host's 16 physical cores. It does not change guest-thread scheduling.

The generator assigns functions to 1,000 fixed groups by address and preserves unchanged files. Existing functions keep their group when the catalog grows, avoiding unnecessary recompilation; a runtime edit does not regenerate game code. Driver/config changes trigger generation automatically. It seeds the original C and C++ initializer tables (1,750 validated function pointers) from the bounds in `config/startup_initializers.json`, follows direct tail thunks, and recovers constant constructor/cleanup callbacks passed to the observed MSVC array-construction helper. Terminal direct jumps out of multi-instruction adapters are also followed after code-entry and instruction-overlap validation; internal, conditional, and indirect jumps are excluded from this rule. This closes the observed `0x004016D1` adapter's missing tail edge to `0x009D9600`. It also follows constant callbacks passed as argument 2 to the game's four-argument parser helpers at `0x00438EF0` and `0x009312F0`, including nested and recursive parser callbacks. These callback rules require bounded stack-argument evidence and executable entry validation. Add other observed indirect targets to `initial_discovery.startup_seeds`; `--seed` and `--seed-file` also support focused experiments. Unsupported instructions remain explicit runtime failures.

Generation invalidates its previous success report before changing outputs and verifies input fingerprints again before publishing a new report. If inputs change during generation, the build stops and must regenerate. Instruction-overlap checks use a page index with the same exact boundaries as the earlier linear scan; a replay of 41,605 functions and 890 callback edges produced byte-identical C while reducing measured discovery time from 92.4 to 38.5 seconds. Reports now separate discovery and emission timings. Private equivalence evidence is in `work/lift_perf_index/`.

Emitted function bodies are cached under `work/emission_cache/`, keyed by the binary, translator inputs, and each recovered function's contents. Discovery still runs on every regeneration; changing the root set can reuse bodies whose recovered instructions remain unchanged. Cache hits verify body hashes, and new entries become persistent only after the generation-input checks pass. Reports include hit/miss counts and publication timing. Pass `--no-emission-cache` directly to `tools/lift_ac1.py` to bypass it. Focused cold, warm, and bypass comparisons produce identical generated files and preserve unchanged file timestamps. A real regeneration reused 63,442 bodies and translated five new functions: emission took 4.4 seconds and generation including discovery took 79.3 seconds, with no cache errors.

Configured roots also include 1,968 validated factory pointers from the game's WinMain registration graph, separate from the CRT initializer tables. Exact registrar calls identify named records whose factory field at `+0x68` feeds the observed object-creation dispatcher. A further 1,746 serialization entries come from unambiguous primary RTTI vtables for those registered `Object` types, using slot 2 as in the observed generic deserializers. Compiling these conditional callbacks avoids discovering each type in a separate run; it does not establish that every registered type has executed. Private provenance, entry-validation and isolated-build evidence is retained in `work/factory_seed_manifest_winmain_1968.json`, `work/factory_slot2_object_broad_rtti_scan.json` and `work/lift_factory_broad/`.

The same registered-type tables also supply 14,601 method candidates after executable-entry and instruction-overlap checks. An adjacent `Pad` string following the nine-entry `PauseInfo` table was excluded: its bytes happen to resemble a code address, so section membership alone is insufficient to establish a table slot. The isolated closure generated about 41,500 functions with no discovery or translation failures; all generated C passed syntax-only compilation, with only the same two explicit x87 gaps described below. This is additional conditional coverage; runtime-created classes outside the registered-type set still need their own evidence. The frozen inputs, candidate provenance, exclusions, and syntax checks are retained in `work/lift_allmethods_snapshot/`.

The root set now also includes 6,641 validated targets from broader Scimitar primary vtables, 1,338 methods from primary Havok `hkBaseObject` descendant tables, 251 Gear virtual methods, 2,020 Magma primary virtual methods, and 1,561 property getter/setter callbacks. The property callbacks come from 387 bounded descriptors passed at decoded calls to the original registrar; their record stride and callback fields match the actual `Component` and `RigidBodyComponent` startup stops. The combined generation covers over 63,500 functions with no discovery or translation failures and an empty work queue. This is conditional code coverage, not evidence that those functions all ran. Private provenance is in `work/scimitar_utility_vtables_final.json`, `work/havok_hkbase_rtti_coverage.json`, `work/gear_namespace_rtti_coverage.json`, `work/magma_primary_vtable_seeds_59995_reviewed_v2.json`, and `work/registered_property_callback_seed_manifest.json`.

Constructor-proven primary and secondary Havok method-table batches add validated physics callbacks, and six exact loader callbacks pass entry and instruction-overlap checks. Their private evidence is in `work/havok_hkbase_rtti_coverage.json`, `work/havok_secondary_coverage/secondary_tables.json`, and `work/loader_registrar_callback_target_validation.json`.

The 59 MMX operations previously left unsupported in two Havok leaf helpers now have scoped lowerings for those routines, validated against 552 generated-versus-native oracle cases including ABI and guard checks; their generated translation units also compile for ARM64. This does not add general MMX-alias support, which remains unsupported as described in `src/runtime/NOTICE.md`. To repeat the focused regression:

```powershell
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tests\run_mmx_havok.py `
    --cc C:\msys64\mingw64\bin\gcc.exe `
    --cxx C:\msys64\mingw64\bin\g++.exe
```

Two apparent `AAD` instructions were instead a decoding error: a four-entry switch table was read past its proven bound into a word pointing inside another instruction. The local lifter pins that exact dispatch's bound; its regression verifies the original table arms and excludes the false blocks.

The same build also translates the required Bink DLL functions using `tools/lift_bink.py` and `config/binkw32.json`. Its 16 stable source groups have a separate dispatcher, and unchanged inputs skip generation. Add observed Bink targets to that configuration's `observed_seeds`. The original DLL remains user-supplied game data; the runtime verifies it and maps its data at the original guest address without loading x86 code into the host process.

```powershell
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tools\lift_ac1.py --selftest
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tools\inventory_ac1.py

# Larger isolated compiler experiment, independent of the active startup slice.
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tools\lift_ac1.py `
    --initializer-group none --max-functions 3000 --max-depth 8 --output-dir work\lift_expanded
```

## Evidence and scope

The Win32 import compatibility work has a separate focused check that does not
regenerate or execute translated game functions:

```powershell
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tests\run_win32_compat.py
```

On 2026-10-29 this passed 280 adapter calls and 1,427 assertions, including
native Windows comparisons, wildcard enumeration, PE string resources,
userdata-only mutations, symlink/hard-link rejection, guest thread aliases,
and suppression of accessibility setting changes. Existing file and profile
contracts also passed. The complete runtime passed host syntax checking with
the graphics/native Bink options enabled and Android ARM64 API 28 syntax
checking with NDK 28.2. Empty lookup-header stubs are confined to
`work/win32_compat/stubs`; these checks do not establish a runtime link, game
execution, menu/save behavior, rendering, or Android hardware verification.
Commands, output and the per-import disposition audit are preserved in
`work/win32_compat/`.

Against committed source, the installed PE has 114 imports absent from the
string-literal scan: KERNEL32 68, USER32 7, ADVAPI32 3, OLEAUT32 1, WS2_32 26,
EAX 1, DSOUND 2, XINPUT1_3 3, WINMM 1, WININET 2. The 14 Bink imports are
already named; EAX #6 and DSOUND #2/#11 already have ordinal handlers.
New coverage is KERNEL32 65 (including three controlled process stops),
USER32 7, ADVAPI32 3, OLEAUT32 1, WS2_32 5 pure helpers, XINPUT1_3 3 and
WINMM 1. Both installed XInput export tables confirm ordinals 2/3/4.

The 21 remaining WS2_32 names and both WININET cookie functions stop with
explicit offline diagnostics because their caller fallbacks are unverified;
the audit lists each name. EAX retains its previously verified optional
provider failure. `DebugBreak`, `RtlUnwind` and `UnhandledExceptionFilter`
also stop explicitly. The runtime starts with an empty `FS:[0]` exception
chain, stores but does not dispatch the unhandled filter, and recognizes
only the debugger thread-name form of `RaiseException`. Full SEH needs
guest exception/context structures, checked frame traversal, guest handler
callbacks and dispositions, nested exceptions, target-frame unwind and
nonlocal transfer across translated C frames. Native host unwinding cannot
substitute for that work. Process termination drains through the existing
stop/cleanup path with exit code and caller logged; the public host result
remains 8. A distinct normal guest-exit result requires an outer run-loop
change beyond the two-line import integration.

Portable branches provide bounded filesystem and time operations plus an
en-US/ASCII locale subset. Unsupported collation, custom date/time formats
and keyboard translation diagnose their exact boundary. POSIX creation-time
setting returns `ERROR_NOT_SUPPORTED`; timezone information describes the
current host offset without transition rules. Android XInput returns
`ERROR_DEVICE_NOT_CONNECTED`, wave output reports no device, and keyboard
state/system-setting GET services return documented unavailability. Windows
forwards those platform queries and NLS operations natively. Console control
handlers are registered in guest state; host signal delivery is not routed
into guest callbacks.

The exact DX9 SHA-256 is `2fa8d50c60398cc249c6d908de46b15f48e445dd0c605b5956e94fa2f445a713`. The PE entry is `0x00F9F445`. Reports and generated C live under ignored `work/`; the latest preserved host boundary is fingerprinted above. The configured direct-call closure compiles, links and executes the startup path; it is not a whole-program port. Discovery now allows depth 32: the isolated closure audit reached actual depth 24 with no missing executable direct-call targets, and the broader build has passed host startup through the earlier parser callbacks. Validated virtual-method families are added as original execution reaches them, including the distinct `BlendCamera` and `BlendCameraSettings` tables. The discovered `RCPPS`, `RSQRTSS`, `RSQRTPS`, XMM `PMULUDQ` and XMM `PMAXSW` gaps now have portable lowerings and focused checks. The broader registered-type closure still contains two explicit instruction stops: `FNSAVE` at `0x00FA0EAD` and `FRSTOR` at `0x00FA0EB9`, a paired x87 context save/restore around a math helper. They have not been reached by the verified startup path and are not silently skipped. The diagnostic dispatch guard defaults to two hundred million calls per guest thread; `AC1_DISPATCH_LIMIT` overrides it and `0` disables it. Lower limits of ten million and fifty million interrupted finite multi-precision arithmetic during original crypto initialization. An earlier retry with a 200-million limit passed that bounded multiply path; runtime costs vary between starts.

The host trace confirms original entry execution, C/C++ initialization, and the CRT's handoff to the game's WinMain thunk at `0x00401375` (original return address `0x00F9F3DC`). The original worker entry at `0x00FA3B17` also executes on a native host thread with a separate guest CPU, stack, TIB and TLS. Guest execution is currently serialized for correctness; this is not yet parallel execution across CPU cores. A same-process repeat-run check passes through worker cleanup and runtime reinitialization. Game startup remains incomplete. The APK's library is ELF64 AArch64; no Android device execution has been verified.

Earlier host graphics milestones (historical; the current boundary is above): `RegisterClassExA`, `CreateWindowExA`, and `Direct3DCreate9` succeeded in `work/startup_9e51f0_window_d3d.log`. A debugger run created the native HAL device and reached `SetRenderState` at `0x00AA6083`; later System32 D3D9 runs rejected device creation and original code dereferenced its null device at `0x00AA6078`. With the official DXVK v3.1 Windows x64 DLL selected, earlier runs passed device creation, state initialization, vertex/index buffer creation, surface setup and color fill, viewport setup, and vertex-declaration creation/readback. `work/startup_dxvk_getdecl.log` preserves the earlier continuation to `D3DXCheckTextureRequirements`; `work/host_dxvk_run/ac1_startup_d3d9.log` identifies the Vulkan device. See [graphics evidence](docs/graphics.md).

The native D3D9 bridge now creates and uploads cube textures. The DirectInput bridge forwards the observed `Acquire` and `GetDeviceState` calls to native interfaces, with the forwarding behavior covered by contract tests; no successful interactive input session is verified. These API milestones do not verify a rendered frame. Optional services remain deferred while work focuses on running the game; a service may report documented unavailability only when the original caller's fallback is verified. Current backlog: `GetAdaptersInfo` enumeration returns `ERROR_NOT_SUPPORTED` (50) on each call, without changing the caller's buffers, length, or `LastError`; its diagnostic is logged once per run. The verified original branch cleans up, installs its built-in `127.0.0.1` fallback, and returns normally (`0x1156CCA` → `0x1156D45` → `0x1156D54` → `0x1156D8A`). This does not enumerate adapters. `inet_addr` is implemented as pure local address conversion and performs no network I/O. The native adapter-enumeration implementation and ABI oracle are preserved under ignored `work/deferred_network_adapters/`; the `iphlpapi` dependency has been removed.

Cursor confinement is also deferred: `USER32!ClipCursor` returns `FALSE` with `ERROR_CALL_NOT_IMPLEMENTED` (120), logs once per process, and leaves native clipping unchanged. Original callers at `0x00A0B670` and `0x00A0B6C0` ignore failure on both confinement and release, and the live run passes this continuation. Native `GetCursorPos` and `ScreenToClient` are implemented and covered by the focused window smoke test. Restore confinement later if it is needed for interactive host controls.

The original audio initialization now reads and writes `DARE.INI` through the profile APIs. Host configuration writes are restricted to `work/userdata/`; Android uses the application's private userdata directory. The GOG installation remains read-only. The lifter also bounds two-level switches using their guarded byte lookup tables, preventing lookup bytes from being mistaken for executable targets. `-Run` saves its complete diagnostics to `work/startup.log`.

`GetPrivateProfileIntA` and the newly implemented `GetPrivateProfileSectionNamesA` use the shared portable profile parser. Thirteen native-oracle cases match both `GetPrivateProfileSectionNamesA` and the `GetPrivateProfileStringA(NULL, NULL, ...)` section-name query, including exact-fit buffers, truncation, and duplicate sections. The earlier trace in `work/startup_privateprofileint_415c60.log` passes the settings and object callback `0x00A4D580`; later host traces continue through graphics initialization as summarized above.

The scalar CRT memory-copy path also exercises biased and negative jump-table indices. Its translated string operations now honor `STD`/`CLD` and the direction flag. The lifter self-test compiles that original function and compares 4,968 alignment, length, and overlap cases against native `memmove`, including stack and register preservation. Original table bytes and generated test code remain under ignored `work/`.

Resource loading also exposed a concrete SIMD overread at original instruction `0x00974270`: `MOVQ xmm0, [eax+0x18]` reads eight bytes, while the donor translation requested sixteen. The local adapter now uses exact-width, bit-preserving reads and writes for XMM `MOVQ`, and corrects the same eight-byte source-width issue in `CVTDQ2PD`. Its executable regression test places the source at the last eight bytes of a guest mapping and checks lane results, signed conversion edges, register aliasing, and flag/register preservation. Guest mapping bounds remain enforced.

Host startup has passed that fault site and continued through original Havok motion and shape methods, registered property setters, and Gear allocation methods. A later `TextureMap` factory failure exposed the donor's fixed 256-region bookkeeping limit. The runtime now allows 4,096 sparse mappings while preserving access bounds. This change lives in `tools/prepare_runtime.py`, so rebuilding the copied runtime preserves it; provenance records prepared-file hashes. Repeated preparation and a 300-region allocation check pass. Actual optimized startup passes the former null read at `0x00930E1D`; `work/startup_after_region_cap_gear_boundary_788190.log` records that earlier continuation and all five worker joins.

A later CRT read at original instruction `0x00F9FCC8` spans guest address `0x1220FFFD` across two adjacent 128 KiB mappings with independent host backing. Checked scalar reads and writes now validate the complete operand before copying in region-sized pieces, reusing the approach from the local Mafia runtime. Holes and 32-bit wrap still fail before any partial copy; raw pointer requests retain their single-region requirement. The captured 3+1-byte case, integer/SSE accesses, holes, and wrap pass in `ac1_guest_memory_contract`, including optimized builds. The memory implementation and fixture also compile for AArch64. The real startup run passes this former fault. These changes are preserved by `tools/prepare_runtime.py`.

Original memory-management code also queries an interior page of a tracked `VirtualAlloc` reservation. `VirtualQuery` now returns that page's base, the original allocation base, and the remaining committed region length in the 28-byte guest structure. Adjacent allocations stay distinct. The focused contract compares this subset against native Windows, and startup passes the captured query at `0x11AE0000` inside the allocation beginning at `0x11AD0000`. Other memory kinds remain explicit unsupported queries. Private evidence is in `work/virtualquery_provenance_11ae.gdb.log` and `work/startup_virtualquery_959870.log`.

Guest executable paths use Windows separators because the original startup searches for a backslash. On Android, guest APIs expose `C:\AC1` while native file access retains the imported folder's POSIX path. Focused path checks cover the split roots, traversal rejection, and the separate userdata write boundary; the combined runtime compiles for AArch64. Android execution remains unverified.

With `AC1_TRACE_IMPORTS=1`, failed dispatches capture checked receiver words and a possible vtable before teardown, alongside window and D3D milestone logs. Reachable `INT3` instructions now stop with their original guest address instead of silently continuing; a focused regression covers the original allocator assertion and verifies that the trap cannot fall through.

The Magma expansion and bounded font/array callback seeds compiled and linked on the host, then passed the observed engine, file-factory, and font initialization stops. Two font callbacks come from a constructor-proven pair; four array constructors come from eight exact calls to the four-argument helper at `0x004020A0` through its `0x0040104B` thunk. Private evidence is in `work/array_helper_4020a0_callbacks.json`. A further audit recovered 76 exact virtual tail adapters from decoded constant pointer stores, including the observed FontFactory, AreaFactory, and WidgetFactory arrays; its frozen catalog and per-instruction evidence are in `work/stack_callback_audit_snapshot/report.json`. The following TimingStrategyManager stop supplied four constructor-registered singleton accessors. Subsequent traced runs passed these callbacks, the scalar-memory and allocation-query fixes, serialization callbacks, a record-sort comparator, and worker callback `0x0043FED0`. The captured continuation reaches missing target `0x009D9600` through startup thunk `0x004016D1`; `work/startup_crossregion_9d9600.log` preserves this boundary and five worker joins during teardown. No window or D3D device was observed.

On Windows, the original code enumerates actual audio devices, follows its own fallback when optional EAX audio is unavailable, and creates a real DirectSound8 device, primary and secondary buffers, and a 3D listener. Guest COM proxies marshal 32-bit pointers into the native interfaces. Unsupported methods remain diagnostic stops. This Windows audio backend is not yet implemented on Android, and no audible playback is claimed.

The original startup successfully probes `DataPC_Common.forge` and `videos\LoadingScreen.bik` through read-only file handles rooted in the installed game directory. Nested relative paths and absolute paths beneath the configured guest installation root are supported; the portable resolver handles the installation's `Videos` capitalization. `work/video_probe_verified.log` preserves the successful probes. The loader now reads actual bytes from `DataPC.forge`, including its `scimitar` header, and completes a 524,288-byte Gear `OVERLAPPED` read using a native completion event. Two original `GetOverlappedResult` calls return the same successful byte count. `GetFileSizeEx` reports 203,259,904 bytes; `work/archive_async_read_verified.log` records this execution and the next untranslated completion callback. Full asset loading and video decoding remain unverified. The optional `ac1_files_contract` target checks path mapping and real I/O against temporary fixtures without game assets; set `AC1_FILES_TEST_ROOT` to the repository's `work/` directory when running it.

DirectInput8W mouse and keyboard creation, object enumeration, and data-format setup all return native success. The original callback handles 11 mouse and 127 keyboard records with Unicode names. Native attached-controller enumeration also succeeds (zero controllers on this host). The observed `SetCooperativeLevel(NULL, 6)` call returns the native failure `0x80070006`, which the original code handles. The bridge forwards `Acquire` and `GetDeviceState` to native interfaces; contract tests cover the forwarding path, but successful interactive input remains unverified.

The pinned Bink 1.8i DLL has SHA-256 `603d0a96d1f27422f88d78b20ca525975ebe5e0d7667e62b98a3db80766702b4`. Its recompiled entry at `0x180017F0` completes `DLL_PROCESS_ATTACH` before EXE execution. The game then calls the original `_BinkSetMemory@8` export at `0x1800DB80`, which stores guest allocator/free callbacks `0x009E8D90` and `0x009E90D0`; the runtime verifies those writes. The preserved host trace is `work/bink_attach_setmemory_verified.log`. Video decoding and playback remain unverified.

The original random-number initialization acquires the real Windows Base Cryptographic Provider, fills a 32-byte buffer through `CryptGenRandom`, and releases the provider successfully. Opaque guest tokens keep native provider handles out of guest memory. `work/crypto_startup_verified.log` preserves this actual execution. The portable adapter uses OS entropy for the same observed random-only context; it has compiled for ARM64 but has not run on Android.

The standalone Android DXVK Native dependency builds with `ports/dxvk_android/build.ps1`, producing an AArch64 shared library with all 54 built-in shaders validated. Android CMake links the D3D9 bridge, and the Java/JNI `SurfaceView` handoff has passed compile checks; the latest runtime C source also compiles to an ARM64 object. No new APK has been assembled from these updates, and Android has not been run on a device. The only APK is the older frozen checkpoint documented in [Android instructions](docs/android.md). See [graphics integration](docs/graphics.md), [binary findings](docs/binary.md), [architecture decision](docs/architecture.md), [NFSU2 reuse findings](docs/nfs-reuse.md), and [runtime source notices](src/runtime/NOTICE.md). Duke and Mafia sources were inspected locally, including Mafia's uncommitted changes. NFSU2 provides the primary reference for the D3D9 COM bridge and DXVK Native integration.

Original executables, `.forge` assets, generated game translations and local binaries/APKs are excluded from Git. The APK contains translated proprietary code and is a private diagnostic build; no game data is packaged. No existing reference project or original game source files are modified by the build.

## Benchmarking

Run `tools/bench_gameplay.ps1` for the host gameplay benchmark; measurement windows default to 60 seconds. Startup is detected from the D3D9 present counter, and the measurement frame is recorded in each run. Runs that stop before gameplay are marked failed with the tail of `startup.log` and are excluded from performance statistics. NVIDIA GPU sampling resolves `nvidia-smi.exe` from PATH, System32, or the standard NVSMI install directory.

For drift-resistant A/B comparisons, pass `-Compare` and optional stripped binaries `-Binary` and `-BinaryB`. Without a binary for a side, that side uses the normal build settings; side B can override them with `-DirectMemoryB ON|OFF`, `-TraceEipB ON|OFF`, `-ForceInlineB ON|OFF`, and `-LiftArgsB`. `-Env` and `-EnvB` apply per-side runtime environment overrides. The script runs A then B for each pair and writes one comparison directory with per-side summary statistics and paired median deltas. Use `-AllowBusy` only when other workloads cannot be paused; the result records that override.

Example using existing stripped binaries:

```powershell
& tools/bench_gameplay.ps1 -Compare -Runs 2 -Seconds 30 `
  -Binary 'work/bench/bin/67a86f7de563-25a4476d867c/ac1_startup.exe' `
  -BinaryB 'work/bench/bin/67a86f7de563-25a4476d867c/ac1_startup.exe' `
  -Env @{ AC1_DISPATCH_FAST = '0' }
```

The two-side environment overrides can also be supplied as `-EnvB @{ AC1_DISPATCH_FAST = '0' }`. Use `tools/bench_report.py <comparison-directory>` to print an existing comparison.

Codegen flag liveness is selected with `tools/build.ps1 -BuildType RelWithDebInfo -Jobs 10 -FlagLiveness cfg`
(default `cfg`; `off`, `straight` and experimental `abi` are also available), or the lifter's `--flag-liveness` option.
`abi` retains complete public dispatch contracts and uses private direct-only bodies where returned flags are dead.
`-LocalRegs all` enables function-gated GPR locals; its default is `off`, and an eight-digit hexadecimal
list such as `-LocalRegs '0051DD60,00428540'` selects individual functions. The lifter accepts the same
selection with `--local-regs`. Preparation checks both selected modes in its report.
`-X87Inline` independently force-inlines the audited integer-conversion helper (default off).
The benchmark preserves a complete frame window by draining the periodic profiler after CPU/GPU accounting stops;
incomplete windows, guest stops and dropped frame samples fail its audit. See [codegen evidence](docs/codegen.md)
for the accepted +5.97% CFG-flags experiment and original -53.19% locals result.
The [follow-up evidence](docs/codegen-followup.md) diagnoses cadence-dependent tutorial progression,
records the FLAGABI comparison (+3.77%) and revised locals comparison (+7.35%), and corrects the old x87 call counter.
Integer-helper inlining is bit-exact in the new oracle but shows no paired FPS win (-0.07% median); it stays off.
These are separate three-pair replay measurements on a shared PC; their FPS changes must not be added.
`-UserdataSeed work/<private-seed>` optionally restores identical workspace-only saves before every benchmark side.
CFG flags remain the default; ABI and locals remain opt-in pending review and broader workload measurements.
