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

## Status — sub-plan B closed (2026-08-03)

Implemented as specified, with one design choice this document left open
and A's status section flagged: `vgeom_iter_next()` now reads the two
prefix entries it already has (`it->idx->prefix[cur_row+1] -
it->idx->prefix[cur_row]`) instead of calling `visual_segments()` once per
screen row. `draw_window_rows()` places one `struct vgeom_iter` at the
window's top visual row before the loop and advances it once per screen
row; the loop body's `fr`/`offset` meanings are unchanged, and the
past-the-end pair (`numrows`, `0`) is `find_visual_row()`'s own rather
than a new sentinel, per `flat_row_advance()`'s non-decreasing-`fr`
assumption.  `vgeom_iter_next()` fills both out-params in that case too,
instead of leaving them untouched, so the draw loop needs no branch for
it — see the per-symbol budget note below for why that matters.

**The counter evidence, and its actual size.** This document's opening
section warned its own framing was likely stale once A landed, and it
was: `KG_PERF_VISUAL_PREFIX_VISIT` does **not** move between A and B --
100,001 stays 100,001 on `visual-line-100k`, 300,003/500,005 stay exactly
so on vsplit/4win. That counter only counts calls to `visual_segments()`,
and A's `row_segment_of()` already answered every `find_visual_row()` call
with a lower-bound search that never touches it; B's contribution to that
particular counter is real (see the inverted assertion below: it goes to
exactly 0 on a warm repaint, not just `<= win_h`) but was already latent
in A's landed index, not something B added.

What B measurably moves is `KG_PERF_VGEOM_HIT`: `vgeom_ensure()` was being
called once per screen row (inside `row_segment_of()`, called from
`find_visual_row()`, called from the draw loop) before B, and once per
window per repaint (from `vgeom_iter_init()`) after. Measured with the
same counting build, same six bench cases, before B's changes and after,
holding everything else fixed:

| bench case | vgeom_hit before | after | prefix_visit (unchanged) |
|---|---|---|---|
| `visual-line-100k` | 162 | 36 | 100,001 |
| `visual-line-warm-100k` | 332 | 80 | 100,001 |
| `visual-line-edit-100k` | 129 | 24 | 200,002 |
| `visual-line-resize-100k` | 103 | 19 | 200,002 |
| `visual-line-vsplit-100k` | 770 | 140 | 300,003 |
| `visual-line-4win-100k` | 618 | 142 | 500,005 |

Wall time on the same counting build (`test/perfobj/kg`, 3 runs, median),
same before/after pair: a consistent but modest -3.0% to -3.8% across all
six cases (e.g. `visual-line-4win-100k` 1024.5ms -> 993.4ms). This is the
"smaller win than 07b's prose implies" the sub-plans README warned about:
A already collapsed the counted-visit cost: B collapses the *count of
searches*, not their cost, since each search was already O(log rows).
Do not read the wall-time number as more than it is -- it is a real,
reproducible few percent, not the order-of-magnitude story the counter
table might suggest at a glance.

**The inverted assertion, and what it asserts now.** Renamed
`test_visual_line_prefix_walk_restarts_per_screen_row` to
`test_visual_line_prefix_visits_bounded_after_warmup` in
`test/test_perf.c`. It now runs one *warm-up* repaint first (excluded from
the counters checked, since that is what builds the index and is not what
B changes), resets counters, runs a second, otherwise-unchanged repaint,
and asserts `KG_PERF_VISUAL_PREFIX_VISIT == 0` -- stricter than the
`O(log rows + win_h)` bound this document named, because B's chosen
design (reading prefix entries instead of calling `visual_segments()`)
removes the `win_h` term entirely rather than merely bounding it. This
inversion is authorized by this document's "The assertion that must be
inverted" section above; the commit message names it explicitly.

**Complexity: net +1, and a per-symbol ratchet this document's gate list
did not name.** `display.c` 147 -> 148; `vgeom.c` unchanged at 46
(splitting `row_segment_of()` into a pure `row_segment_of_idx()` plus a
thin `vgeom_ensure()`-calling wrapper added no branches `scc` counts);
`vgeom.h` 0.  Total 5438 -> 5439 against the 5500 cap, **61 points left
for C and D**.  This document predicted "net-negative or near zero" on the
theory that deleting `display.c`'s call-site pattern would cost nothing;
the iterator's init-before-loop and its past-the-end fallback each added
a branch that `find_visual_row()`'s single call site did not have, so a
small net add is the honest number.

Both of those branches were first written **inside** `draw_window_rows()`,
which took it from 67 to 69 and failed `make pmccabe-check` — a
*per-symbol* ratchet (`.ci/pmccabe-baseline.json`) that this sub-plan's
Design section warned about in prose ("Do not grow the function ... it
goes in a helper for the same reason `flat_row_advance()` did") but whose
Gates list never named the command.  `make complexity-check` is `scc`
only and passes regardless; the per-symbol gate lives in `.ci/ci-01`.
Both branches now sit outside the budgeted function — the init in a
`vline_iter_begin()` helper, exactly `flat_row_advance()`'s precedent, and
the past-the-end answer inside `vgeom_iter_next()` — and
`draw_window_rows()` is back at its baseline 67.  **C and D: run
`make pmccabe-check`, not only `make complexity-check`.**

**PTY additions.** Two new tmux-backed cases beyond the seven named
above: `visual-line-exact-width-eol-resize.yaml` (a row exactly as wide
as the window, `C-e`'s "current segment" vs. "true end" branch in
`editor_move_cursor()`'s `END_KEY` case, then a live `RESIZE` that forces
a rebuild at the new width without touching the buffer) and
`visual-line-vsplit-unequal-widths-edit.yaml` (a 39/40 vertical split,
editing through each pane in turn, where the second edit's landing spot
only comes out right if the second window's index was invalidated and
rebuilt against the first window's edit rather than answered from a stale
or wrong-width cache). Both were run against the pre-B binary as well and
produced byte-identical `expected_saved` output, which is the equivalence
proof this sub-plan is actually judged on.

No field was added to `erow`. `sizeof(erow) <= 64` still holds
(`test_erow_wrap_cache_size_cost`, untouched). The non-visual branch of
`draw_window_rows()`'s loop was not touched.
