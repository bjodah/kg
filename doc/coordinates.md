# Coordinate spaces: buffer bytes, chars, render, display columns, DAP

kg addresses text in one buffer-wide space and a line in three different
spaces, and reads a fourth off the wire when a debug adapter is talking.
Mixing them is not a compile error, and every defect this document exists
to prevent was one function producing a number in one space and another
consuming it as if it were in a different one.

| Space | What one unit is | Where the numbers live |
| --- | --- | --- |
| **buffer byte** | one byte of the flattened buffer, counting one `\n` between rows | persistent markers, edit transaction endpoints, `buffer_byte_length()` |
| **chars** | one byte of `row->chars` | point (`cx + coloff`), resolved mark columns, every row-local match offset, `row->size` |
| **render** | one byte of `row->render` (a TAB is already expanded to spaces there) | `row->hl` indexing, `row->rsize`, the row-drawing loop's cursor |
| **display column** ("visual column", vcol) | one terminal cell | goal column, rectangle bounds, the reported column, everything visual-line mode measures |
| **DAP line/column** | a line and a character position in the *adapter's* notion of the file | `stopped` → `stackTrace` frames, `setBreakpoints` lines, `gotoTargets` — `src/dap_breakpoint.c`, `src/dap_exec.c` |

They coincide only for a row that is pure single-width ASCII with no
tabs, which is why a test that uses such a row proves nothing here.

The last row is a protocol's space rather than the editor's, and it is
here because it looks like the others and is not.  Its **lines are 1-based**,
negotiated with `linesStartAt1` in the initialize request, so the editor
row of a DAP line is `line - 1` and every conversion between them happens
at exactly two seams: `src/dap_breakpoint.c`, where a breakpoint's editor
row becomes a wire line and back, and `src/dap_exec.c`, where a frame's
line becomes the row the source is shown at.  Its **columns are ignored
for navigation in v1**: a DAP column is a character position in the
adapter's own reading of the file, not a byte offset into `row->chars`,
and the two disagree for any line with a tab or a multi-byte glyph in it.
kg therefore navigates by line and passes column 1 where the protocol
insists on one (`gotoTargets`).  Making them agree needs the adapter's
column encoding, which the protocol does not state; until it does,
guessing would be worse than ignoring.

## Naming convention

- A function that *produces* a render-space number says `render` in its
  name (`chars_to_render_col`, `render_offset_at_visual`).  Despite the
  historical `_col` in the first one, **both return a byte offset into
  `row->render`**, not a column.
- A function that produces or consumes a display column says `visual`
  (`editor_visual_col`, `visual_line_cursor_col`, `visual_line_width`,
  `editor_chars_col_at_visual`, `visual_col_to_chars`).
- Row-local names without another qualifier are chars space.  `col`, `cx`,
  `coloff`, `match_col`, `mark_col`, `filecol` are chars offsets everywhere
  in the tree.  `position`, `begin_byte` and `end_byte` are flattened buffer
  byte positions; convert them with `buffer_position_to_row_col()` or
  `buffer_row_col_to_position()` at the seam.
- The one name that lied was `render_col_to_chars()`, which consumes a
  display column; it is `visual_col_to_chars()` now, and
  `find_visual_row()`'s out-parameter is `render_offset` rather than
  `char_offset`.

`KG_ASSERT_CHARS_OFF` / `KG_ASSERT_RENDER_OFF` (`src/def.h`) say which
space an argument is in at the seams below.  They compile to nothing
unless the build defines `KG_DEBUG_COORDS=1`, the same shape as
`KG_PERF_COUNTERS`; `.ci/ci-04-clang-asan-ubsan.sh` arms them, so they
are checked against the whole PTY suite on every run of the deep CI.

## Producers and consumers

Verdicts: **ok** = correct as written; **search** = fixed by the regex/search
integration work; **audit** = was wrong, fixed by the coordinate audit;
**div** = a documented
divergence that is correct but reads wrong.

