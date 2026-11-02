# AC1's occlusion-query wait on Android (2026-11-02)

Phone: Galaxy S24 Ultra (SM-S928B), DXVK 3.1 + Turnip, 1560x720, MSAA 2x, the
busy-1814 city scene (`work/phone/city_run.sh`, 20 s window after the replay).
All comparisons are interleaved runs of the same scene; private evidence is in
`work/phone/city/` (`q*`, `yab-*`, `nb-*`).

## What the game waits for

`0x00AA40B0` takes a query holder, calls `IDirect3DQuery9::GetData(&count, 4,
D3DGETDATA_FLUSH)` and, while it returns `S_FALSE`, calls `SwitchToThread`
(`0x007A0060`) and polls again. It returns the sample count, or 1000 when the
holder has no query. The query is `D3DQUERYTYPE_OCCLUSION`.

The waits come from `0x00ABB0E0`, which draws a list of 0x14-byte items in two
modes of the same frame. Mode 1 wraps each item's draw in `Issue(BEGIN)`
(`0x00AA4080`) and `Issue(END)` (`0x00AA40A0`, called from `0x00ABB412`). Mode 2
reads each flagged item's result through `0x00AA40B0` (`0x00ABB284`): a count
above `[0x0194FD1C]` draws the item again and clears bit 23 of its object's
`[+0x60]`; otherwise the draw is skipped and `0x00449120(1)` sets that bit.
Items without a query take the visible path.

`AC1_TRACE_QUERY_SITES=1` counts query ends per game call site (the caller of
the `0x00AA40A0` wrapper) and how many of them GetData had to wait for. Per 2 s:

| End issued from | Ends | Waited for | Pending polls |
|---|---|---|---|
| `0x00ABB417` (pass 1 of `0x00ABB0E0`) | ~10,500 | ~70 | ~60,000 |
| `0x00AC08D9` | ~73 | all | one each |
| `0x00ABC0EE` | ~23,000 | 0-2 | <=400 |
| `0x00AA4181` | ~3,000 | 0 | 0 |

## Where the wait goes

`AC1_DXVK_QUERY_STATS=1` (counters patched into DXVK by
`ports/dxvk_android/prepare.py`; off by default) attributes every pending poll
to the first unfinished step and times waited queries from their first
GetData. Typical 2 s window at ~36 FPS:

- ~18,000 results/s are ready at the first poll; ~35/s are waited for.
- At the first poll the end is still in DXVK's unsent CS chunk; that poll's
  `ConsiderFlush` sends it (one flush per waited query). From the first
  GetData: CS ran the flush after ~17 us, `vkQueueSubmit` returned after
  ~110 us, the GPU finished and the result was ready after ~1.7 ms (max 3.4 ms).
- Spinning takes 34-60 ms per second, 3.5-6% of frame time. The ~5.4 ms between
  `Issue(END)` and the first GetData is the game's own work, not a wait.
- After the one wait in a pass, the rest of that pass's results are ready.

## What was tried

| Experiment | Result |
|---|---|
| SwitchToThread routed once, no host sleep (stock DXVK, 3 pairs) | 35.1 vs 36.5 FPS; never faster, p99 worse, prime core clocked lower |
| SwitchToThread sleeping 50 us (earlier) | removes the spin's CPU time, -4% FPS (governor lowers the clock) |
| Flush at `Issue(END)` of queries DXVK marks stalling | 9.8 FPS: the flag is sticky, so ~2,700 extra submissions/s; waits rose from ~45/s to ~2,100/s |
| Strong flush hint there instead | 35.7 vs 37.1 FPS; the waited queries are not the flagged ones |
| Pending result treated as visible (opt-in prototype) | 32.1 vs 36.4 FPS, +35% draws: without the wait the GPU never catches up, ~25,000 results/s substitute |
| Pending result replaced by the object's previous result | 34.2 vs 36.4 FPS, +5% draws, and Altaïr disappeared: a stale "hidden" never refreshes |
| `d3d9.floatEmulation = False` | 37.5 vs 36.6 FPS (2 pairs, noise), same GPU load; default kept |
| MSAA off; `TU_DEBUG=sysmem` (earlier, Masyaf scene) | same FPS (GPU busy 65% -> 61%); neutral |

Cheaper polling cannot shorten a wait for the GPU, extra submissions cost more
than they save (likely through split render passes on the tiler; unmeasured), and the single wait is what keeps culling exact
for the rest of the pass, so the synchronous wait stays. The prototype is kept
privately as `work/patches/occlusion-nonblocking-prototype.diff`.
