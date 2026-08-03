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

## Status (2026-08-03)

**A is done** (`6539641`).  It took more of B's win than this README
predicted: routing `find_visual_row()` through the index removes the
per-screen-row walk even while the draw loop still calls it once per
screen row, so `KG_PERF_VISUAL_PREFIX_VISIT` on `visual-line-100k` went
13,699,181 → 100,001 and the unequal-width thrash went 2,900,031 →
300,017 (vsplit) and 3,300,039 → 500,033 (four windows).  What is left in
those two is one cold scan per row per *distinct width ever used*, which
is Phase 1's acceptance, not thrash.

Read A's status section before starting B: it records two things this
document was wrong about (the `def.h` move funds no complexity at all,
and `vgeom_window_free()` is not on the production path), and it hands B
a corrected version of the assertion below.

**B is done too** (`58b25f4`).  The draw loop places one iterator per
window instead of asking `find_visual_row()` per screen row, and
`vgeom_iter_next()` reads segment counts out of the prefix sums.  That is
worth 4-5× fewer index lookups per repaint and 3-4% wall, which is the
honest size of it: A collapsed the *cost* of a geometry query and B
collapses *how many* are asked.  The pinned assertion is inverted and now
reads `== 0` on a warm repaint.

**Budget after A and B: 5439 of 5500, so 61 points for C and D.**  A cost
+37 net, funded by nothing — see its status section.  B cost +1.

**`make complexity-check` is not the whole complexity gate.**  B was
caught by `make pmccabe-check`, a *per-symbol* ratchet in `.ci/ci-01`
that `scc` knows nothing about: two branches added inside
`draw_window_rows()` took it from 67 to 69 against its baseline.  Neither
this README nor any sub-plan's Gates list named that command.  **Run
both.**

**C is done (2026-08-03), decision: defer both questions.**  See its own
status section for the full matrix (including `--big`), the isolated
per-edit rebuild cost (~5 ms at 100k rows, ~49 ms at 1M rows, quiet-tail-
matched so the harness's own overhead cancels in the delta), and the
duplicate-vector memory measurement (computed from `sizeof` and
cross-checked against real peak RSS: ~27–29 MiB worst case at 1M rows
against a ~251 MiB single-window baseline).  Budget unchanged at 5439/5500
— C touched no `src/*.c`/`src/*.h` — so **61 points remain for D**.  C did
touch `utils/bench.py`: it fixed a pre-existing, unrelated bench-case bug
(`visual-line-pos-middle-100k` sent bare `M-g`, which commit `710e9ba`
turned into a prefix map before sub-plan A landed, so the case has
measured nothing since) and added two `--big` cases the existing matrix
was missing (an edit at 1M rows, and a same-width duplicate-window case at
1M rows) — without which the two questions this sub-plan exists to answer
had no evidence at the only corpus size that could plausibly move them.

**D is done (2026-08-03), and the campaign is closed.** One function,
`win_text_width(struct editor_window *w)`, lives in `src/vgeom.c`
(declared in `src/vgeom.h`, beside the other four operations that already
take a window) rather than in a new `winmgr.h` — that header does not
exist, and a two-line wrapper around `win_cells()` does not justify
creating one.  Eight call sites that read a window's `w` field and derive
a text width from it now go through it: `src/vgeom.c`'s five
`get_total_visual_rows()`/`get_visual_row()`/`find_visual_row()`/
`goto_visual_row_col()`/`vgeom_iter_init()`, `src/basic.c`'s and
`src/display.c`'s cursor-placement code (both previously reimplemented
`win_cells()`'s ternary inline instead of calling it), and
`draw_window_rows()`'s call site in `editor_refresh_screen()` (previously
passed the window's raw, unnormalized width).  `src/mode.c`'s own four
`win_cells()` calls stay — they normalize a generic `int win_w` parameter
a unit test calls directly with 0 and -5, not a window — so
`win_cells()`'s only *window*-reading caller is the new seam.  See D's own
status section for what it did not convert and why (there is no
rectangle-specific `w->w` read in `src/rect.c` to convert; `draw_mode_line`
is chrome, not text, and stays untouched), and for a real defect it found:
`test/fuzz_stubs.c` did not link once `src/basic.c` started calling the
seam, caught by `ci-09-fuzz-smoke` and fixed with a stub matching the
pre-seam logic.

`scc` is unchanged at **5439/5500** — every change in D is a value
substitution or a one-line, branch-free wrapper, so nothing is added to
score.  `pmccabe` is unchanged at its ceiling (91/110) and two functions
*improved* (`editor_move_cursor` 62 -> 61, `editor_refresh_screen` 27 ->
26) by replacing an inline ternary with a plain call; both banked with
`make pmccabe-baseline`, which also banked pre-existing, already-passing
symbols from A/B and unrelated recent commits that nobody had banked
before (not a D regression — the baseline was already stale; see D's
status section).  `make check` (32/32 unit, 405/405 PTY),
`WITH_LISP=0`, `header-check`, `format-check`, `docs-check`,
`coverage-check`, and a standalone `JOBS=8 .ci/run-ci-steps.sh --parallel`
(twelve of twelve steps, run twice — the first run's sole failure was the
fuzz-stub link gap, fixed before the second, fully green run) all pass.

Phases 2 through 5 of Plan 07 are complete.  All four sub-plans landed
without raising `SCC_COMPLEXITY_MAX` past 5500, closing at 5439 — 61
points under the cap that D, the last claimant, did not need to spend.

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
   **After A this test still passes, but no longer for its stated
   reason**: it measures a cold index, so what it now counts is the
   single O(rows) rebuild rather than `win_h × numrows` restarts.  B has
   to re-point it at a *warm* repaint first; flipping the comparison as
   written would assert nothing.
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
