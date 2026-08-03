# Sub-plan 07-B — Phase 3: draw a window in one traversal

Converts sub-plan A's index into the win the campaign is actually for.

---

## The defect, exactly

`src/display.c:388`:

```c
for (y = 0; y < win_h; y++) {
        int fr;
        int offset;
        if (visual_line_mode) {
                find_visual_row(
                    rows, numrows, win_w, rowoff, y, &fr, &offset);
        } else {
                ...
```

`find_visual_row()` walks **from logical row 0** every time.  So one
repaint is `win_h` walks of up to `numrows` rows each — the
`win_h × numrows` shape.  Row zero alone is visited once per screen row,
which is precisely what `test_visual_line_prefix_walk_restarts_per_screen_row()`
asserts today.

The non-visual branch of the same loop already got this right: the
comment at `flat_row_idx` explains that `buffer_row_col_to_position()` is
O(row) and is therefore called once and then advanced, "never by calling
`buffer_row_col_to_position()` again, which is O(row) and would make one
repaint O(rows x visible_rows)".  This sub-plan applies that same
reasoning to the visual branch.  The precedent, and its wording, is the
argument.

---

## Design

Initialize **one** iterator (sub-plan A's fourth operation) before the
loop, at the window's top visual row — one lower-bound search per window,
O(log rows).  Inside the loop, read the current segment and advance one
screen row.

Constraints worth stating because they are where this breaks:

- **The loop body must not change what it draws.** `fr` and `offset` have
  the same meanings and the same coordinate spaces as now; `offset`
  slices `row->render`, and the non-visual branch's comment records the
  bug from reading it as anything else.  Assert with
  `KG_ASSERT_RENDER_OFF`.
- **Past-the-end must behave identically.** Today `find_visual_row()`
  sets `*logical_row = numrows` and `*offset = 0` when the target is past
  the last row; the tilde/empty-row drawing below depends on that. The
  iterator must produce the same thing rather than a new sentinel.
- **`flat_row_advance()` must keep working.** It assumes `fr` is
  non-decreasing and advances by at most one buffer row at a time —
  the comment at `flat_row_idx` says visual-line wrapping never violates
  that. An iterator preserves the property; make sure the past-the-end
  path does too, or the flat-position fast path silently falls back to an
  O(row) lookup per screen row and this sub-plan's win evaporates
  somewhere it is not being counted.
- `draw_window_rows()` already carries a documented complexity budget
  (see `flat_row_advance()`'s comment about being kept out of it). Do not
  grow the function; if the iterator's setup needs more than a couple of
  lines, it goes in a helper for the same reason `flat_row_advance()` did.

---

## The assertion that must be inverted

`test/test_perf.c`:

```c
static void test_visual_line_prefix_walk_restarts_per_screen_row(void)
{
        ...
        CHECK(counter(KG_PERF_VISUAL_PREFIX_VISIT)
            > (unsigned long long)wcur()->h);
```

Its comment says it pins "the shape a later persistent prefix index (plan
07 phase 2) has to fix".  **This sub-plan is where that comes due.**
Rename it and invert the bound: after warmup a repaint's prefix visits
must be `O(log rows + win_h)` per window, not `> win_h`.

Say so in the commit message.  A reviewer seeing a `>` become a `<=`
should be able to find the sentence that authorized it, rather than
reading it as a test weakened to fit.

---

## Acceptance

Per unchanged repaint, as counter assertions in `test/test_perf.c`:

- zero visual byte scans after warmup (Phase 1 property, must not
  regress);
- prefix/logical visits bounded by `O(log rows + win_h)` per window;
- no `win_h × numrows` shape at any window count or width mix;
- **byte-for-byte identical frame output** for every existing golden and
  PTY case.

That last one is the real gate. The seven existing visual PTY cases —
`visual-line-mode.yaml`, `-toggle-scrolled`, `-utf8-tab`,
`-window-local`, `128-visual-line-wrap-double-width`,
`isearch-point-after-tab-visual-line`, `yank-pop-repeated-wraps` — must
pass unchanged, and a diff in any of them means the traversal is not
equivalent.

Add screen-content PTY cases (`backend: tmux`,
`expected_screen_contains`) for the shapes the parent plan names: edit,
resize, exact-width EOL, a wide glyph at the wrap edge, unequal-width
splits, and cursor/scroll preservation across each.

---

## Gates

- `make check` — the PTY layer is the equivalence proof here, not an
  afterthought.
- `make WITH_LISP=0 clean all check`, `make format-check`,
  `make complexity-check` (start and end), `make coverage-check`.
- `make bench`: `visual-line-warm-100k`, `-edit-100k`, `-resize-100k`,
  `-vsplit-100k`, `-4win-100k`, and the historical aggregate
  `visual-line-100k`. Compare against the numbers in the parent plan's
  status table, which are the durable baseline — `test/.results/bench.json`
  is gitignored, so re-run before claiming anything.
- `JOBS=8 .ci/run-ci-steps.sh --parallel`. Run it **standalone**, not
  alongside another tree's runner: the `lisp-process-*` PTY cases under
  valgrind flake when lanes compete, and a flake here would be easy to
  misread as a drawing regression.

## Completion gate

- One lower-bound search per window per repaint, not one per screen row.
- Every existing visual PTY case renders and saves identically.
- The inverted counter assertion passes under normal, sanitizer and
  valgrind builds.
- Default (visual-line off) repaint counters and frame bytes unchanged —
  the non-visual branch of the loop is not touched.
- No ratchet raised.
