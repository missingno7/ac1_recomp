# Android startup build

The Android app packages real generated AC1 startup functions and the portable
guest runtime for `arm64-v8a`. Its native layer connects an Android
`SurfaceView` to DXVK Native WSI, provides the AC1 D3D9 COM bridge, and imports
DXVK's `Direct3DCreate9` factory. The x86_64 emulator smoke test below supersedes
the historical compile-only checkpoints. ARM64 phone execution remains unverified.

## x86_64 emulator smoke test (2026-10-30)

AVD `AC1Api36`, serial `emulator-5554`, answers adb as root and reports Android
16/API 36, x86_64 and `PAGE_SIZE=4096`. It uses the host GPU through gfxstream;
the emulator is a correctness smoke target, with no phone performance claim.
The ARM64 and emulator builds use the same runtime sources.

**Title screen verified on the final APK:** `work/emu/final-title.png` shows
the original animated menu and **Press Any Key To Continue**. Its private trace
is `final-title-startup.log` (including `present=350`, line 12047). There is no
guest stop or DirectSound failure in that capture. `smoke-manifest.json` under
`work/emu/` fingerprints the local APK, source changes and evidence.
That captured APK used silent audio; the AAudio implementation below has host
contract and NDK syntax evidence only. This is not ARM64 phone/gameplay evidence.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_android.ps1 -Abi x86_64 -Jobs 12
$adb = "$env:LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
& $adb -e shell echo ok
# Always replace in place: uninstall removes the imported game data.
& $adb -e install -r android/app/build/outputs/apk/debug/app-debug.apk
& $adb -e shell am start -n org.ac1.recomp/.LauncherActivity
& $adb -e root
& $adb -e wait-for-device
& $adb -e shell mkdir -p /sdcard/Android/data/org.ac1.recomp/files/game /sdcard/Android/data/org.ac1.recomp/files/input_scripts
& $adb -e push 'C:/Program Files (x86)/GOG.com/Assassins Creed/.' /sdcard/Android/data/org.ac1.recomp/files/game/
& $adb -e push tools/input/to_gameplay.txt /sdcard/Android/data/org.ac1.recomp/files/input_scripts/to_gameplay.txt
# Root adb pushes need the installed app UID, not root ownership.
$appUid = (& $adb -e shell stat -c %u /data/data/org.ac1.recomp).Trim()
& $adb -e shell chown -R "${appUid}:${appUid}" /sdcard/Android/data/org.ac1.recomp/files/game /sdcard/Android/data/org.ac1.recomp/files/input_scripts
& $adb -e shell am force-stop org.ac1.recomp
& $adb -e logcat -c
& $adb -e shell am start -n org.ac1.recomp/.LauncherActivity --ez run true --es input_script to_gameplay.txt
& $adb -e shell cat /data/data/org.ac1.recomp/files/userdata/startup.log
& $adb -e logcat -d -s AC1Recomp DEBUG AndroidRuntime
# Use binary-safe redirection in cmd, or subprocess stdout opened as wb in Python.
New-Item -ItemType Directory -Force work/emu | Out-Null
cmd /c '"%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe" -e exec-out screencap -p > work\emu\shot.png'
```

In Git Bash set `MSYS_NO_PATHCONV=1` for adb. This session reused the already
imported game/replay data and verified replacement installation and launch;
the push/chown sequence documents provisioning and was not repeated.

Observed fixes: virtual `C:\WINDOWS` and `C:\WINDOWS\system32` queries allow
translated Bink DLL attach to finish. Enumerating the mapped `C:` drive fixes
the null `game:` namespace lookup at guest `0x009A6AAA`; startup then reaches
DirectSound and replay initialization. The actual Buffer8 query at `0x007FCAAF`
exposed a transposed GUID byte in the portable backend, corrected against the
SDK's IID. Unknown GUIDs still return `E_NOINTERFACE`, and failed DirectSound
HRESULTs now include the COM token and guest callsite in the trace.

The next run completed crypto initialization and archive reads, then overflowed
an Android native worker stack in the recursive original loader (`0x0093C740`;
fault in `dispatch`'s prologue, 180 native frames in logcat). The default stack
was about 1 MiB and x86_64 debug `dispatch` reserves about 4 KiB per call.
POSIX guest workers now reserve 64 MiB with checked pthread attributes; the
Java startup executor requests the same stack size. This allocation is separate
from guest x86 stack sizing and retains the 4096-call depth diagnostic.
The rerun passes that loader boundary. `/proc` maps in
`work/emu/stacks-maps.log` confirm approximately 64 MiB native guest-worker
stacks and a 65 MiB Java startup stack (including platform overhead).
It reaches `CreateWindowExA` and then the observed `GetSystemMetrics(23)`
(`SM_SWAPBUTTON`) at guest `0x004021D2`. A native Windows oracle reports zero;
Android now reports the same standard left/right mouse mapping. Unsupported
metric indices still stop and now include the exact index in the diagnostic.
The portable contract includes this query; window source passes Windows and
ARM64/API29 syntax checks.

With that metric implemented, the original startup enters DXVK Vulkan device
initialization. The next observed abort was DXVK's filesystem exception while
creating an empty state-cache directory. JNI now sets `DXVK_STATE_CACHE_PATH`
to the validated private `userdata` directory before calling the runtime and
reports `setenv` failures. No cache writes target the imported game directory.
`work/emu/metrics-final-startup.log` and `metrics-final-logcat.log` preserve the
abort. The subsequent APK rebuild also incorporates the separately implemented
portable D3DX bridge merged in `8426e68`; this smoke-test work adds no D3DX code.

The rerun creates `app_process64.dxvk-cache` and `vulkan_pipelines.bin` in
private userdata and passes original D3DX texture/shader-table calls. Its next
stop is `USER32!EnumDisplayMonitors` at guest `0x00A0B530`, recorded in
`work/emu/cache-final-startup.log` (screenshot `cache-final.png`). Android now
enumerates the single SurfaceView monitor using WSI's handle 1 and the existing
four-argument guest callback marshaller. Clipped/empty rectangles, callback stop
and temporary allocation cleanup pass the portable contract. Non-null HDCs
remain an explicit diagnostic. Windows and ARM64/API29 window syntax checks pass.

The monitor rerun renders the original loading screen at **89%**:
`work/emu/monitors-final.png`. Its trace records Bink `DoFrame=89` and
`CopyToBuffer=3` before the next exact stop at `0x007F9C62`: portable DS8 expected
a 60-byte `DS3DLISTENER` but the SDK and guest use 64 bytes. Listener storage,
default orientation and distance/rolloff/Doppler offsets now match that layout.
Full-parameter/deferred-commit and scalar getter contracts pass, including a
guard beyond the 64-byte output; Windows and ARM64 DS8 syntax checks pass.
This is emulator rendering evidence; the title screen and ARM64 phone remain
unverified at this checkpoint.

With the listener correction, startup completes loading and renders the full
opening video (`work/emu/listener-progress-2.png`, `title-sequence-20.png`).
The next observed call is Buffer8 `AcquireResources(0, 0, NULL)` at
`0x0081B388`. The portable backend now acquires its software PCM resources for
the observed `DSBCAPS_LOCDEFER` buffer, reports `DSBSTATUS_LOCSOFTWARE`, and
releases that status on Stop/natural completion. A Windows native-DLL oracle
confirms `S_OK`, status `0x10`, Stop status zero, and `DSERR_INVALIDCALL` for
ordinary buffers (`work/emu/dsound-oracle*.log`). Effects/other flags remain an
explicit unsupported profile; [the API contract](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418057(v=vs.85))
allows zero effect/result entries. Deferred resource/status contracts pass.

The run also exposed an Android replay-clock bug: successful-Present counting
was accidentally inside the Windows-only frame-trace branch. The counter now
increments on every platform. A dedicated idle script,
`work/emu/to_title_smoke.txt` (only `at 0 mouse move 30 30`), was pushed with app
ownership and selected for the next run so automatic Enter events cannot skip
the title screenshot. The supplied `to_gameplay.txt` remains available.

The title run also exposed nonfatal `Play` retries caused by rejecting valid
priorities/voice flags on deferred buffers. The final sound correction matches
muted Windows native-DLL oracles for software allocation, priority and
termination hints; incompatible flags and hardware-only requests retain the
native failure shapes. Contract tests cover these results. See
[DirectSound Play](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418074(v=vs.85)).

Windows syntax checks used MinGW GCC with `AC1_HOST_D3D9`, `AC1_NATIVE_BINK`,
direct-memory/profile/EIP definitions and `src/runtime`, `src/platform`,
`work/lift_initial`, `work/lift_bink` includes. The host compile database was
absent; no host build was run. The portable DS8/input/window contract passes,
including the observed canonical Buffer8 IID query and release.
Both changed runtime sources also pass ARM64/API29 syntax checks; this does not
establish a new ARM64 APK or phone run.

The sandbox denies shared Gradle-cache writes. Its cached wrapper/dependencies
were copied into ignored `work/emu/gradle` and selected with `GRADLE_USER_HOME`.
The normal build then stalled in Ninja before compiler execution. For this
session, the existing compile database and Ninja link command were executed
directly, the changed objects replaced in the runtime archive, and offline
Gradle `assembleDebug -Pac1Abi=x86_64` packaged the rebuilt library with
`-x 'configureCMakeDebug[x86_64]' -x 'buildCMakeDebug[x86_64]'`. Private command
helpers and build/install/run evidence are under `work/emu/`.

## Build

Install Android SDK platform 35, NDK 28.2.13676358, CMake 3.22.1, Git, Python,
and JDK 17 or 21. Build the pinned DXVK Native Android provider first; the
script fetches DXVK Native if its ignored checkout is absent, prepares the
compatibility headers and shaders, then writes the ARM64 library to
`work/build/dxvk-android-arm64/libac1_dxvk_android.so`:

```powershell
.\ports\dxvk_android\build.ps1
```

The configured original executable, its adjacent `binkw32.dll`, and the local
pcrecomp checkout must also be available. The Android build script prepares or
refreshes generated C in `work/lift_initial` and `work/lift_bink` before Gradle.
It preserves `AC1_GENERATED_DIR` as a CMake cache override for frozen generated
inputs. From the repository root, run:

```powershell
.\tools\build_android.ps1
```

The script selects a compatible JDK, uses the installed Android SDK, restores
the caller's environment after Gradle exits, and writes the debug APK to the
ignored path `android/app/build/outputs/apk/debug/app-debug.apk`. Gradle signs
this local debug build automatically.

## Install and import game data

With an ARM64 Android device connected and USB debugging enabled:

```powershell
adb install -r .\android\app\build\outputs\apk\debug\app-debug.apk
adb shell am start -n org.ac1.recomp/.LauncherActivity
adb logcat -s AC1Recomp
```

In the app, choose **Import game folder**, select the directory containing the
user's GOG installation, and wait for the copy to finish. The app places that
copy in app-scoped storage (using internal storage if external app storage is
unavailable). Runtime settings and game-created files go under the separate
private `userdata` directory. Choose **Run original startup** to call the native
harness on a worker thread. The harness currently stops at the first
unsupported import or guest address; a successful APK build does not establish
that it can reach the menu, render a frame, or run on an Android device. The
Android runtime exposes a virtual `C:\AC1` path to guest code while keeping
POSIX paths for host executable and Bink reads, and maps guest game-file paths
to the imported directory. This path flow has compile and helper-test evidence,
but has not been run on an Android device.

The APK contains generated code from the proprietary executable, so this local
diagnostic package is ignored by Git and must not be redistributed. No game
executable or assets are packaged.

## Current preparation (android-prep, 2026-10-29)

No game-code build, APK build, install or game execution was performed during
this preparation. SM-S928B (`R5CX41MRV6V`) answered Android **16 / API 36**
queries, then disconnected before a page-size query. Reconnect before installing;
this is device-identification evidence only.

All **17 runtime/platform/JNI sources** pass sequential NDK ARM64/API29 syntax
checks, including native Bink, graphics and the portable bridges. Java compiles
against platform 35. Focused host contracts pass for DS8 cursor timing, wrapped
Lock/Unlock, notifications, duplicate lifetime, 3D storage, DirectInput state,
replay, window creation and WndProc input snapshots. Five existing unused D3DX
helper warnings remain. Private logs are in `work/android_prep/`.
These checks do not establish a complete native link, APK, CRT completion,
rendering, title screen, audible output or Android hardware execution.

### Startup dependency audit

| Windows dependency | Android before this pass | Android now / remaining work |
| --- | --- | --- |
| DSOUND ordinal 2 enumeration / ordinal 11 Create8 | Both runtime routes excluded | One default AAudio output. Existing DS8 callback/token interface reaches portable device/buffer/Buffer8/3D/listener/notify proxies. Device open failure logs the AAudio error and selects the clock fallback, with retry. |
| DS8 samples, locks, Play/Stop/cursors | System32 COM only | Host PCM snapshots shared by duplicates, updated at Unlock; linear resampling of mono/stereo PCM 8/16-bit, frequency, volume/pan and primary gain, loop/one-shot completion. Guest cursors follow presented output spans (AAudio hardware timestamps, downstream FramesRead if unavailable); no independent clock while output runs. Clock fallback remains without a device. Unsupported formats/flags diagnose failure. |
| DS8 3D and notifications | Native Windows interfaces / HANDLE events | Listener position/orientation, source position, min/max distance, rolloff, head-relative/disabled mode and mute-at-max attenuation/pan, including deferred commit. Cone/Doppler acoustics remain unapplied. Audio callbacks only mark crossings/stop; guest service signals events under the scheduler baton, with a 10 ms wake when all guests wait. Invalid/closed events diagnose failure. |
| Audio output / lifecycle | No portable output | API29+ AAudio shared low-latency float stereo at device rate; two-burst target buffer. Callback uses host PCM only, no guest memory or event callbacks; lock contention emits silence without moving voices. Control-thread service reopens disconnected streams, retrying failures. Activity pause closes/flushes output and freezes cursors; resume reopens it. **No device audio verification yet.** |
| DirectInput8 | System32 COM; runtime route excluded | Canonical W keyboard (256 bytes), relative mouse (16/20 bytes), enumeration, acquire/unacquire, format/cooperative level and state. Touch holds Enter; keyboard/gamepad map to DIK/mouse. Buffered/custom/force-feedback interfaces remain unsupported. XInput itself still reports DEVICE_NOT_CONNECTED. |
| Input script / recording | Windows headers/layout/clock; excluded | Same parser, successful-Present clock and poll ordinals; static US DIK-to-VK mapping and WndProc event snapshots. `input_script` extra sets `AC1_INPUT_SCRIPT`; bad files remain fatal. Android launcher disables recording. |
| USER32 window/message loop / HWND | Surface-to-DXVK existed; guest USER32 missing | Single guest HWND resolved to JNI's DXVK HWND. Class registration, CREATESTRUCT/WM_NCCREATE/WM_CREATE, activation, Peek/Dispatch/DefWindowProc, rectangles, cursor/keyboard state, US text entry and destruction implemented. Metrics outside the subset stop. No multiwindow/monitor enumeration. SystemParametersInfoA GET still reports ERROR_CALL_NOT_IMPLEMENTED; accessibility SET stays guest-local. Desktop hooks remain unavailable. |
| D3D9 / DXVK / Surface | Bridge + imported provider already wired | Retained platform factory, windowed presentation, landscape SurfaceView, mutex/ref-counted ANativeWindow handoff. Surface loss/reset and Vulkan/device rendering untested. DXVK must be built separately after the benchmark. |
| **D3DX texture requirements / shader constants** | Windows D3DX DLL | Portable caps/format requirements and bounded SM3 CTAB provider wired; native D3DX36 oracle contracts and ARM64 bridge syntax pass. See [portable D3DX evidence](#portable-d3dx-2026-10-30). DXVK/device startup and title-screen execution remain unverified. |
| Archive/file handles / overlapped I/O | Win32 handles plus existing POSIX services | Existing open/pread/stat and case-insensitive component resolution retained. POSIX overlapped reads complete synchronously with guest status/events. Rooted install reads and separate userdata writes remain enforced. No original-installation writes added. |
| INI/profile/save/special folders / registry | Win32 APIs plus project services | Existing portable INI, UTF conversion, private userdata and narrow virtual registry retained. Virtual `C:\AC1` maps to imported game files. Phone save/profile round-trip untested. Unsupported path/API profiles stop. |
| Events/semaphores/mutexes/critical sections / threads | Win32 native objects/threads | Existing guest object state and pthread mutex/cond/thread scheduler retained. DS notifications feed guest events. Priorities/affinities retain existing guest semantics. No device scheduler evidence yet. |
| PE static TLS / Tls* / C `_Thread_local` | Guest TLS modeled; API26 used emutls | Guest TLS retained. minSdk and DXVK platform raised to **29**. NDK28.2 API28 probe references `__emutls_get_address`; API29 emits ELF TLS + AArch64 TLSDESC. Phone API36 meets this. |
| Bink | Translated DLL; native FFmpeg OFF | App CMake selects native Bink plus four static FFmpeg libraries. DLL fingerprint/map/attach retained. Non-Windows s16 PCM now enters the same mixer per record/track with contiguous PTS, bounded four-second queues, resampling, pause/resume/stop and decoder-applied volume. `AC1_BINK_SILENT=1` remains explicit opt-out. Decoder and mixer compilation are separate from device playback verification. Exact dependency build below. |
| Profiler | Windows GetThreadContext/cycle sampler | Existing non-Windows no-op implementation; hooks compile, no sampler thread. No profiling/performance claim. |
| Direct memory / VEH diagnostics | POSIX 4 GiB mmap existed; VEH Windows-only | Direct arena and forced-inline accessors enabled explicitly. No Android equivalent of the Windows VEH guest-EIP report: direct faults produce native SIGSEGV/tombstones. Some guest block placement assumes 4 KiB pages; 16 KiB-page runtime support is unverified (ELF alignment alone does not establish it). |
| CRT locale/time/environment/COM, crypto, network, WinMM | Portable subsets and Android stubs | Existing guest environment, en-US/ASCII locale, clock/FILETIME, COM bookkeeping, entropy-backed crypto and network startup/byte-order helpers retained. Non-ASCII collation/custom date formats need a provider. waveOutGetDevCapsA reports BADDEVICEID; WININET cookies stop pending verified offline fallback. Unknown imports/targets keep DLL/name/callsite diagnostics. |

Native ELF TLS selection agrees with the [bionic NDK linker notes](https://android.googlesource.com/platform/bionic/+/HEAD/android-changes-for-ndk-developers.md#elf-tls-available-for-api-level-29).
No pinned CPU/runtime donor inputs or transformations changed.

### Portable audio (2026-10-30)

The mixer uses DirectSound's [minimum/maximum distance model](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418662(v=vs.85))
and [rolloff factor](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418787(v=vs.85)):
full gain within minimum distance, inverse-distance attenuation with scaled
rolloff, clamping at maximum distance or muting with MUTE3DATMAXDISTANCE.
Panning uses the listener's right vector. The AAudio adapter follows the
[AAudio callback/disconnect rules](https://developer.android.com/ndk/guides/audio/aaudio/aaudio):
callbacks publish errors, and control-thread service closes/reopens the stream.
PCM decoded while the Activity is paused is retired to follow the guest video
clock; existing queued Bink PCM is retained when the decoder itself pauses.

```powershell
# Default generated-header paths are work/lift_initial and work/lift_bink.
# This worktree's check used the existing headers as read-only references:
powershell -NoProfile -ExecutionPolicy Bypass -File tools/check_audio_portable.ps1 `
  -GeneratedDir D:/Prog/ac1_recomp/work/lift_initial `
  -BinkGeneratedDir D:/Prog/ac1_recomp/work/lift_bink
