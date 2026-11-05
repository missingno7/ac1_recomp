# Assassin's Creed 1 native recompilation

A static recompilation of the GOG DX9 build of Assassin's Creed 1. Every recovered
x86 function becomes C, compiled ahead of time. The primary target is Android
ARM64; Windows x64 is the development host.

This is one fixed game, not an emulator. The runtime accepts only the executable
fingerprint in `config/ac1_dx9.json`. Original files, generated code, logs and
build output stay local under ignored `work/`, `build/` and Android build folders,
and must never be published.

## Status

The translator recovers and lifts all 102,014 discovered functions, with two
`RECOMP_TODO` sites left. The game runs to gameplay on Windows and on a Galaxy
S24 Ultra (Snapdragon 8 Gen 3) with DXVK and Turnip.

Evidence is tracked per milestone. A successful compile does not show that a
feature runs, and a Windows run does not show that it works on the phone.

## Build

Host tools: MSYS2 MinGW64 GCC, the Android SDK's CMake 3.22.1 and Ninja, and the
DukeMP pcrecomp checkout and Python environment recorded in `config/ac1_dx9.json`.
Python dependencies are in `requirements.txt`. Android uses NDK 28.2.13676358.

Generation needs the installed game and takes about 20 minutes. It reruns only when
its inputs change.

```powershell
# Generate if needed, compile and run on Windows.
.\tools\build.ps1 -Run

# Force generation.
.\tools\build.ps1 -Generate -Run

# Optimized host build, in build/host-relwithdebinfo.
.\tools\build.ps1 -BuildType RelWithDebInfo -Jobs 8 -Run

# Android ARM64 APK.
.\tools\build_android.ps1
```

The Windows renderer needs a staged DXVK DLL:

```powershell
$env:AC1_D3D9_LIBRARY = (Resolve-Path work\d3d9_dxvk_probe\d3d9.dll).Path
```

See [docs/android.md](docs/android.md) for Android prerequisites, game data import
and running on a device.

## Test

```powershell
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tests\run_all.py
```

This runs the generated-code, instruction semantics and runtime contract checks.
It does not run the game. It covers the host only, so an Android build and a run on
hardware are separate gates.

## Documentation

| Document | Covers |
|---|---|
| [docs/architecture.md](docs/architecture.md) | The pipeline, runtime and correctness contract |
| [docs/decisions.md](docs/decisions.md) | Accepted and rejected mechanisms, with evidence |
| [docs/cpu-audit.md](docs/cpu-audit.md) | Where main-thread time goes |
| [docs/measurement-plan.md](docs/measurement-plan.md) | How CPU work is measured |
| [docs/codegen-lab.md](docs/codegen-lab.md) | Inspecting generated machine code |
| [docs/android.md](docs/android.md) | Android build and device runs |
| [docs/graphics.md](docs/graphics.md) | D3D9 bridge, DXVK and Turnip |
| [docs/occlusion-wait.md](docs/occlusion-wait.md) | The game's occlusion-query wait |
| [docs/threading.md](docs/threading.md) | Guest threads and the scheduler |
| [docs/gamepad.md](docs/gamepad.md) | Controller input |
| [docs/profile-text.md](docs/profile-text.md) | Android profile-name entry |
| [docs/binary.md](docs/binary.md) | The pinned executable |

## Licences

Third-party provenance and notices are in `src/runtime/NOTICE.md`.
