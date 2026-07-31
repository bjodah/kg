/* rect.c - Rectangle operations (C-x SPC, C-x r {k,y,d,c,t}) */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "edit.h"
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

	if (!bcur()->mark_set) {
		editor_set_status_message("No mark set");
		return 0;
	}
	p_row = wcur()->rowoff + wcur()->cy;
	p_col = wcur()->coloff + wcur()->cx;
	m_row = bcur()->mark_row;
	m_col = bcur()->mark_col;
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

/* A block of rows being built off to the side.
 *
 * Every rectangle command rewrites a contiguous run of rows, so every
 * one of them builds the whole run here and hands it to the transaction
 * once.  Nothing the buffer holds moves until the block is complete, so
 * there is no half-applied rectangle to see, no per-row partial failure
 * to unwind, and no coarse rectangle undo record describing rows that
 * may since have moved. */
struct rect_block {
	char *b;
	int len;
	int cap;
	int oom;
};

static void rect_block_free(struct rect_block *t)
{
	free(t->b);
	*t = (struct rect_block) { 0 };
}

/* Append `n` bytes of `s`, growing by doubling.  A failed grow is
 * remembered rather than reported here: the callers append in loops, and
 * one check before publication is the whole of what they must do. */
static void rect_append(struct rect_block *t, const char *s, int n)
{
	int need;

	if (t->oom || n <= 0) {
		return;
	}
	if (!checked_add_int_size(&need, t->len, (size_t)n + 1)) {
		t->oom = 1;
		return;
	}
	/* The `!t->b` half is redundant -- a nonzero capacity always has an
	 * allocation beside it -- but saying so is what keeps the memcpy
	 * below out of gcc -fanalyzer's set of reachable NULL writes. */
	if (!t->b || need > t->cap) {
		int cap = t->cap > 0 ? t->cap : 64;
		char *grown;

		while (cap < need) {
			if (cap > INT_MAX / 2) {
				t->oom = 1;
				return;
			}
			cap *= 2;
		}
		grown = realloc(t->b, (size_t)cap);
		if (!grown) {
			t->oom = 1;
			return;
		}
		t->b = grown;
		t->cap = cap;
	}
	memcpy(t->b + t->len, s, (size_t)n);
	t->len += n;
	t->b[t->len] = '\0';
}

static void rect_append_spaces(struct rect_block *t, int n)
{
	static const char spaces[] = "                ";
	const int chunk = (int)sizeof(spaces) - 1;

	while (n > 0 && !t->oom) {
		int take = n < chunk ? n : chunk;

		rect_append(t, spaces, take);
		n -= take;
	}
}

/* How many spaces `row` needs to reach visual column `target_vcol`. */
static int rect_pad_to_visual(erow *row, int target_vcol)
{
	int end_vcol = editor_visual_col(row, row->size);

	return end_vcol < target_vcol ? target_vcol - end_vcol : 0;
}

/* Build row `r` with its [lo, hi) byte span replaced by `pad` spaces
 * followed by `ins`, and append the result to `t` behind a separator
 * when `need_sep` says a row has already been appended.  `pad` is what
 * the row needs to reach the rectangle's left edge, and is only ever
 * nonzero when the row stops short of it -- in which case lo and hi are
 * both its end.  Returns where in `t` the built row starts, for a caller
 * that has to measure it. */
static int rect_append_row(struct rect_block *t, int r, int need_sep, int lo,
    int hi, int pad, const char *ins, int ins_len)
{
	erow *row = &bcur()->row[r];
	int start;

	if (need_sep) {
		rect_append(t, "\n", 1);
	}
	start = t->len;
	rect_append(t, row->chars, lo);
	rect_append_spaces(t, pad);
	rect_append(t, ins, ins_len);
	rect_append(t, row->chars + hi, row->size - hi);
	return start;
}

/* Put `t` in place of rows [first, last] as one edit.  A `last` below
 * `first` means the buffer has no such rows yet and the whole block is
 * new.  Returns 0 having changed nothing when the block could not be
 * built or the transaction refused it. */
static int rect_publish(struct rect_block *t, int first, int last)
{
	size_t begin;
	struct kg_edit e;

	if (t->oom || first < 0 || last >= bcur()->numrows) {
		return 0;
	}
	begin = buffer_row_col_to_position(bcur(), first, 0);
	e = kg_edit_user(bcur(), begin,
	    last < first ? begin
			 : buffer_row_col_to_position(
			       bcur(), last, bcur()->row[last].size),
	    t->b ? t->b : "", (size_t)t->len);
	return kg_buffer_replace(&e, NULL);
}

