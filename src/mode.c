#include <string.h>

#include "def.h"
#include "perf.h"
#include "vgeom.h"

int chars_to_render_col(
    erow *row, int chars_col, const struct kg_display_options *options)
{
	int j, render_col = 0, visual_col = 0;

	if (chars_col > row->size) {
		chars_col = row->size;
	}
	for (j = 0; j < chars_col; j++) {
		if (row->chars[j] == TAB) {
			int spaces = tab_stop_advance(visual_col, options);

			render_col += spaces;
			visual_col += spaces;
		} else {
			render_col++;
			visual_col += utf8_width_at(row->chars, row->size, j);
		}
	}
	return render_col;
}

/* The inverse walk, for a caller holding a render-byte offset -- a
 * highlight span, a scanner's answer -- that has to become a chars offset
 * before it can be a buffer position.  Kept beside chars_to_render_col()
 * rather than copied into each such caller: the two walks have to agree
 * about what a TAB expands to, and they cannot disagree from here. */
int render_to_chars_col(
    erow *row, int render_col, const struct kg_display_options *options)
{
	int j, render = 0, visual_col = 0;

	for (j = 0; j < row->size && render < render_col; j++) {
		if (row->chars[j] == TAB) {
			int spaces = tab_stop_advance(visual_col, options);

			render += spaces;
			visual_col += spaces;
		} else {
			render++;
			visual_col += utf8_width_at(row->chars, row->size, j);
		}
	}
	return j;
}

/* A window narrower than one cell cannot lay anything out.  Every
 * wrapping calculation below starts by taking its width through here, so
 * a zero or negative width is one line of arithmetic rather than eight
 * copies of the same guard.  Declared in vgeom.h: src/vgeom.c's index
 * keys on the same normalized width, so it has to call this rather than
 * repeat the rule. */
int win_cells(int win_w) { return win_w > 0 ? win_w : 1; }

/* A glyph wider than one cell cannot be split across a wrap, so Emacs
 * moves it whole to the next display row and leaves the edge cells
 * blank.  Returns the blank cells to charge before laying a `width`-cell
 * glyph out at display column `vcol`.  Double-width characters are the
 * common case; display_glyph_at() also spells a C1 control or a
 * malformed byte "\xnn", which is four. */
static int wrap_pad(int vcol, int width, int win_w)
{
	int room;

	/* A window too narrow to hold the glyph at all can never be padded
	 * into fitting it; don't pad forever, and never divide by zero. */
	if (width < 2 || win_w <= 1 || width > win_w) {
		return 0;
	}
	room = win_w - (vcol % win_w);
	return width > room ? room : 0;
}

/* Display column of byte offset `chars_col` once `row` is wrapped every
 * win_w cells.  Same as editor_visual_col() except for rows where a
 * double-width glyph lands on a wrap boundary; those gain one blank
 * column per bumped glyph so this model matches what the terminal draws.
 * Callers pass win_w > 0. */
static int wrapped_visual_col(erow *row, int chars_col, int win_w,
    const struct kg_display_options *options)
{
	int j, vcol = 0;
	int limit = chars_col < row->size ? chars_col : row->size;

	for (j = 0; j < limit; j++) {
		if (row->chars[j] == TAB) {
			vcol += tab_stop_advance(vcol, options);
		} else {
			int w = utf8_width_at(row->chars, row->size, j);

			vcol += wrap_pad(vcol, w, win_w) + w;
		}
	}
	if (chars_col > row->size) {
		vcol += chars_col - row->size;
	}
	return vcol;
}

/* Inverse of wrapped_visual_col(): byte offset whose wrapped display
 * column lands at or just before `target_vcol`.  Targets past the row's
 * end clamp to row->size; callers clamp beforehand anyway. */
static int wrapped_chars_col(erow *row, int target_vcol, int win_w,
    const struct kg_display_options *options)
{
	int j = 0, vcol = 0;

	while (j < row->size) {
		int next;

		if (row->chars[j] == TAB) {
			next = vcol + tab_stop_advance(vcol, options);
		} else {
			int w = utf8_width_at(row->chars, row->size, j);

			next = vcol + wrap_pad(vcol, w, win_w) + w;
		}
		if (next > target_vcol) {
			break;
		}
		vcol = next;
		j++;
	}
	return j;
}

/* Total display columns `row` occupies when wrapped every win_w cells,
 * including any blank cells left by glyphs bumped off a wrap boundary.
 *
 * Window width and display options are the row's wrapped-width cache key
 * (def.h's erow).  A cache hit costs no row-byte scan at all, which is
 * what lets an unchanged repaint stay off the buffer's total size; a miss
 * rescans and refills the cache for next time. */
