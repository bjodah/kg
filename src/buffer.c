/* ======================= Editor rows implementation ======================= */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "edit.h"
#include "localvars.h"
#include "perf.h"
#include "syntax.h"

#define KG_MAX_VIRTUAL_INSERT_GAP (64 * 1024)

static void editor_nomem(void)
{
	editor_set_status_message("Out of memory");
	running = 0;
}

static int editor_nonnegative(int value) { return value < 0 ? 0 : value; }

static int editor_saturating_add(int a, int b)
{
	return a > INT_MAX - b ? INT_MAX : a + b;
}

/* One change to a buffer's content happened.
 *
 * `dirty` is the modified flag the mode line and the quit prompt read; it
 * counts up and is reset to zero when the buffer is saved or undone back
 * to its saved state, so only its nonzero-ness means anything.
 * `content_generation` never resets, which is what makes it usable as an
 * identity: two samples that differ mean bytes changed in between, and
 * two that match mean they did not.  A command that samples it before
 * dispatch and after can tell whether it edited anything -- which the
 * dirty counter could not, because the paths that set it to 1 leave it at
 * 1 when it was already there. */
void buffer_note_change(struct editor_buffer *b)
{
	b->dirty++;
	b->content_generation++;
}

static int editor_virtual_insert_gap_too_large(erow *row, int at)
{
	if (!row) {
		return 1;
	}
	return at > row->size && at - row->size > KG_MAX_VIRTUAL_INSERT_GAP;
}

int editor_current_filerow_or_eof(void)
{
	wcur()->rowoff = editor_nonnegative(wcur()->rowoff);
	wcur()->cy = editor_nonnegative(wcur()->cy);
	if (bcur()->numrows <= 0) {
		return 0;
	}
	if (wcur()->rowoff >= bcur()->numrows
	    || wcur()->cy >= bcur()->numrows - wcur()->rowoff) {
		return bcur()->numrows;
	}
	return wcur()->rowoff + wcur()->cy;
}

int editor_current_filerow(void)
{
	int row = editor_current_filerow_or_eof();

	if (bcur()->numrows > 0 && row >= bcur()->numrows) {
		return bcur()->numrows - 1;
	}
	return row;
}

int editor_current_filecol(void)
{
	wcur()->coloff = editor_nonnegative(wcur()->coloff);
	wcur()->cx = editor_nonnegative(wcur()->cx);
	return editor_saturating_add(wcur()->coloff, wcur()->cx);
}

int editor_current_filecol_in_row(erow *row)
{
	int col = editor_current_filecol();

	return col > row->size ? row->size : col;
}

/* Visual column at byte offset `chars_col` in `row`.  Tabs advance to
 * the next tab stop (tab_stop_advance()) — same as the render in
 * editor_update_row and the cursor-placement loop in
 * editor_refresh_screen, so the two metrics agree.  Each glyph is worth
 * utf8_width_at() cells, so East-Asian-Wide characters count two and
 * combining marks (like UTF-8 continuation bytes) count zero.
 * `chars_col` past row's end maps to (row's visual width) plus the
 * virtual offset, so cursors that sit in virtual space (rect mode) get
 * a well-defined visual column too. */
int editor_visual_col(erow *row, int chars_col)
{
	int j;
	int64_t vcol = 0;
	int limit = chars_col < row->size ? chars_col : row->size;

	if (chars_col < 0) {
		chars_col = 0;
		limit = 0;
	}
	for (j = 0; j < limit; j++) {
		if (row->chars[j] == TAB) {
			vcol += tab_stop_advance((int)(vcol % KG_TAB_WIDTH));
		} else {
			vcol += utf8_width_at(row->chars, row->size, j);
		}
	}
	if (chars_col > row->size) {
		vcol += chars_col - row->size;
	}
	return vcol > INT_MAX ? INT_MAX : (int)vcol;
}

/* Zero-based display column reported to the user for a cursor at
 * (filerow, filecol) in `rows`, matching Emacs' column-number-mode and
 * what-cursor-position: tabs advance to the next tab stop and a
 * multi-byte character counts once.  With no row to measure (empty
 * buffer, cursor past the last row) each byte counts as one column, the
 * same rule editor_visual_col() uses for virtual space. */
int editor_display_col(erow *rows, int numrows, int filerow, int filecol)
{
	if (filecol < 0) {
		filecol = 0;
	}
	if (!rows || filerow < 0 || filerow >= numrows) {
		return filecol;
	}
	return editor_visual_col(&rows[filerow], filecol);
}

/* Inverse of editor_visual_col(): byte offset into row->chars whose
 * visual position lands at or just before `target_vcol`.  A target
 * that falls inside a tab's expansion snaps to the tab's start byte
 * (closest representable position when cx is a byte offset); a target
 * that names the second cell of a double-width glyph snaps to that
 * glyph's start byte by the same rule.  When target is past the row's
 * visual end, returns row->size plus the matching virtual offset (the
 * cursor lives in virtual space). */
int editor_chars_col_at_visual(erow *row, int target_vcol)
{
	int j = 0, vcol = 0;

	if (target_vcol < 0) {
		return 0;
	}
	while (j < row->size) {
		int next_vcol;
		if (row->chars[j] == TAB) {
			next_vcol = vcol + tab_stop_advance(vcol);
		} else {
			next_vcol
			    = vcol + utf8_width_at(row->chars, row->size, j);
		}
		if (next_vcol > target_vcol) {
			break;
		}
		vcol = next_vcol;
		j++;
	}
	/* Past-EOL only fires when we walked the whole row without hitting
	 * target_vcol.  Breaking early means we hit a glyph (typically a
	 * tab) whose expansion would overshoot — round down to j. */
	if (j >= row->size && vcol < target_vcol) {
		int extra = target_vcol - vcol;
		return editor_saturating_add(row->size, extra);
	}
	return j;
}

/* Bring cx back into a row-valid position.  rect_mode lets cx wander
 * past EOL during virtual-column rectangle navigation; this is the
 * matching snap-back used whenever rect_mode is cleared. */
void editor_snap_cx_to_row(void)
{
	int filecol;
	int rowlen;

	if (editor_current_filerow_or_eof() >= bcur()->numrows) {
		return;
	}
	filecol = editor_current_filecol();
	rowlen = bcur()->row[editor_current_filerow()].size;
	if (filecol > rowlen) {
		if (wcur()->coloff > rowlen) {
			wcur()->coloff = rowlen;
			wcur()->cx = 0;
		} else {
			wcur()->cx = rowlen - wcur()->coloff;
		}
	}
}

/* Scroll the viewport just enough to put (row, col) into view, then
 * land cursor on it.  Used by undo replay and region commands that need
 * to land on a target without otherwise centring the view. */
void editor_cursor_goto(int row, int col)
{
	if (row < wcur()->rowoff) {
		wcur()->rowoff = row;
		wcur()->cy = 0;
	} else if (row >= wcur()->rowoff + wcur()->h) {
		wcur()->rowoff = row - wcur()->h + 1;
		wcur()->cy = wcur()->h - 1;
	} else {
		wcur()->cy = row - wcur()->rowoff;
	}
	if (col < wcur()->coloff) {
		wcur()->coloff = col;
		wcur()->cx = 0;
	} else if (col >= wcur()->coloff + wcur()->w) {
		wcur()->coloff = col - wcur()->w + 1;
		wcur()->cx = wcur()->w - 1;
	} else {
		wcur()->cx = col - wcur()->coloff;
	}
}

/* Keep (row, col) visible, but only recenter when it is currently off-screen.
 * Used by isearch so matches already visible in the window do not jerk the
 * viewport forward on every character typed, while distant matches still land
 * in a stable, centered view.  `col` is a byte offset into row->chars, the
 * same space wcur()->coloff + wcur()->cx is measured in — this lands point,
 * so a rendered column would put it elsewhere on any line holding a tab. */
void editor_reveal_position_centered(int row, int col)
{
	int max_rowoff;

	if (row < 0) {
		row = 0;
	}
	if (col < 0) {
		col = 0;
	}

	if (row < wcur()->rowoff || row >= wcur()->rowoff + wcur()->h) {
		wcur()->rowoff = row - wcur()->h / 2;
		if (wcur()->rowoff < 0) {
			wcur()->rowoff = 0;
		}
		max_rowoff = bcur()->numrows - wcur()->h;
		if (max_rowoff < 0) {
			max_rowoff = 0;
		}
		if (wcur()->rowoff > max_rowoff) {
			wcur()->rowoff = max_rowoff;
		}
	}
	wcur()->cy = row - wcur()->rowoff;
	if (wcur()->cy < 0) {
		wcur()->cy = 0;
	} else if (wcur()->cy >= wcur()->h) {
		wcur()->cy = wcur()->h - 1;
	}

	if (col < wcur()->coloff || col >= wcur()->coloff + wcur()->w) {
		wcur()->coloff = col - wcur()->w / 2;
		if (wcur()->coloff < 0) {
			wcur()->coloff = 0;
		}
	}
	wcur()->cx = col - wcur()->coloff;
	if (wcur()->cx < 0) {
		wcur()->cx = 0;
	} else if (wcur()->cx >= wcur()->w) {
		wcur()->cx = wcur()->w - 1;
	}
}