/* The byte column the rectangle's left edge sits at on its first row,
 * which is where the cursor lands afterwards.  Measured before the edit. */
static int rect_anchor_col(int s_row, int s_vcol)
{
	int byte_lo = 0, hi_unused;

	if (s_row < bcur()->numrows) {
		rect_row_byte_range(
		    &bcur()->row[s_row], s_vcol, s_vcol, &byte_lo, &hi_unused);
	}
	return byte_lo;
}

/* Common tear-down after a rectangle command. */
static void rect_deactivate(void)
{
	bcur()->mark_set = 0;
	bcur()->mark_highlight = 0;
	bcur()->rect_mode = 0;
	bcur()->shift_select = 0;
	editor_snap_cx_to_row();
}

/* Kill or delete rectangle.  When save_to_ring is non-zero, the cut
 * rectangle is stored so editor_yank_rect can paste it.  Each row's
 * byte range is resolved from the visual column bounds independently,
 * so tabs and UTF-8 don't bend the cut into a non-rectangular shape. */
static void rect_kill_or_delete(int save_to_ring)
{
	struct rect_block block = { 0 };
	int s_row, s_vcol, e_row, e_vcol;
	int s_row_byte_lo, last;
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
	s_row_byte_lo = rect_anchor_col(s_row, s_vcol);

	/* The whole affected block, built off to the side and published
	 * once: one row rebuild per row, one undo step, and no row left
	 * cut when a later one cannot be. */
	last = e_row < bcur()->numrows ? e_row : bcur()->numrows - 1;
	for (r = s_row; r <= last; r++) {
		int lo, hi;

		rect_row_byte_range(&bcur()->row[r], s_vcol, e_vcol, &lo, &hi);
		rect_append_row(&block, r, r != s_row, lo, hi, 0, "", 0);
	}
	if (!rect_publish(&block, s_row, last)) {
		rect_block_free(&block);
		return;
	}
	rect_block_free(&block);

	editor_cursor_goto(s_row, s_row_byte_lo);
	rect_deactivate();
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
	struct rect_block block = { 0 };
	int s_row, s_vcol, e_row, e_vcol;
	int s_row_byte_lo, last;
	int r;

	if (!rect_bounds(&s_row, &s_vcol, &e_row, &e_vcol)) {
		return;
	}
	if (s_row == e_row && s_vcol == e_vcol) {
		editor_set_status_message("Empty rectangle");
		return;
	}

	s_row_byte_lo = rect_anchor_col(s_row, s_vcol);

	last = e_row < bcur()->numrows ? e_row : bcur()->numrows - 1;
	for (r = s_row; r <= last; r++) {
		erow *row = &bcur()->row[r];
		int pad = rect_pad_to_visual(row, s_vcol);
		int lo, hi, start;

		/* A row that stops short of the left edge is padded out to
		 * it, and then has nothing of its own inside the rectangle. */
		if (pad > 0) {
			lo = hi = row->size;
		} else {
			rect_row_byte_range(row, s_vcol, e_vcol, &lo, &hi);
		}
		start = rect_append_row(
		    &block, r, r != s_row, lo, hi, pad + (hi - lo), "", 0);
		/* And is extended again when the rectangle runs past what
		 * the row is now worth on screen. */
		if (!block.oom) {
			erow built = { .chars = block.b + start,
				.size = block.len - start };

			rect_append_spaces(
			    &block, rect_pad_to_visual(&built, e_vcol));
		}
	}
	if (!rect_publish(&block, s_row, last)) {
		rect_block_free(&block);
		return;
	}
	rect_block_free(&block);

	editor_cursor_goto(s_row, s_row_byte_lo);
	rect_deactivate();
	editor_set_status_message("Rectangle cleared");
}

/* Insert the last killed rectangle at point, padding short target rows
 * with spaces and appending new rows when the buffer is too short. */
