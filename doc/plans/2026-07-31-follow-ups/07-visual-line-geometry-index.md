# Plan 07 — Visual-line geometry cache and index

## Status (2026-08-01, after phases 0-1)

Phases 0 and 1 are done; phases 2-5 are not started.  The `visual-line-100k`
benchmark reproduced the program's cited baseline exactly (13,999,212 row
scans, 713,958,945 row bytes, on the code before either phase) before any
change landed, per rule 1's "characterization precedes changed behavior."

- **Phase 0** added `KG_PERF_VISUAL_PREFIX_VISIT` (rows visited by the
  segment-summing walks, independent of whether a visit rescans bytes) and
  split the bench matrix the phase asked for: warm/motion, edit, resize (a
  mid-session `TIOCSWINSZ`, new to `utils/bench.py`), an unequal-width
  vertical split, window-count variants (vertical/horizontal/four), buffer
  position (top/middle/end), and tabs/invalid-byte corpora, alongside the
  existing tabs-free/unicode ones.  No cache existed yet, so this phase was
  one line in `src/mode.c` (the counter increment) plus tests and bench
  cases; scc stayed at the program's 4144.
- **Phase 1** added the per-row wrapped-width cache (`erow.wrap_cache_win_w`
  / `wrap_cache_vcols`, def.h) and consolidated the width/segments calls the
  plan named as its funding (`goto_visual_row_col()`, `visual_line_cursor_col()`
  at end-of-line).  scc measured 4146 against that consolidation, 2 over
  4144; `SCC_COMPLEXITY_MAX` moved to 4200 by the maintainer's wave-2 budget
  decision (56 shared headroom across concurrent tracks), not a self-funded
  raise -- see the Makefile comment for the exact accounting.

Measured effect on `visual-line-100k` (100,001-row corpus, one window):

| | before (phase 0 baseline) | after (phase 1) |
| --- | --- | --- |
| row scans | 13,999,212 | 100,001 |
| row bytes | 713,958,945 | 5,100,000 |
| wall (counting build, median) | 5.29 s | 0.47 s |

Phase 1's three named acceptance shapes, confirmed both as `test/test_perf.c`
counter assertions and independently via `make bench`:

- warm unchanged repaint (including plain cursor motion): 0 row scans, 0 row
  bytes -- `visual-line-warm-100k`.
- one-row edit: exactly 1 row scan, of exactly the edited row's byte count --
  `visual-line-edit-100k`.
- a genuinely new width: exactly one cold scan per row for *each* width the
  session has used -- `visual-line-resize-100k` measured 200,002 scans
  (100,001 rows at each of two widths) after a mid-session resize, not
  200,000+ again on every subsequent repaint at either width.

`KG_PERF_VISUAL_PREFIX_VISIT` is deliberately unmoved by phase 1: the same
`visual-line-100k` run visits 13,699,181 rows both before and after the
cache lands, because the width cache does not change *how many times* the
O(rows) walks revisit a row, only whether that revisit rescans it.  That
count, and the wall-clock evidence it is not yet material at this corpus
size, is what phase 2's persistent prefix vector is for.

