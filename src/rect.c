/* rect.c - Rectangle operations (C-x SPC, C-x r {k,y,d,c,t}) */

#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "localvars.h"

/* Rectangle kill ring.  Holds the last killed/copied rectangle as a
 * '\n'-joined string of per-row content, plus the row count so yank
 * can rebuild the rectangle exactly even when some rows are empty. */
static char *rect_killed = NULL;
static int rect_killed_len = 0;
static int rect_killed_nrows = 0;

void rect_kill_ring_free(void)
{
	free(rect_killed);
	rect_killed = NULL;
	rect_killed_len = 0;
	rect_killed_nrows = 0;
}

static void rect_kill_ring_set(char *text, int len, int nrows)
{
	int alloc_sz;
	char *new_killed;

	if (!checked_add_int_size(&alloc_sz, len, 1)) {
		return;
	}
	new_killed = malloc(alloc_sz);
	if (!new_killed) {
		return;
	}
	if (len > 0) {
		memcpy(new_killed, text, len);
	}
	new_killed[len] = '\0';
	rect_kill_ring_free();
	rect_killed = new_killed;
	rect_killed_len = len;
	rect_killed_nrows = nrows;
}

/* Resolve point + mark into normalized rectangle bounds.  The column
 * range is in VISUAL columns so tab-laden rows under the rect get the
 * same visible left/right edges; each operation maps the visual range
 * back to its own byte range per row via editor_chars_col_at_visual().
 * Returns 0 (with a status message) if there is no mark. */
static int rect_bounds(int *s_row, int *s_vcol, int *e_row, int *e_vcol)
{
	int p_row, p_col, m_row, m_col;
	int p_vcol, m_vcol;

	if (!editor.mark_set) {
		editor_set_status_message("No mark set");
		return 0;
	}
	p_row = editor.rowoff + editor.cy;
	p_col = editor.coloff + editor.cx;
	m_row = editor.mark_row;
	m_col = editor.mark_col;
	p_vcol = (p_row < bcur()->numrows)
	    ? editor_visual_col(&bcur()->row[p_row], p_col)
	    : p_col;
	m_vcol = (m_row < bcur()->numrows)
	    ? editor_visual_col(&bcur()->row[m_row], m_col)
	    : m_col;
	*s_row = (p_row < m_row) ? p_row : m_row;
	*e_row = (p_row > m_row) ? p_row : m_row;
	*s_vcol = (p_vcol < m_vcol) ? p_vcol : m_vcol;
	*e_vcol = (p_vcol > m_vcol) ? p_vcol : m_vcol;
	return 1;
}

/* Map a row's [s_vcol, e_vcol) visual range to its byte range.  Clamps
 * to row content — past-EOL portions don't contribute bytes, only the
 * visual highlight (rendered as virtual spaces). */
static void rect_row_byte_range(
    erow *row, int s_vcol, int e_vcol, int *byte_lo, int *byte_hi)
{
	int lo = editor_chars_col_at_visual(row, s_vcol);
	int hi = editor_chars_col_at_visual(row, e_vcol);
	if (lo > row->size) {
		lo = row->size;
	}
	if (hi > row->size) {
		hi = row->size;
	}
	*byte_lo = lo;
	*byte_hi = hi;
}

static void rect_row_pad_to_visual(erow *row, int target_vcol)
{
	int end_vcol = editor_visual_col(row, row->size);

	if (end_vcol < target_vcol) {
		editor_row_insert_spaces(
		    row, row->size, target_vcol - end_vcol);
	}
}

static void rect_row_replace_with_spaces(erow *row, int lo, int hi)
{
	if (hi <= lo) {
		return;
	}
	memset(row->chars + lo, ' ', hi - lo);
	editor_update_row(bcur(), row);
	bcur()->dirty++;
}

/* Snapshot rows [start_row, end_row) joined with '\n' for undo storage.
 * Caller frees the returned buffer.  Returns NULL when there is nothing
 * to snapshot, with *out_len = 0. */
