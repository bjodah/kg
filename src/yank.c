/* yank.c - Kill ring (yank buffer) for copy/paste operations */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cmdstate.h"
#include "def.h"
#include "edit.h"
#include "marker.h"
#include "yank.h"

/* Global kill ring */
struct kill_ring killring;

/* How many more allocations this module's own entry storage may make
 * before one of them is failed on purpose.  Negative is how the editor
 * always runs: no seam.  See kg_yank_fail_alloc_after() in yank.h;
 * mirrors edit_alloc_budget in buffer.c and decor_alloc_budget in
 * decor.c, one seam per allocator. */
static int yank_alloc_budget = -1;

void kg_yank_fail_alloc_after(int n) { yank_alloc_budget = n; }

static bool yank_alloc_ok(void)
{
	return yank_alloc_budget < 0 || yank_alloc_budget-- != 0;
}

/* Initialize the kill ring */
void kill_ring_init(void) { memset(&killring, 0, sizeof(killring)); }

/* Free every entry.  Explicitly zeroing each freed slot (rather than
 * just resetting `count`) keeps the invariant kill_ring_get() relies on:
 * every slot at or past `count` is always {NULL, 0}. */
void kill_ring_free(void)
{
	int i;

	for (i = 0; i < killring.count; i++) {
		free(killring.entries[i].text);
		killring.entries[i].text = NULL;
		killring.entries[i].len = 0;
	}
	killring.count = 0;
	killring.total_bytes = 0;
	killring.generation++;
}

/* Drop the single oldest (last) entry. */
static void kill_ring_evict_oldest(void)
{
	int last = killring.count - 1;

	free(killring.entries[last].text);
	killring.total_bytes -= killring.entries[last].len;
	killring.entries[last].text = NULL;
	killring.entries[last].len = 0;
	killring.count--;
}

/* Evict oldest entries until adding one more of `incoming_len` bytes
 * fits both caps.  The caller has already rejected an `incoming_len`
 * that could never fit alone, so this always terminates with room:
 * worst case it empties the ring (total_bytes reaches 0). */
static void kill_ring_make_room(size_t incoming_len)
{
	while (killring.count > 0
	    && (killring.count == KG_KILL_RING_MAX_ENTRIES
		|| killring.total_bytes + incoming_len
		    > KG_KILL_RING_MAX_BYTES)) {
		kill_ring_evict_oldest();
	}
}

/* Publish `copy` (already allocated, already within the per-entry cap)
 * as the new newest entry, evicting whatever must go first. */
static void kill_ring_push_entry(char *copy, size_t len)
{
	kill_ring_make_room(len);
	memmove(&killring.entries[1], &killring.entries[0],
	    (size_t)killring.count * sizeof(killring.entries[0]));
	killring.entries[0].text = copy;
	killring.entries[0].len = len;
	killring.count++;
	killring.total_bytes += len;
	killring.generation++;
}

/* One allocation: `len` bytes of `text` plus a defensive trailing NUL. */
static char *yank_dup(const char *text, size_t len)
{
	size_t alloc_sz;
	char *copy;

	if (!checked_add_size_t(&alloc_sz, len, 1)) {
		return NULL;
	}
	copy = yank_alloc_ok() ? malloc(alloc_sz) : NULL;
	if (!copy) {
		return NULL;
	}
	memcpy(copy, text, len);
	copy[len] = '\0';
	return copy;
}

/* Start a new newest entry (replaces nothing; see yank.h). */
void kill_ring_set(const char *text, size_t len)
{
	char *copy;

	if (len == 0) {
		return;
	}
	if (len > KG_KILL_RING_MAX_BYTES) {
		return;
	}
	copy = yank_dup(text, len);
	if (!copy) {
		return;
	}
	kill_ring_push_entry(copy, len);
}

/* One allocation: `a` (`alen` bytes) followed by `b` (`blen` bytes) plus
 * a defensive trailing NUL, `total` == alen + blen. */
static char *kill_ring_concat(
    const char *a, size_t alen, const char *b, size_t blen, size_t total)
{
	size_t alloc_sz;
	char *combined;

	if (!checked_add_size_t(&alloc_sz, total, 1)) {
		return NULL;
	}
	combined = yank_alloc_ok() ? malloc(alloc_sz) : NULL;
	if (!combined) {
		return NULL;
	}
	memcpy(combined, a, alen);
	memcpy(combined + alen, b, blen);
	combined[total] = '\0';
	return combined;
}

/* Replace the newest entry's bytes with `combined`/`new_len`, then shed
 * older entries if growing it pushed the ring over the byte cap.
 * `combined` is never itself oversize (the caller already checked), so
 * shedding every other entry is guaranteed to make it fit. */
static void kill_ring_replace_newest(char *combined, size_t new_len)
{
	free(killring.entries[0].text);
	killring.total_bytes
	    = killring.total_bytes - killring.entries[0].len + new_len;
	killring.entries[0].text = combined;
	killring.entries[0].len = new_len;
	killring.generation++;
	while (killring.count > 1
	    && killring.total_bytes > KG_KILL_RING_MAX_BYTES) {
		kill_ring_evict_oldest();
	}
}

