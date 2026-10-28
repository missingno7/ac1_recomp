# Android ARM64 startup build

The current APK packages the real generated AC1 startup functions and the
portable guest runtime for `arm64-v8a`. It attempts the original DX9 executable's
entry path and reports the first unsupported startup boundary. D3D9, Vulkan
presentation, game input, audio and gameplay are not implemented yet.

## Build

Install Android SDK platform 35, NDK 28.2.13676358, CMake 3.22.1 and JDK 17 or
21. The configured original executable and local pcrecomp checkout must be
available; the script prepares or refreshes generated C in `work/lift_initial`
before Gradle. From the repository root, run:

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
that it can reach the menu, render a frame, or run on an Android device.

The APK contains generated code from the proprietary executable, so this local
diagnostic package is ignored by Git and must not be redistributed. No game
executable or assets are packaged.