/* Rows shorter than this are grown to exactly what they need. */
#define KG_ROW_SLACK_MIN 1024

/* How much storage to reserve for a row that needs `need` bytes.
 *
 * A long row gets an eighth of slack, so that typing into it -- which
 * lengthens it one byte at a time -- does not reallocate and copy the
 * whole row per keystroke: a 1 MiB row absorbs 128k insertions between
 * allocations.  A short row gets none, because slack that looks free per
 * row is megabytes across a file of a hundred thousand of them, and
 * reallocating a row of a few dozen bytes costs nothing worth avoiding.
 * (Measured: an eighth on every row, or even a flat 16 bytes, cost 4 MiB
 * of RSS on a 100k-line file, for rows nobody is typing into.) */
int editor_row_grown_capacity(int need)
{
	if (need < KG_ROW_SLACK_MIN) {
		return need < 0 ? 0 : need;
	}
	if (need > INT_MAX - need / 8) {
		return need;
	}
	return need + need / 8;
}

/* Make row->render able to hold `need` bytes.  Returns 0 having reported
 * the failure and left the row's existing render untouched. */
static int row_render_reserve(erow *row, int need)
{
	char *grown;
	int newcap;

	if (need <= row->render_capacity) {
		return 1;
	}
	newcap = editor_row_grown_capacity(need);
	KG_PERF_INC(KG_PERF_RENDER_ALLOC);
	KG_PERF_ADD(KG_PERF_RENDER_BYTES, newcap);
	grown = realloc(row->render, (size_t)newcap);
	if (!grown) {
		editor_nomem();
		return 0;
	}
	row->render = grown;
	row->render_capacity = newcap;
	return 1;
}

/* Update the rendered version and the syntax highlight of a row.
 *
 * The render buffer is reused: it carries a capacity, so an edit that
 * does not make the row longer than it has ever been costs no
 * allocation at all, where this used to free and malloc the whole
 * worst-case buffer on every call -- including the 32 call sites that
 * update a row per typed byte.
 *
 * An allocation failure now leaves the previous render in place instead
 * of freeing it and leaving row->rsize describing bytes that are gone.
 * running is cleared either way, so the callers that watch for that (and
 * for a NULL render on a row that never had one) still see it.
 *
 * The row's wrapped-width cache (def.h's erow, src/mode.c) is invalidated
 * first and unconditionally, before anything below can run out of memory
 * or bail out: every caller has already changed row->chars by the time it
 * gets here, so a cache a failure left marked valid would go on claiming
 * the width of text that is no longer there. */
void editor_update_row(struct editor_buffer *b, erow *row)
{
	unsigned int tabs = 0;
	unsigned long long allocsize;
	int j, idx, render_cap, vcol;

	row->wrap_cache_win_w = 0;
	KG_PERF_INC(KG_PERF_ROW_UPDATE);
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == TAB) {
			tabs++;
		}
	}

	/* Worst case a TAB widens to KG_TAB_WIDTH columns, the last byte
	 * being the terminator. */
	allocsize = (unsigned long long)row->size
	    + (unsigned long long)tabs * KG_TAB_WIDTH + 1;
	if (allocsize > INT_MAX) {
		editor_set_status_message("Line too long for editor");
		running = 0;
		return;
	}

	if (!row_render_reserve(row, (int)allocsize)) {
		return;
	}
	render_cap = row->render_capacity;
	idx = 0;
	vcol = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == TAB) {
			int spaces = tab_stop_advance(vcol);

			if (idx + spaces >= render_cap) {
				/* Unreachable -- allocsize is the worst
				 * case -- so leave a consistent empty
				 * render rather than a half-written one. */
				row->render[0] = '\0';
				row->rsize = 0;
				editor_set_status_message(
				    "Line render overflow");
				running = 0;
				return;
			}
			memset(row->render + idx, ' ', spaces);
			idx += spaces;
			vcol += spaces;
		} else {
			row->render[idx++] = row->chars[j];
			/* Display width, not byte count: a TAB after a CJK
			 * glyph must reach the same tab stop the terminal
			 * does. */
			vcol += utf8_width_at(row->chars, row->size, j);
		}
	}
	row->rsize = idx;
	row->render[idx] = '\0';

	/* Update the syntax highlighting attributes of the row. */
	editor_update_syntax(b, row);
}

/* Rows a fresh row array starts out able to hold.  Small enough that a
 * one-line buffer is not paying for a page, large enough that the first
 * screenful of a file costs one allocation. */
#define KG_ROWS_INITIAL_CAPACITY 16

/* Make room for `need` row records in `*rows`, doubling so that filling a
 * buffer with R rows costs O(log R) reallocations instead of R of them --
 * which is what an exact-size realloc per row costs, and R of them copy
 * O(R^2) row records between them.
 *
 * `*capacity` is how many records the allocation holds, and only ever
 * grows: shrinking on deletion would make delete/insert at a size
 * boundary pay the copy twice, and the memory is a row record (not row
 * text) per deleted line.  Returns 0 with the array and the capacity
 * untouched when the size would overflow or the allocation fails, so a
 * caller that has already published nothing stays failure-atomic. */
