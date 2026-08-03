# Sub-plan 07-D — Phase 5: one window text width

The smallest of the four, and the one whose value is entirely in what it
prevents later.

---

## Problem

"How wide is the text area of this window" is answered ad hoc today.
`struct editor_window` carries `w` (`def.h`), `src/mode.c` normalizes it
through `win_cells()` before every geometry call, `draw_window_rows()`
takes `win_w` as a parameter, and `goto_visual_row_col()` reaches into
`winlist[win_current].w` on its own.

Nothing is wrong with the answers today, because there is nothing to
subtract: separators are handled by the caller and there is no gutter.
The moment a line-number gutter exists, every one of those sites needs
the *same* reduced width, and any that is missed produces geometry that
disagrees with what was drawn — wrapped text measured against one width
and painted against another.

Plan 07's Phase 5 is explicit that line numbers are **not** built here.
This is the seam, not the feature.

---

## Design

One function, in `src/vgeom.h` beside the geometry API sub-plan A creates
(or in `winmgr.h` if that reads better once A has landed — decide when
the header exists, not now):

```
window text width = the columns available to buffer text,
                    after separators and any future gutter
```

It subsumes `win_cells()`'s normalization, so it never returns less than
1, and it is the **only** thing the geometry index, rendering, cursor
placement and rectangles key on.  Sub-plan A's vector key uses it.

Today its body is `win_cells(w)` plus nothing.  That is the point: it is
a named seam with one implementation, so a later gutter is a change in
one function rather than an archaeology exercise across four files.

Convert the existing consumers:

- `src/mode.c`'s eight `win_cells()` calls;
- `goto_visual_row_col()`'s direct `winlist[win_current].w` read — this
  is where its signature question from sub-plan A gets settled;
- `draw_window_rows()`'s `win_w` parameter and the width its callers
  compute;
- rectangle and cursor-placement paths that currently take `w`.

Do **not** change what any of them compute.  A pure rename-and-route with
identical output is the whole deliverable, and it is what makes the
change safe to review: every PTY and golden case must be byte-for-byte
identical, because nothing about the geometry moved.

---

## Tests

- Every existing visual PTY case unchanged, byte for byte.
- A unit test that the seam normalizes nonpositive widths to 1, which is
  the behavior `win_cells()` provides today and that several callers
  quietly depend on.
- A test asserting the geometry index and the draw path receive the
  *same* value for the same window — the invariant that a later gutter
  would otherwise break silently.

---

## Gates

- `make check`, `make WITH_LISP=0 clean all check`.
- `make header-check`, `make format-check`, `make complexity-check`.
- `JOBS=8 .ci/run-ci-steps.sh --parallel`, standalone.

`.ci/ci-04` builds with `-DKG_DEBUG_COORDS=1`; this sub-plan moves a
display-column value through several functions, so state the space at
each new seam per `doc/coordinates.md`.

## Completion gate

- One function answers "window text width", and the geometry index,
  renderer, cursor placement and rectangles all call it.
- `win_cells()` has no remaining direct callers outside it.
- Output is byte-for-byte unchanged everywhere — this sub-plan computes
  nothing new.
- A later line-number gutter has exactly one place to subtract from, and
  a test that would fail if a consumer stopped using it.
- No ratchet raised.

## Status — sub-plan D closed (2026-08-03)

**Where the seam lives, and why.** `win_text_width(struct editor_window *w)`
is declared in `src/vgeom.h` and defined in `src/vgeom.c`, next to
`get_total_visual_rows()` and the other four operations that already take
a `struct editor_window *`.  Not `winmgr.h`: that header does not exist
(`src/winmgr.c` has none), so the choice was never "which existing header
reads better" but "does this earn a new module."  A two-line function
whose only job is reading one struct field through `win_cells()` does not
justify a new header purely so `struct editor_window` doesn't have to be
forward-declared a second time — `vgeom.h` already forward-declares it for
`get_total_visual_rows()` and friends, so adding one more declaration
there costs nothing `winmgr.h` would not also cost, minus the new-module
overhead.  `src/mode.c` was the other candidate (it defines `win_cells()`
itself) and was rejected for the reason `vgeom.h`'s own section comment
already states: `mode.c` is "per-row measurement," never touching
`struct editor_window`; `vgeom.c` is "the per-window index," already the
only file that reads a window's `w` field for geometry purposes.  Keeping
that boundary means `mode.c` stays exactly what its header comment says it
is.

