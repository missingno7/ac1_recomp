# ABI + dirty-sync locals: region compatibility

Update 2026-10-30: register locals (now GPR and XMM) became the default after
the controlled comparisons in [perf-architecture.md](perf-architecture.md);
CFG flags remain the default and ABI stays opt-in. The region-compatibility
fix below applies to both. The original candidate notes follow.

Source analysis found two interactions with the new native regions:

* Private ABI copies could bypass a region's public native wrapper. Region
  roots now receive the same private-clone exclusion as native leaf entries.
* Copying already transformed bodies into a closed oracle could retain ABI
  private calls outside its graph, or split GPR local declarations away from
  their uses in a continuation. Region bodies now take an independent CFG
  snapshot before ABI/private-body and GPR-local transformations. Ordinary
  guest functions still receive the requested optimization.

Only pinned region members receive that additional CFG pass; unrelated
functions do not need another whole-program transformation.

A real-emitter regression constructs a three-function PE fixture, keeps an
ordinary private ABI callee, pins the native root, and compiles its closed
region oracle and split continuation with ABI + locals enabled in both
checked and direct memory configurations. All 98 tests and lifter self-tests
pass, including existing full CPU/stack public/private/dispatch fallback
comparisons and dirty-sync branch/loop/opaque-helper contracts.
The same regression fails before this fix during region generation because
an appended private root body duplicates the continuation boundary label.

The provisional host combination includes the captured-memory candidate;
therefore its correct control is that same memory candidate with CFG/locals
off. It must not be compared to the pre-memory baseline to attribute a
defaults-only gain. First accept or reject memory in the calibrated window,
then compare ABI + locals against the accepted current default. No gameplay
replay or Android hardware verification is claimed for this step.

The provisional RelWithDebInfo host build passed: 708.04 s preparation and
468.33 s native configure/compile/link. The extra whole-program snapshot was
subsequently restricted to region members; emitted-C identity is checked
separately. The combination admits 86,994 public functions to register locals
and narrows 5,988 private ABI functions, 28,152 private returns and 32,733
direct call sites. All 101,995 public entries keep full return flags; five
native leaf/region roots are pinned.

With captured memory as the control, static instruction sites in three hot
public bodies change as follows; each retains one arena-value load.

| Function | x64 instructions | ARM64 instructions |
|---|---:|---:|
| 004F5A70 | 2850 → 2756 | 3031 → 2856 |
| 00428540 | 566 → 510 | 698 → 584 |
| 0051DD60 | 242 → 221 | 197 → 191 |

ARM64 evidence uses separately compiled real generated bodies and prepared
headers, not a full APK or hardware run. Aggregate `.text` nevertheless grows
147,912,584 → 166,788,744 bytes (+12.76%). Fewer instructions in these bodies
do not establish a whole-game win. The combined executable remains unstripped
in `build/host-relwithdebinfo/`; the two stripped controls are preserved under
`work/global/bin/` for the calibrated comparison.