static char *rect_snapshot_rows(int start_row, int end_row, int *out_len)
{
	int total = 0;
	int r;
	char *buf, *p;

	if (start_row < 0) {
		start_row = 0;
	}
	if (end_row > bcur()->numrows) {
		end_row = bcur()->numrows;
	}
	if (end_row <= start_row) {
		*out_len = 0;
		return NULL;
	}

	for (r = start_row; r < end_row; r++) {
		if (!checked_add_int_size(&total, total, bcur()->row[r].size)) {
			*out_len = 0;
			return NULL;
		}
		if (r < end_row - 1) {
			if (!checked_add_int_size(&total, total, 1)) {
				*out_len = 0;
				return NULL;
			}
		}
	}

	int alloc_sz;
	if (!checked_add_int_size(&alloc_sz, total, 1)) {
		*out_len = 0;
		return NULL;
	}
	buf = malloc(alloc_sz);
	if (!buf) {
		*out_len = 0;
		return NULL;
	}
	p = buf;
	for (r = start_row; r < end_row; r++) {
		memcpy(p, bcur()->row[r].chars, bcur()->row[r].size);
		p += bcur()->row[r].size;
		if (r < end_row - 1) {
			*p++ = '\n';
		}
	}
	*p = '\0';
	*out_len = total;
	return buf;
}

/* Snapshot rows [s_row, e_row] and push a single UNDO_RECT_OVERWRITE
 * step anchored at the rect's left edge on the start row.  Returns the
 * anchor's byte column so callers can land the cursor on it.  Must run
 * before the rows are modified. */
static int rect_push_overwrite_undo(int s_row, int s_vcol, int e_row)
{
	int byte_lo = 0;
	int snap_len;
	char *snap = rect_snapshot_rows(s_row, e_row + 1, &snap_len);

	if (s_row < bcur()->numrows) {
		int hi_unused;
		rect_row_byte_range(
		    &bcur()->row[s_row], s_vcol, s_vcol, &byte_lo, &hi_unused);
	}
	undo_push(bcur(), UNDO_RECT_OVERWRITE, s_row, byte_lo, bcur()->numrows,
	    snap ? snap : (char *)"", snap_len);
	free(snap);
	return byte_lo;
}

/* Common tear-down after a rectangle command. */
static void rect_deactivate(void)
{
	editor.mark_set = 0;
	editor.mark_highlight = 0;
	editor.rect_mode = 0;
	editor.shift_select = 0;
	editor_snap_cx_to_row();
}

/* Kill or delete rectangle.  When save_to_ring is non-zero, the cut
 * rectangle is stored so editor_yank_rect can paste it.  Each row's
 * byte range is resolved from the visual column bounds independently,
 * so tabs and UTF-8 don't bend the cut into a non-rectangular shape. */
