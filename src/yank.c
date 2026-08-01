/* yank.c - Kill ring (yank buffer) for copy/paste operations */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "edit.h"
#include "marker.h"

/* Global kill ring */
struct kill_ring killring = { NULL, 0 };

/* Initialize the kill ring */
void kill_ring_init(void)
{
	killring.text = NULL;
	killring.len = 0;
}

/* Free the kill ring */
void kill_ring_free(void)
{
	if (killring.text) {
		free(killring.text);
		killring.text = NULL;
		killring.len = 0;
	}
}

/* Set the kill ring to new text (replaces existing content) */
void kill_ring_set(char *text, int len)
{
	int alloc_sz;

	if (len <= 0) {
		return;
	}
	if (!checked_add_int_size(&alloc_sz, len, 1)) {
		return;
	}

	char *new_text = malloc(alloc_sz);
	if (!new_text) {
		return;
	}

	memcpy(new_text, text, len);
	new_text[len] = '\0';
	kill_ring_free();
	killring.text = new_text;
	killring.len = len;
}

/* Append text to the kill ring (for consecutive kills) */
void kill_ring_append(char *text, int len)
{
	char *new_text;
	int new_len;
	int alloc_sz;

	if (len <= 0) {
		return;
	}

	if (!killring.text) {
		kill_ring_set(text, len);
		return;
	}

	if (!checked_add_int_size(&new_len, killring.len, len)
	    || !checked_add_int_size(&alloc_sz, new_len, 1)) {
		return;
	}

	new_text = realloc(killring.text, alloc_sz);
	if (!new_text) {
		return;
	}

	memcpy(new_text + killring.len, text, len);
	new_text[new_len] = '\0';
	killring.text = new_text;
	killring.len = new_len;
}

/* Get the kill ring text (returns NULL if empty) */
char *kill_ring_get(void) { return killring.text; }

static int yank_current_row(void) { return editor_current_filerow_or_eof(); }

static int yank_current_col(void) { return editor_current_filecol(); }

static int region_point_before(int row, int col, int other_row, int other_col)
{
	if (row != other_row) {
		return row < other_row;
	}
	return col < other_col;
}

/* Normalized point/mark bounds, clamped to the buffer.  Returns 0 when no
 * mark is set or the region does not overlap any row. */
int editor_region_bounds(
    int *start_row, int *start_col, int *end_row, int *end_col)
{
	int cur_row = yank_current_row();
	int cur_col = yank_current_col();
	int mark_row, mark_col;

	if (!kg_mark_get_row_col(bcur(), &mark_row, &mark_col)
	    || bcur()->numrows <= 0) {
		return 0;
	}
	if (region_point_before(mark_row, mark_col, cur_row, cur_col)) {
		*start_row = mark_row;
		*start_col = mark_col;
		*end_row = cur_row;
		*end_col = cur_col;
	} else {
		*start_row = cur_row;
		*start_col = cur_col;
		*end_row = mark_row;
		*end_col = mark_col;
	}

	if (*end_row < 0 || *start_row >= bcur()->numrows) {
		return 0;
	}
	if (*start_row < 0) {
		*start_row = 0;
		*start_col = 0;
	}
	if (*end_row >= bcur()->numrows) {
		*end_row = bcur()->numrows - 1;
		*end_col = bcur()->row[*end_row].size;
	}
	if (*start_col < 0) {
		*start_col = 0;
	}
	if (*start_col > bcur()->row[*start_row].size) {
		*start_col = bcur()->row[*start_row].size;
	}
	if (*end_col < 0) {
		*end_col = 0;
	}
	if (*end_col > bcur()->row[*end_row].size) {
		*end_col = bcur()->row[*end_row].size;
	}
	return *start_row < *end_row
	    || (*start_row == *end_row && *start_col < *end_col);
}

/* Remember the current mark (if any) on the mark ring, newest first.
 * The oldest entry falls off when the ring is full. */
