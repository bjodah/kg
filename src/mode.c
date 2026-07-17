#include "def.h"
#include "localvars.h"

int chars_to_render_col(erow *row, int chars_col)
{
	int j, render_col = 0, visual_col = 0;

	if (chars_col > row->size) {
		chars_col = row->size;
	}
	for (j = 0; j < chars_col; j++) {
		if (row->chars[j] == TAB) {
			int spaces = 7 - visual_col % 8;

			if (spaces == 0) {
				spaces = 8;
			}
			render_col += spaces;
			visual_col += spaces;
		} else {
			render_col++;
			if (!utf8_is_cont((unsigned char)row->chars[j])) {
				visual_col++;
			}
		}
	}
	return render_col;
}

static int visual_width(erow *row) { return editor_visual_col(row, row->size); }

int visual_line_cursor_col(erow *row, int chars_col, int win_w)
{
	int rcol = editor_visual_col(row, chars_col);
	int width = visual_width(row);

	if (win_w <= 0) {
		win_w = 1;
	}
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

	if (win_w <= 0) {
		win_w = 1;
	}
	width = visual_width(row);
	return width > 0 ? 1 + (width - 1) / win_w : 1;
}

/* Map a display column to a byte offset in row->render.  Consume a whole
 * UTF-8 glyph when crossing its one-column display cell so wrapping never
 * begins on a continuation byte. */
static int render_offset_at_visual(erow *row, int target_vcol)
{
	int off = 0;
	int vcol = 0;

	if (target_vcol <= 0) {
		return 0;
	}
	while (off < row->rsize && vcol < target_vcol) {
		off++;
		while (off < row->rsize
		    && utf8_is_cont((unsigned char)row->render[off])) {
			off++;
		}
		vcol++;
	}
	return off;
}

int get_visual_row(erow *rows, int numrows, int win_w, int cy, int cx)
{
	int vrow = 0;
	int r;

	if (win_w <= 0) {
		win_w = 1;
	}
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
    int target_y, int *logical_row, int *char_offset)
{
	int visual_row_count = 0;
	int r;

	if (win_w <= 0) {
		win_w = 1;
	}
	for (r = 0; r < numrows; r++) {
		int segments = visual_segments(&rows[r], win_w);
		if (visual_row_count + segments > rowoff_visual + target_y) {
			int segment
			    = rowoff_visual + target_y - visual_row_count;

			*logical_row = r;
			*char_offset = render_offset_at_visual(
			    &rows[r], segment * win_w);
			return;
		}
		visual_row_count += segments;
	}
	*logical_row = numrows;
	*char_offset = 0;
}

int render_col_to_chars(erow *row, int target_rcol)
{
	if (target_rcol > visual_width(row)) {
		target_rcol = visual_width(row);
	}
	return editor_chars_col_at_visual(row, target_rcol);
}

void goto_visual_row_col(int target_vrow, int target_rcol_in_segment)
{
	int r;
	int visual_row_count = 0;
	int win_w = winlist[win_current].w;

	if (win_w <= 0) {
		win_w = 1;
	}
	if (target_vrow < 0) {
		target_vrow = 0;
	}
	for (r = 0; r < editor.numrows; r++) {
		int width = visual_width(&editor.row[r]);
		int segments = visual_segments(&editor.row[r], win_w);
		if (visual_row_count + segments > target_vrow) {
			int segment_idx = target_vrow - visual_row_count;
			int start_rcol = segment_idx * win_w;
			int target_rcol = start_rcol + target_rcol_in_segment;
			if (target_rcol > width) {
				target_rcol = width;
			}
			int char_idx
			    = render_col_to_chars(&editor.row[r], target_rcol);
			editor_cursor_goto(r, char_idx);
			return;
		}
		visual_row_count += segments;
	}
	if (editor.numrows > 0) {
		editor_cursor_goto(
		    editor.numrows - 1, editor.row[editor.numrows - 1].size);
	}
}

int get_total_visual_rows(erow *rows, int numrows, int win_w)
{
	int total = 0;
	int r;

	if (win_w <= 0) {
		win_w = 1;
	}
	for (r = 0; r < numrows; r++) {
		total += visual_segments(&rows[r], win_w);
	}
	return total;
}