int editor_rows_reserve(erow **rows, int *capacity, int need)
{
	int newcap;
	size_t bytes;
	erow *grown;

	if (need <= *capacity) {
		return need >= 0;
	}
	newcap = *capacity > 0 ? *capacity : KG_ROWS_INITIAL_CAPACITY;
	while (newcap < need) {
		if (newcap > INT_MAX / 2) {
			newcap = need;
			break;
		}
		newcap *= 2;
	}
	/* newcap is positive by construction -- it starts at a positive
	 * number and only doubles.  Saying so is what keeps a zero-byte
	 * realloc out of reach of a future edit to the doubling above, and
	 * out of the static analyzer's set of reachable states. */
	if (newcap <= 0
	    || !checked_mul_size_t(&bytes, (size_t)newcap, sizeof(**rows))) {
		return 0;
	}
	KG_PERF_INC(KG_PERF_ROW_ARRAY_GROW);
	KG_PERF_ADD(KG_PERF_ROW_ARRAY_BYTES, bytes);
	grown = realloc(*rows, bytes);
	if (!grown) {
		return 0;
	}
	*rows = grown;
	*capacity = newcap;
	return 1;
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required.  `s` is a byte slice of exactly `len` bytes: the row is
 * terminated here rather than by copying whatever follows the caller's
 * slice, which for the '\n'-bounded slices the undo replays hand over is
 * a newline, and for a slice ending at the end of an allocation is a read
 * past it. */
void editor_insert_row(
    struct editor_buffer *b, int at, const char *s, size_t len)
{
	char *newchars;
	size_t alloc_sz;

	if (at > b->numrows) {
		return;
	}

	if (!checked_add_size_t(&alloc_sz, len, 1)) {
		editor_nomem();
		return;
	}
	newchars = malloc(alloc_sz);
	if (!newchars) {
		editor_nomem();
		return;
	}
	memcpy(newchars, s, len);
	newchars[len] = '\0';

	if (!editor_rows_reserve(&b->row, &b->row_capacity, b->numrows + 1)) {
		free(newchars);
		editor_nomem();
		return;
	}
	if (at != b->numrows) {
		memmove(b->row + at + 1, b->row + at,
		    sizeof(b->row[0]) * (b->numrows - at));
		for (int j = at + 1; j <= b->numrows; j++) {
			b->row[j].idx++;
		}
	}

	b->row[at] = (erow) {
		.idx = at,
		.size = (int)len,
		.chars = newchars,
	};
	b->numrows++;
	editor_update_row(b, b->row + at);
	buffer_note_change(b);
}

/* Free row's heap allocated stuff. */
/* Free a row's heap storage and leave the record describing nothing.
 * The capacities are the reason it has to: a freed pointer with a
 * capacity still beside it is storage the next update would reuse. */
void editor_free_row(erow *row)
{
	free(row->render);
	free(row->chars);
	free(row->hl);
	row->render = NULL;
	row->chars = NULL;
	row->hl = NULL;
	row->size = 0;
	row->rsize = 0;
	row->render_capacity = 0;
	row->hl_capacity = 0;
}

/* Free every row of `b` and leave it empty.  Used whenever a buffer's
 * contents are rebuilt from scratch.  The capacity goes with the pointer:
 * one left behind describes storage that has been freed. */
void editor_free_all_rows(struct editor_buffer *b)
{
	int i;

	for (i = 0; i < b->numrows; i++) {
		editor_free_row(&b->row[i]);
	}
	free(b->row);
	b->row = NULL;
	b->numrows = 0;
	b->row_capacity = 0;
}

/* Remove the row at the specified position, shifting the remaining on the top.
 */
void editor_del_row(struct editor_buffer *b, int at)
{
	erow *row;

	if (at >= b->numrows) {
		return;
	}
	row = b->row + at;
	editor_free_row(row);
	memmove(b->row + at, b->row + at + 1,
	    sizeof(b->row[0]) * (b->numrows - at - 1));
	for (int j = at; j < b->numrows - 1; j++) {
		b->row[j].idx--;
	}
	b->numrows--;
	buffer_note_change(b);
	editor_rehighlight_from(b, at);
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, excluding
 * the final nulterm. */
char *editor_rows_to_string(erow *rows, int numrows, int *buflen)
{
	char *buf = NULL, *p;
	size_t totlen_sz = 0;
	int j;

	KG_PERF_INC(KG_PERF_BUFFER_FLATTEN);
	/* Compute count of bytes */
	for (j = 0; j < numrows; j++) {
		if (rows[j].size < 0) {
			*buflen = 0;
			return NULL;
		}
		if (!checked_add_size_t(
			&totlen_sz, totlen_sz, (size_t)rows[j].size)) {
			*buflen = 0;
			return NULL;
		}
	}
	if (numrows > 1) {
		if (!checked_add_size_t(
			&totlen_sz, totlen_sz, (size_t)(numrows - 1))) {
			*buflen = 0;
			return NULL;
		}
	}

	int totlen;
	if (!checked_size_to_int(&totlen, totlen_sz)) {
		*buflen = 0;
		return NULL;
	}

	size_t alloc_sz;
	if (!checked_add_size_t(&alloc_sz, totlen_sz, 1)) {
		*buflen = 0;
		return NULL;
	}

	p = buf = malloc(alloc_sz);
	if (!buf) {
		editor_nomem();
		*buflen = 0;
		return NULL;
	}
	*buflen = totlen;
	for (j = 0; j < numrows; j++) {
		memcpy(p, rows[j].chars, rows[j].size);
		p += rows[j].size;
		if (j != numrows - 1) {
			*p++ = '\n';
		}
	}
	*p = '\0';
	return buf;
}

static int utf8_next_glyph_len(const char *buf, int len, int start)
{
	int n = 1;

	while (start + n < len && utf8_is_cont((unsigned char)buf[start + n])) {
		n++;
	}
	return n;
}

static int utf8_prev_glyph_start(const char *buf, int pos)
{
	int start;

	if (pos <= 0) {
		return -1;
	}
	start = pos - 1;
	while (start > 0 && utf8_is_cont((unsigned char)buf[start])) {
		start--;
	}
	return start;
}

/* ---- Flat byte positions ----------------------------------------------
 * The buffer's text read as one string: every row's bytes, one '\n'
 * between neighbours, none after the last.  A position is a byte offset
 * into that string, so 0 is the start, buffer_byte_length() is the end,
 * and the offset of a row separator is the offset of the row's last byte
 * plus one.  This is the unit the edit transaction addresses in; the
 * codepoint offsets below are a separate dialect and are the Lisp
 * adapter's alone.
 *
 * Nothing caches: a conversion walks the rows it crosses, which is what
 * the two dialects have always cost.  Add a per-row prefix sum here if a
 * measurement ever asks for one -- but add it once, to this layer. */

/* Bytes in the whole buffer, separators included.  An empty buffer -- and
 * a slot that has never been loaded -- is 0 bytes long. */
size_t buffer_byte_length(const struct editor_buffer *b)
{
	size_t total = 0;
	int i;

	for (i = 0; i < b->numrows; i++) {
		total += (size_t)b->row[i].size + 1;
	}
	return total ? total - 1 : 0;
}

/* Byte position of (row, byte column).  A row past the last one, or a
 * column past its row's end, names the nearest position that exists
 * rather than failing: callers arrive here holding point, which may sit
 * past the end of a row that just got shorter. */
size_t buffer_row_col_to_position(
    const struct editor_buffer *b, int row, int col)
{
	size_t pos = 0;
	int i;

	if (b->numrows <= 0 || row < 0) {
		return 0;
	}
	if (row >= b->numrows) {
		row = b->numrows - 1;
		col = b->row[row].size;
	}
	for (i = 0; i < row; i++) {
		pos += (size_t)b->row[i].size + 1;
	}
	if (col < 0) {
		col = 0;
	} else if (col > b->row[row].size) {
		col = b->row[row].size;
	}
	return pos + (size_t)col;
}

/* Inverse.  Returns 0 for a position past the end of the buffer, having
 * clamped `*row`/`*col` to the end anyway, so a caller that ignores the
 * result still gets a position that exists. */
int buffer_position_to_row_col(
    const struct editor_buffer *b, size_t pos, int *row, int *col)
{
	int i;

	*row = 0;
	*col = 0;
	if (b->numrows <= 0) {
		return pos == 0;
	}
	for (i = 0; i < b->numrows; i++) {
		size_t len = (size_t)b->row[i].size;

		if (pos <= len) {
			*row = i;
			*col = (int)pos;
			return 1;
		}
		pos -= len + 1;
	}
	*row = b->numrows - 1;
	*col = b->row[*row].size;
	return 0;
}

/* ---- Codepoint offset conversions ------------------------------------
 * kg stores positions as (row, byte column); Emacs-shaped APIs address the
 * buffer by a single codepoint offset where every row separator counts as
 * one character.  These helpers convert between the two on demand; nothing
 * here caches, so callers pay a walk of the rows they cross.
 *
 * This dialect is `src/lisp.c`'s and no other module's -- see the note on
 * the flat byte positions above.  Anything inside the editor that needs a
 * position uses those. */

/* Codepoints before `byte_index` in `row`.  A byte index inside a glyph
 * rounds down to that glyph's start, so the result never names a fractional
 * position.  Out-of-range indices clamp to the row. */
int editor_row_byte_to_char(erow *row, int byte_index)
{
	int i, n = 0;

	if (!row || byte_index <= 0) {
		return 0;
	}
	if (byte_index > row->size) {
		byte_index = row->size;
	}
	while (byte_index > 0 && byte_index < row->size
	    && utf8_is_cont((unsigned char)row->chars[byte_index])) {
		byte_index--;
	}
	for (i = 0; i < byte_index; i++) {
		if (!utf8_is_cont((unsigned char)row->chars[i])) {
			n++;
		}
	}
	return n;
}

/* Inverse of editor_row_byte_to_char(): byte index of the `char_index`'th
 * codepoint in `row`.  Char indices past the row's end clamp to row->size. */
int editor_row_char_to_byte(erow *row, int char_index)
{
	int i = 0, n = 0;

	if (!row || char_index <= 0) {
		return 0;
	}
	while (i < row->size) {
		if (!utf8_is_cont((unsigned char)row->chars[i])) {
			if (n == char_index) {
				break;
			}
			n++;
		}
		i++;
	}
	return i;
}

/* Codepoint offset from buffer start for (row, byte column).  A row past the
 * last one clamps to end of buffer; a negative row clamps to offset 0 --
 * which is the byte layer's clamp, taken from it rather than restated, so
 * the two dialects cannot come to disagree about where the buffer ends. */
long editor_char_offset(int row, int col)
{
	long off = 0;
	int i;

	if (bcur()->numrows <= 0) {
		return 0;
	}
	buffer_position_to_row_col(
	    bcur(), buffer_row_col_to_position(bcur(), row, col), &row, &col);
	for (i = 0; i < row; i++) {
		off += editor_row_byte_to_char(
			   &bcur()->row[i], bcur()->row[i].size)
		    + 1;
	}
	return off + editor_row_byte_to_char(&bcur()->row[row], col);
}

/* Inverse of editor_char_offset().  `*col` comes back as a byte index so it
 * can be handed straight to editor_cursor_goto(). */
void editor_offset_to_rowcol(long offset, int *row, int *col)
{
	int i;

	*row = 0;
	*col = 0;
	if (bcur()->numrows <= 0 || offset <= 0) {
		return;
	}
	for (i = 0; i < bcur()->numrows; i++) {
		long len = editor_row_byte_to_char(
		    &bcur()->row[i], bcur()->row[i].size);
		if (offset <= len || i == bcur()->numrows - 1) {
			if (offset > len) {
				offset = len;
			}
			*row = i;
			*col = editor_row_char_to_byte(
			    &bcur()->row[i], (int)offset);
			return;
		}
		offset -= len + 1;
	}
}

/* Total codepoints in the buffer, counting the '\n' separators — i.e. the
 * offset of the end of buffer. */
long editor_buffer_char_length(void)
{
	if (bcur()->numrows <= 0) {
		return 0;
	}
	return editor_char_offset(bcur()->numrows - 1, INT_MAX);
}

/* How many more allocations the edit transaction's staging path may make
 * before one of them is failed on purpose.  Negative is how the editor
 * always runs: no seam.  See kg_edit_fail_alloc_after() in edit.h. */
static int edit_alloc_budget = -1;

void kg_edit_fail_alloc_after(int n) { edit_alloc_budget = n; }

/* Whether the transaction may make one more allocation.  Every staging
 * allocation asks, so a test picks which one fails; the seam disarms
 * itself once it has fired, so the failure is one point in one edit and
 * not a state the rest of the run has to be dug out of. */
static int edit_alloc_ok(void)
{
	return edit_alloc_budget < 0 || edit_alloc_budget-- != 0;
}

/* malloc() and calloc() for the staging path, failable at a chosen
 * point.  Identical to the libc calls while the seam is disarmed. */
static void *edit_malloc(size_t size)
{
	return edit_alloc_ok() ? malloc(size) : NULL;
}

static void *edit_calloc(size_t count, size_t size)
{
	return edit_alloc_ok() ? calloc(count, size) : NULL;
}

/* And the row array's growth, which is the staging path's one
 * allocation that is not a malloc() of its own. */
static int edit_rows_reserve(erow **rows, int *capacity, int need)
{
	return edit_alloc_ok() && editor_rows_reserve(rows, capacity, need);
}

/* Build the `nl` rows that follow the split row: segments 1..nl of
 * `text`, with `suffix` (the split row's bytes after point) appended to
 * the last of them.  `rows` is zeroed on entry.  Returns 0 having freed
 * everything it had allocated, so the caller has published nothing. */
static int splice_build_rows(const char *text, int len, int nl,
    const char *suffix, int suffix_len, erow *rows)
{
	const char *p = (const char *)memchr(text, '\n', (size_t)len) + 1;
	const char *end = text + len;
	int i;

	for (i = 0; i < nl; i++) {
		const char *stop = (i + 1 < nl)
		    ? (const char *)memchr(p, '\n', (size_t)(end - p))
		    : end;
		int seg = (int)(stop - p);
		int tail = (i + 1 < nl) ? 0 : suffix_len;
		int size, alloc_sz;

		if (!checked_add_int_size(&size, seg, (size_t)tail)
		    || !checked_add_int_size(&alloc_sz, size, 1)) {
			goto fail;
		}
		rows[i].chars = edit_malloc((size_t)alloc_sz);
		if (!rows[i].chars) {
			goto fail;
		}
		memcpy(rows[i].chars, p, (size_t)seg);
		memcpy(rows[i].chars + seg, suffix, (size_t)tail);
		rows[i].chars[size] = '\0';
		rows[i].size = size;
		p = stop + 1;
	}
	return 1;

fail:
	while (i-- > 0) {
		free(rows[i].chars);
		rows[i].chars = NULL;
	}
	return 0;
}

/* Count of '\n' in `text`. */
static int splice_newlines(const char *text, int len)
{
	int i, n = 0;

	for (i = 0; i < len; i++) {
		if (text[i] == '\n') {
			n++;
		}
	}
	return n;
}

/* The first affected row's new content: `prefix`, then the `first_seg`
 * bytes of `text` before its first separator, then `tail` bytes of
 * `suffix` -- which is all of it when the text holds no separator at all
 * and the whole edit therefore lands in one row.  Returns 0 having
 * allocated nothing. */
static int splice_head_alloc(const char *text, int first_seg,
    const char *prefix, int prefix_len, const char *suffix, int tail,
    char **head, int *head_len)
{
	int alloc_sz;

	if (!checked_add_int_size(head_len, prefix_len, (size_t)first_seg)
	    || !checked_add_int_size(head_len, *head_len, (size_t)tail)
	    || !checked_add_int_size(&alloc_sz, *head_len, 1)) {
		return 0;
	}
	*head = edit_malloc((size_t)alloc_sz);
	if (!*head) {
		return 0;
	}
	memcpy(*head, prefix, (size_t)prefix_len);
	memcpy(*head + prefix_len, text, (size_t)first_seg);
	memcpy(*head + prefix_len + first_seg, suffix, (size_t)tail);
	(*head)[*head_len] = '\0';
	return 1;
}

/* Allocate everything a splice will publish: the first affected row's
 * new content (`*head`, `*head_len` bytes) and the `nl` rows that follow
 * it (`*rows`, NULL when nl is zero, the last of them carrying
 * `suffix`).  `prefix` and `suffix` are copied here, so the caller may
 * reallocate its rows afterwards.  Returns 0 having allocated nothing. */
static int splice_rows_alloc(const char *text, int len, int nl,
    const char *suffix, int suffix_len, erow **rows)
{
	*rows = edit_calloc((size_t)nl, sizeof(**rows));
	if (!*rows
	    || !splice_build_rows(text, len, nl, suffix, suffix_len, *rows)) {
		free(*rows);
		*rows = NULL;
		return 0;
	}
	return 1;
}

static int splice_alloc(const char *text, int len, int nl, const char *prefix,
    int prefix_len, const char *suffix, int suffix_len, char **head,
    int *head_len, erow **rows)
{
	const char *sep = memchr(text, '\n', (size_t)len);
	int first_seg = sep ? (int)(sep - text) : len;

	*rows = NULL;
	if (!splice_head_alloc(text, first_seg, prefix, prefix_len, suffix,
		nl ? 0 : suffix_len, head, head_len)) {
		return 0;
	}
	if (nl && !splice_rows_alloc(text, len, nl, suffix, suffix_len, rows)) {
		free(*head);
		*head = NULL;
		return 0;
	}
	return 1;
}

/* Insert `text` at point.
 *
 * Inserting is replacing an empty range, so this is the edit transaction
 * with nothing to delete: the row at point is cut in two, the inserted
 * lines are built, the row array is grown once and its tail moved once.
 * It used to serialise the whole buffer, build a second copy of it with
 * the text inserted, and rebuild every row from that -- so pasting one
 * line into a 100k-line file rewrote all 100k of them.  The record is
 * the caller's: `editor_insert_text_raw()` is the raw insertion the
 * commands build their own undo step around. */
static int editor_insert_text_raw_bulk(const char *text, int insert_len)
{
	size_t pos = buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());
	struct kg_edit e
	    = kg_edit_user_part(bcur(), pos, pos, text, (size_t)insert_len);
	int row, col;

	if (!kg_buffer_replace(&e, NULL)) {
		return 0;
	}
	/* Point ends where the inserted text does, which is in front of
	 * whatever followed it on the split row. */
	buffer_position_to_row_col(
	    bcur(), pos + (size_t)insert_len, &row, &col);
	editor_cursor_goto(row, col);
	return 1;
}

/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editor_row_insert_char(erow *row, int at, int c)
{
	char *newchars;

	if (!row) {
		return;
	}
	if (at < 0) {
		return;
	}
	if (editor_virtual_insert_gap_too_large(row, at)) {
		editor_nomem();
		return;
	}
	if (at > row->size) {
		/* Pad the string with spaces if the insert location is outside
		 * the current length by more than a single character. */
		int padlen = at - row->size;
		/* In the next line +2 means: new char and null term. */
		if (at > INT_MAX - 2) {
			editor_nomem();
			return;
		}
		newchars = realloc(row->chars, row->size + padlen + 2);
		if (!newchars) {
			editor_nomem();
			return;
		}
		row->chars = newchars;
		memset(row->chars + row->size, ' ', padlen);
		row->chars[row->size + padlen + 1] = '\0';
		row->size += padlen + 1;
	} else {
		/* If we are in the middle of the string just make space for 1
		 * new char plus the (already existing) null term. */
		if (row->size > INT_MAX - 2) {
			editor_nomem();
			return;
		}
		newchars = realloc(row->chars, row->size + 2);
		if (!newchars) {
			editor_nomem();
			return;
		}
		row->chars = newchars;
		memmove(
		    row->chars + at + 1, row->chars + at, row->size - at + 1);
		row->size++;
	}
	row->chars[at] = c;
	editor_update_row(bcur(), row);
	buffer_note_change(bcur());
}

void editor_row_insert_string(erow *row, int at, const char *s, int len)
{
	char *newchars;
	int padlen = 0;
	int old_size;
	int new_size;

	if (!row || at < 0 || len <= 0) {
		return;
	}
	if (editor_virtual_insert_gap_too_large(row, at)) {
		editor_nomem();
		return;
	}
	old_size = row->size;
	if (at > old_size) {
		padlen = at - old_size;
		new_size = at;
	} else {
		new_size = old_size;
	}
	if (new_size > INT_MAX - len - 1) {
		editor_nomem();
		return;
	}
	new_size += len;
	newchars = realloc(row->chars, new_size + 1);
	if (!newchars) {
		editor_nomem();
		return;
	}
	row->chars = newchars;
	if (at > old_size) {
		memset(row->chars + old_size, ' ', padlen);
		memcpy(row->chars + old_size + padlen, s, len);
	} else {
		memmove(
		    row->chars + at + len, row->chars + at, old_size - at + 1);
		memcpy(row->chars + at, s, len);
	}
	row->size = new_size;
	row->chars[new_size] = '\0';
	editor_update_row(bcur(), row);
	buffer_note_change(bcur());
}

/* Append the string 's' at the end of a row */
void editor_row_append_string(erow *row, char *s, size_t len)
{
	size_t len_plus_1;
	int alloc_sz;

	if (!checked_add_size_t(&len_plus_1, len, 1)
	    || !checked_add_int_size(&alloc_sz, row->size, len_plus_1)) {
		editor_nomem();
		return;
	}

	char *newchars = realloc(row->chars, alloc_sz);

	if (!newchars) {
		editor_nomem();
		return;
	}
	row->chars = newchars;
	memcpy(row->chars + row->size, s, len);
	row->size += (int)len;
	row->chars[row->size] = '\0';
	editor_update_row(bcur(), row);
	buffer_note_change(bcur());
}

/* Delete the character at offset 'at' from the specified row. */
void editor_row_del_char(erow *row, int at)
{
	if (row->size <= at) {
		return;
	}
	memmove(row->chars + at, row->chars + at + 1, row->size - at);
	row->size--;
	editor_update_row(bcur(), row);
	buffer_note_change(bcur());
}

/* Insert the specified char at the current prompt position. */
void editor_insert_char(int c)
{
	erow *row;
	int filerow;
	int filecol;

	filerow = editor_current_filerow_or_eof();
	filecol = editor_current_filecol();
	row = (filerow >= bcur()->numrows) ? NULL : &bcur()->row[filerow];

	/* If the row where the cursor is currently located does not exist in
	 * our logical representation of the file, add enough empty rows as
	 * needed. */
	if (!row) {
		if (!bcur()->row && bcur()->numrows > 0) {
			editor_nomem();
			return;
		}
		while (bcur()->numrows <= filerow) {
			editor_insert_row(bcur(), bcur()->numrows, "", 0);
		}
		if (bcur()->numrows <= filerow) {
			return;
		}
	}
	if (filerow >= bcur()->numrows) {
		return;
	}
	row = &bcur()->row[filerow];
	if (!bcur()->rect_mode && filecol > row->size) {
		filecol = row->size;
		editor_cursor_goto(filerow, filecol);
	}
	if (editor_virtual_insert_gap_too_large(row, filecol)) {
		editor_nomem();
		return;
	}

	/* Record undo operation */
	undo_push(bcur(), UNDO_INSERT_CHAR, filerow, filecol, c, NULL, 0);

	editor_row_insert_char(row, filecol, c);
	if (wcur()->cx == wcur()->w - 1) {
		wcur()->coloff++;
	} else {
		wcur()->cx++;
	}
	buffer_note_change(bcur());
}

/* Insert `text` at point as one edit and leave point after it.  The
 * commands that put a run of text into the buffer are all this call with
 * a different string: a newline is one byte between two rows, a newline
 * plus an indent is that byte and the bytes the next line starts with,
 * and a yank is the kill ring's contents.
 *
 * The record is KG_EDIT_USER's, which is dropped while yank replay and
 * the batched kills hold `suppress_undo` -- that is what lets one
 * primitive serve both them and a typed newline, and is the last thing
 * the flag is doing here. */
void editor_insert_text_at_point(const char *text, int len)
{
	size_t pos = buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());
	struct kg_edit e = kg_edit_user(bcur(), pos, pos, text, (size_t)len);
	int row, col;

	if (!kg_buffer_replace(&e, NULL)) {
		return;
	}
	buffer_position_to_row_col(bcur(), pos + (size_t)len, &row, &col);
	wcur()->coloff = 0;
	editor_cursor_goto(row, col);
}

