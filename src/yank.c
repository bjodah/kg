/* yank.c - Kill ring (yank buffer) for copy/paste operations */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "localvars.h"

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

	if (!bcur()->mark_set || bcur()->numrows <= 0) {
		return 0;
	}
	if (region_point_before(
		bcur()->mark_row, bcur()->mark_col, cur_row, cur_col)) {
		*start_row = bcur()->mark_row;
		*start_col = bcur()->mark_col;
		*end_row = cur_row;
		*end_col = cur_col;
	} else {
		*start_row = cur_row;
		*start_col = cur_col;
		*end_row = bcur()->mark_row;
		*end_col = bcur()->mark_col;
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
static void mark_ring_push_current(void)
{
	if (!bcur()->mark_set) {
		return;
	}
	memmove(&bcur()->mark_ring_row[1], &bcur()->mark_ring_row[0],
	    (MARK_RING_MAX - 1) * sizeof(bcur()->mark_ring_row[0]));
	memmove(&bcur()->mark_ring_col[1], &bcur()->mark_ring_col[0],
	    (MARK_RING_MAX - 1) * sizeof(bcur()->mark_ring_col[0]));
	bcur()->mark_ring_row[0] = bcur()->mark_row;
	bcur()->mark_ring_col[0] = bcur()->mark_col;
	if (bcur()->mark_ring_len < MARK_RING_MAX) {
		bcur()->mark_ring_len++;
	}
}

/* Set mark at current cursor position without echoing to the minibuffer.
 * Used by shift-select and rectangle commands where a status message
 * would be noisy. */
void editor_set_mark_silent(void)
{
	mark_ring_push_current();
	bcur()->mark_set = 1;
	bcur()->mark_row = yank_current_row();
	bcur()->mark_col = yank_current_col();
	bcur()->mark_highlight = 1;
}

/* Set mark at point and remember the old one on the ring, without
 * activating the region highlight.  Motion commands that jump far
 * (M-<, M->) and yank use this, matching Emacs' push-mark. */
void editor_push_mark(void)
{
	mark_ring_push_current();
	bcur()->mark_set = 1;
	bcur()->mark_row = yank_current_row();
	bcur()->mark_col = yank_current_col();
}

/* C-u C-SPC: jump to mark, then rotate the mark ring so repeated pops
 * walk older and older marks (Emacs' pop-to-mark-command).  Positions
 * are clamped since edits after the push may have shrunk the buffer. */
void editor_pop_to_mark(void)
{
	int row, col;

	if (!bcur()->mark_set) {
		editor_set_status_message("No mark set");
		return;
	}

	/* Edits since the push may have shrunk the buffer; clamp the row
	 * here and let editor_snap_cx_to_row settle an overlong column. */
	row = bcur()->mark_row;
	col = bcur()->mark_col;
	if (row >= bcur()->numrows) {
		row = bcur()->numrows > 0 ? bcur()->numrows - 1 : 0;
		col = 0;
	}
	editor_cursor_goto(row, col);
	editor_snap_cx_to_row();

	if (bcur()->mark_ring_len > 0) {
		int last = bcur()->mark_ring_len - 1;
		int old_row = bcur()->mark_row;
		int old_col = bcur()->mark_col;

		bcur()->mark_row = bcur()->mark_ring_row[0];
		bcur()->mark_col = bcur()->mark_ring_col[0];
		memmove(&bcur()->mark_ring_row[0], &bcur()->mark_ring_row[1],
		    last * sizeof(bcur()->mark_ring_row[0]));
		memmove(&bcur()->mark_ring_col[0], &bcur()->mark_ring_col[1],
		    last * sizeof(bcur()->mark_ring_col[0]));
		bcur()->mark_ring_row[last] = old_row;
		bcur()->mark_ring_col[last] = old_col;
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

	if (!bcur()->mark_set) {
		editor_set_status_message("No mark set");
		return;
	}

	cur_row = yank_current_row();
	cur_col = yank_current_col();
	mark_row = bcur()->mark_row;
	mark_col = bcur()->mark_col;

	editor_cursor_goto(mark_row, mark_col);

	bcur()->mark_row = cur_row;
	bcur()->mark_col = cur_col;
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
	erow *first;
	erow *last;
	char *newchars;
	int suffix_len;
	int new_size;
	int row;
	int removed;

	if (start_row == end_row) {
		erow *r = &bcur()->row[start_row];
		memmove(r->chars + start_col, r->chars + end_col,
		    r->size - end_col + 1);
		r->size -= end_col - start_col;
		editor_update_row(bcur(), r);
		bcur()->dirty++;
		return 1;
	}

	first = &bcur()->row[start_row];
	last = &bcur()->row[end_row];
	suffix_len = last->size - end_col;
	if (!checked_add_int_size(
		&new_size, start_col, (size_t)suffix_len + 1)) {
		return 0;
	}
	new_size--;
	newchars = realloc(first->chars, (size_t)new_size + 1);
	if (!newchars) {
		return 0;
	}
	first->chars = newchars;
	memcpy(first->chars + start_col, last->chars + end_col, suffix_len);
	first->chars[new_size] = '\0';
	first->size = new_size;
	editor_update_row(bcur(), first);

	for (row = start_row + 1; row <= end_row; row++) {
		editor_free_row(&bcur()->row[row]);
	}
	removed = end_row - start_row;
	memmove(bcur()->row + start_row + 1, bcur()->row + end_row + 1,
	    (size_t)(bcur()->numrows - end_row - 1) * sizeof(*bcur()->row));
	bcur()->numrows -= removed;
	for (row = start_row + 1; row < bcur()->numrows; row++) {
		bcur()->row[row].idx = row;
	}
	bcur()->dirty++;
	return 1;
}

int editor_delete_text_range_raw(int start_row, int start_col, int byte_len)
{
	if (byte_len <= 0) {
		return 0;
	}
	if (start_row < 0 || start_row >= bcur()->numrows) {
		return 0;
	}
	if (start_col < 0 || start_col > bcur()->row[start_row].size) {
		return 0;
	}

	int row = start_row;
	int col = start_col;
	int rem = byte_len;

	while (rem > 0) {
		int row_len = bcur()->row[row].size;
		if (rem <= row_len - col) {
			col += rem;
			rem = 0;
		} else {
			if (row + 1 >= bcur()->numrows) {
				return 0;
			}
			rem -= (row_len - col + 1);
			row++;
			col = 0;
		}
	}

	int end_row = row;
	int end_col = col;

	if (start_row == end_row && start_col == end_col) {
		return 0;
	}

	int saved_suppress = suppress_undo;
	suppress_undo = 1;
	int deleted = editor_delete_region_range(
	    start_row, start_col, end_row, end_col);
	suppress_undo = saved_suppress;

	return deleted;
}

/* Cut (save==1) or delete (save==0) the linear region.  Cursor lands at
 * the start of the region; undo restores it as a single step. */
static void region_kill_or_delete(int save)
{
	int start_row, start_col;
	int end_row, end_col;
	char *text;
	int len;

	if (!bcur()->mark_set) {
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

	undo_push(bcur(), UNDO_KILL_TEXT, start_row, start_col, 0, text, len);

	suppress_undo = 1;
	editor_delete_region_range(start_row, start_col, end_row, end_col);
	suppress_undo = 0;

	/* Drop the highlight and any transient-region machinery, but keep
	 * mark_set so C-x C-x after a region command can still bounce back
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

	if (!bcur()->mark_set) {
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
	if (bcur()->mark_set && bcur()->mark_highlight) {
		if (bcur()->rect_mode) {
			editor_delete_rect();
		} else {
			editor_delete_region();
		}
		return;
	}
	editor_del_forward_char();
}

/* Yank (paste) from kill ring */
void editor_yank(void)
{
	int filerow = editor_current_filerow_or_eof();
	int filecol = editor_current_filecol();
	char *text = kill_ring_get();

	if (!text) {
		editor_set_status_message("Kill ring is empty");
		return;
	}

	/* Mark the start of the yanked text, as Emacs does. */
	editor_push_mark();

	/* Record single undo operation for entire yank */
	undo_push(
	    bcur(), UNDO_YANK_TEXT, filerow, filecol, 0, text, killring.len);

	editor_insert_text_raw(text, killring.len);

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
	int nlines, orig_len, i;
	char *orig_text;
	erow *temp;

	if (!bcur()->mark_set) {
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

	orig_text
	    = editor_rows_to_string(&bcur()->row[start_row], nlines, &orig_len);
	if (!orig_text) {
		return;
	}

	temp = malloc(nlines * sizeof(erow));
	if (!temp) {
		free(orig_text);
		editor_set_status_message("Out of memory");
		return;
	}
	memcpy(temp, &bcur()->row[start_row], nlines * sizeof(erow));

	qsort(temp, nlines, sizeof(erow), sort_lines_cmp);

	for (i = 0; i < nlines; i++) {
		bcur()->row[start_row + i] = temp[i];
		bcur()->row[start_row + i].idx = start_row + i;
	}

	free(temp);

	undo_push(bcur(), UNDO_REPLACE_TEXT, start_row, 0, orig_len, orig_text,
	    orig_len);
	free(orig_text);

	bcur()->mark_highlight = 0;
	bcur()->rect_mode = 0;
	bcur()->shift_select = 0;
	editor_snap_cx_to_row();
	bcur()->dirty = 1;
	editor_set_status_message("Sorted %d lines", nlines);
}
