# Android ARM64 startup build

The Android app packages real generated AC1 startup functions and the portable
guest runtime for `arm64-v8a`. Its native layer connects an Android
`SurfaceView` to DXVK Native WSI, provides the AC1 D3D9 COM bridge, and imports
DXVK's `Direct3DCreate9` factory. This wiring has compile evidence only; there
has been no Android device run.

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
| DSOUND ordinal 2 enumeration / ordinal 11 Create8 | Both runtime routes excluded | One explicitly silent default output. Existing DS8 callback/token interface reaches portable device/buffer/Buffer8/3D/listener/notify proxies. |
| DS8 samples, locks, Play/Stop/cursors | System32 COM only | Shared guest PCM storage, wrap locks, monotonic sample-rate/frequency cursors, loop/nonloop completion, volume/pan storage, duplicate lifetime. PCM mono/stereo 8/16-bit only; FX and other profiles diagnose unsupported slots. **No AAudio/mixer/output yet.** |
| DS8 3D and notifications | Native Windows interfaces / HANDLE events | 3D values and deferred commit stored, no spatialization. Notification crossings/stop signal guest events under the scheduler baton, with a 10 ms wake when all guests wait. Invalid/closed events diagnose failure. No audio worker. |
| DirectInput8 | System32 COM; runtime route excluded | Canonical W keyboard (256 bytes), relative mouse (16/20 bytes), enumeration, acquire/unacquire, format/cooperative level and state. Touch holds Enter; keyboard/gamepad map to DIK/mouse. Buffered/custom/force-feedback interfaces remain unsupported. XInput itself still reports DEVICE_NOT_CONNECTED. |
| Input script / recording | Windows headers/layout/clock; excluded | Same parser, successful-Present clock and poll ordinals; static US DIK-to-VK mapping and WndProc event snapshots. `input_script` extra sets `AC1_INPUT_SCRIPT`; bad files remain fatal. Android launcher disables recording. |
| USER32 window/message loop / HWND | Surface-to-DXVK existed; guest USER32 missing | Single guest HWND resolved to JNI's DXVK HWND. Class registration, CREATESTRUCT/WM_NCCREATE/WM_CREATE, activation, Peek/Dispatch/DefWindowProc, rectangles, cursor/keyboard state, US text entry and destruction implemented. Metrics outside the subset stop. No multiwindow/monitor enumeration. SystemParametersInfoA GET still reports ERROR_CALL_NOT_IMPLEMENTED; accessibility SET stays guest-local. Desktop hooks remain unavailable. |
| D3D9 / DXVK / Surface | Bridge + imported provider already wired | Retained platform factory, windowed presentation, landscape SurfaceView, mutex/ref-counted ANativeWindow handoff. Surface loss/reset and Vulkan/device rendering untested. DXVK must be built separately after the benchmark. |
| **D3DX texture requirements / shader constants** | Windows D3DX DLL | **Still a title-screen blocker:** non-Windows `ac1_d3d9_check_texture_requirements` and `ac1_d3d9_get_shader_constant_table` explicitly reject these APIs. CTAB proxy methods also rely on native D3DX. A portable requirements checker and CTAB provider are required. APK success will not remove this stop. |
| Archive/file handles / overlapped I/O | Win32 handles plus existing POSIX services | Existing open/pread/stat and case-insensitive component resolution retained. POSIX overlapped reads complete synchronously with guest status/events. Rooted install reads and separate userdata writes remain enforced. No original-installation writes added. |
| INI/profile/save/special folders / registry | Win32 APIs plus project services | Existing portable INI, UTF conversion, private userdata and narrow virtual registry retained. Virtual `C:\AC1` maps to imported game files. Phone save/profile round-trip untested. Unsupported path/API profiles stop. |
| Events/semaphores/mutexes/critical sections / threads | Win32 native objects/threads | Existing guest object state and pthread mutex/cond/thread scheduler retained. DS notifications feed guest events. Priorities/affinities retain existing guest semantics. No device scheduler evidence yet. |
| PE static TLS / Tls* / C `_Thread_local` | Guest TLS modeled; API26 used emutls | Guest TLS retained. minSdk and DXVK platform raised to **29**. NDK28.2 API28 probe references `__emutls_get_address`; API29 emits ELF TLS + AArch64 TLSDESC. Phone API36 meets this. |
| Bink | Translated DLL; native FFmpeg OFF | App CMake now selects native Bink plus four static ARM64 FFmpeg libraries. DLL fingerprint/map/attach retained. Decoder syntax passes; **archives not built/linked here**. Existing non-Windows native Bink PCM is explicitly silent. Exact dependency build below. |
| Profiler | Windows GetThreadContext/cycle sampler | Existing non-Windows no-op implementation; hooks compile, no sampler thread. No profiling/performance claim. |
| Direct memory / VEH diagnostics | POSIX 4 GiB mmap existed; VEH Windows-only | Direct arena and forced-inline accessors enabled explicitly. No Android equivalent of the Windows VEH guest-EIP report: direct faults produce native SIGSEGV/tombstones. Some guest block placement assumes 4 KiB pages; 16 KiB-page runtime support is unverified (ELF alignment alone does not establish it). |
| CRT locale/time/environment/COM, crypto, network, WinMM | Portable subsets and Android stubs | Existing guest environment, en-US/ASCII locale, clock/FILETIME, COM bookkeeping, entropy-backed crypto and network startup/byte-order helpers retained. Non-ASCII collation/custom date formats need a provider. waveOutGetDevCapsA reports BADDEVICEID; WININET cookies stop pending verified offline fallback. Unknown imports/targets keep DLL/name/callsite diagnostics. |

Native ELF TLS selection agrees with the [bionic NDK linker notes](https://android.googlesource.com/platform/bionic/+/HEAD/android-changes-for-ndk-developers.md#elf-tls-available-for-api-level-29).
No pinned CPU/runtime donor inputs or transformations changed.

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