int visual_line_width(
    erow *row, int win_w, const struct kg_display_options *options)
{
	struct kg_wrap_cache_key key;

	win_w = win_cells(win_w);
	key = (struct kg_wrap_cache_key) {
		.win_w = win_w,
		.tab_width = display_tab_width(options),
	};
	if (memcmp(&row->wrap_cache_key, &key, sizeof(key)) == 0) {
		KG_PERF_INC(KG_PERF_VISUAL_WIDTH_CACHE_HIT);
		return row->wrap_cache_vcols;
	}
	KG_PERF_INC(KG_PERF_VISUAL_ROW_SCAN);
	KG_PERF_ADD(KG_PERF_VISUAL_BYTE_SCAN, row->size);
	row->wrap_cache_vcols
	    = wrapped_visual_col(row, row->size, win_w, options);
	row->wrap_cache_key = key;
	return row->wrap_cache_vcols;
}

int visual_line_cursor_col(erow *row, int chars_col, int win_w,
    const struct kg_display_options *options)
{
	int rcol, width;

	win_w = win_cells(win_w);
	/* visual_line_width() already walks the row to chars_col == size;
	 * asking wrapped_visual_col() to redo that same walk when point
	 * sits at or past EOL (end-of-line motions, appends, a cursor
	 * parked past a short line) would query the row's width twice. */
	width = visual_line_width(row, win_w, options);
	rcol = chars_col < row->size
	    ? wrapped_visual_col(row, chars_col, win_w, options)
	    : width + (chars_col - row->size);
	/* Point at EOL on an exact-width line remains on the final display
	 * row.  Treat its screen cell as the last cell of that segment. */
	if (rcol > 0 && rcol == width && rcol % win_w == 0) {
		rcol--;
	}
	return rcol;
}

/* Wrap segments a row of display width `width` occupies at `win_w`.  Pure
 * arithmetic on an already-known width, so a caller that has one (a cache
 * hit, or a width it just computed for another reason) never has to ask
 * visual_line_width() for the same row again just to get its segment
 * count. */
static int visual_segments_for_width(int width, int win_w)
{
	return width > 0 ? 1 + (width - 1) / win_w : 1;
}

/* Declared in vgeom.h: src/vgeom.c's index rebuild calls this once per
 * row, which is what makes KG_PERF_VISUAL_PREFIX_VISIT -- incremented
 * here -- still mean "a row the O(rows) side of the geometry API
 * visited" once that rebuild, rather than a per-screen-row walk, is the
 * thing incrementing it. */
int visual_segments(
    erow *row, int win_w, const struct kg_display_options *options)
{
	win_w = win_cells(win_w);
	KG_PERF_INC(KG_PERF_VISUAL_PREFIX_VISIT);
	return visual_segments_for_width(
	    visual_line_width(row, win_w, options), win_w);
}

/* Byte offset in row->render where the display column `target_vcol`
 * begins.  Advances a whole glyph at a time so a wrap never starts on a
 * continuation byte, and a double-width glyph pushed off the edge by
 * wrap_pad() starts the following display row rather than being split.
 * Zero-width glyphs stay with the base character they decorate.
 * Declared in vgeom.h: src/vgeom.c's index consumers use it to turn a
 * (row, segment) pair into the render_offset find_visual_row() reports. */
int render_offset_at_visual(erow *row, int target_vcol, int win_w)
{
	int off = 0;
	int vcol = 0;

	while (off < row->rsize) {
		int w = utf8_width_at(row->render, row->rsize, off);
		int start = vcol + wrap_pad(vcol, w, win_w);

		if (w > 0 && start >= target_vcol) {
			break;
		}
		vcol = start + w;
		off += utf8_glyph_span_at(row->render, row->rsize, off);
	}
	return off;
}

/* Byte offset into row->chars at wrapped display column `target_vcol`.
 * The argument is a display column -- what visual_line_cursor_col() and
 * a segment boundary produce -- and never the render byte offset
 * chars_to_render_col() returns; see doc/coordinates.md row 5. */
int visual_col_to_chars(erow *row, int target_vcol, int win_w,
    const struct kg_display_options *options)
{
	int width;

	win_w = win_cells(win_w);
	width = visual_line_width(row, win_w, options);
	if (target_vcol > width) {
		target_vcol = width;
	}
	if (target_vcol < 0) {
		target_vcol = 0;
	}
	{
		int chars_col
		    = wrapped_chars_col(row, target_vcol, win_w, options);

		KG_ASSERT_CHARS_OFF(row, chars_col);
		return chars_col;
	}
}

/* get_visual_row(), find_visual_row(), get_total_visual_rows() and
 * goto_visual_row_col() moved to src/vgeom.c: they are the O(rows) walks
 * plan 07 phase 2 replaces with the persistent per-window prefix index,
 * built from the row-at-a-time primitives above (visual_segments() in
 * particular). */
