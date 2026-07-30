# Plan 14 — Coordinate-space invariants (chars / render / display)

kg addresses row content in three coordinate spaces: byte offsets into
`row->chars`, byte offsets into `row->render` (tabs expanded), and
terminal display columns. The review found several defects that are all
one systemic problem — a producer in one space feeding a consumer in
another — spread across `search.c`, `mode.c`, `display.c`, and `basic.c`.
Plan 03 phase 4 fixes the two isearch instances; this plan owns the
audit, the remaining fixes, and the invariant that prevents recurrence.

Read first: `src/buffer.c` (`editor_update_row`, `chars_to_render_col`),
`src/mode.c` (`render_col_to_chars` — visual-line aware and *not* an
inverse of `chars_to_render_col`), `src/search.c:22-30` and `:584`,
`src/display.c` row drawing, Plan 03's verified-state table.

## Phase 1 — Audit table

Produce (as a comment block in `src/def.h` or a short
`doc/coordinates.md`) a table of every producer and consumer of:

- `editor.cx` / `editor.coloff` (chars or render? — verified: chars at
  `buffer.c:58`, but `do_isearch()` feeds a render offset to
  `editor_reveal_position_centered()` at `search.c:584`);
- `row->hl` indexing (verified render-indexed);
- `chars_to_render_col()` / `render_col_to_chars()` call sites and
  whether each pair actually round-trips under tabs, multibyte glyphs,
  and visual-line mode.

Grep starting points: `coloff + editor.cx`, `->render`, `->hl[`,
`chars_to_render_col`, `render_col_to_chars`. Every row of the table
gets a verdict: correct / fixed in Plan 03 / fixed here / documented
divergence.

Deliverable is the table plus a native test that encodes the round-trip
property for representative rows (tab-prefixed, UTF-8, both modes) in
`test/test_basic.c` or a new `test/test_coords.c` (wire via `EXTRA_*` in
the Makefile like the existing suites).

## Phase 2 — `RESTORE_HL` bounds defect (possible P0; triage first)

Verified hazard: `search.c:22-30` saves `row->hl` into `saved_hl`
(allocated at the row's rsize at *save* time) keyed by row index, and
`RESTORE_HL` copies back `editor.row[saved_hl_line].rsize` bytes — the
rsize at *restore* time. If the row shortened in between (query-replace
does exactly this), the copy reads past the `saved_hl` allocation; if it
grew, `row->hl` is partially stale.

1. First write a failing test or an ASan reproduction (isearch highlight,
   then a replacement that shortens the row, then restore). If it
   reproduces as an OOB read, treat as release-blocking and land the fix
   immediately after Plan 01, not in plan order.
2. Fix by storing the saved length alongside the pointer and clamping,
   and by invalidating the save when the row's generation changes.
   The durable fix (decorations on the transaction boundary) is Plan 10
   phase 6; this is the bounded interim fix and its regression.

## Phase 3 — Remaining cross-space fixes

Fix, with one focused test each, every table row from phase 1 whose
verdict was "fixed here". Known members:

- `kg_regex_match_backward()` offset handling (`src/regex.c:175`) shares
  the mid-glyph `+1` defect (Plan 03 phase 1 fixes the forward helper;
  confirm the backward path consumed it, else fix here).
- Any `editor_reveal_position_centered()` caller passing render offsets
  beyond the isearch site fixed in Plan 03.
- `row->hl` consumers that index with chars offsets, if the audit finds
  any.

## Phase 4 — Enforcement

- Name the convention: functions taking/returning render offsets get a
  `_render` suffix (or a typedef pair `chars_off` / `render_off`) —
  choose whichever is smaller in the diff, document in the phase-1 table.
- Add an assertion helper (debug builds) validating offsets against
  `row->size` / `row->rsize` at the API seams touched in phase 3.
- Update `CLAUDE.md`'s editing-expectations section with one line
  pointing at the table.

## Interactions

- Plan 03 phase 4 must land first (it converts isearch to buffer-byte
  matching; this plan audits what remains).
- Plan 10 (decorations/markers) supersedes the phase-2 interim fix;
  leave a `TODO(plan-10)` at the clamp site.
- Plan 08's row-rebuild changes touch `editor_update_row`; coordinate to
  avoid churn — the audit table is the shared reference.

## Definition of done

- The table exists, every row has a verdict, and the round-trip property
  test runs in `make check`.
- `RESTORE_HL` cannot read or write out of bounds (regression proves it).
- No producer/consumer mismatch remains unfixed and undocumented.
- `make check`, `WITH_LISP=0`, and `.ci/run-ci-steps.sh --parallel` green.
