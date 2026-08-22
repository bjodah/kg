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

struct kg_wrap_segment {
	int start_chars;
	int end_chars;
	int eol_chars;
	int vcols;
};

static void get_row_segment(erow *row, int seg_start, int win_w,
    const struct kg_display_options *options, struct kg_wrap_segment *seg)
{
	int j = seg_start;
	int vcol = 0;
	int last_break_chars = -1;
	int last_break_vcol = 0;
	int last_break_eol = -1;
	int curr_word_end = seg_start;
	int last_col_chars = seg_start;
	int word_wrap = options ? options->word_wrap : 0;

	win_w = win_cells(win_w);
	seg->start_chars = seg_start;

	while (j < row->size) {
		int span, w, pad;
		int glyph_start = j;

		if (row->chars[j] == TAB) {
			span = 1;
			w = tab_stop_advance(vcol, options);
		} else {
			span = utf8_glyph_span_at(row->chars, row->size, j);
			w = utf8_width_at(row->chars, row->size, j);
		}
		pad = wrap_pad(vcol, w, win_w);

		if (vcol + pad + w > win_w) {
			if (word_wrap && last_break_chars > seg_start) {
				seg->end_chars = last_break_chars;
				seg->eol_chars = last_break_eol >= seg_start
				    ? last_break_eol
				    : last_break_chars;
				seg->vcols = last_break_vcol;
				return;
			}
			if (glyph_start > seg_start) {
				seg->end_chars = glyph_start;
				seg->eol_chars = last_col_chars;
				seg->vcols = vcol;
				return;
			}
		}

		if (vcol + pad < win_w) {
			last_col_chars = glyph_start;
		}

		vcol += pad + w;
		j += span;

		if (word_wrap) {
			if (row->chars[glyph_start] == ' '
			    || row->chars[glyph_start] == '\t') {
				last_break_chars = j;
				last_break_vcol = vcol;
				last_break_eol = curr_word_end;
			} else {
				curr_word_end = j;
			}
		}
	}

	seg->end_chars = row->size;
	seg->eol_chars = row->size;
	seg->vcols = vcol;
}

/* Display column of byte offset `chars_col` once `row` is wrapped every
 * win_w cells.  Same as editor_visual_col() except for rows where a
 * double-width glyph lands on a wrap boundary; those gain one blank
 * column per bumped glyph so this model matches what the terminal draws.
 * Callers pass win_w > 0. */
static int wrapped_visual_col(erow *row, int chars_col, int win_w,
    const struct kg_display_options *options)
{
	int seg_start = 0;
	int seg_idx = 0;
	struct kg_wrap_segment seg;

	win_w = win_cells(win_w);
	while (seg_start < row->size) {
		get_row_segment(row, seg_start, win_w, options, &seg);
		if (chars_col < seg.end_chars || seg.end_chars >= row->size) {
			int j = seg.start_chars;
			int vcol = 0;
			int limit
			    = chars_col < row->size ? chars_col : row->size;

			while (j < limit) {
				if (row->chars[j] == TAB) {
					vcol += tab_stop_advance(vcol, options);
					j++;
				} else {
					int w = utf8_width_at(
					    row->chars, row->size, j);

					vcol += wrap_pad(vcol, w, win_w) + w;
					j += utf8_glyph_span_at(
					    row->chars, row->size, j);
				}
			}
			if (chars_col > row->size) {
				vcol += chars_col - row->size;
			}
			return seg_idx * win_w + vcol;
		}
		seg_start = seg.end_chars;
		seg_idx++;
	}
	if (chars_col > row->size) {
		return seg_idx * win_w + (chars_col - row->size);
	}
	return seg_idx * win_w;
}

/* Inverse of wrapped_visual_col(): byte offset whose wrapped display
 * column lands at or just before `target_vcol`.  Targets past the row's
 * end clamp to row->size; callers clamp beforehand anyway. */
