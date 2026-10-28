# Android ARM64 bring-up

The Android app builds the same generated AC1 functions and portable startup
runtime used by the host harness. It imports a user-selected installation
folder with the Storage Access Framework, copies the files to app-scoped
storage, sets app-private userdata storage, and invokes `ac1_run` on a worker
thread. The executable, game assets, and generated translation units are local
inputs and are not included in the repository or a distributable package.

Build with the Android SDK platform 35, NDK 28.2.13676358, CMake 3.22.1, and
JDK 17 or 21. The build script generates or refreshes the private translated
source under `../work/lift_initial` using the configured local game and lifter.
From the repository root run:

```powershell
.\tools\build_android.ps1
```

The debug APK builds and links the current generated AC1 function set for ARM64.
The app currently reports the native startup boundary only. It has no D3D9
renderer, surface integration, or gameplay yet. No Android device is connected,
so native execution on hardware has not been verified. The local APK is a
diagnostic artifact containing proprietary generated game code; do not publish
or redistribute it.
