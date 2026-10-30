# Fingerprint-pinned native regions

Regions extend the leaf override mechanism to a closed cone of guest functions.
They apply only to the executable fingerprint in `config/ac1_dx9.json`.
`native_overrides` entries with `kind: region` pin the complete recovered
instruction stream of the entry and every member, plus SHA-256 hashes of the
explicit subsumed byte ranges. Generation rejects changed bytes, uncovered
instructions, missing callees, indirect transfers and an invalid effect boundary.
The generated private oracle cannot dispatch outside the pinned cone.

`AC1_NATIVE_REGIONS=00AE11A0` selects the palette region; `1` selects all regions,
and `0` forces translated execution. With the variable unset, the palette is on
for the measured Windows x64 direct-memory/trace-EIP build; other configurations,
including ARM64 and the portable scalar build, remain opt-in. Reuse and modifier
fusion stay off. `AC1_NATIVE_OVERRIDES` controls the separate leaf namespace.
Twenty interleaved gameplay pairs measured +2.49% median paired FPS
(bootstrap 95% interval [+1.58%, +5.71%]) and -2.79% cycles/frame for the palette.
Reuse was inconclusive; modifier fusion regressed -3.91% over ten pairs.
`AC1_NATIVE_REGION_VERIFY=N` checks the first N eligible calls of each region
(or inherits `AC1_NATIVE_VERIFY`). Unsupported inputs run the complete translated
entry and increment a fallback counter, with the first reason/address logged.

The palette oracle stops at `00AE1707`, before the indirect D3D upload. Verification
snapshots the CPU and the complete audited write set, including below-SP scratch,
runs the translated prefix, saves its pending upload descriptor and payload,
restores memory, and runs the native prefix. It compares every CPU byte, every
byte in the write ranges and every pending-upload byte. The first mismatch stops
execution with the call number and CPU offset or guest address. Only a successful
comparison reaches the common upload/epilogue continuation, once. No real upload
is executed by the oracle.

The current palette contract accepts at most 32 bones, float32, packed16 and
indexed float32 poses, mapped nonaliasing inputs, ordinary finite values and
guest FP control fields. The arithmetic follows the translated C casts under
all four guest rounding modes; it does not add extended x87 precision. SSE2 and
NEON perform matrix products and transposes; the
scalar reference uses the same operation order. Quaternion construction retains
the original binary64 operations and binary32 rounding points. FMA contraction
is disabled. A generated final-element/control residual preserves exact CPU and
guest scratch state. It does not redo the bulk palette math. Outputs go directly
to `D+0x790+64*i`; pending fields remain at `D+0x2a70/+0x2a74`.

`AC1_NATIVE_PALETTE_REUSE=1` enables a separate numerical cache when the region is
selected. It compares every resolved quaternion, position and bind matrix, plus
identity, scale and context inputs. A bucket hash cannot establish a hit. Entries
are restricted to one Present interval and a reset/lifetime epoch; allocation is
bounded to 1,024 entries. Every hit still reconstructs CPU/stack scratch and
executes the original upload once. This avoids assuming an unproven pose version.
Use `AC1_NATIVE_STATS=1` for progress/fallback/reuse statistics.

Focused verification commands (all generated/original artifacts stay ignored):

```powershell
python -B tests/test_native_bindings.py
python -B tests/test_region_bindings.py
python -B tests/palette_region_oracle.py --cases 100000
python -B tests/palette_region_oracle.py --portable --cases 100000
python -B tests/palette_region_oracle.py --reuse --cases 100000
```

Synthetic verification, host gameplay, measured FPS improvements, ARM64 compilation
and ARM64 hardware execution are separate milestones. Results and exact commands
for this experiment are retained in `work/engine_prof/phase2/`.

The separate `004E6820` trial uses a transactional write journal for the whole
modifier batch. It fuses Hinge (`00654090`) and Compress (`006C4FF0`) with their
transform/math children and the existing SIMD kernels. Large unchanged pose/CRT
kernels remain direct private C calls. Roll and LookAt callbacks run their pinned
translated algebra, so mixed batches can retain those paths. Only Hinge/Compress
appear in the native callback whitelist; other unbound types use full translated
fallback. Recursive CRT helpers are never forced inline.

Every write in the closed cone, including x87 stores, saved registers and stack
scratch, records its initial 16-byte block. Verification runs the translated cone,
saves final block contents, restores them, then compares the native CPU and union
of written blocks. An additional native write is also compared against its initial
contents. The journal is bounded to 64 KiB of distinct written guest memory. An
unsupported transfer unwinds before dispatch, restores all writes and CPU state,
then executes the complete translated batch once. No external import or callback
is executed by a rejected attempt. A whole-list preflight avoids starting mixed
batches with an unbound callback or pending lazy pose evaluation. This journal
overhead is part of the measured production path, rather than hidden by disabling
rollback during benchmarks.

```powershell
python -B tests/modifier_region_oracle.py --cases 200000
```