**What got converted — eight call sites, matching the document's count
even though none of them are where the document expected.** The document
was written before sub-plan A moved `get_total_visual_rows()`,
`get_visual_row()`, `find_visual_row()` and `goto_visual_row_col()` out of
`mode.c` into `vgeom.c`, so "`mode.c`'s eight `win_cells()` calls" no
longer describes the tree — `mode.c` has four today, all internal
normalization of an already-generic `int win_w` parameter (see below), and
none of them read a window.  The eight sites that *do* read a window's `w`
field and derive a text width from it, all now routed through
`win_text_width(w)`:

- `src/vgeom.c`: `get_total_visual_rows()`, `get_visual_row()`,
  `find_visual_row()`, `goto_visual_row_col()`, `vgeom_iter_init()` — the
  five `win_cells(w->w)` calls sub-plan A left in place.
- `src/basic.c:54` (`editor_move_cursor()`'s visual-line HOME/END/UP/DOWN
  block) and `src/display.c:908` (cursor-placement in
  `editor_refresh_screen()`) — both previously spelled `win_cells()`'s own
  logic inline as `w->w > 0 ? w->w : 1` rather than calling it, which is
  the "ad hoc" the Problem section names.
- `src/display.c:799`, the `draw_window_rows()` call site — previously
  passed the window's raw, unnormalized `w->w`.  `draw_window_rows()`
  keeps its `int win_w` parameter (removing it in favor of an internal
  `win_text_width(w)` call was the other option; kept as a parameter to
  avoid touching a function already at its per-symbol pmccabe cap with no
  headroom, per the assignment's warning — a call-site substitution is
  zero-risk to that budget, a body edit is not for free even when it
  looks free).  Since every window's width is already clamped to >= 6 by
  `winmgr.c`'s resize floor, `win_cells()` was a no-op on this path in
  every reachable state; the call site now says so instead of relying on
  a reader to know it.

**What did *not* get converted, on purpose.**
- `src/mode.c`'s own four `win_cells()` calls (inside `visual_line_width()`,
  `visual_line_cursor_col()`, `visual_segments()`, `visual_col_to_chars()`)
  stay.  They normalize a plain `int win_w` parameter that is not
  necessarily a window's width — `test/test_basic.c:375-383` calls
  `visual_line_width(row, 0)` and `visual_line_width(row, -5)` directly,
  with no window in sight, and asserts the `win_cells()` normalization
  fires.  Removing that internal guard to satisfy a literal reading of
  "`win_cells()` has no remaining direct callers outside [the seam]" would
  break a passing test and remove a real defense a public per-row API
  needs regardless of any caller's window.  The completion-gate sentence
  is read here as being about *window*-width-reading call sites — the
  eight above — not about `win_cells()`'s own module normalizing a
  parameter that is not always a window's width.  `win_text_width()` is
  now the only caller that reads `win_cells()` from *outside* `mode.c`.
- `draw_mode_line()`'s `win_w` (the status-line width) is untouched: the
  mode line is chrome, not buffer text, and a line-number gutter would
  not shrink it the way it shrinks the text area.  The Design section's
  "renderer" means the buffer-text renderer.