/* Combine `lead` (`lead_len` bytes) and `trail` (`trail_len` bytes), in
 * that order, into the newest entry.  Shared by kill_ring_append()
 * (lead is the existing entry, trail is the new bytes) and
 * kill_ring_prepend() (the other way around) -- both already know the
 * ring is non-empty, so unlike kill_ring_set() this never has to touch
 * `count`. */
static void kill_ring_combine_newest(
    const char *lead, size_t lead_len, const char *trail, size_t trail_len)
{
	size_t new_len;
	char *combined;

	if (!checked_add_size_t(&new_len, lead_len, trail_len)) {
		return;
	}
	if (new_len > KG_KILL_RING_MAX_BYTES) {
		return;
	}
	combined = kill_ring_concat(lead, lead_len, trail, trail_len, new_len);
	if (!combined) {
		return;
	}
	kill_ring_replace_newest(combined, new_len);
}

/* Append text to the newest entry (for consecutive forward kills) */
void kill_ring_append(const char *text, size_t len)
{
	if (len == 0) {
		return;
	}
	if (killring.count == 0) {
		kill_ring_set(text, len);
		return;
	}
	kill_ring_combine_newest(
	    killring.entries[0].text, killring.entries[0].len, text, len);
}

/* Prepend text to the newest entry (for consecutive backward kills) */
void kill_ring_prepend(const char *text, size_t len)
{
	if (len == 0) {
		return;
	}
	if (killring.count == 0) {
		kill_ring_set(text, len);
		return;
	}
	kill_ring_combine_newest(
	    text, len, killring.entries[0].text, killring.entries[0].len);
}

/* Forward-kill/backward-kill/copy: the coalescing-class-aware entry
 * points every kill and copy producer uses instead of
 * kill_ring_set()/kill_ring_append()/kill_ring_prepend() directly, so
 * "who may grow the newest entry" lives in one place.  See yank.h. */
void kill_ring_kill_forward(const char *text, size_t len)
{
	if (cmd_last_kill_class() == KILL_COALESCE_KILL) {
		kill_ring_append(text, len);
	} else {
		kill_ring_set(text, len);
	}
	cmd_set_kill_class(KILL_COALESCE_KILL);
}

void kill_ring_kill_backward(const char *text, size_t len)
{
	if (cmd_last_kill_class() == KILL_COALESCE_KILL) {
		kill_ring_prepend(text, len);
	} else {
		kill_ring_set(text, len);
	}
	cmd_set_kill_class(KILL_COALESCE_KILL);
}

void kill_ring_copy(const char *text, size_t len)
{
	kill_ring_set(text, len);
	cmd_set_kill_class(KILL_COALESCE_COPY);
}

/* Get the newest entry's bytes (returns NULL if the ring is empty) */
char *kill_ring_get(void) { return killring.entries[0].text; }

/* Get the newest entry's length (returns 0 if the ring is empty) */
size_t kill_ring_get_len(void) { return killring.entries[0].len; }

/* `n` copies of entries[index]'s bytes in one allocation; see yank.h. */
char *kill_ring_entry_repeated(int index, int n, size_t *out_len)
{
	size_t entry_len, total_len;
	char *combined;
	int i;

	if (n <= 0 || index < 0 || index >= killring.count) {
		return NULL;
	}
	entry_len = killring.entries[index].len;
	if (!checked_mul_size_t(&total_len, entry_len, (size_t)n)
	    || total_len > KG_YANK_BATCH_MAX) {
		return NULL;
	}
	combined = yank_alloc_ok() ? malloc(total_len) : NULL;
	if (!combined) {
		return NULL;
	}
	for (i = 0; i < n; i++) {
		memcpy(combined + (size_t)i * entry_len,
		    killring.entries[index].text, entry_len);
	}
	*out_len = total_len;
	return combined;
}

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

/* editor_kill_region()'s direction, matching Emacs' own kill-region:
 * forward when point was ahead of the mark (the usual C-Space-then-
 * move-right selection), backward when point was behind it
 * (C-Space-then-move-left).  editor_region_bounds() already normalizes
 * mark/point into start/end, putting point at the *end* when point was
 * ahead of the mark and at the *start* when it was behind -- so "point
 * is not the start" is "point was ahead", i.e. forward. */
static void kill_ring_region_kill(char *text, int len, int point_row,
    int point_col, int start_row, int start_col)
{
	if (point_row == start_row && point_col == start_col) {
		kill_ring_kill_backward(text, (size_t)len);
	} else {
		kill_ring_kill_forward(text, (size_t)len);
	}
}

/* Cut (save==1) or delete (save==0) the linear region.  Cursor lands at
 * the start of the region; undo restores it as a single step. */