/* Split the current line at the cursor without auto-indent.
 * Used by yank, kill-undo, and the paste-mode short-circuit in
 * editor_insert_newline to re-insert newlines exactly as typed. */
void editor_insert_newline_raw(void) { editor_insert_text_at_point("\n", 1); }

/* Insert `text` at point without recording undo of its own, using raw
 * newlines (no auto-indent).  Used by editor_yank and UNDO_KILL_TEXT.
 *
 * A rectangle mark is the one thing that keeps this off the transaction:
 * point may then sit in virtual space past the end of its row, which is
 * a column no byte position names, so the row primitive pads out to it
 * one run at a time.  Everything else is one replacement of the empty
 * range at point, whether or not the text carries separators. */
void editor_insert_text_raw(const char *text, int len)
{
	int saved = suppress_undo;
	int i = 0;

	if (len <= 0) {
		return;
	}
	suppress_undo = 1;
	if (!bcur()->rect_mode) {
		editor_insert_text_raw_bulk(text, len);
		suppress_undo = saved;
		return;
	}
	while (i < len) {
		if (text[i] == '\n') {
			editor_insert_newline_raw();
			i++;
		} else {
			erow *row;
			int filerow;
			int filecol;
			int start = i;
			int run_len;

			while (i < len && text[i] != '\n') {
				i++;
			}
			run_len = i - start;
			filerow = editor_current_filerow_or_eof();
			filecol = editor_current_filecol();
			while (bcur()->numrows <= filerow) {
				editor_insert_row(
				    bcur(), bcur()->numrows, "", 0);
			}
			if (filerow >= bcur()->numrows) {
				break;
			}
			row = &bcur()->row[filerow];
			editor_row_insert_string(
			    row, filecol, text + start, run_len);
			editor_cursor_goto(
			    filerow, editor_saturating_add(filecol, run_len));
		}
	}
	suppress_undo = saved;
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed.
 *
 * During a paste the source already carries its own indentation, so inheriting
 * the current line's indent on top of it produces an ever-deepening staircase.
 * Delegate to the raw variant in that case. */
void editor_insert_newline(void)
{
	int filerow, filecol, indent = 0;
	char *text;
	erow *row;

	if (editor.paste_mode) {
		editor_insert_newline_raw();
		return;
	}

	filerow = editor_current_filerow_or_eof();
	filecol = editor_current_filecol();
	row = (filerow >= bcur()->numrows) ? NULL : &bcur()->row[filerow];
	/* The indent is the current line's leading whitespace, and never
	 * reaches past the split point: splitting inside the indentation
	 * copies only the part that is above the split. */
	while (row && indent < filecol && indent < row->size
	    && (row->chars[indent] == ' ' || row->chars[indent] == TAB)) {
		indent++;
	}
	/* One separator followed by that indent, inserted as one edit --
	 * so one row rebuild, and one C-_ that rejoins the line exactly,
	 * the indent going with it because it was never there before. */
	text = malloc((size_t)indent + 2);
	if (!text) {
		editor_nomem();
		return;
	}
	text[0] = '\n';
	memcpy(text + 1, row ? row->chars : "", (size_t)indent);
	editor_insert_text_at_point(text, indent + 1);
	free(text);
}

/* Insert a newline at point without advancing the cursor (C-o).
 * Splits the current line and leaves the cursor on the original line. */
void editor_open_line(void)
{
	int cy = wcur()->cy;
	int cx = wcur()->cx;
	int rowoff = wcur()->rowoff;
	int coloff = wcur()->coloff;

	editor_insert_newline();

	wcur()->cy = cy;
	wcur()->cx = cx;
	wcur()->rowoff = rowoff;
	wcur()->coloff = coloff;
}

/* ---- The edit transaction ---------------------------------------------
 * One replacement of a byte range by other bytes, which is the whole of
 * what a command may do to a buffer's text.  Every allocation the *text*
 * needs is made first -- the replaced bytes, the rows that replace them,
 * the row array's growth and the undo record; only once none of them can
 * fail does anything the buffer holds move.  A refused or failed edit
 * therefore leaves the text, the undo stack, the modified flag and the
 * content generation exactly as they were -- there is no partial edit to
 * observe or to unwind.
 *
 * The derived state is the stated exception: the commit calls
 * `editor_update_row()` on each row it published, which grows that row's
 * render and highlight buffers and can run out of memory there.  That
 * failure is `editor_nomem()`'s, not the transaction's -- the text is
 * already correct and the editor is on its way down -- but it is the
 * reason this is atomicity of the text rather than of everything.
 *
 * Follow-up Plan 03 grows the rest of the layer here: markers relocate in the
 * commit, decorations follow them, and one change event per committed
 * transaction is queued. */

/* Everything a replacement will publish, allocated before any of it is.
 * The commit hands `head` and the `rows` contents to the buffer and NULLs
 * what it took, so one cleanup serves the failure path and the success
 * path alike. */
struct edit_staged {
	char *head; /* new content of the first row the edit touches */
	int head_len;
	erow *rows; /* the `nl` rows that follow it */
	int nl;
	char *old_text; /* the bytes the edit removes, for undo */
	int old_len;
};

static void edit_staged_free(struct edit_staged *st)
{
	int i;

	for (i = 0; st->rows && i < st->nl; i++) {
		free(st->rows[i].chars);
	}
	free(st->rows);
	free(st->head);
	free(st->old_text);
	*st = (struct edit_staged) { 0 };
}

/* The `len` bytes at flat position `pos`, NUL-terminated, or NULL when
 * they cannot be allocated.  The caller has already established that the
 * span is inside the buffer. */
static char *buffer_copy_span(
    const struct editor_buffer *b, size_t pos, size_t len)
{
	char *out = edit_malloc(len + 1);
	size_t n = 0;
	int row, col;

	if (!out) {
		return NULL;
	}
	buffer_position_to_row_col(b, pos, &row, &col);
	while (n < len) {
		size_t take = (size_t)(b->row[row].size - col);

		if (take > len - n) {
			take = len - n;
		}
		memcpy(out + n, b->row[row].chars + col, take);
		n += take;
		if (n < len) {
			out[n++] = '\n';
			row++;
			col = 0;
		}
	}
	out[len] = '\0';
	return out;
}

/* The policy, one row per intent: between them these three answers are
 * the whole of what an edit's intent decides.  A table rather than a
 * chain of tests, for the same reason cmdtable is one -- reading the
 * policy off is reading four rows, and adding an intent is adding a
 * fifth rather than remembering every place that asks.
 *
 * The content generation is deliberately not a column: it advances for
 * every intent, because the bytes did change, whatever else follows. */
static const struct {
	unsigned char records_undo; /* transaction writes the record */
	unsigned char obeys_readonly; /* a read-only buffer refuses it */
	unsigned char marks_modified; /* the buffer comes out dirty */
} edit_policy[] = {
	[KG_EDIT_USER] = { 1, 1, 1 },
	[KG_EDIT_USER_PART] = { 0, 1, 1 },
	[KG_EDIT_REPLAY] = { 0, 0, 1 },
	[KG_EDIT_INTERNAL] = { 0, 0, 0 },
};

/* Count the change the edit just made.  The modified flag only advances
 * for text that was the user's to modify, so a listing or a process's
 * output can be rewritten without the mode line claiming unsaved work.
 * buffer_note_change() is the two together, which is what every
 * mutation that has not reached the gateway still wants. */
static void edit_note_change(
    struct editor_buffer *b, enum kg_edit_intent intent)
{
	b->content_generation++;
	b->dirty += edit_policy[intent].marks_modified;
}

static struct kg_edit edit_make(enum kg_edit_intent intent,
    struct editor_buffer *b, size_t begin, size_t end, const char *replacement,
    size_t replacement_len)
{
	return (struct kg_edit) { .buffer = b,
		.begin_byte = begin,
		.end_byte = end,
		.replacement = replacement,
		.replacement_len = replacement_len,
		.intent = intent };
}

struct kg_edit kg_edit_user(struct editor_buffer *b, size_t begin, size_t end,
    const char *replacement, size_t replacement_len)
{
	return edit_make(
	    KG_EDIT_USER, b, begin, end, replacement, replacement_len);
}

struct kg_edit kg_edit_user_part(struct editor_buffer *b, size_t begin,
    size_t end, const char *replacement, size_t replacement_len)
{
	return edit_make(
	    KG_EDIT_USER_PART, b, begin, end, replacement, replacement_len);
}

struct kg_edit kg_edit_replay(struct editor_buffer *b, size_t begin, size_t end,
    const char *replacement, size_t replacement_len)
{
	return edit_make(
	    KG_EDIT_REPLAY, b, begin, end, replacement, replacement_len);
}

struct kg_edit kg_edit_internal(struct editor_buffer *b, size_t begin,
    size_t end, const char *replacement, size_t replacement_len)
{
	return edit_make(
	    KG_EDIT_INTERNAL, b, begin, end, replacement, replacement_len);
}

/* Whether `e` names a range this buffer has, and may have replaced. */
static int edit_valid(const struct kg_edit *e)
{
	const struct editor_buffer *b = e->buffer;

	if (!b || !b->active || !e->replacement) {
		return 0;
	}
	if (b->readonly && edit_policy[e->intent].obeys_readonly) {
		return 0;
	}
	if (e->begin_byte > e->end_byte
	    || e->end_byte > buffer_byte_length(b)) {
		return 0;
	}
	return e->end_byte - e->begin_byte <= (size_t)INT_MAX
	    && e->replacement_len <= (size_t)INT_MAX;
}

/* A buffer with no rows and one holding a single empty row spell the same
 * zero bytes, so the transaction may make the second out of the first
 * without that counting as a change. */
static int edit_ensure_one_row(struct editor_buffer *b)
{
	if (b->numrows > 0) {
		return 1;
	}
	if (!editor_rows_reserve(&b->row, &b->row_capacity, 1)) {
		return 0;
	}
	b->row[0] = (erow) { 0 };
	b->row[0].chars = calloc(1, 1);
	if (!b->row[0].chars) {
		return 0;
	}
	b->numrows = 1;
	editor_update_row(b, &b->row[0]);
	return 1;
}

/* Allocate the replaced text, the rows that replace it, and the row-array
 * growth.  Returns 0 with nothing published; the caller frees `st`. */
static int edit_stage(const struct kg_edit *e, int r0, int c0, int r1, int c1,
    struct edit_staged *st)
{
	struct editor_buffer *b = e->buffer;
	int len = (int)e->replacement_len;

	st->old_len = (int)(e->end_byte - e->begin_byte);
	st->old_text = buffer_copy_span(b, e->begin_byte, (size_t)st->old_len);
	st->nl = splice_newlines(e->replacement, len);
	if (!st->old_text
	    || !splice_alloc(e->replacement, len, st->nl, b->row[r0].chars, c0,
		b->row[r1].chars + c1, b->row[r1].size - c1, &st->head,
		&st->head_len, &st->rows)) {
		return 0;
	}
	/* Reserving after the rows are built is what lets them be built
	 * from `b->row`: this call may move that array. */
	return edit_rows_reserve(
	    &b->row, &b->row_capacity, b->numrows + st->nl - (r1 - r0));
}

/* The undo record this edit owes, or 1 when it owes none.  The record's
 * own two allocations are undo.c's, so the failure seam stands in for
 * them here: what the transaction has to answer for is a record it could
 * not write, however the writing failed. */
static int edit_record_undo(
    const struct kg_edit *e, const struct edit_staged *st)
{
	if (!edit_policy[e->intent].records_undo) {
		return 1;
	}
	return edit_alloc_ok()
	    && undo_push_change(e->buffer, e->begin_byte, st->old_text,
		st->old_len, (int)e->replacement_len);
}

/* Whether the replacement is the bytes that are already there.  Staging
 * has copied them, so this is the comparison and not a second walk of
 * the buffer.  Nothing follows from such an edit: no record, no
 * modified flag, no generation step, and -- once Plan 03 lands the event
 * queue -- nothing for a reader to be told about. */
static int edit_is_noop(const struct kg_edit *e, const struct edit_staged *st)
{
	return (size_t)st->old_len == e->replacement_len
	    && memcmp(st->old_text, e->replacement, e->replacement_len) == 0;
}

/* Put the staged rows in place of rows [r0, r1].  Nothing here can fail
 * the edit, which is the whole point of everything above it; the trailing
 * `editor_update_row()` pass rebuilds derived state and can still run out
 * of memory, which quits rather than unwinding (see the section header). */
static void edit_publish(
    struct editor_buffer *b, int r0, int r1, struct edit_staged *st)
{
	int old_span = r1 - r0 + 1;
	int new_span = st->nl + 1;
	/* Row r0 is replaced in place, whatever the topology around it, so
	 * its render and highlight storage outlives the text it described:
	 * both carry a capacity, and the update below refills them without
	 * allocating when the new content is no wider than the old.  That
	 * is the whole of a one-row edit's derived-state cost -- freeing
	 * the pair here charged two megabytes per keystroke in a megabyte
	 * row.  Rows r0+1..r1 are going away, so they are freed outright. */
	erow derived = b->row[r0];
	int last;
	int i;

	free(b->row[r0].chars);
	for (i = r0 + 1; i <= r1; i++) {
		editor_free_row(&b->row[i]);
	}
	if (new_span != old_span) {
		memmove(b->row + r0 + new_span, b->row + r1 + 1,
		    sizeof(*b->row) * (size_t)(b->numrows - r1 - 1));
		b->numrows += new_span - old_span;
	}
	b->row[r0] = (erow) { .idx = r0,
		.size = st->head_len,
		.chars = st->head,
		.render = derived.render,
		.hl = derived.hl,
		.render_capacity = derived.render_capacity,
		.hl_capacity = derived.hl_capacity };
	if (st->nl) {
		memcpy(b->row + r0 + 1, st->rows,
		    sizeof(*st->rows) * (size_t)st->nl);
	}
	free(st->rows);
	st->rows = NULL;
	st->head = NULL;
	/* The staged rows arrive with no number of their own, so the rows
	 * the edit installed are always renumbered.  Only a change in row
	 * count renumbers what is *below* the edit as well, which is what
	 * keeps an in-row edit off an O(rows) walk. */
	last = new_span != old_span ? b->numrows : r0 + new_span;
	for (i = r0; i < last; i++) {
		b->row[i].idx = i;
	}
	for (i = r0; i < r0 + new_span; i++) {
		editor_update_row(b, &b->row[i]);
	}
}

/* Replace the bytes [begin_byte, end_byte) of `e->buffer` with
 * `e->replacement`, as one step: one undo record, one step of the
 * modified flag and the content generation, one row rebuild per row the
 * result has.  What of that the edit's intent actually asks for is
 * edit_records_undo() and friends above; see enum kg_edit_intent.
 *
 * Returns 0 with the buffer byte-identical when the range is not one the
 * buffer has, the intent has no authority over a read-only buffer, or
 * memory runs out.  `out` may be NULL, and is zeroed on a refusal -- the
 * two generations are equal either way, which is the thing a caller asks
 * it, and they are equal for a replacement by identical bytes too. */
int kg_buffer_replace(const struct kg_edit *e, struct kg_edit_result *out)
{
	struct editor_buffer *b = e->buffer;
	struct edit_staged st = { 0 };
	int r0, c0, r1, c1;

	if (out) {
		*out = (struct kg_edit_result) { 0 };
	}
	if (!edit_valid(e) || !edit_ensure_one_row(b)) {
		return 0;
	}
	if (out) {
		out->old_length = buffer_byte_length(b);
		out->before_generation = b->content_generation;
		out->new_length = out->old_length;
		out->after_generation = out->before_generation;
	}
	buffer_position_to_row_col(b, e->begin_byte, &r0, &c0);
	buffer_position_to_row_col(b, e->end_byte, &r1, &c1);
	if (!edit_stage(e, r0, c0, r1, c1, &st)) {
		edit_staged_free(&st);
		editor_nomem();
		return 0;
	}
	if (edit_is_noop(e, &st)) {
		edit_staged_free(&st);
		return 1;
	}
	if (!edit_record_undo(e, &st)) {
		edit_staged_free(&st);
		editor_nomem();
		return 0;
	}
	edit_publish(b, r0, r1, &st);
	edit_staged_free(&st);
	edit_note_change(b, e->intent);
	if (out) {
		out->new_length = buffer_byte_length(b);
		out->after_generation = b->content_generation;
	}
	return 1;
}

/* Fetch point's row and column for the editing commands.  Returns 0 when
 * point sits past the last row, where there is nothing to edit. */
static int editor_point_row(int *filerow, erow **row, int *filecol)
{
	if (editor_current_filerow_or_eof() >= bcur()->numrows) {
		return 0;
	}
	*filerow = editor_current_filerow();
	*row = &bcur()->row[*filerow];
	*filecol = editor_current_filecol();
	return 1;
}

/* Whether [at, at + delete_len) names bytes of `filerow` that may be
 * replaced by `insert_len` bytes, and what the row's size would become.
 * Returns 0 for a range no row has, or a result that does not fit. */
static int row_replace_range_valid(
    int filerow, int at, int delete_len, int insert_len, int *new_size)
{
	erow *row;

	if (filerow < 0 || filerow >= bcur()->numrows || at < 0
	    || delete_len < 0 || insert_len < 0) {
		return 0;
	}
	row = &bcur()->row[filerow];
	if (at > row->size || delete_len > row->size - at) {
		return 0;
	}
	return checked_add_int_size(
	    new_size, row->size - delete_len, (size_t)insert_len);
}

/* Replace the `delete_len` bytes at (filerow, at) with the `insert_len`
 * bytes at `insert` -- one row rebuild, one dirty step and one undo
 * record, whatever the lengths are.  `insert` must be non-NULL; either
 * length may be zero.  `intent` says whose edit it is; see enum
 * kg_edit_intent.  Returns 0 with the row byte-identical when the range
 * is bogus, the resulting size does not fit an int, or memory runs out:
 * the capacity is reserved and the undo payload copied before anything
 * moves.
 *
 * Replacing bytes by the same bytes -- including nothing by nothing --
 * is a well-formed request that changes none of them, so it costs
 * neither a modification nor an undo step.  Emacs answers the same way,
 * and query-replace-regexp of a zero-width pattern by the empty string
 * is the caller that asks it for every glyph. */
int editor_row_replace_range(int filerow, int at, int delete_len,
    const char *insert, int insert_len, enum kg_edit_intent intent)
{
	size_t pos;
	struct kg_edit e;
	int new_size;

	if (!row_replace_range_valid(
		filerow, at, delete_len, insert_len, &new_size)) {
		return 0;
	}
	/* A range inside one row is a byte range like any other, so this is
	 * the edit transaction with the row bounds already checked -- which
	 * is the checking the transaction cannot do, since a flat position
	 * clamps where a row column is refused. */
	pos = buffer_row_col_to_position(bcur(), filerow, at);
	e = edit_make(intent, bcur(), pos, pos + (size_t)delete_len, insert,
	    (size_t)insert_len);
	return kg_buffer_replace(&e, NULL);
}

/* ---- The staged row builder --------------------------------------------
 * See edit.h for why this exists alongside the byte-range transaction
 * above: building a buffer's whole content from nothing is not editing
 * its old content, so it is not shaped like a replacement of one span by
 * another. */

/* Append one row of `len` bytes to `*rows`.  `at` is what the row's index
 * becomes; the same value the caller already has as `*numrows`, before
 * `*numrows` is `at + 1`.  errno is EOVERFLOW when `len` cannot be an
 * erow.size or the resulting row count cannot be reached, ENOMEM when an
 * allocation fails; `*rows` is untouched either way. */
int kg_row_builder_add_line(
    erow **rows, int *numrows, int *row_capacity, const char *s, size_t len)
{
	int at = *numrows;
	int new_numrows;
	int len_int;
	size_t alloc_sz;
	char *newchars;

	if (!checked_size_to_int(&len_int, len)
	    || !checked_add_int_size(&new_numrows, at, 1)
	    || !checked_add_size_t(&alloc_sz, len, 1)) {
		errno = EOVERFLOW;
		return 0;
	}
	newchars = malloc(alloc_sz);
	if (!newchars) {
		errno = ENOMEM;
		return 0;
	}
	memcpy(newchars, s, len);
	newchars[len] = '\0';

	if (!editor_rows_reserve(rows, row_capacity, new_numrows)) {
		free(newchars);
		errno = ENOMEM;
		return 0;
	}
	(*rows)[at] = (erow) { .idx = at, .size = len_int, .chars = newchars };
	*numrows = new_numrows;
	return 1;
}

int kg_row_builder_highlight(
    erow *rows, int numrows, struct editor_syntax *syntax)
{
	struct editor_buffer staged = { 0 };
	int saved_running = running;
	int i;

	staged.row = rows;
	staged.syntax = syntax;
	for (i = 0; i < numrows; i++) {
		staged.numrows = i + 1;
		editor_update_row(&staged, &rows[i]);
		if (!rows[i].render || running != saved_running) {
			running = saved_running;
			errno = ENOMEM;
			return -1;
		}
	}
	return 0;
}

void kg_row_builder_free(erow **rows, int *numrows, int *row_capacity)
{
	int i;

	for (i = 0; i < *numrows; i++) {
		editor_free_row(&(*rows)[i]);
	}
	free(*rows);
	*rows = NULL;
	*numrows = 0;
	*row_capacity = 0;
}

void kg_buffer_adopt_rows(
    struct editor_buffer *b, erow **rows, int *numrows, int *row_capacity)
{
	editor_free_all_rows(b);
	b->row = *rows;
	b->numrows = *numrows;
	b->row_capacity = *row_capacity;
	*rows = NULL;
	*numrows = 0;
	*row_capacity = 0;

	b->dirty = 0;
	b->content_generation++;
}

void kg_buffer_append_internal(
    struct editor_buffer *b, const char *text, size_t len)
{
	const char *p = text;
	const char *end = text + len;

	if (len == 0) {
		return;
	}
	if (b->numrows == 0) {
		editor_insert_row(b, 0, "", 0);
	}
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		erow *row = &b->row[b->numrows - 1];

		if (!nl) {
			editor_row_append_string(
			    row, (char *)p, (size_t)(end - p));
			p = end;
		} else {
			editor_row_append_string(
			    row, (char *)p, (size_t)(nl - p));
			editor_insert_row(b, b->numrows, "", 0);
			p = nl + 1;
		}
	}
	/* editor_insert_row()/editor_row_append_string() each note their own
	 * change as they go, the same as they would for typed text -- so
	 * dirty is forced back to zero here rather than left to their count:
	 * process output was never the user's to save, whatever it took to
	 * lay the rows out. */
	b->dirty = 0;
}