- Rectangles: `src/rect.c` does not read a window's `w` field at all — its
  bounds are computed in visual-column space from mark/point and never
  clamped against window width, so there was nothing in that file to
  convert.  The region/rectangle highlight overlay inside
  `draw_window_rows()` is bounded by the same `win_w` the row-width loop
  already uses (drawn in the same per-row pass), so it inherits the seam
  through that shared loop rather than reading `w->w` a second time.  The
  Design section's "rectangles" is accurate about the *effect* — one
  shared width — but there was never a second, separate rectangle
  call site to change.

**A real defect this slice found and fixed: `test/fuzz_stubs.c` did not
link.** `test/fuzz_keypress` links `src/basic.c` directly (Makefile
`FUZZ_SRCS`) but not `src/vgeom.c`, and stubs the visual-line functions
`src/basic.c` calls.  Once `editor_move_cursor()` started calling
`win_text_width()`, the fuzz target failed to link
(`undefined reference to 'win_text_width'`) — caught by
`ci-09-fuzz-smoke` on the first standalone CI run, not by `make check`
(which does not build fuzz targets).  Fixed by adding the same stub
`fuzz_stubs.c` already has for `get_visual_row()`/`goto_visual_row_col()`:
`int win_text_width(struct editor_window *w) { return w->w > 0 ? w->w : 1; }`,
mirroring the pre-seam logic it replaces.  A second, full standalone CI
run after the fix passed `ci-09` along with every other step.

**Two tests added to `test/test_vgeom.c`**, per the Tests section:
`test_win_text_width_normalizes_nonpositive` (0 and -5 both become 1, 80
and 1 pass through) and `test_win_text_width_is_the_single_geometry_key`,
which sets a degenerate `wcur()->w = 0`, reads `win_text_width(wcur())`
once, and independently recomputes `get_total_visual_rows()`'s expected
answer using only that externally-visible value fed into
`visual_segments()` — the same row-level primitive both the index
(`src/vgeom.c`) and the draw loop (`src/display.c`'s `vline_iter_begin()`
-> `vgeom_iter_init()`) bottom out at.  This proves the index side keys on
exactly `win_text_width(w)`'s value.  It does not reach into
`draw_window_rows()`, which is `static` in `src/display.c` and has no
callable seam a unit test can invoke independently of a real terminal
frame; the draw path's use of the same seam is established by inspection
(one call expression, `win_text_width(w)`, at the one call site that feeds
it) and by the unchanged byte-for-byte PTY suite, not by a second unit
test that cannot reach the static function.

**Complexity: net zero on `scc`, and two functions improved on
`pmccabe`.** `make complexity-check` before and after: `5439` both times
— every change here is a value substitution or a one-line wrapper
function with no branch of its own, so `scc` (which scores branches) sees
nothing to add.  `make pmccabe-check`: `draw_window_rows` unchanged at 67
(its parameter list changed, not its body); `win_text_width` itself is a
new one-line, complexity-1 function; and two functions actually *dropped*
because replacing an inline `w->w > 0 ? w->w : 1` ternary with a plain
call removed one decision point from the caller: `editor_move_cursor`
62 -> 61, `editor_refresh_screen` 27 -> 26.  Ran `make pmccabe-baseline`
to bank both improvements, per the Makefile's own instruction that this is
"how a decrease is banked."  That run also picked up baseline entries for
symbols sub-plans A/B (and unrelated recent commits, e.g. `32abaee`'s
`lisp_hooks.c:resolve_hook_function`) had already introduced but never
banked — they were passing only under the flat 15-point new-function
grace, not their own recorded value, because nobody had run
`make pmccabe-baseline` since those symbols appeared.  That staleness
predates this slice (`git stash`-style verification: the baseline at
`a4ff23d`, before any change here, already reported "22 new" symbols
against `make pmccabe-check`, all of them pre-existing) and is not this
sub-plan's regression, but running the sanctioned bank command folds it
in as a side effect; it is called out here rather than left silent.