```

Evidence: `work/audio/checks.log`. Both Windows host contracts pass: the new
audio fixture covers separate rendering/consumption heads, notifications,
golden PCM/gain/pan/rate sums, 3D, Bink tracks/control, snapshots, concurrent
Lock/Unlock/seek and clock fallback; `android_portable_contract` still passes.
All six changed native translation units (including the new fixture) pass
individual NDK 28.2/API29 syntax checks for ARM64 and x86_64. Audio-only shared
libraries link against `libaaudio` for both ABIs and their ELF headers/dependencies
are inspected. These checks compile no translated game functions and run no game.
Actual audibility, callback underruns, A/V sync, device-route disconnect/restart,
and background/resume require a Galaxy S24 Ultra run; none is claimed here.

### Portable D3DX (2026-10-30)

`src/graphics/ac1_d3dx_portable.h` supplies the non-Windows implementation;
Windows still forwards to System32 `d3dx9_36.dll`. Texture requirements query
the supplied device's caps, creation parameters and display format, then its
`IDirect3D9::CheckDeviceFormat`. Covered behavior includes default dimensions,
formats and usage, zero sizes, mip limits/autogen, DXT block alignment, conditional
power-of-two caps, square-only caps, scratch-pool exceptions and directional
format fallback across the supported color/float/luminance/signed/depth profiles.
Unknown formats/usages report their numeric value and fail.

The existing bridge's `arity10` table and Windows
`invoke_constant_table_method` previously admitted only slots **5, 6, 8**
(GetDesc, GetConstantDesc, GetConstant), plus IUnknown. The shared checked guest
adapter now also admits **7, 9–22, 24–25**: GetSamplerIndex, GetConstantByName,
GetConstantElement, SetDefaults, SetValue, SetBool/Int/Float and their arrays,
SetVector(Array), SetMatrix(Array), SetMatrixTranspose(Array). Names including
`$`, `[i]` and `.member` work through guest strings or owned opaque handles;
descriptors/defaults are copied into guest allocations released with the table.
Numeric writes select vertex/pixel F/I/B banks, convert through the declared
parameter type, honor matrix class and truncate to available CTAB registers.
Native D3DX36 struct-array/default-descriptor quirks are retained where tested.

Run only the small, sequential checks during the benchmark:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/check_d3dx_portable.ps1 -DxvkInclude D:/Prog/ac1_recomp/work/dxvk/prepared/include/native
```

