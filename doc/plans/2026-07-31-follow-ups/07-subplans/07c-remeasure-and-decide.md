# Sub-plan 07-C — Phase 4: re-measure, and decide against the tree

The parent plan's Phase 4 is not a build task.  Its deliverable is a
**measurement and a written decision**, and the decision it expects is
"no".  This sub-plan exists so that "no" is recorded with evidence rather
than reached by running out of time.

---

## The question

After sub-plans A and B, `content_generation` invalidates the whole
prefix vector on every edit.  So editing a 1M-line buffer still performs
an **O(rows) integer rebuild** per edit — no byte scans, thanks to Phase
1's width cache, but a full integer walk.

Is that walk material?  A Fenwick tree (or any incremental prefix sum)
is the fix if it is.  It is also a large, invasive structure that has to
cover row insertion and deletion, multiple widths, failure atomicity,
window detach, broad reload, and checked sum overflow — and the parent
plan is explicit: add it **only if** edit-plus-repaint counters and wall
evidence show the integer walk is now material.

---

## Method

1. Run the **full** bench matrix, including `--big` (the 1M-line corpus).
   `--big` is the only case where an O(rows) integer walk per edit could
   plausibly be felt; the 100k cases are there for continuity.
2. Compare against the parent plan's status table, which holds the
   durable baseline (13,999,212 scans / 713,958,945 bytes / 5.29 s before
   Phase 1; 100,001 / 5,100,000 / 0.47 s after).  `test/.results/bench.json`
   is gitignored — re-run, never cite a stale file.
3. Record median **and p95** wall time, peak RSS, and the counters, per
   case.  p95 is where a per-edit rebuild would show up first.
4. Measure the memory sub-plan A deliberately left unoptimized:
   duplicate vectors for equal-width views.  At 1M rows an `int32_t`
   vector is ~4 MB per view, up to ~32 MB across `MAX_WINDOWS` = 8.
   The question is whether a global `(buffer, width)` LRU is warranted —
   the parent plan says measure before inventing one.

Benchmark numbers are only comparable **against another counting build**
(`test/perfobj/kg`), never against a release `-Os` one, and `make bench`
is deliberately not a CI step because numbers taken while sanitizer lanes
drive PTYs measure the box. Run it on an idle machine and say so.

---

## The decision, either way, gets written down here

**If the evidence is small** — the expected outcome — record the defer
decision in this file: the numbers, the corpus, the build, and the
sentence that a Fenwick tree is not justified. That is a completed
sub-plan, not an abandoned one.  Do the same for the LRU.

**If it is material**, do not start building in this slice.  Write what
the evidence showed and what the incremental design must cover (the six
items above), and let it be scheduled as its own plan with its own
budget.  There are 99 points of `scc` headroom for all of Plan 07's
remaining work; an incremental tree does not fit in what is left, and
finding that out halfway through is the failure mode this sub-plan is
shaped to avoid.

---

## Gates

- `make bench` on an idle box, counting build, full matrix plus `--big`.
- `make check` — this sub-plan should change no behavior at all, so a
  failure here means something leaked in from A or B.
- `JOBS=8 .ci/run-ci-steps.sh --parallel`, standalone.

## Completion gate

- The full matrix has been re-run post-A/B and the numbers are in this
  file.
- Memory for duplicate equal-width vectors is measured, not estimated.
- A decision on the incremental tree is written here with its evidence —
  including, explicitly, "deferred" if that is the answer.
- No code changed, or if it did, why.

## Status — measured; both questions decided "defer" (2026-08-03)

