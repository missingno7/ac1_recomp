# Project constraints

- Build a native static recompilation of AC1, not matching decompilation or a replacement gameplay engine.
- The GOG installation and `D:/Prog/dukemp_recomp`, `D:/Prog/mafia_recomp` are read-only references. Inspect actual local source; Mafia has substantial uncommitted work. Do not reset or modify them. Disable Python bytecode writes when importing their tools.
- Keep original files, translated game functions, logs and build products under ignored `work/`, `build/`, or Android build directories. Never publish those proprietary outputs. The runtime only accepts the fingerprint in `config/ac1_dx9.json`.
- Use `tools/build.ps1 -Run` for the short host loop; it regenerates when driver/config changes. `-Generate` forces analysis/generation. `tools/build_android.ps1` packages the same runtime for ARM64.
- Add observed missing executable targets to `initial_discovery.startup_seeds`, then rerun. Fix systematic translator issues in `tools/lift_ac1.py`, never by hand-editing generated C.
- Unknown API/instruction/dispatch targets must give a precise diagnostic; do not pretend success. Reuse existing implementations before adding wrappers. New file-write services must keep the original installation read-only.
- Runtime source copies and CPU-header transformations are in `tools/prepare_runtime.py`; preserve its pinned inputs and update the transformation for changes to generated adapters.
- Record actual execution evidence. Compilation, entry execution, CRT completion, rendering, gameplay and Android hardware verification are separate milestones. Current progress and commands are in README.md; latest private trace is `work/startup.log`.
- No Android device was connected during initial work. Build and inspect ARM64 outputs; do not claim hardware verification without a real run.