static void rect_kill_or_delete(int save_to_ring)
{
	int s_row, s_vcol, e_row, e_vcol;
	int s_row_byte_lo;
	int r;

	if (!rect_bounds(&s_row, &s_vcol, &e_row, &e_vcol)) {
		return;
	}
	if (s_row == e_row && s_vcol == e_vcol) {
		editor_set_status_message("Empty rectangle");
		return;
	}

	/* Build the rectangle text for the kill ring (each row's chars
	 * intersected with the per-row byte range, joined with '\n'). */
	if (save_to_ring) {
		int killed_total = 0;
		int killed_nrows = e_row - s_row + 1;
		char *killed_text;
		int killed_ok = 1;

		for (r = s_row; r <= e_row && r < bcur()->numrows; r++) {
			int lo, hi;
			rect_row_byte_range(
			    &bcur()->row[r], s_vcol, e_vcol, &lo, &hi);
			if (!checked_add_int_size(
				&killed_total, killed_total, hi - lo)) {
				killed_ok = 0;
				break;
			}
			if (r < e_row) {
				if (!checked_add_int_size(
					&killed_total, killed_total, 1)) {
					killed_ok = 0;
					break;
				}
			}
		}
		int killed_alloc;
		if (!killed_ok
		    || !checked_add_int_size(&killed_alloc, killed_total, 1)) {
			killed_text = NULL;
		} else {
			killed_text = malloc(killed_alloc);
		}
		if (killed_text) {
			char *p = killed_text;
			for (r = s_row; r <= e_row && r < bcur()->numrows;
			    r++) {
				erow *row = &bcur()->row[r];
				int lo, hi;
				rect_row_byte_range(
				    row, s_vcol, e_vcol, &lo, &hi);
				if (hi > lo) {
					memcpy(p, row->chars + lo, hi - lo);
					p += hi - lo;
				}
				if (r < e_row) {
					*p++ = '\n';
				}
			}
			*p = '\0';
			rect_kill_ring_set(
			    killed_text, killed_total, killed_nrows);
			free(killed_text);
		}
	}

	/* Cursor lands at the rect's left edge on the start row. */
	s_row_byte_lo = rect_push_overwrite_undo(s_row, s_vcol, e_row);

	/* One row rebuild per row, not one per byte: the whole rectangle is
	 * covered by the single UNDO_RECT_OVERWRITE pushed above, which is
	 * what KG_EDIT_NO_UNDO says here.  The global suppress_undo this
	 * loop used to toggle was doing nothing -- editor_row_del_char()
	 * records no undo of its own -- and would have started to once the
	 * loop moved onto a primitive that does. */
	for (r = s_row; r <= e_row && r < bcur()->numrows; r++) {
		int lo, hi;

		rect_row_byte_range(&bcur()->row[r], s_vcol, e_vcol, &lo, &hi);
		editor_row_replace_range(
		    r, lo, hi - lo, "", 0, KG_EDIT_NO_UNDO);
	}

	editor_cursor_goto(s_row, s_row_byte_lo);
	rect_deactivate();
	bcur()->dirty++;
	editor_set_status_message(
	    save_to_ring ? "Rectangle killed" : "Rectangle deleted");
}

void editor_kill_rect(void) { rect_kill_or_delete(1); }

void editor_delete_rect(void) { rect_kill_or_delete(0); }

/* Replace each row's chars in the visual [s_vcol, e_vcol) span with
 * spaces, padding short rows out so the rectangle "exists" everywhere
 * it should.  Each row resolves the visual range to its own byte range. */
void editor_clear_rect(void)
{
	int s_row, s_vcol, e_row, e_vcol;
	int s_row_byte_lo;
	int r;

	if (!rect_bounds(&s_row, &s_vcol, &e_row, &e_vcol)) {
		return;
	}
	if (s_row == e_row && s_vcol == e_vcol) {
		editor_set_status_message("Empty rectangle");
		return;
	}

	s_row_byte_lo = rect_push_overwrite_undo(s_row, s_vcol, e_row);

	suppress_undo = 1;
	for (r = s_row; r <= e_row && r < bcur()->numrows; r++) {
		erow *row = &bcur()->row[r];
		int lo, hi;

		/* Pad with spaces until row's visual width reaches s_vcol. */
		rect_row_pad_to_visual(row, s_vcol);

		rect_row_byte_range(row, s_vcol, e_vcol, &lo, &hi);
		rect_row_replace_with_spaces(row, lo, hi);

		/* Extend with spaces if the rect runs past row's visual end. */
		rect_row_pad_to_visual(row, e_vcol);
	}
	suppress_undo = 0;

	editor_cursor_goto(s_row, s_row_byte_lo);
	rect_deactivate();
	bcur()->dirty++;
	editor_set_status_message("Rectangle cleared");
}

/* Insert the last killed rectangle at point, padding short target rows
 * with spaces and appending new rows when the buffer is too short. */