/* Remove `len` bytes at (filerow, col) as one undoable step.  Returns 0
 * and leaves the buffer untouched when the range is bogus or the undo
 * payload cannot be recorded, so a failed record never costs text.
 *
 * The record is an UNDO_REPLACE_TEXT whose replacement length is zero:
 * its reverse skips the delete and re-inserts the saved bytes, which is
 * exactly delete-undo.  A per-byte deleted-character record cannot serve,
 * since one keystroke removes a whole glyph and undo must put every byte
 * of it back in one step. */
static int editor_delete_span_with_undo(int filerow, int col, int len)
{
	/* A delete is a replacement by nothing, so the range checking, the
	 * undo record and the single row rebuild are all already written --
	 * including what a `len` of zero or below means, which this must not
	 * answer differently. */
	return editor_row_replace_range(filerow, col, len, "", 0, KG_EDIT_USER);
}

/* The same, for a span that crosses a row separator: the separator is a
 * byte of the buffer like any other, and removing it is what joins two
 * rows.  Addressed flat, because a row column cannot name it. */
static int editor_delete_span_at(size_t pos, size_t len)
{
	struct kg_edit e = kg_edit_user(bcur(), pos, pos + len, "", 0);

	return kg_buffer_replace(&e, NULL);
}

/* Delete the char at the current prompt position. */
void editor_del_char(void)
{
	erow *row;
	int filerow;
	int filecol;
	int glyph_start;

	if (!editor_point_row(&filerow, &row, &filecol)) {
		return;
	}
	if (!row || (filecol == 0 && filerow == 0)) {
		return;
	}
	if (filecol > row->size) {
		return;
	}
	if (filecol == 0) {
		/* At column 0 the byte before point is the separator above
		 * it, so backspace removing that byte is what joins the two
		 * rows -- no appending, no row deletion, no record saying
		 * which of the two happened. */
		int prev_end = bcur()->row[filerow - 1].size;
		size_t pos = buffer_row_col_to_position(bcur(), filerow, 0);

		if (editor_delete_span_at(pos - 1, 1)) {
			editor_cursor_goto(filerow - 1, prev_end);
		}
		return;
	}
	/* Backspace removes one whole character, however many bytes it
	 * spells; a malformed byte counts as its own one-byte glyph.  Point
	 * follows it to the glyph's first byte, so it never rests inside a
	 * UTF-8 sequence. */
	glyph_start = utf8_glyph_start_before(row->chars, row->size, filecol);
	if (editor_delete_span_with_undo(
		filerow, glyph_start, filecol - glyph_start)) {
		editor_cursor_goto(filerow, glyph_start);
	}
}

