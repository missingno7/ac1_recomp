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

## Current Android status

The Android CMake build links the D3D9 bridge and DXVK Native library, and the
Java/JNI `SurfaceView` handoff has passed compilation. The latest runtime C
source also compiles to a 230,472-byte ARM64 object without diagnostics. These
changes have not been packaged into a new APK; the only assembled APK is the
older frozen checkpoint below.
There has been no Android device run. The original startup still cannot reach
the Android DXVK factory because ordinary USER32 calls such as `LoadCursorA`,
`RegisterClassExA`, `AdjustWindowRect`, and `CreateWindowExA` lack an Android
implementation. Windows-only audio and input providers and portable Android
D3DX services also remain unresolved.

The latest host-only evidence is `work/startup_slice_1000.log` (SHA-256
`E45E3C1FC0F5DDCEDFC8999D333DD6836FFC36CB68009380D2CA07118A70B45B`): the
host completed 460 shader constant-table calls and one monitor callback, joined
five workers, and stopped at D3D9 `TestCooperativeLevel` slot 3. This does not
establish Android startup or rendering.

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
