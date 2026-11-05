# Project constraints

This is a static recompilation of one fixed game, not a general emulator, a matching
decompilation or a replacement engine. Android ARM64 is the primary target; Windows
x64 is the development host. The authoritative description is `docs/architecture.md`,
and accepted and rejected mechanisms are in `docs/decisions.md`.

## Engineering rules

- One production implementation. Each guest function has one selected body behind
  `L_<VA>`, with no runtime backend selection and no per-call implementation switch.
- Ask whether a mechanism would be built today, knowing what we know now. If not,
  delete it. Git history keeps old experiments.
- Prefer static decisions: generated constants over runtime discovery, direct calls
  over dispatch, proven facts over repeated checks, deleting work over speeding it up.
  Keep genuinely dynamic behaviour dynamic, and keep runtime checks correctness needs.
- Prefer what the compiler can already do, given a clearer representation, over a
  custom pass.
- No new optimization switches or experimental modes in production. A candidate
  becomes the default, or it is removed. Experiments stay in isolated lab builds.
- No new framework, registry or abstraction unless it removes more complexity than it
  adds. Moving code between files is not simplification.
- A test of a rejected mechanism does not justify keeping it. Keep tests of real
  instruction semantics, CPU state, memory behaviour and runtime contracts. Never
  weaken a test to make cleanup pass.
- Preserve observable behaviour: memory writes, aliasing, indirect transfers, callbacks,
  threads, exceptions and API effects. Remove guest state only with a static proof that
  nothing reads it. Unknown effects stay conservative.
- Unknown API, instruction and dispatch targets stop with a precise diagnostic. Never
  pretend success.

## Evidence

- Compilation, startup, rendering, gameplay and Android hardware are separate
  milestones. Do not claim hardware verification without a real run.
- Instruction counts are diagnostics, not performance. FPS is not a CPU metric.
  Measure CPU work for equivalent guest work, with waits separated and clocks
  recorded. Never decide performance on a cutscene.
- Compare regenerated output with the previous commit when a change should not alter
  generated code. Generation refuses to report success if its inputs change during a
  run, so do not edit the lifter while one runs.
- Documentation describes the current verified system. Do not append history or
  corrections; replace the statement.

## Working rules

- The GOG installation, `D:/Prog/dukemp_recomp` and `D:/Prog/mafia_recomp` are read-only
  references. Mafia has substantial uncommitted work. Do not reset or modify them, and
  disable Python bytecode writes when importing their tools.
- Original files, generated functions, logs and build products stay under ignored
  `work/`, `build/` or Android build folders. Never publish them. The runtime accepts
  only the fingerprint in `config/ac1_dx9.json`.
- `tools/build.ps1 -Run` is the short host loop, `-Generate` forces generation, and
  `tools/build_android.ps1` packages for ARM64. `tests/run_all.py` is the host test gate.
- Add observed missing executable targets to `initial_discovery.startup_seeds`. Fix
  translator issues in `tools/lift_ac1.py`, never in generated C.
- New file-writing services must keep the original installation read-only.