/* Forward-delete the char at the current cursor position (DEL key).
 * At end of line, joins with the next line. */
void editor_del_forward_char(void)
{
	erow *row;
	int filerow;
	int filecol;

	if (!editor_point_row(&filerow, &row, &filecol)) {
		return;
	}

	if (filecol > row->size) {
		return;
	}
	if (filecol == row->size) {
		/* At end of line the byte at point is the separator below
		 * it; removing it joins the next row on, and point does not
		 * move because it is already where the join happens. */
		if (filerow + 1 >= bcur()->numrows) {
			return;
		}
		editor_delete_span_at(
		    buffer_row_col_to_position(bcur(), filerow, filecol), 1);
		return;
	}
	/* Forward-delete takes the whole glyph at point and leaves point
	 * where it was — see editor_del_char() for the granularity rule. */
	editor_delete_span_with_undo(filerow, filecol,
	    utf8_glyph_span_at(row->chars, row->size, filecol));
}

void editor_refresh_readonly_state(void)
{
	bcur()->readonly = (bcur()->readonly_override >= 0)
	    ? bcur()->readonly_override
	    : bcur()->readonly_local;
}

void editor_set_local_readonly(int enabled)
{
	bcur()->readonly_local = enabled ? 1 : 0;
	editor_refresh_readonly_state();
}