Build: `test/perfobj/kg` (counting build, `DKG_PERF_COUNTERS=1`), after
`make clean` (a prior `make coverage` in this tree had left `src/fe.o` /
`src/tiny_regex.o` compiled `--coverage`, which fails the perfobj link
until cleaned — as the assignment warned it would).  Box: idle — checked
with `.ci/run-ci-steps.sh --status` before starting (no run in progress,
last logged run was historical) and again before the standalone CI run
below.  `--runs 3` throughout (bench.py's own default).

### A harness bug found first, fixed, and worth recording on its own

The full `--big` run hung on `visual-line-pos-middle-100k` (`kg did not
exit within 120 s`).  Cause: the case's key script sends bare `M-g`
(`\x1bg`) expecting the pre-existing direct `goto-line` binding, but
commit `710e9ba` — already on this branch, well before sub-plan A's
`6539641` — turned `M-g` into a prefix map (`kbd.c`: `M-g g` / `M-g M-g`).
Sending bare `M-g` now leaves kg waiting on the next key of the prefix;
`"50000\r"` therefore self-inserts as literal text instead of reaching the
goto-line minibuffer, dirties the buffer, and the case's script has no key
left to answer the resulting "Modified buffer, really quit? (y/n)" prompt
at `C-x C-c` — so the process never exits and the harness times out.
**This case has measured nothing since `M-g` became a prefix map**; the
parent plan's Phase 0 buffer-position matrix (top/middle/end) has had no
working middle-of-buffer number for as long as this branch has had that
rebinding, independent of sub-plans A/B/C.  Fixed in `utils/bench.py` by
sending `"g"` after `M-g`, matching `test/pty/131-meta-g-prefix-goto-line.yaml`'s
own spelling; the case now completes (504.36 ms median, 28,844 KiB peak,
in the full-matrix run below).  Separately, `utils/bench.py` aborts the
*entire* run on the first case that raises (`RuntimeError` from
`run_once`) rather than recording the failure and continuing to the next
case — worth naming as a robustness gap; not fixed here since it did not
block this slice once the actual bug was fixed.

**This slice did change code**, all in `utils/bench.py`, none in `src/`:
1. The `M-g` fix above, needed for the matrix to complete at all.
2. Added `visual-line-edit-1m` to `BIG_CASES`: the existing `--big` set
   had `open-lines-1m` (no visual-line mode) and `visual-line-1m` (visual-
   line mode, cursor motion only, no edit) but **no case that both builds
   the index and then edits it at 1M rows** — exactly the shape this
   sub-plan's first question needs evidence for, and the gap this
   sub-plan exists to close.
3. Added `visual-line-hsplit-1m`: two windows on the *same* buffer at the
   *same* width, at 1M rows — the actual duplicate-vector scenario the
   second question is about.  The existing unequal-width cases
   (`vsplit`/`4win`) are legitimately distinct keys an LRU would not
   touch; this is the case that would benefit from one.

`make complexity-check` (5439/5500) and `make pmccabe-check`
(`draw_window_rows` at 67) both ran clean and unchanged from sub-plan B's
closing numbers, confirming nothing in `src/*.c`/`src/*.h` moved.

### Full matrix, 100k/20k corpora (`make bench`, no `--big`)

| case | median ms | p95 ms | peak RSS KiB | vgeom_hit | vgeom_rebuild | vgeom_fallback | prefix_visit | row_scan | byte_scan |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| visual-line-100k | 442.24 | 445.12 | 28,268 | 36 | 1 | 0 | 100,001 | 100,001 | 5,100,000 |
| visual-line-warm-100k | 441.90 | 444.14 | 28,116 | 80 | 1 | 0 | 100,001 | 100,001 | 5,100,000 |
| visual-line-edit-100k | 506.90 | 509.49 | 28,116 | 24 | 2 | 0 | 200,002 | 100,002 | 5,100,001 |
| visual-line-resize-100k | 721.48 | 723.81 | 28,620 | 19 | 2 | 0 | 200,002 | 200,002 | 10,200,000 |
| visual-line-vsplit-100k | 757.52 | 760.96 | 29,048 | 140 | 3 | 0 | 300,003 | 300,017 | 15,300,714 |
| visual-line-hsplit-100k | 687.60 | 688.32 | 28,900 | 141 | 2 | 0 | 200,002 | 100,001 | 5,100,000 |
| visual-line-4win-100k | 1019.02 | 1023.48 | 29,716 | 142 | 5 | 0 | 500,005 | 500,033 | 25,501,428 |
| visual-line-pos-top-100k | 322.04 | 326.34 | 28,604 | 10 | 1 | 0 | 100,001 | 100,001 | 5,100,000 |
| visual-line-pos-middle-100k | 504.36 | 506.11 | 28,844 | 44 | 1 | 0 | 100,001 | 100,001 | 5,100,000 |
| visual-line-pos-end-100k | 386.24 | 388.41 | 28,524 | 15 | 1 | 0 | 100,001 | 100,001 | 5,100,000 |
| visual-line-tabs-100k | 350.10 | 352.13 | 28,360 | 15 | 1 | 0 | 100,001 | 100,001 | 3,100,000 |
| visual-line-invalid-bytes-20k | 314.49 | 314.51 | 11,840 | 15 | 1 | 0 | 20,001 | 20,001 | 1,020,000 |
| visual-line-unicode-20k | 380.88 | 381.52 | 11,868 | 50 | 1 | 0 | 20,001 | 20,001 | 1,240,000 |

The six cases the assignment gave as a same-day reference reproduced
exactly (`vgeom_hit`/`rebuild`/`fallback`/`prefix_visit`/`row_scan` all
identical), which is the box behaving consistently rather than a
coincidence.

### `--big` matrix (1M-line corpus)

| case | median ms | p95 ms | peak RSS KiB | vgeom_hit | vgeom_rebuild | vgeom_fallback | prefix_visit | row_scan | byte_scan |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| open-lines-1m | 599.09 | 600.70 | 252,888 | — | — | — | — | — | — |
| visual-line-1m | 1229.92 | 1230.98 | 256,772 | 50 | 1 | 0 | 1,000,001 | 1,000,001 | 51,000,000 |
| visual-line-edit-1m | 1339.14 | 1340.86 | 256,956 | 24 | 2 | 0 | 2,000,002 | 1,000,002 | 51,000,001 |
| visual-line-hsplit-1m | 1483.03 | 1499.08 | 261,020 | 141 | 2 | 0 | 2,000,002 | 1,000,001 | 51,000,000 |

`visual_row_scan` moving by exactly 1 across an edit (`1,000,001` →
`1,000,002`), while `visual_prefix_visit` jumps by `numrows + 1`
(`1,000,001` → `2,000,002`), is the counter evidence for the question
itself: an edit rebuild is a pure O(rows) *integer* walk — Phase 1's width
cache means it does not rescan bytes for any row but the one actually
touched.

### Isolating the per-edit rebuild's wall cost

The process-level delta between `visual-line-edit-1m` and `visual-line-1m`
(109 ms) is not a clean number — the two cases send different key counts.
To isolate the rebuild itself, a one-off script
(`/tmp/.../scratchpad/isolate_rebuild_cost.py`, not checked in) drives
`test/perfobj/kg` under a real pty mid-session, past the cold first build,
and brackets one cache-hit repaint (`Up`) against one edit-triggered
repaint (`x`, forcing exactly one rebuild), both measured with the
**identical** quiet-period tail (0.1 s) so that fixed cost cancels in the
delta rather than being folded into only one side of the comparison (an
earlier version of the script used different tails for the two
measurements and produced a spurious ~100 ms inflation — caught by
`vgeom_rebuild`'s counter not matching the wall-time story, not by
inspection).  Three runs, two edits measured per run (6 deltas each):

| corpus | motion repaint (cache hit) | edit repaint (1 rebuild) | delta range | delta mean |
|---|---:|---:|---:|---:|
| 100k rows | ~101 ms | ~106 ms | 4.50–6.24 ms | ~5.2 ms |
| 1M rows | ~102 ms | ~151 ms | 46.36–51.02 ms | ~48.7 ms |

The ~9.4x delta ratio for a 10x row-count ratio is consistent with a pure
O(rows) walk (~48–55 ns/row either way), and matches the counter evidence
above.

### Duplicate equal-width vector memory

**Computed**, from `sizeof` on this compiler (gcc, C23) applied to the
actual struct in `src/vgeom.c`:

```
sizeof(struct kg_buffer_handle)                          = 24 bytes
sizeof(struct kg_vgeom_index) header (buf + generation +
        win_w + row_count, before the flexible array)    = 40 bytes
per-view vector = 40 + (numrows + 1) * sizeof(int32_t)
  at   100,000 rows:  400,044 bytes ≈ 0.382 MiB
  at 1,000,000 rows: 4,000,044 bytes ≈ 3.815 MiB
```

**Measured**, peak RSS delta between two windows on the same buffer at the
same width (`hsplit`) and one window (from the tables above):

| corpus | measured Δ (one duplicate vector) | computed vector size |
|---|---:|---:|
| 100k rows | 632 KiB ≈ 0.617 MiB | 0.382 MiB |
| 1M rows | 4,248 KiB ≈ 4.148 MiB | 3.815 MiB |

The measured number is somewhat larger than the pure vector size at both
scales (a second window carries other state too, not only its `vgeom`
vector), but the two agree to within the same order of magnitude and
scale together, which is the cross-check that matters.

**Worst case**: `MAX_WINDOWS` is 8.  The only arrangement a global
`(buffer, width)` LRU could help is *identical* buffer and width across
windows — the `vsplit`/`4win` cases above are legitimately distinct keys,
untouched by such an LRU regardless.  Eight windows on the same 1M-row
buffer at the same width, the single most degenerate arrangement a user
would have to construct deliberately, wastes at most 7 duplicate vectors:
7 × 3.815 MiB (computed) ≈ 26.7 MiB, or 7 × 4.148 MiB (measured) ≈ 29.0
MiB.  Against that: one window's own baseline footprint for a 1M-row
buffer is already 256,772 KiB ≈ 250.8 MiB (`visual-line-1m`'s peak RSS
above), of which the row array alone (`row_array_bytes`) is 134,216,704
bytes ≈ 128.0 MiB.  The worst-case duplication waste is therefore roughly
11% of one window's own footprint, in an arrangement most sessions never
approach.  At 100k rows the same worst case is 7 × 0.382–0.617 MiB ≈
2.7–4.3 MiB — trivial next to the ~28 MiB baseline.

### Decision 1 — the O(rows) integer rebuild per edit: **defer** the Fenwick tree

The walk is real and the counters confirm its shape exactly as the parent
plan described: `content_generation` invalidates the whole prefix vector
on every edit, and the next geometry query pays a full O(rows) integer
walk (no byte scans beyond the touched row).  Isolated wall-clock cost is
~5 ms at 100k rows and ~49 ms at 1M rows, scaling linearly with row count.

**Defer**, because:
- ~49 ms is measurable and sits at the edge of perceptibility for one
  isolated keystroke, but it is not paid on every keystroke of sustained
  fast typing: the main loop's flood detector (`INPUT_FLOOD_THRESHOLD` =
  64 bytes queued, `src/tty.c`) already skips the redraw once typing
  outpaces it (`editor_input_flood()`, checked before
  `editor_refresh_screen()` in `src/main.c`), so a burst that queues past
  that threshold amortizes the cost over several keystrokes rather than
  paying it once per key.
