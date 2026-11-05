# Android

The app packages the generated game code and the portable runtime for `arm64-v8a`.
Its native layer connects an Android `SurfaceView` to DXVK Native, and the D3D9 bridge
sits on DXVK, with Turnip as the Vulkan driver on Snapdragon. No game files are
packaged. The APK contains code generated from the proprietary executable, so it is
local only and must not be redistributed.

## Build

Install Android SDK platform 35, NDK 28.2.13676358, CMake 3.22.1, Git, Python, and
JDK 17 or 21. Build the pinned DXVK provider and FFmpeg once:

```powershell
.\ports\dxvk_android\build.ps1
```

FFmpeg build recipes are in `ports/ffmpeg/`.

The Android script checks both providers, generates what is out of date, and
packages the APK:

```powershell
.\tools\build_android.ps1
```

| Output | Path |
|---|---|
| APK | `android/app/build/outputs/apk/debug/app-debug.apk` |
| Symbols, sizes and timings | `work/android_symbols/<abi>/` |

Native code uses `RelWithDebInfo` with the debug signing key. If Ninja cannot spawn
processes in a sandbox, add `-NativeBuilder Make`. `-Abi x86_64` builds for the
emulator.

## Install and run

```powershell
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
adb shell am start -n org.ac1.recomp/.LauncherActivity
adb logcat -s AC1Recomp
```

In the app, choose **Import game folder** and pick the GOG installation. The copy
goes to app-scoped storage. Game-created files go to the private `userdata` folder.
Guest code sees a virtual `C:\AC1`.

To push the data with adb instead, copy the whole installation to
`/sdcard/Android/data/org.ac1.recomp/files/game/`. To start directly, optionally with
a scripted input from `tools/input/`:

```powershell
adb shell am start -n org.ac1.recomp/.LauncherActivity --ez run true --es input_script to_gameplay.txt
```

The startup trace is `files/userdata/startup.log`, readable with `run-as` only on a
debuggable build.

## Input

A physical controller supplies XInput. Without one, a touch overlay supplies the
same state ([gamepad.md](gamepad.md)). The Keyboard button in the upper right opens
the system keyboard for profile names ([profile-text.md](profile-text.md)).

## Profiling

The APK is `profileable`, so `simpleperf --app org.ac1.recomp` attaches to the
running game. Never relaunch the game to profile it.

## Known limits

- The phone caps CPU and GPU clocks during play; see [cpu-audit.md](cpu-audit.md).
- Force-stop is the way to end a stuck guest, because an Activity cannot safely cancel
  lifted code.
- Direct guest memory needs a 4 KiB page size.