static int wrapped_chars_col(erow *row, int target_vcol, int win_w,
    const struct kg_display_options *options)
{
	int target_seg, target_col_in_seg;
	int seg_start = 0, seg_idx = 0;
	struct kg_wrap_segment seg;

	win_w = win_cells(win_w);
	if (target_vcol <= 0) {
		return 0;
	}
	target_seg = target_vcol / win_w;
	target_col_in_seg = target_vcol % win_w;

	while (seg_start < row->size) {
		get_row_segment(row, seg_start, win_w, options, &seg);
		if (seg_idx == target_seg || seg.end_chars >= row->size) {
			int j = seg.start_chars;
			int vcol = 0;

			while (j < seg.end_chars) {
				int next, span;

				if (row->chars[j] == TAB) {
					span = 1;
					next = vcol
					    + tab_stop_advance(vcol, options);
				} else {
					span = utf8_glyph_span_at(
					    row->chars, row->size, j);
					next = vcol
					    + wrap_pad(vcol,
						utf8_width_at(row->chars,
						    row->size, j),
						win_w)
					    + utf8_width_at(
						row->chars, row->size, j);
				}
				if (next > target_col_in_seg) {
					break;
				}
				vcol = next;
				j += span;
			}
			return j;
		}
		seg_start = seg.end_chars;
		seg_idx++;
	}
	return row->size;
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
	int word_wrap = options ? options->word_wrap : 0;

	win_w = win_cells(win_w);
	key = (struct kg_wrap_cache_key) {
		.win_w = win_w,
		.tab_width = display_tab_width(options),
		.word_wrap = word_wrap,
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

/* Declared in vgeom.h: src/vgeom.c's index rebuild calls this once per
 * row, which is what makes KG_PERF_VISUAL_PREFIX_VISIT -- incremented
 * here -- still mean "a row the O(rows) side of the geometry API
 * visited" once that rebuild, rather than a per-screen-row walk, is the
 * thing incrementing it. */
static int visual_segments_for_width(int width, int win_w)
{
	return width > 0 ? 1 + (width - 1) / win_w : 1;
}

int visual_segments(
    erow *row, int win_w, const struct kg_display_options *options)
{
	win_w = win_cells(win_w);
	KG_PERF_INC(KG_PERF_VISUAL_PREFIX_VISIT);
	return visual_segments_for_width(
	    visual_line_width(row, win_w, options), win_w);
}

/* Render-byte offset in row->render where the display column `target_vcol`
 * begins.  Advances a whole glyph at a time so a wrap never starts on a
 * continuation byte, and a double-width glyph pushed off the edge by
 * wrap_pad() starts the following display row rather than being split.
 * Zero-width glyphs stay with the base character they decorate.
 * Declared in vgeom.h: src/vgeom.c's index consumers use it to turn a
 * (row, segment) pair into the render_offset find_visual_row() reports.
 * NOTE: Must only be called for fixed-width wrapping (word_wrap == 0).
 * For word-wrap segments, use render_span_of_segment(). */
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

void render_span_of_segment(erow *row, int segment, int win_w,
    const struct kg_display_options *options, int *out_start, int *out_end)
{
	int seg_start = 0;
	int seg_idx = 0;
	struct kg_wrap_segment seg;

	win_w = win_cells(win_w);
	while (seg_start < row->size) {
		get_row_segment(row, seg_start, win_w, options, &seg);
		if (seg_idx == segment || seg.end_chars >= row->size) {
			*out_start
			    = chars_to_render_col(row, seg.start_chars, options);
			*out_end = seg.end_chars >= row->size
			    ? row->rsize
			    : chars_to_render_col(row, seg.end_chars, options);
			return;
		}
		seg_start = seg.end_chars;
		seg_idx++;
	}
	*out_start = row->rsize;
	*out_end = row->rsize;
}

int visual_bol_to_chars(erow *row, int chars_col, int win_w,
    const struct kg_display_options *options)
{
	int seg_start = 0;
	struct kg_wrap_segment seg;

	win_w = win_cells(win_w);
	if (!row || chars_col <= 0) {
		return 0;
	}
	while (seg_start < row->size) {
		get_row_segment(row, seg_start, win_w, options, &seg);
		if (chars_col < seg.end_chars || seg.end_chars >= row->size) {
			return seg.start_chars;
		}
		seg_start = seg.end_chars;
	}
	return 0;
}

int visual_eol_to_chars(erow *row, int chars_col, int win_w,
    const struct kg_display_options *options)
{
	int seg_start = 0;
	struct kg_wrap_segment seg;

	win_w = win_cells(win_w);
	if (!row || chars_col >= row->size) {
		return row ? row->size : 0;
	}
	while (seg_start < row->size) {
		get_row_segment(row, seg_start, win_w, options, &seg);
		if (chars_col < seg.end_chars || seg.end_chars >= row->size) {
			if (options && options->word_wrap) {
				return seg.eol_chars;
			}
			if (seg.end_chars >= row->size && seg.vcols > 0
			    && seg.vcols % win_w == 0
			    && chars_col < seg.eol_chars) {
				return seg.eol_chars - 1;
			}
			return seg.eol_chars;
		}
		seg_start = seg.end_chars;
	}
	return row->size;
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