void editor_yank_rect(void)
{
	struct rect_block block = { 0 };
	int cur_row, cur_col, last;
	char *p, *end;
	int i;

	if (!rect_killed || rect_killed_nrows == 0) {
		editor_set_status_message("No rectangle to yank");
		return;
	}

	cur_row = editor_current_filerow();
	cur_col = editor_current_filecol();

	/* The block covers every row the rectangle lands on.  Rows the
	 * buffer does not have yet are not appended first: they are simply
	 * part of the text that replaces the rows it does have, so the
	 * whole paste is still one edit and one undo step. */
	last = cur_row + rect_killed_nrows - 1;
	if (last >= bcur()->numrows) {
		last = bcur()->numrows - 1;
	}
	p = rect_killed;
	end = rect_killed + rect_killed_len;
	for (i = 0; i < rect_killed_nrows; i++) {
		char *nl = (p < end) ? memchr(p, '\n', end - p) : NULL;
		int line_len = nl ? (int)(nl - p) : (int)(end - p);
		int target = cur_row + i;

		if (target <= last) {
			erow *row = &bcur()->row[target];
			int at = cur_col < row->size ? cur_col : row->size;

			rect_append_row(&block, target, i > 0, at, at,
			    cur_col > row->size ? cur_col - row->size : 0, p,
			    line_len);
		} else {
			/* Past the end of the buffer: a whole new row, its
			 * left edge made of spaces. */
			if (i > 0) {
				rect_append(&block, "\n", 1);
			}
			rect_append_spaces(&block, cur_col);
			rect_append(&block, p, line_len);
		}
		if (!nl) {
			break;
		}
		p = nl + 1;
	}
	if (rect_publish(&block, cur_row, last)) {
		rect_deactivate();
		editor_set_status_message("Rectangle yanked");
	}
	rect_block_free(&block);
}

/* Emacs' string-rectangle prompt has a history of its own. */
static struct minibuf_history string_rectangle_history;

/* C-x r t: replace each row's chars in the visual [s_vcol, e_vcol) span
 * with a string read from the minibuffer.  A zero-width rectangle makes
 * this a per-row column insert.  Rows shorter than the left edge are
 * padded with spaces first, like editor_clear_rect. */
void editor_string_rect(int fd)
{
	struct rect_block block = { 0 };
	int s_row, s_vcol, e_row, e_vcol;
	int s_row_byte_lo, last;
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
	/* A replacement carrying a row separator has no rectangle meaning:
	 * it would split the very rows this loop walks, and the one coarse
	 * record pushed below describes a row block that would no longer
	 * exist -- so not even undo could put the buffer back.  Refuse it
	 * before the record and before a short row is padded out to the
	 * left edge, so text, point, mark, undo, dirty and the content
	 * generation are all still what they were.  Teaching a rectangle
	 * replacement to change the row count is a different feature, and
	 * needs a different iteration and a different undo. */
	if (memchr(input, '\n', (size_t)input_len)) {
		editor_set_status_message(
		    "Rectangle string cannot contain a newline");
		return;
	}

	s_row_byte_lo = rect_anchor_col(s_row, s_vcol);

	/* Every affected row, with its span replaced and any padding it
	 * needed to reach the left edge, built and published once. */
	last = e_row < bcur()->numrows ? e_row : bcur()->numrows - 1;
	for (r = s_row; r <= last; r++) {
		erow *row = &bcur()->row[r];
		int pad = rect_pad_to_visual(row, s_vcol);
		int lo, hi;

		if (pad > 0) {
			lo = hi = row->size;
		} else {
			rect_row_byte_range(row, s_vcol, e_vcol, &lo, &hi);
		}
		rect_append_row(
		    &block, r, r != s_row, lo, hi, pad, input, input_len);
	}
	if (!rect_publish(&block, s_row, last)) {
		rect_block_free(&block);
		return;
	}
	rect_block_free(&block);

	editor_cursor_goto(s_row, s_row_byte_lo);
	rect_deactivate();
	editor_set_status_message("Rectangle replaced");
}

/* C-x SPC: start a rectangular region at point, or cancel an active one. */
void editor_rect_mode_toggle(void)
{
	if (bcur()->rect_mode && bcur()->mark_highlight) {
		bcur()->rect_mode = 0;
		bcur()->mark_highlight = 0;
		editor_snap_cx_to_row();
		editor_set_status_message("");
		return;
	}
	editor_set_mark_silent();
	bcur()->rect_mode = 1;
	editor_set_status_message("Rectangle mark set");
}
