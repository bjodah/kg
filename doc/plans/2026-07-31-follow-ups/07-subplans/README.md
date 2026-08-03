# Plan 07 sub-plans — Visual-line geometry cache and index

Phases 0 and 1 are done (see the parent plan's status section).  Phases
2–5 are unstarted, and are split here into four sub-plans by dependency
and natural delivery order.

## Grouping

| Sub-plan | Phase | Focus | Prerequisites |
|----------|-------|-------|---------------|
| [A](07a-prefix-index-and-geometry-api.md) | 2 | View-owned prefix vector, one geometry API, a real `src/vgeom.h` | Phase 1 (done) |
| [B](07b-one-traversal-draw.md)            | 3 | `draw_window_rows()` draws from one iterator, not one search per screen row | Sub-plan A |
| [C](07c-remeasure-and-decide.md)          | 4 | Re-run the matrix; decide *against* an incremental tree unless evidence says otherwise | Sub-plan B |
| [D](07d-window-text-width-seam.md)        | 5 | One "window text width" value every display consumer shares | Sub-plan A (may land beside B) |

A is the substance.  B is what converts A's index into the win_h × numrows
win.  C is a measurement and a written decision, and is expected to
*defer* rather than build.  D is small, and exists so a later line-number
gutter has an unambiguous seam rather than a fourth definition of width.

## The binding constraint is complexity budget, not difficulty

`scc` is at **5401 against `SCC_COMPLEXITY_MAX` = 5500** — 99 points for
all four sub-plans.  The follow-ups README's rule 6 forbids raising the
cap per feature commit, every raise is a written Decision (4223 → 4450 →
4750 → 5500), and **the README's named repayment sources are spent.**

The parent plan's "do not raise the 4223 cap" line is stale by three
raises, and its status section's "cap moved to 4200 by the wave-2 budget
decision" describes a different, earlier tree.  Read the follow-ups
README's Decision sections, not those lines, for the live number.

Each sub-plan below therefore names its own funding.  The largest single
source is structural and already identified: **`src/mode.c`'s
declarations live in `src/def.h`** (lines 1037–1045), which `CLAUDE.md`
explicitly calls out as the thing that got `def.h` to its current size.
Sub-plan A moves them to a real header as part of doing its own work, and
that is a `def.h` reduction rather than a new cost.

If a sub-plan cannot fit its budget, the answer is to stop and ask for a
Decision, not to spend the shared 99 and leave nothing for the next
track.  A cap can also sit **silently red** — Plan 05 closed 75 over and
nobody noticed until Plan 06 measured it — so run `make complexity-check`
at the *start* of a slice as well as the end.

## What Phase 1 already established, and what it did not

Phase 1's per-row width cache (`erow.wrap_cache_win_w` /
`wrap_cache_vcols`) means a *revisit* of a row costs no byte scan.  It
deliberately did not change **how many times** the O(rows) walks revisit
a row: `KG_PERF_VISUAL_PREFIX_VISIT` measured 13,699,181 on
`visual-line-100k` both before and after.  That count is what sub-plans A
and B exist to collapse.

The measured thrash numbers are the evidence for A's view-owned design,
and are worth restating because they decide it:

| bench case | row scans | reading |
|------------|-----------|---------|
| `visual-line-100k` (one window) | 100,001 | one per row, the floor |
| `visual-line-hsplit-100k` (two windows, equal width) | 100,001 | splitting alone costs nothing |
| `visual-line-vsplit-100k` (two windows, 39 and 40) | 2,900,031 | 29×, unequal widths evict each other |
| `visual-line-4win-100k` (mixed widths) | 3,300,039 | worse again |

A single cache entry per row is measurably wrong for that shape.  A
view-owned vector keyed by width sidesteps it without a second eviction
policy bolted onto `erow`.

## Two existing assertions these sub-plans must move deliberately

Both are in `test/test_perf.c`, and both will look like "the test broke"
to anyone who has not read this:

1. `test_visual_line_prefix_walk_restarts_per_screen_row()` asserts
   `KG_PERF_VISUAL_PREFIX_VISIT > win_h` — it **pins the current bad
   shape on purpose**, and its comment says so.  Sub-plan B inverts it.
   Inverting it is the deliverable; weakening it is not.
2. `CHECK(sizeof(erow) <= 64)` pins Phase 1's RSS cost.  Nothing in
   sub-plans A–D may add a field to `erow` — the index is view-owned
   precisely so it does not have to.

## Coordinate discipline

`doc/coordinates.md` is not optional reading here.  This plan moves
values between three spaces — `row->chars` bytes, `row->render` bytes,
and display columns — and `visual_col_to_chars()`'s comment already
records one bug from confusing the last two.  State the space at every
new seam with `KG_ASSERT_CHARS_OFF` / `KG_ASSERT_RENDER_OFF`, which
`.ci/ci-04` builds with `-DKG_DEBUG_COORDS=1`.