**Byte-for-byte equivalence.** `make check`: 32/32 unit tests, 405/405 PTY
cases, unchanged.  `make WITH_LISP=0 clean all check`: 34 unit tests
(includes the two Lisp-gated tests skipping cleanly), 337 PASS / 68 SKIP /
0 FAIL PTY cases — the same shape `make check` has under that
configuration.  No `expected_saved`/`expected_saved_any` fixture changed.
No PTY case's `xfail` status changed.  This is the equivalence proof per
the assignment: every visual-line-mode PTY case (`visual-line-mode.yaml`,
`-toggle-scrolled`, `-utf8-tab`, `-window-local`,
`128-visual-line-wrap-double-width`,
`isearch-point-after-tab-visual-line`, `yank-pop-repeated-wraps`, plus
sub-plan B's `visual-line-exact-width-eol-resize` and
`-vsplit-unequal-widths-edit`) renders and saves identically against
fixtures that predate this slice.  Analytically: every one of the eight
converted call sites fed `win_cells(w->w)` or its inline equivalent before
this change and feeds `win_text_width(w) == win_cells(w->w)` after, by
construction.  Seven of the eight are therefore identities on their face.
The eighth is `draw_window_rows()`'s call site, which used to pass the
window's **raw** `w->w`, so it needs the extra step: `win_reflow()` clamps
`winlist[i].w` to at least 1 (`src/winmgr.c`, `if (winlist[i].w < 1)`),
which is precisely `win_cells()`'s own rule, and `win_init()` ends by
calling `win_reflow()` — so `w->w >= 1` holds from initialization onward
and `win_cells(w->w) == w->w` in every reachable state.  (An earlier draft
of this section cited a floor of 6 instead.  That number is the *split*
guard at `winmgr.c`'s `if (winlist[win_current].w < 6)`, which refuses to
divide a window too narrow to split; it says nothing about how narrow a
window can be, and is not what makes this substitution safe.)

**Gates:**
- `make complexity-check` and `make pmccabe-check`, start: 5439/5500 and
  91/110 (max function `localvars_parse_footer`), both passing —
  PASS.
- `make check` — PASS (32/32 unit, 405/405 PTY).
- `make WITH_LISP=0 clean all check` — PASS (see above); default build
  restored afterward.
- `make header-check` — PASS (31 headers). `make format-check` — PASS,
  no reformatting needed. `make docs-check` — PASS (unaffected; no
  keybinding or help-table change).
- `make complexity-check` and `make pmccabe-check`, end: `scc` unchanged
  at 5439/5500; `pmccabe` max function unchanged at 91/110, baseline
  banked with 2 improvements, 0 regressions (see above).
- `make coverage-check` — PASS, 5 files above their floor (pre-existing
  headroom elsewhere in the tree), none below; `make coverage-baseline`
  was not run.
- `JOBS=8 .ci/run-ci-steps.sh --parallel`, standalone, run twice: the
  first run (before the `fuzz_stubs.c` fix) failed only
  `ci-09-fuzz-smoke` on the link error above, with `ci-01` through `ci-08`
  and `ci-10` through `ci-12` all PASS, including `ci-04` (built with
  `-DKG_DEBUG_COORDS=1`, so `KG_ASSERT_RENDER_OFF`/`KG_ASSERT_CHARS_OFF`
  ran against the whole PTY suite and found nothing) and `ci-03`, whose
  valgrind-lane `lisp-process-*` flake this task's brief warned about did
  not occur.  The second run, after the fix, passed all twelve steps.
- `make bench` was not run as evidence — this slice changes no arithmetic
  (every changed line is a value substitution), so, per the assignment,
  the PTY layer is the equivalence proof and bench is optional here; the
  fuzz-smoke defect above was the only thing a wall-clock or counter run
  would not have caught either, so nothing was skipped that mattered.

No ratchet raised. `win_cells()` has exactly one caller reading a window's
`w` field (`win_text_width()`); its four remaining callers inside
`src/mode.c` normalize a caller-supplied `int`, not a window, and stay for
the reason given above.