- 1M-line files edited in visual-line-mode are the extreme end of this
  bench matrix's own stress corpus (`--big` exists to probe headroom), not
  a demonstrated everyday target for this editor. At 100k rows — closer to
  files this editor is actually used on — the cost is ~5 ms, well under
  anything a user would notice.
- The fix the parent plan itself specifies is large and invasive: row
  insertion/deletion, multiple widths, failure atomicity, window detach,
  broad reload, and checked sum overflow, all against a shared budget of
  **61 scc points left for sub-plans C and D combined** — nowhere near
  enough to build it responsibly in this campaign, and finding that out
  after starting is exactly the failure mode this sub-plan exists to
  avoid.

**What would flip this**: if kg's supported use grows to explicitly
include smooth, uninterrupted per-keystroke latency on multi-million-line
buffers in visual-line-mode — rather than merely surviving them — the
~49 ms/edit number at 1M rows is the evidence that would justify
scheduling the incremental design, with its own budget, to cover the six
items above.

### Decision 2 — duplicate equal-width vectors: **defer** the global LRU

**Defer**, because:
- The waste is bounded, quantified, and modest relative to the buffer's
  own memory footprint even in the single most degenerate arrangement a
  user would have to construct on purpose (all 8 available windows on the
  identical file at the identical width): ~27–29 MiB against a ~251 MiB
  single-window baseline, itself dominated by the row array (~128 MiB).
