# Architecture

One pipeline turns one pinned executable into one native program.

## Pipeline

1. **Discovery** (`tools/lift_ac1.py`, using the pinned pcrecomp disassembler).
   Starts from the entry point, initializer tables and `startup_seeds` in
   `config/ac1_dx9.json`. It follows calls, jump tables, code immediates, indexed
   function tables and register-loaded switches, and validates each target.
2. **Lifting.** Each recovered function becomes one C function, `L_<VA>`. Only
   instructions in recovered blocks are lifted, so data between blocks stays out.
3. **Passes**, applied to every function. A pass that cannot handle a function
   leaves it unchanged; nothing is selected by configuration.
   - Whole-program flag contracts: only flags a reader needs are computed.
   - Native x87: the register stack lives in block-local doubles.
   - GPR and XMM locals: registers live in locals between calls.
   - Stack lane forwarding, and direct calls between lifted functions.
4. **Emission** into 1,000 stable translation units under `work/lift_initial`.
5. **Compilation** with GCC on Windows or NDK Clang on Android, always with
   `-fwrapv -fno-strict-aliasing -ffp-contract=off`.

## Guest state

Each guest thread has a `CPU` struct, stack, TIB and TLS. Generated functions keep
registers in locals and synchronize with the struct at calls, returns and any helper
that takes the CPU. Guest memory is one reserved 4 GiB arena: guest address `a` is
host address `arena + a`.

## Calls

- **Direct guest calls** are plain C calls to `L_<VA>`. Calls on a loop also count
  returns, so a guest thread that never reaches an import still offers the baton.
- **Everything else** goes through `dispatch`. Targets in the lifted range use a
  small cache and a 16-byte line index. Imports have unique tokens and go to their
  handlers. COM methods go to the D3D9, DirectInput or DirectSound bridges.
- **Unknown targets stop** with a diagnostic. They are never treated as success.

## Runtime services

`src/runtime/ac1_runtime.c` implements the Win32 surface the game imports, including
files under a read-only install root, heap, threads and synchronization.
`src/platform` holds window, input and audio services, and `src/graphics` the D3D9
bridge. Bink imports are decoded natively through FFmpeg.

Guest threads are native threads under a serializing baton, so only one runs guest
code at a time. See [threading.md](threading.md).

Rendering goes through DXVK to Vulkan, with Turnip on Android.

## Correctness contract

The original game's observable behaviour is the contract. That covers guest memory
writes and aliasing, registers and flags where something can read them, x87, MMX
and SSE results, calls and stack effects, indirect branches and callbacks,
synchronization and external API effects.

Guest state may be dropped only where a static proof shows nothing can read it.
Unknown effects are treated conservatively.

Known limits:

- Native x87 drops the tag word, FPU IP/DP/opcode and unmasked-exception
  diagnostics. Functions that observe them keep the plain lifted form.
- Guest SEH is not dispatched. `RaiseException` handles only the debugger
  thread-name form, and other exceptions stop the guest.
- NaN payloads can differ from x86, because host operand order differs.

## Static first

Prefer a compile-time fact over a run-time check, a direct call over dispatch, and
deleting work over speeding it up. Keep real run-time behaviour, such as object
lifetimes, unproven indirect targets, threads and external APIs, at run time.