**Unequal-width thrashing, measured rather than assumed** (the plan's "one
entry per row may thrash... measure this case"): `visual-line-vsplit-100k`
(80-column terminal, `win_reflow()` gives the two windows 39 and 40) scans
2,900,031 rows -- 29x the single-window baseline -- because the two windows'
alternating repaints evict each other's cache entry for every row on every
switch.  `visual-line-hsplit-100k` (two windows, same width) scans exactly
100,001, confirming the thrash is specific to *unequal* widths and not
splitting itself.  `visual-line-4win-100k` (mixed widths) scans 3,300,039.
This is real evidence for phase 2's "initially tolerate duplicate vectors
... measure memory before inventing a global LRU" -- a single-entry-per-row
design is measurably wrong for this shape, and a view-owned prefix vector
(keyed by width, one per view) sidesteps it entirely rather than needing a
second cache-eviction policy bolted onto `erow`.

RSS: `sizeof(erow)` went from 56 to 64 bytes on the 64-bit build this ran
on -- the two new `int` fields land exactly in the struct's existing
trailing alignment padding, so the cost is 8 bytes/row here, the low end of
the plan's "roughly 8-16" estimate, not 16.

Deliberately not done: phases 2-5 (the persistent prefix vector, the
one-traversal draw loop, the incremental-tree re-measurement gate, and the
window-text-width seam for later line numbers) are unstarted, per this
plan's own phasing -- phase 1 is where this campaign's assignment ends.

## Outcome

An unchanged visual-line repaint performs no row-byte width scans and does not
walk from row zero once per screen line.  Geometry remains exact for tabs,
wide/combining glyphs, escaped invalid bytes, exact-width EOL, multiple window
widths, resize, and edits.  CI gates deterministic work counters; wall time is
recorded only by `make bench`.

## Evidence and priority

The existing `visual-line-100k` benchmark measured 13,999,212 row scans and
713,958,945 row bytes across 24 refreshes: roughly 583k rows and 30 MB per
repaint, 6.8 s median versus 0.18 s with the mode off.  `make bench` writes to
the gitignored `test/.results/bench.json`, so the figures recorded here are
the durable baseline; re-run the case before claiming an improvement.  The feature is optional but
user-reachable and its evidence gate is met.  Schedule it before optional Lisp
packages once another migration/extraction has freed complexity; do not raise
the 4223 cap.

Coordinate display changes with Plan 03 decorations and later line-number
gutters.  All consumers must use the same width argument.

## Phase 0 — Refine counters and benchmark cases

Keep `KG_PERF_VISUAL_ROW_SCAN`/`BYTE_SCAN` as actual row-byte width work.  Add a
prefix-row-visit counter plus cache hit/miss/rebuild counters only where they
answer the new shape.

Split measurement into:

- cold first repaint;
- warm unchanged repaint/cursor motion;
- one-row edit then repaint;
- width/resize change;
- same buffer in two unequal-width windows;
- one, vertical, horizontal, and four windows;
- top/middle/end positions and 24x80/large terminal;
- ASCII, tabs, wide/combining, and invalid-byte corpora;
- 100k default and optional 1M-line `--big` cases.

Retain the historical aggregate case for comparison, but do not hide load time
inside the new repaint assertions.  Record baseline counters, median/p95, and
RSS from the counting build.

## Phase 1 — Per-row wrapped-width cache

Add only the minimum cache to `erow` initially:

```text
cached window width + cached wrapped display width + valid bit/key
```

Wrap count derives from width; do not store it twice.  Invalidate before every
`editor_update_row()` rebuild so even later render-allocation failure cannot
leave geometry claiming the old text.  Fresh rows start invalid; row moves and
sorts carry a cache with the text it describes.  Window width is the key, so
resize needs no buffer sweep.  Tab width and Unicode width tables are fixed
today; add a semantics generation only when one becomes mutable.

One entry per row may thrash when unequal-width windows alternately paint the
same buffer.  Measure this case.  Do not add multiple entries preemptively.

Consolidate width/segments calls so `goto_visual_row_col()` and cursor helpers
do not ask for the same row width twice.  Native tests pin empty/exact-width
rows, tabs, wide glyph wrap padding, combining marks, invalid bytes, virtual
space, and nonpositive window width.

Acceptance:

- warm unchanged single-width repaint scans zero row bytes;
- one-row edit misses only that row's width cache for the same width;
- a genuinely new width performs at most one cold byte scan per row;
- RSS cost is recorded (roughly 8–16 bytes per row must be justified).

## Phase 2 — Persistent view-owned prefix vector

Give each window/view a lazily allocated cumulative segment vector keyed by:

```text
buffer handle + buffer content generation + window text width + row count
```

Entry `i` stores visual segments before logical row `i`; use checked
`size_t`/64-bit arithmetic internally and clamp/refuse at existing int API
seams.  On key mismatch rebuild O(rows).  Thanks to Phase 1, an edit rebuild
walks integers across all rows but scans text bytes only for invalid/width-miss
rows.  On allocation failure fall back to the current correct scan path and
report only through established OOM policy; cache failure must not break
editing or drawing.

View ownership naturally supports simultaneous widths and ties cache lifetime
to window detach.  Initially tolerate duplicate vectors for equal-width views;
measure memory before inventing a global `(buffer,width)` LRU.

Expose one geometry API:

- total visual rows from the final prefix;
- logical row/column to visual row;
- visual row to logical row/segment by lower-bound search;
- iterator initialized at a visual top row and advanced screen-row by
  screen-row.

Route `get_visual_row()`, `get_total_visual_rows()`, `find_visual_row()`, and
`goto_visual_row_col()` through it or retire their duplicate scans.  Keep
coordinates explicit per `doc/coordinates.md`.

## Phase 3 — Draw with one traversal

`draw_window_rows()` currently starts `find_visual_row()` from logical row zero
for every screen row.  Initialize one iterator for the window's top visual row,
draw its segment, then advance.  The prefix lower-bound occurs once per window,
not once per screen line.

Acceptance counters per unchanged repaint:

- zero visual byte scans after warmup;
- prefix/logical visits bounded by `O(log rows + win_h)` per window;
- no `win_h * numrows` shape;
- byte-for-byte identical frame output for existing golden/PTY cases.

Add screen-content PTYs for edit, resize, exact-width EOL, wide glyph at wrap
edge, unequal-width splits, and cursor/scroll preservation.

## Phase 4 — Re-measure before incremental trees

Run the full matrix.  `content_generation` intentionally invalidates the
prefix vector after every edit, so editing a 1M-line buffer still performs an
O(rows) integer rebuild.  Add a Fenwick tree or other incremental prefix sum
only if edit-plus-repaint counters/wall evidence show that integer walk is now
material.

If justified, the incremental design must cover row insertion/deletion,
multiple widths, failure atomicity, window detach, broad reload, and checked
sum overflow.  Update affected rows from Plan 03 change events or the edit
result; do not add per-row generations merely to support an unmeasured design.
If evidence is small, keep the simple persistent vector and record the defer
decision here.

## Phase 5 — Prepare later display consumers

Expose "window text width" as one value after separators and any future gutter
are subtracted.  Visual-line cache/index keys, rendering, cursor placement,
rectangles, and future line numbers all consume that same width.  Do not build
line numbers in this plan; make their later geometry seam unambiguous.

## Completion gate

- Counter bounds above pass in `test/test_perf` under normal, sanitizer, and
  valgrind builds.
- `make bench --case visual-line-*` records before/after median/p95/RSS and
  counters; no hard wall-time CI gate is added.
- Existing and new visual-line PTYs render and save identically across tabs,
  UTF-8, invalid bytes, resize, splits, and tiny terminals.
- Width/index arithmetic is overflow checked and OOM falls back correctly.
- Default (visual-line off) repaint counters and frame bytes do not regress.
- Both Lisp builds and the full CI runner pass without raising complexity or
  coverage ratchets.
