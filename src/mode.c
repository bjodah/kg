#include "def.h"
#include "localvars.h"
#include "perf.h"

int chars_to_render_col(erow *row, int chars_col)
{
	int j, render_col = 0, visual_col = 0;

	if (chars_col > row->size) {
		chars_col = row->size;
	}
	for (j = 0; j < chars_col; j++) {
		if (row->chars[j] == TAB) {
			int spaces = tab_stop_advance(visual_col);

			render_col += spaces;
			visual_col += spaces;
		} else {
			render_col++;
			visual_col += utf8_width_at(row->chars, row->size, j);
		}
	}
	return render_col;
}

/* A window narrower than one cell cannot lay anything out.  Every
 * wrapping calculation below starts by taking its width through here, so
 * a zero or negative width is one line of arithmetic rather than eight
 * copies of the same guard. */
static int win_cells(int win_w) { return win_w > 0 ? win_w : 1; }

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
static int wrapped_visual_col(erow *row, int chars_col, int win_w)
{
	int j, vcol = 0;
	int limit = chars_col < row->size ? chars_col : row->size;

	for (j = 0; j < limit; j++) {
		if (row->chars[j] == TAB) {
			vcol += tab_stop_advance(vcol);
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
static int wrapped_chars_col(erow *row, int target_vcol, int win_w)
{
	int j = 0, vcol = 0;

	while (j < row->size) {
		int next;

		if (row->chars[j] == TAB) {
			next = vcol + tab_stop_advance(vcol);
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
 * including any blank cells left by glyphs bumped off a wrap boundary. */
int visual_line_width(erow *row, int win_w)
{
	win_w = win_cells(win_w);
	KG_PERF_INC(KG_PERF_VISUAL_ROW_SCAN);
	KG_PERF_ADD(KG_PERF_VISUAL_BYTE_SCAN, row->size);
	return wrapped_visual_col(row, row->size, win_w);
}

int visual_line_cursor_col(erow *row, int chars_col, int win_w)
{
	int rcol, width;

	win_w = win_cells(win_w);
	rcol = wrapped_visual_col(row, chars_col, win_w);
	width = visual_line_width(row, win_w);
	/* Point at EOL on an exact-width line remains on the final display
	 * row.  Treat its screen cell as the last cell of that segment. */
	if (rcol > 0 && rcol == width && rcol % win_w == 0) {
		rcol--;
	}
	return rcol;
}

static int visual_segments(erow *row, int win_w)
{
	int width;

	win_w = win_cells(win_w);
	KG_PERF_INC(KG_PERF_VISUAL_PREFIX_VISIT);
	width = visual_line_width(row, win_w);
	return width > 0 ? 1 + (width - 1) / win_w : 1;
}

/* Byte offset in row->render where the display column `target_vcol`
 * begins.  Advances a whole glyph at a time so a wrap never starts on a
 * continuation byte, and a double-width glyph pushed off the edge by
 * wrap_pad() starts the following display row rather than being split.
 * Zero-width glyphs stay with the base character they decorate. */
static int render_offset_at_visual(erow *row, int target_vcol, int win_w)
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

int get_visual_row(erow *rows, int numrows, int win_w, int cy, int cx)
{
	int vrow = 0;
	int r;

	win_w = win_cells(win_w);
	for (r = 0; r < cy && r < numrows; r++) {
		vrow += visual_segments(&rows[r], win_w);
	}
	if (cy < numrows) {
		int rcol = visual_line_cursor_col(&rows[cy], cx, win_w);
		vrow += rcol / win_w;
	}
	return vrow;
}

void find_visual_row(erow *rows, int numrows, int win_w, int rowoff_visual,
    int target_y, int *logical_row, int *render_offset)
{
	int visual_row_count = 0;
	int r;

	win_w = win_cells(win_w);
	for (r = 0; r < numrows; r++) {
		int segments = visual_segments(&rows[r], win_w);
		if (visual_row_count + segments > rowoff_visual + target_y) {
			int segment
			    = rowoff_visual + target_y - visual_row_count;

			*logical_row = r;
			*render_offset = render_offset_at_visual(
			    &rows[r], segment * win_w, win_w);
			return;
		}
		visual_row_count += segments;
	}
	*logical_row = numrows;
	*render_offset = 0;
}

/* Byte offset into row->chars at wrapped display column `target_vcol`.
 * The argument is a display column -- what visual_line_cursor_col() and
 * a segment boundary produce -- and never the render byte offset
 * chars_to_render_col() returns; see doc/coordinates.md row 5. */
int visual_col_to_chars(erow *row, int target_vcol, int win_w)
{
	int width;

	win_w = win_cells(win_w);
	width = visual_line_width(row, win_w);
	if (target_vcol > width) {
		target_vcol = width;
	}
	if (target_vcol < 0) {
		target_vcol = 0;
	}
	{
		int chars_col = wrapped_chars_col(row, target_vcol, win_w);

		KG_ASSERT_CHARS_OFF(row, chars_col);
		return chars_col;
	}
}

void goto_visual_row_col(int target_vrow, int target_rcol_in_segment)
{
	int r;
	int visual_row_count = 0;
	int win_w = winlist[win_current].w;

	win_w = win_cells(win_w);
	if (target_vrow < 0) {
		target_vrow = 0;
	}
	for (r = 0; r < bcur()->numrows; r++) {
		int width = visual_line_width(&bcur()->row[r], win_w);
		int segments = visual_segments(&bcur()->row[r], win_w);
		if (visual_row_count + segments > target_vrow) {
			int segment_idx = target_vrow - visual_row_count;
			int start_rcol = segment_idx * win_w;
			int target_rcol = start_rcol + target_rcol_in_segment;
			if (target_rcol > width) {
				target_rcol = width;
			}
			int char_idx = visual_col_to_chars(
			    &bcur()->row[r], target_rcol, win_w);
			editor_cursor_goto(r, char_idx);
			return;
		}
		visual_row_count += segments;
	}
	if (bcur()->numrows > 0) {
		editor_cursor_goto(
		    bcur()->numrows - 1, bcur()->row[bcur()->numrows - 1].size);
	}
}

int get_total_visual_rows(erow *rows, int numrows, int win_w)
{
	int total = 0;
	int r;

	win_w = win_cells(win_w);
	for (r = 0; r < numrows; r++) {
		total += visual_segments(&rows[r], win_w);
	}
	return total;
}