- A global `(buffer, width)` LRU reintroduces exactly the second
  cache-eviction policy sub-plan A's design deliberately declined to bolt
  onto `erow`, and would have to answer the same invalidation/lifetime
  questions (buffer close, generation bump, width change, window detach)
  a second time, against the same 61-point shared budget.

**What would flip this**: routine editing of multi-million-line buffers
with more than 2–3 simultaneous same-width windows on the same buffer, or
a memory-constrained deployment where tens of MiB is significant, would
justify it.

### Gates

- `make check`: PASS — 32/32 native unit binaries, 405/405 PTY cases, 0
  FAIL/ERROR/XPASS.
- `make complexity-check`: 5439/5500, unchanged from sub-plan B's close.
- `make pmccabe-check`: PASS, `draw_window_rows` at 67 (baseline),
  unchanged from sub-plan B's close.
- `JOBS=8 .ci/run-ci-steps.sh --parallel`, standalone (checked
  `--status` idle both before starting and before the confirming rerun
  below): 11/12 lanes passed on the first run.
  `ci-03-gcc-analyzer-valgrind` failed with a single PTY failure
  (`lisp-process-cwd`, 404/405) — one of the two cases CLAUDE.md already
  names as flaking under parallel-lane contention.  Re-ran
  `.ci/ci-03-gcc-analyzer-valgrind.sh` standalone on the same, otherwise
  idle box: 405/405 PTY, 32/32 unit, exit 0.  Per CLAUDE.md's own rule
  ("a lone failure ... is a flake until a standalone re-run of that lane
  says otherwise"), that closes it as a flake, not a regression — nothing
  in this slice touched `src/*.c`/`src/*.h`, so there is no mechanism by
  which it could have caused a valgrind-side failure in an unrelated
  Lisp-process test.

No behavior changed; the only files touched are this document and
`utils/bench.py` (bench-harness fix and two new `--big` cases, detailed
above).
