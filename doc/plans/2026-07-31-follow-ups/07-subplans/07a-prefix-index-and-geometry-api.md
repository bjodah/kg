# Sub-plan 07-A — Phase 2: view-owned prefix vector and one geometry API

The substance of Plan 07's remaining work.  Everything after it either
consumes this index (B, D) or measures it (C).

---

## What is there now

Five geometry entry points, all in `src/mode.c`, all declared in
`src/def.h` (lines 1037–1045):

| function | shape today |
|----------|-------------|
| `get_visual_row(rows, numrows, win_w, cy, cx)` | sums `visual_segments()` over rows `0..cy` |
| `find_visual_row(rows, numrows, win_w, rowoff_visual, target_y, ...)` | walks from row 0 until the running segment count passes the target |
| `get_total_visual_rows(rows, numrows, win_w)` | sums over every row |
| `goto_visual_row_col(target_vrow, target_rcol_in_segment)` | walks from row 0; reads `winlist[win_current].w` itself |
| `visual_line_cursor_col(row, chars_col, win_w)` | per-row, no walk — not an index consumer |

Callers outside `mode.c`: `src/display.c:392` (inside the per-screen-row
loop — sub-plan B's target), `display.c:725`, `785`, `794`, `888`,
`src/basic.c:88`, `:99`, `src/cmd.c:353`.

Each walk calls `visual_segments()`, which increments
`KG_PERF_VISUAL_PREFIX_VISIT` and derives segments from
`visual_line_width()` — a Phase 1 cache hit, so cheap per visit, but
O(rows) per call.

`win_cells()` (`mode.c:30`) normalizes a nonpositive width to 1.  **Every
key must use the normalized value**, the same rule `erow`'s cache comment
already states; keying on the raw `w` would make width 0 and width 1
different keys for identical geometry.

---

## Design

### Where it lives

A new module, `src/vgeom.[ch]`, not more of `mode.c`.  `CLAUDE.md`'s rule
is that a module owns its own header and that `def.h` must not grow — and
`def.h` is where these five declarations sit today.

**Move the five declarations out of `def.h` into `src/vgeom.h`** as part
of this sub-plan.  That is the plan's main complexity funding, and it is
structural rather than clever: consumers include `vgeom.h` directly,
which `.ci/ci-06`'s IWYU would force anyway.  `make header-check`
compiles every `src/*.h` standalone, so `vgeom.h` must be self-contained
— it needs `erow` and `struct kg_buffer_handle`, so it includes `def.h`
or forward-declares, in the shape `src/localvars.h` already has.

`src/mode.c` is 297 lines against a 520 per-file cap, so *fitting* is not
the issue; ownership is.

### The vector

Per view, lazily allocated, hanging off `struct editor_window`
(`def.h:280`).  `MAX_WINDOWS` is 8.

Key, all four parts:

```
buffer handle (id + generation)   -- not the slot; a slot is reused
buffer content_generation         -- def.h:329, bumped by buffer_note_change()
normalized window text width      -- win_cells(), and see sub-plan D
row count (numrows)               -- cheap belt-and-braces against a missed bump
```

`content_generation` already exists and already has exactly the right
semantics: bumped once per content change (`src/buffer.c:49`, `:1432`,
`:1877`), never reset, documented as "sample it before and after and know
whether it edited anything".  Do not add a second generation.

Entry `i` = visual segments before logical row `i`; length `numrows + 1`,
so the final entry is the total and `get_total_visual_rows()` becomes a
single read.

**Element type is `int32_t`, accumulated and bounds-checked in `size_t`.**
The existing API returns `int`, so a total that does not fit `int` cannot
be represented by callers anyway — the honest response is to refuse at
the seam, not to widen storage nobody can consume.  Memory matters here:
at 1M rows a 4-byte vector is ~4 MB per view and up to ~32 MB across 8
windows; 8-byte entries would double that for no reachable benefit.
Record the measured RSS in sub-plan C.

Overflow: accumulate in `size_t`, and if the running total exceeds
`INT_MAX`, stop and fall back to the scan path rather than truncating.
Use `<stdckdint.h>` — `src/lisp_core.c` already uses `ckd_add` for the
same class of problem.

### The API

One header, four operations, named so the coordinate space is in the name
(`doc/coordinates.md`'s rule):

- total visual rows — read the last prefix entry;
- logical (row, chars col) → visual row;
- visual row → (logical row, segment index) by **lower-bound binary
  search** over the prefix, which is the O(log rows) that replaces
  today's O(rows);
- an **iterator** initialized at a visual top row and advanced one screen
  row at a time — sub-plan B's whole reason for existing.

Route the five existing entry points through it, or retire their walks.
Keep `visual_line_cursor_col()` as-is: it is per-row and not an index
consumer.

`goto_visual_row_col()` reads `winlist[win_current].w` internally.  Leave
that signature alone in this sub-plan — changing it touches `cmd.c` and
`basic.c` and belongs with sub-plan D's width seam, not here.

### Failure is a fallback, never an error

On allocation failure, or key mismatch mid-flight, or the `INT_MAX`
refusal above: **fall back to the existing scan path and draw correctly.**
The cache is an optimization; a failed allocation must not break editing
or drawing, and must not surface as a user-visible error beyond
established OOM policy.  Every new entry point needs a test that forces
the fallback path and asserts the geometry still matches the scan path's
answer exactly.

### Lifetime

Tie the vector to window detach.  `winmgr.c` claims a fresh
`id`/`generation` whenever a slot starts a new window, split included —
so a split must not inherit the parent's vector pointer.  Free on detach,
on buffer switch, and on session teardown; `.ci/ci-04`'s LeakSanitizer
will find it if not, and Plan 06 shipped exactly that class of leak
undetected for four phases.

Duplicate vectors for equal-width views are **tolerated on purpose** in
this sub-plan.  Do not build a global `(buffer, width)` LRU; sub-plan C
measures whether one is warranted.

---

## Tasks

1. `src/vgeom.h` + `src/vgeom.c`; move the five declarations out of
   `def.h`; `make header-check` green.
2. The vector, its key, allocation, invalidation and free-on-detach.
3. The four operations, including the lower-bound search and the
   iterator.
4. Route `get_visual_row()`, `find_visual_row()`,
   `get_total_visual_rows()`, `goto_visual_row_col()` through it.
5. Counters: cache hit / miss / rebuild, per the parent plan's Phase 0
   instruction to add them "only where they answer the new shape".
   `KG_PERF_VISUAL_PREFIX_VISIT` stays and is the before/after evidence.
6. Tests — see below.

---

## Tests

Native (`test/test_perf.c` for shapes, a unit test for correctness):

- Every geometry answer is **identical to the scan path's**, on: empty
  buffer, single empty row, exact-width row, a row one cell over,
  tabs, wide/combining glyphs, escaped invalid bytes, virtual space past
  EOL, and nonpositive window width. This is the acceptance that matters
  — an index that is fast and wrong is worse than the walk.
- Rebuild happens exactly once per key change, not per query.
- An edit bumps `content_generation` and invalidates; the next query
  rebuilds once.
- Two windows at different widths each keep their own vector and neither
  evicts the other — the `vsplit` thrash, as a counter assertion rather
  than only a bench number.
- Forced allocation failure falls back and still answers correctly.

`make bench`: re-run `visual-line-vsplit-100k` and
`visual-line-4win-100k`.  Those are the cases whose 29× and 33× numbers
this design exists to remove; if they do not move, the design did not
work.

---

## Gates

- `make check`, `make WITH_LISP=0 clean all check`.
- `make header-check`, `make format-check`, `make docs-check`.
- **`make complexity-check` at the start and the end.** State the `def.h`
  reduction and the `vgeom.c` addition separately in the commit message,
  so the net is auditable rather than asserted.
- `make coverage-check` — a new module with untested fallback paths is
  how a floor drops.  Do **not** run `make coverage-baseline`; it
  rewrites every file's floor and silently banks pre-existing
  regressions.
- `JOBS=8 .ci/run-ci-steps.sh --parallel`.  ci-04 builds with
  `-DKG_DEBUG_COORDS=1`, which is what will catch a coordinate-space
  mistake at a new seam.

## Completion gate

- Geometry answers are byte-for-byte the scan path's on every corpus
  above, including the degenerate widths.
- `KG_PERF_VISUAL_PREFIX_VISIT` per repaint is no longer O(rows) for the
  routed entry points.
- Unequal-width windows no longer evict each other; the bench numbers say
  so.
- Allocation failure falls back and draws correctly, proven by a test.
- No field was added to `erow` (`sizeof(erow) <= 64` still holds).
- `def.h` is smaller than it was; no ratchet raised.
