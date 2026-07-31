/* ======================= Editor rows implementation ======================= */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "localvars.h"
#include "perf.h"

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
 * for a NULL render on a row that never had one) still see it. */
void editor_update_row(struct editor_buffer *b, erow *row)
{
	unsigned int tabs = 0;
	unsigned long long allocsize;
	int j, idx, render_cap, vcol;

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
		rows[i].chars = malloc((size_t)alloc_sz);
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
	*head = malloc((size_t)alloc_sz);
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
	*rows = calloc((size_t)nl, sizeof(**rows));
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

/* Insert `text`, which holds at least one '\n', at point.
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
	struct kg_edit e = {
		.buffer = bcur(),
		.begin_byte = pos,
		.end_byte = pos,
		.replacement = text,
		.replacement_len = (size_t)insert_len,
		.options = KG_EDIT_NO_UNDO,
	};
	int row, col;

	if (!memchr(text, '\n', (size_t)insert_len)
	    || !kg_buffer_replace(&e, NULL)) {
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

void editor_row_insert_spaces(erow *row, int at, int len)
{
	char *spaces;

	if (len <= 0) {
		return;
	}
	spaces = malloc(len);
	if (!spaces) {
		editor_nomem();
		return;
	}
	memset(spaces, ' ', len);
	editor_row_insert_string(row, at, spaces, len);
	free(spaces);
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

/* Split the current line at the cursor without auto-indent.
 * Used by yank, kill-undo, and the paste-mode short-circuit in
 * editor_insert_newline to re-insert newlines exactly as typed.
 * Pushes the same UNDO_SPLIT_LINE / UNDO_INSERT_LINE records as
 * editor_insert_newline so a terminal paste stays reversible — yank
 * and undo replay both raise suppress_undo so the record is dropped
 * when they don't want it. */
void editor_insert_newline_raw(void)
{
	int filerow;
	int filecol;
	int rest_len;
	erow *row;
	int target_row;

	filerow = editor_current_filerow_or_eof();
	filecol = editor_current_filecol();

	if (filerow >= bcur()->numrows) {
		undo_push(bcur(), UNDO_INSERT_LINE, filerow, 0, 0, NULL, 0);
		editor_insert_row(bcur(), filerow, "", 0);
		target_row = filerow;
	} else {
		row = &bcur()->row[filerow];
		if (filecol > row->size) {
			filecol = row->size;
		}
		rest_len = row->size - filecol;
		undo_push(bcur(), UNDO_SPLIT_LINE, filerow, filecol, 0,
		    row->chars + filecol, rest_len);
		editor_insert_row(
		    bcur(), filerow + 1, row->chars + filecol, rest_len);
		row = &bcur()->row[filerow];
		row->chars[filecol] = '\0';
		row->size = filecol;
		editor_update_row(bcur(), row);
		target_row = filerow + 1;
	}
	wcur()->cx = 0;
	wcur()->coloff = 0;
	editor_cursor_goto(target_row, 0);
}

/* Insert text character by character without recording undo, using raw
 * newlines (no auto-indent).  Used by editor_yank and UNDO_KILL_TEXT. */
void editor_insert_text_raw(const char *text, int len)
{
	int saved = suppress_undo;
	int i = 0;

	if (len <= 0) {
		return;
	}
	suppress_undo = 1;
	if (!bcur()->rect_mode && memchr(text, '\n', len)) {
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
			if (!bcur()->rect_mode && filecol > row->size) {
				filecol = row->size;
				editor_cursor_goto(filerow, filecol);
			}
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
	int rest_len, indent = 0;
	char *new_content;
	int filerow;
	int filecol;
	erow *row;

	if (editor.paste_mode) {
		editor_insert_newline_raw();
		return;
	}

	filerow = editor_current_filerow_or_eof();
	filecol = editor_current_filecol();
	row = (filerow >= bcur()->numrows) ? NULL : &bcur()->row[filerow];

	if (!row) {
		if (filerow == bcur()->numrows) {
			editor_insert_row(bcur(), filerow, "", 0);
			goto fixcursor;
		}
		return;
	}
	/* If the cursor is over the current line size, we want to conceptually
	 * think it's just over the last character. */
	if (filecol >= row->size) {
		filecol = row->size;
	}
	if (filecol <= 0) {
		undo_push(bcur(), UNDO_INSERT_LINE, filerow, 0, 0, NULL, 0);
		editor_insert_row(bcur(), filerow, "", 0);
	} else {
		/* Compute leading whitespace of the current line for
		 * auto-indent. */
		while (indent < row->size
		    && (row->chars[indent] == ' '
			|| row->chars[indent] == TAB)) {
			indent++;
		}
		/* Don't indent past the split point. */
		if (indent > filecol) {
			indent = filecol;
		}

		/* Build new line: indent prefix + rest of split. */
		rest_len = row->size - filecol;
		new_content = malloc(indent + rest_len + 1);
		if (!new_content) {
			editor_nomem();
			return;
		}
		memcpy(new_content, row->chars, indent);
		memcpy(new_content + indent, row->chars + filecol, rest_len);
		new_content[indent + rest_len] = '\0';

		/* Record undo: save the original rest without the indent
		 * prefix. */
		undo_push(bcur(), UNDO_SPLIT_LINE, filerow, filecol, 0,
		    row->chars + filecol, rest_len);
		editor_insert_row(
		    bcur(), filerow + 1, new_content, indent + rest_len);
		free(new_content);
		row = &bcur()->row[filerow];
		row->chars[filecol] = '\0';
		row->size = filecol;
		editor_update_row(bcur(), row);
	}
fixcursor:
	if (wcur()->cy == wcur()->h - 1) {
		wcur()->rowoff++;
	} else {
		wcur()->cy++;
	}
	wcur()->cx = indent;
	wcur()->coloff = 0;
	if (wcur()->cx >= wcur()->w) {
		wcur()->coloff = indent - wcur()->w + 1;
		wcur()->cx = wcur()->w - 1;
	}
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
 * Plan 10 grows the rest of the layer here: markers relocate in the
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
	char *out = malloc(len + 1);
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

/* Whether `e` names a range this buffer has, and may have replaced. */
static int edit_valid(const struct kg_edit *e)
{
	const struct editor_buffer *b = e->buffer;
	unsigned quiet = KG_EDIT_NO_UNDO | KG_EDIT_REPLAY;

	if (!b || !b->active || !e->replacement) {
		return 0;
	}
	/* An edit the user did not ask for -- a load, a rebuild, undo
	 * putting a record back -- is not what read-only is about. */
	if (b->readonly && !(e->options & quiet)) {
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
	return editor_rows_reserve(
	    &b->row, &b->row_capacity, b->numrows + st->nl - (r1 - r0));
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
	int last;
	int i;

	for (i = r0; i <= r1; i++) {
		editor_free_row(&b->row[i]);
	}
	if (new_span != old_span) {
		memmove(b->row + r0 + new_span, b->row + r1 + 1,
		    sizeof(*b->row) * (size_t)(b->numrows - r1 - 1));
		b->numrows += new_span - old_span;
	}
	b->row[r0]
	    = (erow) { .idx = r0, .size = st->head_len, .chars = st->head };
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
 * `e->replacement`, as one step: one undo record, one bump of the
 * modified flag and the content generation, one row rebuild per row the
 * result has.  Returns 0 with the buffer byte-identical when the range is
 * not one the buffer has, the buffer is read-only, or memory runs out.
 * `out` may be NULL, and is zeroed on a refusal -- the two generations
 * are equal either way, which is the thing a caller asks it. */
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
	if (!edit_stage(e, r0, c0, r1, c1, &st)
	    || (!(e->options & (KG_EDIT_NO_UNDO | KG_EDIT_REPLAY))
		&& !undo_push_change(b, e->begin_byte, st.old_text, st.old_len,
		    (int)e->replacement_len))) {
		edit_staged_free(&st);
		editor_nomem();
		return 0;
	}
	edit_publish(b, r0, r1, &st);
	edit_staged_free(&st);
	buffer_note_change(b);
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
 * length may be zero.  `options` is a bitwise OR of enum edit_option, 0
 * for an ordinary edit.  Returns 0 with the row byte-identical when the
 * range is bogus, the resulting size does not fit an int, or memory runs
 * out: the capacity is reserved and the undo payload copied before
 * anything moves.
 *
 * Replacing nothing by nothing is a well-formed request that changes no
 * byte, so it costs neither a modification nor an undo step -- Emacs
 * answers the same way, and query-replace-regexp of a zero-width pattern
 * by the empty string is the caller that asks it for every glyph. */
int editor_row_replace_range(int filerow, int at, int delete_len,
    const char *insert, int insert_len, unsigned options)
{
	size_t pos;
	struct kg_edit e;
	int new_size;

	if (!row_replace_range_valid(
		filerow, at, delete_len, insert_len, &new_size)) {
		return 0;
	}
	if (delete_len == 0 && insert_len == 0) {
		return 1;
	}
	/* A range inside one row is a byte range like any other, so this is
	 * the edit transaction with the row bounds already checked -- which
	 * is the checking the transaction cannot do, since a flat position
	 * clamps where a row column is refused. */
	pos = buffer_row_col_to_position(bcur(), filerow, at);
	e = (struct kg_edit) {
		.buffer = bcur(),
		.begin_byte = pos,
		.end_byte = pos + (size_t)delete_len,
		.replacement = insert,
		.replacement_len = (size_t)insert_len,
		.options = options,
	};
	return kg_buffer_replace(&e, NULL);
}

/* Remove `len` bytes at (filerow, col) as one undoable step.  Returns 0
 * and leaves the buffer untouched when the range is bogus or the undo
 * payload cannot be recorded, so a failed record never costs text.
 *
 * The record is an UNDO_REPLACE_TEXT whose replacement length is zero:
 * its reverse skips the delete and re-inserts the saved bytes, which is
 * exactly delete-undo.  A per-byte UNDO_DELETE_CHAR cannot serve, since
 * one keystroke removes a whole glyph and undo must put every byte of it
 * back in one step. */
static int editor_delete_span_with_undo(int filerow, int col, int len)
{
	/* A delete is a replacement by nothing, so the range checking, the
	 * undo record and the single row rebuild are all already written --
	 * including what a `len` of zero or below means, which this must not
	 * answer differently. */
	return editor_row_replace_range(filerow, col, len, "", 0, 0);
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
		/* Handle the case of column 0, we need to move the current line
		 * on the right of the previous one. */
		/* Record undo: save the line that will be joined */
		undo_push(bcur(), UNDO_JOIN_LINE, filerow - 1,
		    bcur()->row[filerow - 1].size, 0, row->chars, row->size);
		filecol = bcur()->row[filerow - 1].size;
		editor_row_append_string(
		    &bcur()->row[filerow - 1], row->chars, row->size);
		editor_del_row(bcur(), filerow);
		editor_cursor_goto(filerow - 1, filecol);
		buffer_note_change(bcur());
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
		if (filerow + 1 >= bcur()->numrows) {
			return;
		}
		undo_push(bcur(), UNDO_JOIN_LINE, filerow, filecol, 0,
		    bcur()->row[filerow + 1].chars,
		    bcur()->row[filerow + 1].size);
		editor_row_append_string(row, bcur()->row[filerow + 1].chars,
		    bcur()->row[filerow + 1].size);
		editor_del_row(bcur(), filerow + 1);
		buffer_note_change(bcur());
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

void editor_set_readonly_override(int enabled)
{
	bcur()->readonly_override = enabled ? 1 : 0;
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
 * Undo granularity therefore follows yank, which is the other command
 * that inserts multi-byte text: one UNDO_YANK_TEXT record whose reverse
 * deletes `len` bytes forward from the insertion point, so a single
 * undo removes the whole character.  Insertion goes through
 * editor_insert_text_raw(), which suppresses the per-byte records and
 * leaves point `len` bytes further along — exactly where C-f over the
 * glyph lands, since wcur()->cx is a byte offset into row->chars.
 *
 * No autopair is multi-byte, so unlike editor_self_insert_char() there
 * is nothing for editor_insert_char_auto_complete() to do; overwrite
 * mode is honoured with the same UNDO_REPLACE_TEXT record
 * editor_overwrite_char() uses, only with a replacement longer than one
 * byte. */
void editor_self_insert_glyph(const char *seq, int len)
{
	int filerow = editor_current_filerow_or_eof();
	int filecol = editor_current_filecol();
	erow *row = (filerow >= bcur()->numrows) ? NULL : &bcur()->row[filerow];
	int old_len = 0;

	if (bcur()->overwrite_mode && row && filecol < row->size) {
		old_len = utf8_glyph_span_at(row->chars, row->size, filecol);
	}
	if (old_len > 0) {
		if (!undo_push(bcur(), UNDO_REPLACE_TEXT, filerow, filecol, len,
			row->chars + filecol, old_len)) {
			editor_nomem();
			return;
		}
		editor_delete_text_range_raw(filerow, filecol, old_len);
	} else if (!undo_push(bcur(), UNDO_YANK_TEXT, filerow, filecol, 0, NULL,
		       len)) {
		editor_nomem();
		return;
	}
	editor_insert_text_raw(seq, len);
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
		/* At end of line, join with next line like C-k in Emacs. */
		if (filerow + 1 < bcur()->numrows) {
			/* Save newline to kill ring */
			kill_ring_append("\n", 1);
			/* Record undo: the newline is what leaves the
			 * buffer.  The next row's bytes stay -- they are
			 * appended to this one -- so re-inserting a "\n"
			 * here splits them back off, while re-inserting
			 * the row itself would duplicate it. */
			undo_push(bcur(), UNDO_KILL_TEXT, filerow, filecol, 0,
			    "\n", 1);
			editor_row_append_string(row,
			    bcur()->row[filerow + 1].chars,
			    bcur()->row[filerow + 1].size);
			editor_del_row(bcur(), filerow + 1);
		}
	} else {
		/* Delete from cursor to end of line and save to kill ring. */
		int kill_len = row->size - filecol;
		if (kill_len > 0) {
			kill_ring_append(row->chars + filecol, kill_len);
			/* Record undo operation */
			undo_push(bcur(), UNDO_KILL_TEXT, filerow, filecol, 0,
			    row->chars + filecol, kill_len);
		}
		row->chars[filecol] = '\0';
		row->size = filecol;
		editor_update_row(bcur(), row);
		buffer_note_change(bcur());
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