static int mark_ring_push_current(void)
{
	size_t pos;
	struct kg_marker_handle pushed;
	int move;

	if (!kg_mark_get_position(bcur(), &pos)) {
		return 1;
	}
	pushed = kg_marker_create(bcur(), pos, KG_MARKER_GRAV_LEFT);
	if (pushed.id == 0) {
		return 0;
	}
	if (bcur()->mark_ring_len == MARK_RING_MAX) {
		kg_marker_delete(bcur()->mark_ring[MARK_RING_MAX - 1].marker);
	} else {
		bcur()->mark_ring_len++;
	}
	move = bcur()->mark_ring_len - 1;
	memmove(&bcur()->mark_ring[1], &bcur()->mark_ring[0],
	    (size_t)move * sizeof(bcur()->mark_ring[0]));
	bcur()->mark_ring[0] = (struct kg_buffer_mark) {
		.marker = pushed,
		.virtual_chars_col = bcur()->mark.virtual_chars_col,
	};
	return 1;
}

/* Set mark at current cursor position without echoing to the minibuffer.
 * Used by shift-select and rectangle commands where a status message
 * would be noisy. */
void editor_set_mark_silent(void)
{
	if (!mark_ring_push_current()
	    || !kg_mark_set_row_col(
		bcur(), yank_current_row(), yank_current_col())) {
		editor_set_status_message("Out of memory");
		running = 0;
		return;
	}
	bcur()->mark_highlight = 1;
}

/* Set mark at point and remember the old one on the ring, without
 * activating the region highlight.  Motion commands that jump far
 * (M-<, M->) and yank use this, matching Emacs' push-mark. */
void editor_push_mark(void)
{
	if (!mark_ring_push_current()
	    || !kg_mark_set_row_col(
		bcur(), yank_current_row(), yank_current_col())) {
		editor_set_status_message("Out of memory");
		running = 0;
	}
}

/* C-u C-SPC: jump to mark, then rotate the mark ring so repeated pops
 * walk older and older marks (Emacs' pop-to-mark-command). */
void editor_pop_to_mark(void)
{
	int row, col;

	if (!kg_mark_get_row_col(bcur(), &row, &col)) {
		editor_set_status_message("No mark set");
		return;
	}

	editor_cursor_goto(row, col);
	editor_snap_cx_to_row();

	if (bcur()->mark_ring_len > 0) {
		int last = bcur()->mark_ring_len - 1;
		struct kg_buffer_mark old = bcur()->mark;

		bcur()->mark = bcur()->mark_ring[0];
		memmove(&bcur()->mark_ring[0], &bcur()->mark_ring[1],
		    (size_t)last * sizeof(bcur()->mark_ring[0]));
		bcur()->mark_ring[last] = old;
	}
	bcur()->mark_highlight = 0;
	editor_set_status_message("Mark popped");
}

/* Set mark at current cursor position (C-Space, explicit set-mark). */
void editor_set_mark(void)
{
	editor_set_mark_silent();
	editor_set_status_message("Mark set");
}

/* Swap cursor and mark positions (C-x C-x).
 * Scrolls only as needed to keep the target visible, rather than centering,
 * so the user's view context is preserved when the mark is on-screen. */
void editor_exchange_point_and_mark(void)
{
	int cur_row, cur_col, mark_row, mark_col;

	if (!kg_mark_get_row_col(bcur(), &mark_row, &mark_col)) {
		editor_set_status_message("No mark set");
		return;
	}

	cur_row = yank_current_row();
	cur_col = yank_current_col();

	editor_cursor_goto(mark_row, mark_col);

	if (!kg_mark_set_row_col(bcur(), cur_row, cur_col)) {
		editor_set_status_message("Out of memory");
		running = 0;
		return;
	}
	bcur()->mark_highlight = 1;
	editor_set_status_message("Mark exchanged");
}

