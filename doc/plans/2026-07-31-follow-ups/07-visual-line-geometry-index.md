# Plan 07 — Visual-line geometry cache and index

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
