# Coordinate spaces: chars, render, display columns

kg addresses a line in three different spaces.  Mixing them is not a
compile error, and every defect this document exists to prevent was one
function producing a number in one space and another consuming it as if
it were in a different one.

| Space | What one unit is | Where the numbers live |
| --- | --- | --- |
| **chars** | one byte of `row->chars` | point (`cx + coloff`), the mark, every match offset, `row->size` |
| **render** | one byte of `row->render` (a TAB is already expanded to spaces there) | `row->hl` indexing, `row->rsize`, the row-drawing loop's cursor |
| **display column** ("visual column", vcol) | one terminal cell | goal column, rectangle bounds, the reported column, everything visual-line mode measures |

They coincide only for a row that is pure single-width ASCII with no
tabs, which is why a test that uses such a row proves nothing here.

## Naming convention

- A function that *produces* a render-space number says `render` in its
  name (`chars_to_render_col`, `render_offset_at_visual`).  Despite the
  historical `_col` in the first one, **both return a byte offset into
  `row->render`**, not a column.
- A function that produces or consumes a display column says `visual`
  (`editor_visual_col`, `visual_line_cursor_col`, `visual_line_width`,
  `editor_chars_col_at_visual`).
- Anything else is chars space.  `col`, `cx`, `coloff`, `match_col`,
  `mark_col`, `filecol` are chars offsets everywhere in the tree.
- `render_col_to_chars()` is the one name that lies, and it is kept for
  the diff it would cost: it consumes a **display column**, not a render
  offset — see the divergence row below.

## Producers and consumers

Verdicts: **ok** = correct as written; **P03** = fixed by plan 03;
**P14** = wrong, and fixed by this plan's phase 3; **div** = a
documented divergence that is correct but reads wrong.

| # | Producer | Space out | Consumers | Verdict |
| --- | --- | --- | --- | --- |
| 1 | `editor_visual_col(row, chars_col)` (`buffer.c`) | vcol | `basic.c:48` goal column; `display.c:210,213` region bounds in rect mode; `display.c:421` rect virtual tail; `rect.c:65,68,97`; `lisp.c:580` `current-column` | ok |
| 2 | `editor_display_col(rows, n, filerow, filecol)` (`buffer.c`) | vcol | `cmd.c:50` `C-x =`; `display.c:601` mode line | ok |
| 3 | `editor_chars_col_at_visual(row, vcol)` (`buffer.c`) | chars | `basic.c:232` goal-column snap; `display.c:346,349` rect region; `rect.c:83,84` | ok — inverse of 1 at glyph starts |
| 4 | `chars_to_render_col(row, chars_col)` (`mode.c`) | **render byte offset** | `display.c:357-365` `hi_lo`/`hi_hi`, compared against the render index `j`; `search.c:69` highlight span (`row->hl`) | ok — both consumers are render-indexed |
| 5 | `render_col_to_chars(row, target, win_w)` (`mode.c`) | chars | `basic.c:60,77` visual-line Home/End; `mode.c:242` `goto_visual_row_col` | **div** — takes a *display column*, not a render offset; every caller feeds it one (`visual_line_cursor_col`, `segment * win_w`).  It is not the inverse of 4 and must never be paired with it |
| 6 | `visual_line_cursor_col(row, chars_col, win_w)` (`mode.c`) | vcol (wrapped) | `basic.c:57,68,85,96`; `display.c:707`; `mode.c:171` | ok |
| 7 | `visual_line_width(row, win_w)` (`mode.c`) | vcol (wrapped) | `basic.c:70`; `mode.c:116,132,210,233` | ok |
| 8 | `render_offset_at_visual(row, vcol, win_w)` (`mode.c`) | **render byte offset** | `mode.c:193` `find_visual_row`'s out-param, named `char_offset` but consumed by `display.c:238` as the render slice start | ok — misleading parameter name only |
| 9 | `get_visual_row(rows, n, win_w, cy, cx)` (`mode.c`) | visual row index; `cx` in | chars | `basic.c:87,98`; `cmd.c:319`; `display.c:542,595,709` | ok |
| 10 | `row->hl[i]` | render | `syntax.c` highlighters (walk `row->render`, bound by `row->rsize`); `display.c:394` the draw loop; `search.c` match highlight; `dired.c:119-129` (gutter is ASCII, so chars == render) | ok, except `generic_keyword_scan`'s single-line comment, which used the **chars** length `row->size - i` to fill a render-indexed run — **P14** |
| 11 | `wcur()->coloff` | chars (`filecol = coloff + cx` at `buffer.c:80`) | every command; `editor_reveal_position_centered`; `basic.c` motion | ok |
| 11b | ... the same `coloff` | consumed as a **render** offset | `display.c` `draw_window_rows` (`offset = coloff`, indexing `r->render`) and the cursor-placement loop (walking `row->chars` from `coloff`) | **P14** — the fix is to convert with 4 where the row is drawn, and to take the cursor column as the difference of two `editor_visual_col()` readings.  This was `doc/TODO.md`'s "horizontal-scroll + tab units mismatch" |
| 12 | isearch match offsets (`search.c`) | chars (`row->chars` is what is searched) | `editor_reveal_position_centered(match_row, point_col)` — takes chars, documented at `buffer.c:229` | **P03** |
| 13 | `search.c` highlight span | render, converted from chars inside `hl_snapshot_mark()` | `row->hl` | fixed in this plan's phase 2 — the literal query-replace used to pass the chars *length* `slen` as a render length, wrong for a match containing a tab |
| 14 | `kg_regex_match_forward/backward` (`regex.c`) | chars (byte offsets into the subject) | `search.c` | **P03** — both paths step with `kg_utf8_forward_boundary()`; the backward scan at `regex.c:256` consumes the shared helper, so the mid-glyph `+1` is gone from both |
| 15 | `utf8_width_at` / `display_glyph_at` (`width.c`) | columns / bytes | the draw loop charges `vcol_used` in **columns** while `len` counts render **bytes**; both are needed and neither substitutes for the other | ok |

## Round-trip properties

`test/test_basic.c:test_coordinate_space_round_trips` encodes what
actually holds, on rows with tabs, 2-byte glyphs and double-width
glyphs:

- `editor_chars_col_at_visual(row, editor_visual_col(row, c)) == c` for
  every `c` that starts a glyph.  It is *not* true for a `c` inside a
  multi-byte glyph: continuation bytes are zero-width, so they share
  their glyph's column and round-trip to the glyph's first byte.
- `chars_to_render_col()` is non-decreasing, maps `0` to `0` and
  `row->size` to `row->rsize`, and for a non-tab glyph the byte it names
  in `row->render` is the byte at `c` in `row->chars`.
- `render_col_to_chars(row, visual_line_cursor_col(row, c, w), w) == c`
  at glyph starts, for a wide `w` and for a `w` narrow enough to wrap.
- Feeding `render_col_to_chars()` the output of `chars_to_render_col()`
  gives a wrong answer as soon as a row holds a double-width glyph; the
  test pins that so the divergence in row 5 stays deliberate.