/* Get text from region (between mark and point) */
char *editor_get_region_text(int *out_len)
{
	int start_row, start_col, end_row, end_col;
	int total_len = 0;
	char *text;
	int pos = 0;
	int row;

	if (!editor_region_bounds(&start_row, &start_col, &end_row, &end_col)) {
		return NULL;
	}

	/* Calculate total length needed */
	for (row = start_row; row <= end_row && row < bcur()->numrows; row++) {
		int copy_start = (row == start_row) ? start_col : 0;
		int copy_end
		    = (row == end_row) ? end_col : bcur()->row[row].size;

		if (copy_end > bcur()->row[row].size) {
			copy_end = bcur()->row[row].size;
		}
		if (copy_start > bcur()->row[row].size) {
			copy_start = bcur()->row[row].size;
		}
		if (copy_start < 0) {
			copy_start = 0;
		}
		if (copy_end < copy_start) {
			copy_end = copy_start;
		}

		if (copy_end - copy_start > INT_MAX - total_len) {
			return NULL;
		}
		total_len += copy_end - copy_start;
		if (row < end_row) {
			if (total_len == INT_MAX) {
				return NULL;
			}
			total_len++;
		}
	}

	if (total_len == 0) {
		return NULL;
	}

	/* Allocate and copy text */
	text = malloc((size_t)total_len + 1);
	if (!text) {
		return NULL;
	}

	for (row = start_row; row <= end_row && row < bcur()->numrows; row++) {
		int copy_start = (row == start_row) ? start_col : 0;
		int copy_end
		    = (row == end_row) ? end_col : bcur()->row[row].size;
		int copy_len;

		if (copy_end > bcur()->row[row].size) {
			copy_end = bcur()->row[row].size;
		}
		if (copy_start > bcur()->row[row].size) {
			copy_start = bcur()->row[row].size;
		}
		if (copy_start < 0) {
			copy_start = 0;
		}
		if (copy_end < copy_start) {
			copy_end = copy_start;
		}

		copy_len = copy_end - copy_start;
		if (copy_len > 0) {
			if (copy_len > total_len - pos) {
				free(text);
				return NULL;
			}
			memcpy(text + pos, bcur()->row[row].chars + copy_start,
			    copy_len);
			pos += copy_len;
		}

		/* Add newline except for last line */
		if (row < end_row) {
			if (pos >= total_len) {
				free(text);
				return NULL;
			}
			text[pos++] = '\n';
		}
	}

	if (pos > total_len) {
		free(text);
		return NULL;
	}
	text[pos] = '\0';
	*out_len = pos;
	return text;
}

static int editor_delete_region_range(
    int start_row, int start_col, int end_row, int end_col)
{
	/* Two row/column pairs name a byte range, and removing a byte range
	 * is the edit transaction with nothing to put back in its place --
	 * including the one record that covers the whole region, which the
	 * two callers used to push themselves around a suppressed edit.
	 *
	 * This used to be a second implementation of the multi-row splice:
	 * join the first row to the tail of the last, free the rows
	 * between, move the array down, renumber.  Two of those are one too
	 * many. */
	size_t begin = buffer_row_col_to_position(bcur(), start_row, start_col);
	struct kg_edit e = kg_edit_user(bcur(), begin,
	    buffer_row_col_to_position(bcur(), end_row, end_col), "", 0);

	return kg_buffer_replace(&e, NULL);
}

/* Remove `byte_len` bytes from (start_row, start_col) forward, recording
 * nothing: undo replay and overwrite-mode self-insert use it, and both
 * own the record that covers what they are doing.  Returns 0 without
 * touching the buffer when the span is not one the buffer has. */
int editor_delete_text_range_raw(int start_row, int start_col, int byte_len)
{
	size_t begin;
	struct kg_edit e;

	if (byte_len <= 0 || start_row < 0 || start_row >= bcur()->numrows
	    || start_col < 0 || start_col > bcur()->row[start_row].size) {
		return 0;
	}
	begin = buffer_row_col_to_position(bcur(), start_row, start_col);
	if ((size_t)byte_len > buffer_byte_length(bcur()) - begin) {
		return 0;
	}
	e = kg_edit_user_part(bcur(), begin, begin + (size_t)byte_len, "", 0);
	return kg_buffer_replace(&e, NULL);
}

/* Cut (save==1) or delete (save==0) the linear region.  Cursor lands at
 * the start of the region; undo restores it as a single step. */
static void region_kill_or_delete(int save)
{
	int start_row, start_col;
	int end_row, end_col;
	char *text;
	int len;

	if (!kg_mark_is_set(bcur())) {
		editor_set_status_message("No mark set");
		return;
	}
	if (!editor_region_bounds(&start_row, &start_col, &end_row, &end_col)) {
		editor_set_status_message("Empty region");
		return;
	}

	text = editor_get_region_text(&len);
	if (!text) {
		editor_set_status_message("Empty region");
		return;
	}

	if (save) {
		kill_ring_set(text, len);
	}

	editor_cursor_goto(start_row, start_col);
	editor_delete_region_range(start_row, start_col, end_row, end_col);

	/* Drop the highlight and any transient-region machinery, but keep
	 * the mark so C-x C-x after a region command can still bounce back
	 * to where the region started (matches Emacs, the C-g teardown,
	 * and the first-edit teardown in kbd.c). */
	bcur()->mark_highlight = 0;
	bcur()->rect_mode = 0;
	bcur()->shift_select = 0;
	free(text);
	editor_set_status_message(save ? "Region killed" : "Region deleted");
}

