# Runtime sources

The guest memory, PE loader, clock and heap adapters were copied from the user's current local DukeMP working tree. `tools/prepare_runtime.py` records and checks the exact source hashes; `work/runtime-provenance.json` records each input. AC1 startup services live separately in `ac1_runtime.c`.

`cpu.h` and `pcrecomp_heap.inc` derive from pcrecomp, copyright 2026 sp00nz, MIT; see `LICENSE.pcrecomp.txt`. The CPU header also contains helper fragments from recomp-kit, copyright 2026 veritr1x, MIT; see `LICENSE.recomp-kit.txt`. This applies to those reusable runtime implementations, not the original Assassin's Creed executable or its generated translations.

Generated game functions are private local build products under ignored `work/`. APKs and host executables that include those translations are likewise local build products. No game executable, assets, or translated game functions belong in the source repository.
