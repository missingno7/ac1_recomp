# Assassin's Creed 1 native recompilation

This repository currently builds and executes a statically translated portion of the original GOG DX9 executable on a 64-bit Windows host. It also builds an Android ARM64 diagnostic APK from the same generated code and runtime. **The game is not playable yet.** Graphics, menu, audio playback, gameplay, and Android device execution are not verified.

Generated functions are native C compiled ahead of time. Guest registers and 32-bit addresses preserve the original x86 program's semantics; there is no instruction decoder, JIT, Wine, or x86 interpreter in the runtime. Untranslated function targets and unimplemented imports stop with the original instruction address. Only the selected executable fingerprint is accepted.

## Build and execute

The current configuration uses the actual local DukeMP pcrecomp checkout and its Python environment, recorded in `config/ac1_dx9.json`. Runtime inputs copied from that working tree are pinned by content in `tools/prepare_runtime.py`. The GOG directory is read-only.

```powershell
# Analyze, generate, compile and run the real startup path.
.\tools\build.ps1 -Generate -Run

# Runtime edits only: incremental compile and execute.
.\tools\build.ps1 -Run

# Run twice in one process to check repeatability and worker cleanup.
.\tools\build.ps1 -CheckRepeat

# Build the local Android ARM64 diagnostic APK.
.\tools\build_android.ps1
```

Host tools: MSYS2 MinGW64 GCC and the Android SDK's CMake 3.22.1/Ninja. Python dependencies are in `requirements.txt`. Android uses NDK 28.2.13676358, SDK 35 and JDK 17 or 21. See [Android instructions](docs/android.md) for installation and user-supplied game data.

The generator splits functions into groups of 100 and preserves unchanged files, so a runtime edit does not regenerate game code. Driver/config changes trigger generation automatically. It seeds the original C and C++ initializer tables (1,750 validated function pointers) from the bounds in `config/startup_initializers.json`. Add other observed indirect targets to `initial_discovery.startup_seeds`; `--seed` and `--seed-file` also support focused experiments. Unsupported instructions remain explicit runtime failures.

```powershell
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tools\lift_ac1.py --selftest
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tools\inventory_ac1.py

# Larger isolated compiler experiment, independent of the active startup slice.
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' -B tools\lift_ac1.py `
    --initializer-group none --max-functions 3000 --max-depth 8 --output-dir work\lift_expanded
```

## Evidence and scope

The exact DX9 SHA-256 is `2fa8d50c60398cc249c6d908de46b15f48e445dd0c605b5956e94fa2f445a713`. The PE entry is `0x00F9F445`. Reports and generated C live under ignored `work/`; `work/startup.log` records the latest observed startup boundary. The depth-six direct-call closure compiles, links and executes the same startup path; it is not a whole-program port. The discovered `RCPPS`, `RSQRTSS` and XMM `PMULUDQ` gaps now have portable lowerings and focused x86 checks. Other unsupported instructions, if discovered, remain explicit runtime errors. The diagnostic dispatch guard defaults to ten million calls per guest thread; `AC1_DISPATCH_LIMIT` overrides it and `0` disables it.

The host trace confirms original entry execution, C/C++ initialization, and the CRT's handoff to the game's WinMain thunk at `0x00401375` (original return address `0x00F9F3DC`). The original worker entry at `0x00FA3B17` also executes on a native host thread with a separate guest CPU, stack, TIB and TLS. Guest execution is currently serialized for correctness; this is not yet parallel execution across CPU cores. A same-process repeat-run check passes through worker cleanup and runtime reinitialization. Game startup remains incomplete: no D3D9 device or rendered frame has been demonstrated. The APK's library is ELF64 AArch64; no Android device execution has been verified.

The original audio initialization now reads and writes `DARE.INI` through the profile APIs. Host configuration writes are restricted to `work/userdata/`; Android uses the application's private userdata directory. The GOG installation remains read-only. The lifter also bounds two-level switches using their guarded byte lookup tables, preventing lookup bytes from being mistaken for executable targets. `-Run` saves its complete diagnostics to `work/startup.log`.

On Windows, the original code enumerates actual audio devices, follows its own fallback when optional EAX audio is unavailable, and creates a real DirectSound8 device, primary and secondary buffers, and a 3D listener. Guest COM proxies marshal 32-bit pointers into the native interfaces. Unsupported methods remain diagnostic stops. This Windows audio backend is not yet implemented on Android, and no audible playback is claimed.

The standalone Android DXVK Native dependency also builds with `ports/dxvk_android/build.ps1`, producing an AArch64 shared library with all 54 built-in shaders validated. It is not yet connected to guest D3D9 calls or the APK's surface lifecycle. See [graphics integration](docs/graphics.md), [binary findings](docs/binary.md), [architecture decision](docs/architecture.md), [NFSU2 reuse findings](docs/nfs-reuse.md), and [runtime source notices](src/runtime/NOTICE.md). Duke and Mafia sources were inspected locally, including Mafia's uncommitted changes. NFSU2 provides the primary reference for the D3D9 COM bridge and DXVK Native integration.

Original executables, `.forge` assets, generated game translations and local binaries/APKs are excluded from Git. The APK contains translated proprietary code and is a private diagnostic build; no game data is packaged. No existing reference project or original game source files are modified by the build.