void editor_kill_region(void) { region_kill_or_delete(1); }
void editor_delete_region(void) { region_kill_or_delete(0); }

/* Copy region - saves to kill ring without removing */
void editor_copy_region(void)
{
	char *text;
	int len;

	if (!kg_mark_is_set(bcur())) {
		editor_set_status_message("No mark set");
		return;
	}

	text = editor_get_region_text(&len);
	if (!text) {
		editor_set_status_message("Empty region");
		return;
	}

	kill_ring_set(text, len);
	bcur()->mark_highlight = 0;
	bcur()->rect_mode = 0;
	bcur()->shift_select = 0;
	editor_snap_cx_to_row();
	free(text);
	editor_set_status_message("Region copied");
}

/* Delete key dispatch: consume an active region (rect or linear) without
 * saving, otherwise just delete the character ahead. */
void editor_delete_region_or_char(void)
{
	if (kg_mark_is_set(bcur()) && bcur()->mark_highlight) {
		if (bcur()->rect_mode) {
			editor_delete_rect();
		} else {
			editor_delete_region();
		}
		return;
	}
	editor_del_forward_char();
}

/* Yank (paste) from kill ring.  One insertion of the ring's whole
 * contents, so one row rebuild per row it brings and one C-_ that takes
 * all of it back out again. */
void editor_yank(void)
{
	char *text = kill_ring_get();

	if (!text) {
		editor_set_status_message("Kill ring is empty");
		return;
	}
	/* Mark the start of the yanked text, as Emacs does. */
	editor_push_mark();
	editor_insert_text_at_point(text, killring.len);
	editor_set_status_message("Yanked");
}

static int sort_lines_cmp(const void *a, const void *b)
{
	const erow *ra = (const erow *)a;
	const erow *rb = (const erow *)b;

	return strcmp(ra->chars, rb->chars);
}

void editor_sort_lines(void)
{
	int start_row, start_col, end_row, end_col;
	int nlines, sorted_len, i;
	char *sorted, *p;
	struct kg_edit e;
	erow *temp;

	if (!kg_mark_is_set(bcur())) {
		editor_set_status_message("No mark set");
		return;
	}
	if (!editor_region_bounds(&start_row, &start_col, &end_row, &end_col)) {
		editor_set_status_message("Empty region");
		return;
	}

	if (end_col == 0 && end_row > start_row) {
		end_row--;
	}

	nlines = end_row - start_row + 1;
	if (nlines < 2) {
		return;
	}

	/* Sort a copy of the row records, then write the sorted text back as
	 * one replacement of the span they cover: the transaction copies the
	 * original bytes for undo, so nothing has to snapshot the region
	 * first, and the rows are never half-sorted. */
	temp = malloc((size_t)nlines * sizeof(erow));
	if (!temp) {
		editor_set_status_message("Out of memory");
		return;
	}
	memcpy(temp, &bcur()->row[start_row], (size_t)nlines * sizeof(erow));
	qsort(temp, (size_t)nlines, sizeof(erow), sort_lines_cmp);

	sorted_len = 0;
	for (i = 0; i < nlines; i++) {
		sorted_len += temp[i].size + (i > 0);
	}
	sorted = malloc((size_t)sorted_len + 1);
	if (!sorted) {
		free(temp);
		editor_set_status_message("Out of memory");
		return;
	}
	p = sorted;
	for (i = 0; i < nlines; i++) {
		if (i > 0) {
			*p++ = '\n';
		}
		memcpy(p, temp[i].chars, (size_t)temp[i].size);
		p += temp[i].size;
	}
	*p = '\0';
	free(temp);

	e = kg_edit_user(bcur(),
	    buffer_row_col_to_position(bcur(), start_row, 0),
	    buffer_row_col_to_position(
		bcur(), end_row, bcur()->row[end_row].size),
	    sorted, (size_t)sorted_len);
	kg_buffer_replace(&e, NULL);
	free(sorted);

	bcur()->mark_highlight = 0;
	bcur()->rect_mode = 0;
	bcur()->shift_select = 0;
	editor_snap_cx_to_row();
	editor_set_status_message("Sorted %d lines", nlines);
}