| # | Producer | Space out | Consumers | Verdict |
| --- | --- | --- | --- | --- |
| 1 | `editor_visual_col(row, chars_col, display)` (`buffer.c`) | vcol | `basic.c:48` goal column; `display.c:210,213` region bounds in rect mode; `display.c:421` rect virtual tail; `rect.c:65,68,97`; `lisp_buffer.c` `current-column` | ok |
| 2 | `editor_display_col(rows, n, filerow, filecol, display)` (`buffer.c`) | vcol | `cmd.c:50` `C-x =`; `display.c:601` mode line | ok |
| 3 | `editor_chars_col_at_visual(row, vcol, display)` (`buffer.c`) | chars | `basic.c:232` goal-column snap; `display.c:346,349` rect region; `rect.c:83,84` | ok — inverse of 1 at glyph starts |
| 4 | `chars_to_render_col(row, chars_col, display)` (`mode.c`) | **render byte offset** | `display.c:357-365` `hi_lo`/`hi_hi`, compared against the render index `j`; `search.c:69` highlight span (`row->hl`) | ok — both consumers are render-indexed |
| 4b | `render_to_chars_col(row, render_col, display)` (`mode.c`) | chars | `showparen.c:284` the partner paren's column; `gitdiag.c:31` the commit-subject limit, which the legacy scanner measured in render bytes | ok — row 4's inverse at chars-byte starts; an offset inside an expanded TAB answers the offset just past it |
| 5 | `visual_col_to_chars(row, target_vcol, win_w, display)` (`mode.c`) | chars | `basic.c:60,77` visual-line Home/End; `mode.c:242` `goto_visual_row_col` | **div** — takes a *display column*, not a render offset; every caller feeds it one (`visual_line_cursor_col`, `segment * win_w`).  It is not the inverse of 4 and must never be paired with it.  Named `render_col_to_chars()` until this plan |
| 6 | `visual_line_cursor_col(row, chars_col, win_w, display)` (`mode.c`) | vcol (wrapped) | `basic.c:57,68,85,96`; `display.c:707`; `mode.c:171` | ok |
| 7 | `visual_line_width(row, win_w, display)` (`mode.c`) | vcol (wrapped) | `basic.c:70`; `mode.c:116,132,210,233` | ok |
| 8 | `render_offset_at_visual(row, vcol, win_w)` (`mode.c`) | **render byte offset** | `mode.c:193` `find_visual_row`'s `render_offset` out-param, consumed by `display.c:238` as the render slice start | ok |
| 9 | `get_visual_row(rows, n, win_w, cy, cx)` (`mode.c`) | visual row index; `cx` in | chars | `basic.c:87,98`; `cmd.c:319`; `display.c:542,595,709` | ok |
| 10 | `row->hl[i]` | render | `syntax.c` highlighters (walk `row->render`, bound by `row->rsize`); `display.c:394` the draw loop; `search.c` match highlight; `dired.c:119-129` (gutter is ASCII, so chars == render) | ok, except `generic_keyword_scan`'s single-line comment, which filled a render-indexed run with the **chars** length `row->size - i` and so stopped the colour one tab expansion short — **audit** |
| 11 | `wcur()->coloff` | chars (`filecol = coloff + cx` at `buffer.c:80`) | every command; `editor_reveal_position_centered`; `basic.c` motion | ok |
| 11b | ... the same `coloff` | consumed as a **render** offset | `display.c` `draw_window_rows` (`offset = coloff`, indexing `r->render`) and the cursor-placement loop (walking `row->chars` from `coloff`) | **audit** — the row is drawn from `chars_to_render_col(r, coloff)` (row 4), and the cursor column is the difference of two `editor_visual_col()` readings.  This was `doc/TODO.md`'s "horizontal-scroll + tab units mismatch" |
| 12 | isearch match offsets (`search.c`) | chars (`row->chars` is what is searched) | `editor_reveal_position_centered(match_row, point_col)` — takes chars, documented at `buffer.c:229` | **search** |
| 13 | `search.c` highlight span | render, converted from chars inside `hl_snapshot_mark()` | `row->hl` | fixed during the coordinate audit — literal query-replace used to pass the chars *length* `slen` as a render length, wrong for a match containing a tab |
| 14 | `kg_regex_match_forward/backward` (`regex.c`) | chars (byte offsets into the subject) | `search.c` | **search** — both paths step with `kg_utf8_forward_boundary()`; the backward scan at `regex.c:256` consumes the shared helper, so the mid-glyph `+1` is gone from both |
| 15 | `utf8_width_at` / `display_glyph_at` (`width.c`) | columns / bytes | the draw loop charges `vcol_used` in **columns** while `len` counts render **bytes**; both are needed and neither substitutes for the other | ok |
| 16 | marker position (`marker.c`) | flat buffer byte | `kg_marker_get_row_col()` converts once to a chars row/column for mark, search and Lisp consumers; rectangle marks may additionally retain a virtual chars column past EOL | ok — edits relocate the flat position before consumers resolve it |

`display` above is one `struct kg_display_options`, normally the buffer's.
It keeps the seam at one argument as more display rules become configurable;
every producer and inverse in a round trip receives the same value.

## Round-trip properties

`test/test_basic.c:test_coordinate_space_round_trips` encodes what
actually holds, on rows with tabs, 2-byte glyphs and double-width
glyphs:

- `editor_chars_col_at_visual(row, editor_visual_col(row, c, d), d) == c` for
  every `c` that starts a glyph.  It is *not* true for a `c` inside a
  multi-byte glyph: continuation bytes are zero-width, so they share
  their glyph's column and round-trip to the glyph's first byte.
- `chars_to_render_col()` is non-decreasing, maps `0` to `0` and
  `row->size` to `row->rsize`, and for a non-tab glyph the byte it names
  in `row->render` is the byte at `c` in `row->chars`.
- `visual_col_to_chars(row, visual_line_cursor_col(row, c, w, d), w, d) == c`
  at glyph starts, for a wide `w` and for a `w` narrow enough to wrap.
- Feeding `visual_col_to_chars()` the output of `chars_to_render_col()`
  gives a wrong answer as soon as a row holds a double-width glyph; the
  test pins that so the divergence in row 5 stays deliberate.