void editor_toggle_read_only_mode(void)
{
	bcur()->readonly_override = !bcur()->readonly;
	editor_refresh_readonly_state();
	editor_set_status_message(bcur()->readonly ? "Read-only" : "Writable");
}

void editor_toggle_overwrite_mode(void)
{
	bcur()->overwrite_mode = !bcur()->overwrite_mode;
	editor_set_status_message(bcur()->overwrite_mode
		? "Overwrite mode enabled"
		: "Overwrite mode disabled");
}

void editor_overwrite_char(int c)
{
	int filerow = editor_current_filerow_or_eof();
	int filecol = editor_current_filecol();
	erow *row = (filerow >= bcur()->numrows) ? NULL : &bcur()->row[filerow];
	int old_len;

	if (!row || filecol >= row->size) {
		editor_insert_char(c);
		return;
	}

	old_len = utf8_glyph_span_at(row->chars, row->size, filecol);

	if (!undo_push(bcur(), UNDO_REPLACE_TEXT, filerow, filecol, 1,
		row->chars + filecol, old_len)) {
		editor_nomem();
		return;
	}

	row->chars[filecol] = (char)c;
	if (old_len > 1) {
		memmove(row->chars + filecol + 1,
		    row->chars + filecol + old_len,
		    row->size - filecol - old_len + 1);
		row->size -= (old_len - 1);
	}

	editor_update_row(bcur(), row);
	buffer_note_change(bcur());

	if (wcur()->cx == wcur()->w - 1) {
		wcur()->coloff++;
	} else {
		wcur()->cx++;
	}
}