static void region_kill_or_delete(int save)
{
	int start_row, start_col;
	int end_row, end_col;
	int point_row = yank_current_row();
	int point_col = yank_current_col();
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
		kill_ring_region_kill(
		    text, len, point_row, point_col, start_row, start_col);
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

	kill_ring_copy(text, len);
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
	size_t start;

	if (!text) {
		editor_set_status_message("Kill ring is empty");
		return;
	}
	/* Mark the start of the yanked text, as Emacs does. */
	editor_push_mark();
	start = buffer_row_col_to_position(
	    bcur(), yank_current_row(), yank_current_col());
	/* KG_KILL_RING_MAX_BYTES is well under INT_MAX, so this narrowing
	 * is exact for every entry the ring can ever hold. */
	editor_insert_text_at_point(text, (int)kill_ring_get_len());
	kill_ring_note_yank(start, kill_ring_get_len(), 1);
	editor_set_status_message("Yanked");
}

/* yank-pop's transient state: the span the immediately preceding yank or
 * yank-pop inserted, and enough to replace it again with the next-older
 * ring entry.  Owned here, not by struct kill_ring -- the ring is
 * bytes-only storage.  Valid only while cmd_last_kill_class() reads
 * KILL_COALESCE_YANK; that is the whole of yank-pop's eligibility test
 * (see cmdstate.h), so nothing here duplicates it.  `start`'s buffer
 * field is this record's buffer identity too: a marker handle already
 * carries it, so a second copy would just be two sources of truth that
 * could disagree. */
struct yank_pop_state {
	struct kg_marker_handle start;
	size_t inserted_len;
	uint32_t ring_generation;
	int ring_index;
	int repeat_count;
};

static struct yank_pop_state yank_pop;

void kill_ring_note_yank(size_t start, size_t inserted_len, int repeat_count)
{
	struct kg_marker_handle m;

	/* Harmless on the very first yank of a session: deleting a
	 * zero-valued handle resolves nothing and kg_marker_delete() just
	 * says so. */
	kg_marker_delete(yank_pop.start);
	m = kg_marker_create(bcur(), start, KG_MARKER_GRAV_LEFT);
	if (m.id == 0) {
		yank_pop = (struct yank_pop_state) { 0 };
		return;
	}
	yank_pop = (struct yank_pop_state) {
		.start = m,
		.inserted_len = inserted_len,
		.ring_generation = killring.generation,
		.ring_index = 0,
		.repeat_count = repeat_count,
	};
	cmd_set_kill_class(KILL_COALESCE_YANK);
}

/* Whether `yank_pop` still names a span in the current buffer that M-y
 * may replace: the coalescing class, a marker that still resolves, that
 * marker's buffer being the one point is in now, and a ring that has not
 * mutated since (belt-and-suspenders past the coalescing class -- see
 * yank.h's struct kill_ring comment).  `*out_start` is set only on
 * success. */
static bool yank_pop_eligible(size_t *out_start)
{
	struct kg_buffer_handle here;

	if (cmd_last_kill_class() != KILL_COALESCE_YANK || killring.count == 0
	    || yank_pop.ring_generation != killring.generation) {
		return false;
	}
	here = buf_handle_of(bcur());
	if (yank_pop.start.buffer.slot != here.slot
	    || yank_pop.start.buffer.id != here.id
	    || yank_pop.start.buffer.generation != here.generation) {
		return false;
	}
	return kg_marker_resolve(yank_pop.start, out_start) == KG_MARKER_OK;
}

void editor_yank_pop(void)
{
	size_t start, new_len;
	int new_index;
	char *combined;
	struct kg_edit e;
	int row, col;

	if (!yank_pop_eligible(&start)) {
		editor_set_status_message("Previous command was not a yank");
		return;
	}

	new_index = (yank_pop.ring_index + 1) % killring.count;
	combined = kill_ring_entry_repeated(
	    new_index, yank_pop.repeat_count, &new_len);
	if (!combined) {
		editor_set_status_message("Yank too large");
		return;
	}

	e = kg_edit_user(
	    bcur(), start, start + yank_pop.inserted_len, combined, new_len);
	if (!kg_buffer_replace(&e, NULL)) {
		free(combined);
		editor_set_status_message("Out of memory");
		return;
	}
	free(combined);
	/* Merge this call's own undo record with the one beneath it (the
	 * yank or yank-pop this call replaced), so one editor_undo() undoes
	 * the whole chain regardless of how many pops ran.  A merge that
	 * declines (the stack was not what was expected) just leaves two
	 * records instead of one; the edit itself already succeeded either
	 * way. */
	undo_merge_at_top(bcur(), start);

	yank_pop.ring_index = new_index;
	yank_pop.inserted_len = new_len;
	cmd_set_kill_class(KILL_COALESCE_YANK);

	buffer_position_to_row_col(bcur(), start + new_len, &row, &col);
	wcur()->coloff = 0;
	editor_cursor_goto(row, col);
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
