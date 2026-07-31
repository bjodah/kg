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

## Landed / deferred

Landed on `stricter-emacs-adherence` as four commits, one per phase,
phase 2 first because it was a live memory-safety defect.

### Landed

- **Phase 2 — the `RESTORE_HL` bounds defect is real, and was a
  keyboard-reachable heap-buffer-overflow READ.**  Reproduced before
  fixing: `C-s a b` then `C-d` on
  `test/pty/isearch-ctrl-d-join-restores-highlight.yaml`, under an ASan
  build, reads 36 bytes out of a 2-byte allocation (`do_isearch`
  restore at the old `search.c:548`, allocation at `:610`).
  The plan's stated direction was wrong and worth recording: a
  *shortening* row is safe, because the restore then copies fewer bytes
  than were saved.  It is **growth** that overreads, and the reachable
  growth is a handoff key that edits — `C-d` at end of line joins the
  next row into the match's row.  Plan 01's `hl_capacity` slack does not
  mask it either: the slack is on the destination, and the source is an
  exact `malloc(row->rsize)`.  Both query-replace loops already restored
  before mutating (reviewer-verified, re-verified here), so isearch was
  the only live caller.
  The fix is `struct hl_snapshot`: the bytes, the length they were saved
  at, the row, and the buffer's `content_generation`.  Restore clamps to
  the saved length (`TODO(plan-10)` there) and skips a row whose
  generation moved, because such a row has already been re-highlighted.
  The two save sites were the same code twice and are one function now,
  which takes its span in chars and converts internally — that also
  fixed the literal query-replace passing a chars *length* as a render
  length.
- **Phase 1 — `doc/coordinates.md`**, 15 rows, each with a verdict:
  11 `ok`, 2 fixed by plan 03 (one of them the confirmation this plan
  owed: `kg_regex_match_backward()` *does* step with
  `kg_utf8_forward_boundary()`, so plan 03 phase 1 covered both
  directions), 1 documented divergence, 2 wrong and fixed in phase 3.
  The round-trip property is `test_coordinate_space_round_trips` in
  `test/test_basic.c` (already linked against `basic.o`/`mode.o`, so no
  Makefile change), plus
  `test_render_offset_is_not_a_display_column`, which pins the
  divergence by asserting what pairing the two spaces wrongly costs.
- **Phase 3 — the two wrong rows.**  `generic_keyword_scan()` ended a
  single-line comment run with the chars length `row->size - i` in a
  render-indexed array, so an indented `// comment` lost its colour
  after one tab expansion (`test_c_line_comment_after_tab`).  And
  `coloff` — a chars offset in every command in the tree — was read as
  a render offset by `draw_window_rows()` and by the cursor-placement
  loop, which is `doc/TODO.md`'s "horizontal-scroll + tab units
  mismatch", now closed.  The slice converts with
  `chars_to_render_col()` and the cursor column is the difference of two
  `editor_visual_col()` readings; `test/pty/horizontal-scroll-tab-slices-render.yaml`
  fails without it.
- **Phase 4 — convention, assertions, `AGENTS.md`.**  The convention is
  the words already in the names (`chars` / `render` / `visual`), which
  made it one rename rather than a typedef pair:
  `render_col_to_chars()` → `visual_col_to_chars()`, and
  `find_visual_row()`'s `char_offset` out-param → `render_offset`.
  `KG_ASSERT_CHARS_OFF` / `KG_ASSERT_RENDER_OFF` (`src/def.h`) are
  `KG_PERF_COUNTERS`-shaped: nothing unless `KG_DEBUG_COORDS=1`, which
  `.ci/ci-04-clang-asan-ubsan.sh` now builds with, so they run against
  the whole PTY suite in the deep CI.

### Budget

Self-funded at every commit; `SCC_COMPLEXITY_MAX` lowered at each and
never raised: 4256 → 4251 → (4251) → 4245 → 4239.  The three dedups that
paid for it are all in this plan's own subject matter — one isearch
prompt instead of four branches choosing between two message shapes,
one `hl_snapshot_mark()` instead of two copies, `editor_visual_col()`
instead of the cursor loop's private copy of it, and one `win_cells()`
instead of eight `if (win_w <= 0)` guards.  `pmccabe` improvements
banked at each step (`do_isearch` 52 → 49, `editor_refresh_screen`
30 → 26, seven `mode.c` functions down one each).

### Deferred, with pick-up points

- **The interim fix is still interim.**  `hl_snapshot_restore()` carries
  `TODO(plan-10)`: a highlight span is a decoration, and decorations
  belong on the edit transaction (plan 10 phase 6).  What is here is a
  clamp and a generation check, not an invalidation protocol — nothing
  tells the snapshot that its row changed, it only notices at restore.
- **`chars_to_render_col()` keeps its `_col` name.**  It returns a
  render *byte offset*.  The space word in the name is right, so the
  rename was not worth ~12 more call sites; the table says what it
  returns and the assertion at its consumers says it too.
- **`editor_visual_col()` and `wrapped_visual_col()` are still two
  walks of the same model**, differing only in `wrap_pad()` and an
  `int64_t` saturation.  Merging them would move `editor_visual_col()`
  out of `buffer.c`, and `buffer.o` is in `TEST_SRCS_OBJS` while
  `mode.o` is not, so every unit-test binary would need a new object.
  Worth doing when `mode.o` joins that set for another reason.
- **The rect-mode virtual highlight ignores `coloff` entirely**
  (`display.c:421`).  It was already approximate before this plan and is
  unchanged by it; a rectangle on a horizontally scrolled window is the
  case to write a test for first.