/* Self-insert one whole multi-byte character, the `seq` bytes (`len` of
 * them, at least one — callers check) collected by
 * editor_read_utf8_seq() for a single keystroke.  The byte-at-a-time
 * path (editor_self_insert_char) cannot serve here: it would push one
 * UNDO_INSERT_CHAR per byte, so undo would peel a glyph apart and leave
 * the buffer holding invalid UTF-8.
 *
 * One replacement is therefore the whole command: the glyph at point in
 * overwrite mode, or nothing at all otherwise, becomes `seq`, and the
 * record the transaction writes covers every byte of it.  Point ends
 * `len` bytes further along — exactly where C-f over the glyph lands,
 * since wcur()->cx is a byte offset into row->chars.
 *
 * No autopair is multi-byte, so unlike editor_self_insert_char() there
 * is nothing for editor_insert_char_auto_complete() to do. */
void editor_self_insert_glyph(const char *seq, int len)
{
	int filerow = editor_current_filerow_or_eof();
	int filecol = editor_current_filecol();
	erow *row = (filerow >= bcur()->numrows) ? NULL : &bcur()->row[filerow];
	int old_len = 0;

	/* A rectangle mark puts point in virtual space past the end of its
	 * row, and point can sit below the last row.  Neither is a byte
	 * range, so both keep the padding insertion, and the record that
	 * covers the whole glyph with it. */
	if (!row || filecol > row->size) {
		if (undo_push(bcur(), UNDO_YANK_TEXT, filerow, filecol, 0, NULL,
			len)) {
			editor_insert_text_raw(seq, len);
		}
		return;
	}
	if (bcur()->overwrite_mode && filecol < row->size) {
		old_len = utf8_glyph_span_at(row->chars, row->size, filecol);
	}
	if (editor_row_replace_range(
		filerow, filecol, old_len, seq, len, KG_EDIT_USER)) {
		editor_cursor_goto(filerow, filecol + len);
	}
}

/* Kill (delete) from cursor to end of line (C-k).
 *
 * Invariant relied on by the C-u-batched kill in kbd.c: every byte removed
 * from the buffer here is also kill_ring_append()-ed in the same step.
 * Keep them in lock-step — diverging would silently corrupt undo replay
 * for `C-u N C-k`. */
void editor_kill_line(void)
{
	erow *row;
	int filerow;
	int filecol;

	if (!editor_point_row(&filerow, &row, &filecol)) {
		return;
	}

	if (filecol > row->size) {
		return;
	}
	if (filecol == row->size) {
		/* At end of line, join with next line like C-k in Emacs.
		 * The newline is the only byte that leaves the buffer: the
		 * next row's bytes stay, so the record undo needs is the
		 * one the transaction writes, describing that byte. */
		if (filerow + 1 < bcur()->numrows) {
			kill_ring_append("\n", 1);
			editor_delete_span_at(buffer_row_col_to_position(
						  bcur(), filerow, filecol),
			    1);
		}
		return;
	}
	/* Otherwise the rest of the line, to the kill ring and out of the
	 * buffer in one step. */
	if (row->size > filecol) {
		kill_ring_append(row->chars + filecol, row->size - filecol);
		editor_row_replace_range(
		    filerow, filecol, row->size - filecol, "", 0, KG_EDIT_USER);
	}
}

/* Transpose characters around point (C-t), following Emacs' newline and
 * end-of-line cases.  kg stores rows as bytes, so this swaps UTF-8 codepoint
 * byte spans rather than single bytes. */
void editor_transpose_chars(void)
{
	struct kg_edit edit;
	char *buf, *repl;
	int len, point, a_start, b_start, a_len, b_len, b_end, span_len;
	int row, col;

	if (bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}

	buf = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
	if (!buf) {
		return;
	}
	if (len < 2) {
		goto done;
	}

	/* `buf` is the buffer's bytes in exactly the form the flat position
	 * layer addresses, so point converts straight across -- clamped
	 * there, which is why nothing is re-clamped here. */
	point = (int)buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());
	while (point > 0 && point < len
	    && utf8_is_cont((unsigned char)buf[point])) {
		point--;
	}

	if (point >= len || buf[point] == '\n') {
		b_start = utf8_prev_glyph_start(buf, point);
		if (b_start < 0) {
			goto done;
		}
		a_start = utf8_prev_glyph_start(buf, b_start);
		if (a_start < 0) {
			goto done;
		}
		b_len = point - b_start;
	} else {
		b_start = point;
		a_start = utf8_prev_glyph_start(buf, b_start);
		if (a_start < 0) {
			goto done;
		}
		b_len = utf8_next_glyph_len(buf, len, b_start);
	}
	a_len = b_start - a_start;
	b_end = b_start + b_len;
	span_len = b_end - a_start;

	repl = malloc(span_len);
	if (!repl) {
		editor_nomem();
		goto done;
	}
	memcpy(repl, buf + b_start, b_len);
	memcpy(repl + b_len, buf + a_start, a_len);

	/* The two glyphs swapped is a replacement of the span they cover,
	 * so the transaction does the rest: it copies the original bytes
	 * for undo, splices only the row or two the span crosses, and
	 * counts one change.  This used to rebuild every row in the buffer
	 * from a second copy of the whole text. */
	edit = (struct kg_edit) {
		.buffer = bcur(),
		.begin_byte = (size_t)a_start,
		.end_byte = (size_t)b_end,
		.replacement = repl,
		.replacement_len = (size_t)span_len,
	};
	if (kg_buffer_replace(&edit, NULL)) {
		buffer_position_to_row_col(bcur(), (size_t)b_end, &row, &col);
		editor_cursor_goto(row, col);
	}
	free(repl);
done:
	free(buf);
}