This compiles `tests/d3dx_portable_contract.cpp` with MinGW g++ (`-O0`, warnings
as errors), runs it against the absolute System32 DLL, and syntax-checks the
Windows and ARM64/API29 bridge individually. No generated game C, dependency
build, Gradle, game run or background process is involved. The test is also
registered as `ac1_d3dx_portable_contract` in standalone graphics CTest, with an
`EXCLUDE_FROM_ALL` executable independent of the bridge/game link.

Actual result: **80,425 matches, zero mismatches**; 73 recursive constant
descriptors, 2,973 setter cases and 77,088 texture cases. Four in-memory shaders
(vs_3_0/ps_3_0, compiled by native D3DXCompileShader) exercise every register set,
row/column matrices, partial register allocation, initialized arrays, mixed
structs, samplers, bools/ints, one `$` name and nested lookup. Device callbacks
record register addresses and exact bits, normalizing bulk writes to individual
registers; no GPU rendering is performed. Malformed CTAB and device-write failure
checks also pass. Both bridge syntax checks pass with `-Wall -Wextra -Werror`.
Private logs stay in `work/d3dx_portable/`.

Native HAL device creation returned `D3DERR_NOTAVAILABLE`, and native
`IDirect3D9::GetDeviceCaps` was unavailable in this session. Texture comparisons
therefore used eleven deterministic query-only COM fixtures with identical caps and
format support for native and portable calls. The test automatically adds a real
native caps/CheckDeviceFormat profile when available. Actual DXVK format/caps
integration, title-screen execution and Android hardware remain unverified.