void editor_yank_rect(void)
{
	int cur_row, cur_col;
	int orig_numrows;
	int rows_to_snap;
	char *snap;
	int snap_len;
	char *p, *end;
	int i;

	if (!rect_killed || rect_killed_nrows == 0) {
		editor_set_status_message("No rectangle to yank");
		return;
	}

	cur_row = editor.rowoff + editor.cy;
	cur_col = editor.coloff + editor.cx;
	orig_numrows = bcur()->numrows;

	rows_to_snap = rect_killed_nrows;
	if (cur_row + rows_to_snap > bcur()->numrows) {
		rows_to_snap = bcur()->numrows - cur_row;
	}
	if (rows_to_snap < 0) {
		rows_to_snap = 0;
	}
	snap = rect_snapshot_rows(cur_row, cur_row + rows_to_snap, &snap_len);

	undo_push(bcur(), UNDO_RECT_OVERWRITE, cur_row, cur_col, orig_numrows,
	    snap ? snap : (char *)"", snap_len);
	free(snap);

	suppress_undo = 1;
	p = rect_killed;
	end = rect_killed + rect_killed_len;
	i = 0;
	while (i < rect_killed_nrows) {
		char *nl = (p < end) ? memchr(p, '\n', end - p) : NULL;
		int line_len = nl ? (nl - p) : (end - p);
		int target = cur_row + i;
		erow *r;

		while (target >= bcur()->numrows) {
			editor_insert_row(bcur(), bcur()->numrows, "", 0);
		}
		r = &bcur()->row[target];
		if (r->size < cur_col) {
			editor_row_insert_spaces(r, r->size, cur_col - r->size);
		}
		if (line_len > 0) {
			editor_row_insert_string(r, cur_col, p, line_len);
		}

		if (!nl) {
			break;
		}
		p = nl + 1;
		i++;
	}
	suppress_undo = 0;

	rect_deactivate();
	bcur()->dirty++;
	editor_set_status_message("Rectangle yanked");
}

/* Emacs' string-rectangle prompt has a history of its own. */
static struct minibuf_history string_rectangle_history;

/* C-x r t: replace each row's chars in the visual [s_vcol, e_vcol) span
 * with a string read from the minibuffer.  A zero-width rectangle makes
 * this a per-row column insert.  Rows shorter than the left edge are
 * padded with spaces first, like editor_clear_rect. */
void editor_string_rect(int fd)
{
	int s_row, s_vcol, e_row, e_vcol;
	int s_row_byte_lo;
	char input[256];
	int input_len;
	int r;

	if (!rect_bounds(&s_row, &s_vcol, &e_row, &e_vcol)) {
		return;
	}
	input[0] = '\0';
	if (editor_read_line_with_history(fd, "String rectangle: ", input,
		sizeof(input), &string_rectangle_history)
	    < 0) {
		editor_set_status_message("");
		return;
	}
	input_len = strlen(input);

	s_row_byte_lo = rect_push_overwrite_undo(s_row, s_vcol, e_row);

	/* Delete and insert are one replacement, so a row is rebuilt once
	 * (twice when it had to be padded out to the rectangle's left
	 * edge first) rather than once per byte on either side. */
	for (r = s_row; r <= e_row && r < bcur()->numrows; r++) {
		int lo, hi;

		rect_row_pad_to_visual(&bcur()->row[r], s_vcol);
		rect_row_byte_range(&bcur()->row[r], s_vcol, e_vcol, &lo, &hi);
		editor_row_replace_range(
		    r, lo, hi - lo, input, input_len, KG_EDIT_NO_UNDO);
	}

	editor_cursor_goto(s_row, s_row_byte_lo);
	rect_deactivate();
	bcur()->dirty++;
	editor_set_status_message("Rectangle replaced");
}

/* C-x SPC: start a rectangular region at point, or cancel an active one. */
void editor_rect_mode_toggle(void)
{
	if (editor.rect_mode && editor.mark_highlight) {
		editor.rect_mode = 0;
		editor.mark_highlight = 0;
		editor_snap_cx_to_row();
		editor_set_status_message("");
		return;
	}
	editor_set_mark_silent();
	editor.rect_mode = 1;
	editor_set_status_message("Rectangle mark set");
}
