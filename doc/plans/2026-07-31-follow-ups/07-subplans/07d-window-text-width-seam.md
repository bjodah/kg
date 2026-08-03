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