Buffer-pointer slots **3–4** and matrix-pointer-array slots **23, 26** remain
explicit unsupported-method stops. Transposed matrix input targeting a struct
larger than four scalar components reports its name and fails; this cross-class
conversion remains outside the implemented profile. Ordinary
matrix targets and packed SetValue for mixed structs are covered. Only observed
SM3 bytecode and the enumerated texture format/usage profiles are accepted.

Behavior references (independent implementation, no Wine code copied):
[Microsoft texture requirements](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxchecktexturerequirements),
[Wine texture behavior](https://github.com/wine-mirror/wine/blob/master/dlls/d3dx9_36/texture.c),
[Wine CTAB behavior](https://github.com/wine-mirror/wine/blob/master/dlls/d3dx9_36/shader.c).
The local native D3DX36 DLL is the comparison oracle when its behavior differs.

### Remaining dependency build, after the benchmark

This worktree initially had no generated/dependency cache. Syntax checks read
existing generated **headers only** and public FFmpeg/DXVK headers under
`D:/Prog/ac1_recomp/work`, without copying or compiling generated game code.
The later normal build prepares this worktree's own private generated output.

FFmpeg is pinned to official n8.1 commit
`9047fa1b084f76b1b4d065af2d743df1b40dfb56`. The recipe now produces static PIC
`lib{avformat,avcodec,swscale,avutil}.a` at API29 with Bink demux/video/DCT/RDFT
audio and file protocol. **MSYS GNU make is absent on this PC**; install the
MSYS2 `make` package (`pacman -S --needed make` in an MSYS2 shell) before these
commands. Configure/build was not attempted.

From `D:\Prog\ac1_wt_android`, after the benchmark:

```powershell
# Reuse the existing public source read-only; outputs stay in this worktree.
$env:ANDROID_NDK_HOME = "$env:LOCALAPPDATA/Android/Sdk/ndk/28.2.13676358"
$env:FFMPEG_SOURCE = 'D:/Prog/ac1_recomp/work/bink_native/ffmpeg-src'
$env:JOBS = '1'
$env:MAKE = '/usr/bin/make'
& C:/msys64/usr/bin/bash.exe ports/ffmpeg/build.sh android-arm64
if ($LASTEXITCODE) { throw 'FFmpeg ARM64 build failed; inspect work/bink_native/ffmpeg-android-arm64/build/ffbuild/config.log' }
powershell -NoProfile -ExecutionPolicy Bypass -File ports/dxvk_android/build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_android.ps1 -Jobs 2
```

The last command generates and compiles game functions. Do not run it during
the benchmark. Do not substitute MinGW FFmpeg archives for ARM64 ones. A fresh
official n8.1 checkout can instead live under `work/bink_native/ffmpeg-src`;
omit FFMPEG_SOURCE in that case. The recipe verifies its commit. Build preflight
reports missing FFmpeg/DXVK before generating game code. This is a prepared
recipe, not a verified ARM64 FFmpeg build.

### Install, supply data and launch, after the build

App-scoped storage needs no storage permission. Use the document picker, or the
adb sequence below. The first am start opens the importer UI only; the final
command explicitly starts the guest. Push the complete user-owned installation,
including pinned DX9 EXE, adjacent binkw32.dll, videos and archives, preserving
subdirectories. Allow sufficient device storage for that copy.

```powershell
$adb = "$env:LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
& $adb -s R5CX41MRV6V get-state
& $adb -s R5CX41MRV6V shell getconf PAGE_SIZE
& $adb -s R5CX41MRV6V install -r android/app/build/outputs/apk/debug/app-debug.apk
& $adb -s R5CX41MRV6V shell am start -n org.ac1.recomp/.LauncherActivity
& $adb -s R5CX41MRV6V shell mkdir -p /sdcard/Android/data/org.ac1.recomp/files/game /sdcard/Android/data/org.ac1.recomp/files/input_scripts
& $adb -s R5CX41MRV6V push 'C:/Program Files (x86)/GOG.com/Assassins Creed/.' /sdcard/Android/data/org.ac1.recomp/files/game/
& $adb -s R5CX41MRV6V push tools/input/to_gameplay.txt /sdcard/Android/data/org.ac1.recomp/files/input_scripts/to_gameplay.txt
& $adb -s R5CX41MRV6V shell am force-stop org.ac1.recomp
& $adb -s R5CX41MRV6V shell am start -n org.ac1.recomp/.LauncherActivity --ez run true --es input_script to_gameplay.txt
# Afterward: private runtime trace and Android native-crash evidence.
& $adb -s R5CX41MRV6V shell run-as org.ac1.recomp cat files/userdata/startup.log
& $adb -s R5CX41MRV6V logcat -d -s AC1Recomp DEBUG AndroidRuntime
```

Omit the input_script extra for physical input. Touch and hold the surface for
Enter; release ends the press. Gamepad A/Start=Enter, B/Select=Escape, X=Space,
Y=E, shoulders=Shift/Ctrl, D-pad=arrows, left stick=WASD, right stick=mouse.
The replay follows successful presents and does not bypass initialization or
the remaining D3DX stop. The existing dispatch budget still applies. Force-stop
terminates a guest stuck in a native/game loop; Activity destruction cannot
safely cancel lifted execution. Surface loss/background/resume and controller
disconnect need real-device validation. If PAGE_SIZE is 16384, guest allocation
work or a deliberate checked-memory diagnostic configuration is needed before
a direct-memory run.

### Repeat small checks without a game build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/check_android_sources.ps1 -GeneratedDir D:/Prog/ac1_recomp/work/lift_initial -BinkGeneratedDir D:/Prog/ac1_recomp/work/lift_bink -DxvkInclude D:/Prog/ac1_recomp/work/dxvk/prepared/include/native -FFmpegInclude D:/Prog/ac1_recomp/work/bink_native/ffmpeg-src -FFmpegConfigInclude D:/Prog/ac1_recomp/work/bink_native/ffmpeg-native
```

Only the explicit runtime/platform list is syntax-checked. No objects, build
wrapper or generated C source. Donor FFmpeg config is used only for public-header
syntax checking; it is not ARM64 FFmpeg link validation.

## Isolated ARM64 package checkpoint

On 2026-10-29, a clean `assembleDebug` completed in 38.7 seconds with JDK 17,
Android SDK platform 35, NDK 28.2.13676358, and CMake 3.22.1. The ignored
snapshot at `work/android_frozen/20261029_074524` contains 63,548 main function
entries and 65 Bink functions; generated main C has 61 `RECOMP_TODO` markers
and Bink has none. `snapshot_manifest.tsv` records and hash-checks 1,079 source,
app, and generated build inputs as copied at the checkpoint.

The resulting local APK is
`work/android_frozen/20261029_074524/android/app/build/outputs/apk/debug/app-debug.apk`
(133,399,370 bytes). Its packaged `libac1_android.so` is ELF64 AArch64 and
exports both JNI entry points. The APK's library matches the stripped ARM64
build output byte-for-byte (SHA-256
`CE1CF673E2CB8CFEC74F8620EDCAED29D0FE58F03996A777BB4E47EF17AE285B`); APK
signature verification passed. The APK has no assets directory and contains no
EXE, DLL, FORGE, BIK, or DataPC game files.

This remains the only APK checkpoint. It predates the SurfaceView/DXVK
integration and later host runtime changes. The snapshot's recorded host boundary was the
unsupported `KERNEL32.dll!GetPrivateProfileIntA` import at `0x009d9600`; later
host progress and the Android window/factory wiring are not included in that
APK. It has not been installed or run on a device.
