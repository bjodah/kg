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

static int editor_virtual_insert_gap_too_large(erow *row, int at)
{
	if (!row) {
		return 1;
	}
	return at > row->size && at - row->size > KG_MAX_VIRTUAL_INSERT_GAP;
}

int editor_current_filerow_or_eof(void)
{
	editor.rowoff = editor_nonnegative(editor.rowoff);
	editor.cy = editor_nonnegative(editor.cy);
	if (bcur()->numrows <= 0) {
		return 0;
	}
	if (editor.rowoff >= bcur()->numrows
	    || editor.cy >= bcur()->numrows - editor.rowoff) {
		return bcur()->numrows;
	}
	return editor.rowoff + editor.cy;
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
	editor.coloff = editor_nonnegative(editor.coloff);
	editor.cx = editor_nonnegative(editor.cx);
	return editor_saturating_add(editor.coloff, editor.cx);
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
		if (editor.coloff > rowlen) {
			editor.coloff = rowlen;
			editor.cx = 0;
		} else {
			editor.cx = rowlen - editor.coloff;
		}
	}
}

/* Scroll the viewport just enough to put (row, col) into view, then
 * land cursor on it.  Used by undo replay and region commands that need
 * to land on a target without otherwise centring the view. */
void editor_cursor_goto(int row, int col)
{
	if (row < editor.rowoff) {
		editor.rowoff = row;
		editor.cy = 0;
	} else if (row >= editor.rowoff + editor.screenrows) {
		editor.rowoff = row - editor.screenrows + 1;
		editor.cy = editor.screenrows - 1;
	} else {
		editor.cy = row - editor.rowoff;
	}
	if (col < editor.coloff) {
		editor.coloff = col;
		editor.cx = 0;
	} else if (col >= editor.coloff + editor.screencols) {
		editor.coloff = col - editor.screencols + 1;
		editor.cx = editor.screencols - 1;
	} else {
		editor.cx = col - editor.coloff;
	}
}

/* Keep (row, col) visible, but only recenter when it is currently off-screen.
 * Used by isearch so matches already visible in the window do not jerk the
 * viewport forward on every character typed, while distant matches still land
 * in a stable, centered view.  `col` is a byte offset into row->chars, the
 * same space editor.coloff + editor.cx is measured in — this lands point,
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

	if (row < editor.rowoff || row >= editor.rowoff + editor.screenrows) {
		editor.rowoff = row - editor.screenrows / 2;
		if (editor.rowoff < 0) {
			editor.rowoff = 0;
		}
		max_rowoff = bcur()->numrows - editor.screenrows;
		if (max_rowoff < 0) {
			max_rowoff = 0;
		}
		if (editor.rowoff > max_rowoff) {
			editor.rowoff = max_rowoff;
		}
	}
	editor.cy = row - editor.rowoff;
	if (editor.cy < 0) {
		editor.cy = 0;
	} else if (editor.cy >= editor.screenrows) {
		editor.cy = editor.screenrows - 1;
	}

	if (col < editor.coloff || col >= editor.coloff + editor.screencols) {
		editor.coloff = col - editor.screencols / 2;
		if (editor.coloff < 0) {
			editor.coloff = 0;
		}
	}
	editor.cx = col - editor.coloff;
	if (editor.cx < 0) {
		editor.cx = 0;
	} else if (editor.cx >= editor.screencols) {
		editor.cx = editor.screencols - 1;
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
	if (!checked_mul_size_t(&bytes, (size_t)newcap, sizeof(**rows))) {
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
	b->dirty++;
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
	b->dirty++;
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

static int editor_current_flat_offset(void)
{
	int filerow = editor_current_filerow_or_eof();
	int filecol = editor_current_filecol();
	int off = 0;
	int r;

	for (r = 0; r < filerow && r < bcur()->numrows; r++) {
		off += bcur()->row[r].size + 1;
	}
	if (filerow < bcur()->numrows) {
		if (filecol > bcur()->row[filerow].size) {
			filecol = bcur()->row[filerow].size;
		}
		off += filecol;
	}
	return off;
}

static void editor_flat_offset_to_row_col(
    const char *buf, int len, int off, int *row, int *col)
{
	int r = 0, c = 0, i;

	if (off < 0) {
		off = 0;
	}
	if (off > len) {
		off = len;
	}
	for (i = 0; i < off; i++) {
		if (buf[i] == '\n') {
			r++;
			c = 0;
		} else {
			c++;
		}
	}
	*row = r;
	*col = c;
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

/* ---- Codepoint offset conversions ------------------------------------
 * kg stores positions as (row, byte column); Emacs-shaped APIs address the
 * buffer by a single codepoint offset where every row separator counts as
 * one character.  These helpers convert between the two on demand; nothing
 * here caches, so callers pay a walk of the rows they cross. */

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
 * last one clamps to end of buffer; a negative row clamps to offset 0. */
long editor_char_offset(int row, int col)
{
	long off = 0;
	int i;

	if (bcur()->numrows <= 0 || row < 0) {
		return 0;
	}
	if (row >= bcur()->numrows) {
		row = bcur()->numrows - 1;
		col = bcur()->row[row].size;
	}
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

static int editor_init_row(erow *row, int idx, const char *s, int len)
{
	row->idx = idx;
	row->size = len;
	row->chars = malloc(row->size + 1);
	if (!row->chars) {
		editor_nomem();
		return 0;
	}
	memcpy(row->chars, s, row->size);
	row->chars[row->size] = '\0';
	editor_update_row(bcur(), row);
	return 1;
}

static void editor_replace_rows_from_text(const char *text, int len)
{
	int start = 0;
	int i;
	int row_count = 1;

	KG_PERF_INC(KG_PERF_BUFFER_REBUILD);
	editor_free_all_rows(bcur());

	for (i = 0; i < len; i++) {
		if (text[i] == '\n') {
			row_count++;
		}
	}
	bcur()->row = calloc(row_count, sizeof(erow));
	if (!bcur()->row) {
		editor_nomem();
		return;
	}
	bcur()->row_capacity = row_count;

	for (i = 0; i < len; i++) {
		if (text[i] == '\n') {
			erow *row = &bcur()->row[bcur()->numrows];
			if (!editor_init_row(row, bcur()->numrows, text + start,
				i - start)) {
				return;
			}
			bcur()->numrows++;
			start = i + 1;
		}
	}
	if (!editor_init_row(&bcur()->row[bcur()->numrows], bcur()->numrows,
		text + start, len - start)) {
		return;
	}
	bcur()->numrows++;
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

/* Allocate everything the splice will publish: the split row's new
 * content (`*head`, `*head_len` bytes: what was before point, plus the
 * text's first segment) and the `nl` rows that follow it (`*rows`).
 * Returns 0 having allocated nothing. */
static int splice_alloc(const char *text, int len, int nl, int filecol,
    char **head, int *head_len, erow **rows)
{
	erow *row = &bcur()->row[editor_current_filerow()];
	int first_seg
	    = (int)((const char *)memchr(text, '\n', (size_t)len) - text);
	int alloc_sz;

	if (!checked_add_int_size(head_len, filecol, (size_t)first_seg)
	    || !checked_add_int_size(&alloc_sz, *head_len, 1)) {
		return 0;
	}
	*head = malloc((size_t)alloc_sz);
	*rows = *head ? calloc((size_t)nl, sizeof(**rows)) : NULL;
	if (!*rows
	    || !splice_build_rows(text, len, nl, row->chars + filecol,
		row->size - filecol, *rows)) {
		free(*head);
		free(*rows);
		return 0;
	}
	memcpy(*head, row->chars, (size_t)filecol);
	memcpy(*head + filecol, text, (size_t)first_seg);
	(*head)[*head_len] = '\0';
	return 1;
}

/* Insert `text`, which holds at least one '\n', at point.
 *
 * A local splice: the row at point is cut in two, the inserted lines are
 * built, the row array is grown once and its tail moved once.  This used
 * to serialise the whole buffer, build a second copy of it with the text
 * inserted, and rebuild every row from that -- so pasting one line into
 * a 100k-line file rewrote all 100k of them.
 *
 * Failure is atomic: every allocation the new rows need is made before
 * any of them is published, so an out-of-memory paste leaves the buffer
 * exactly as it was. */
static int editor_insert_text_raw_bulk(const char *text, int insert_len)
{
	erow *newrows = NULL;
	erow *row;
	char *head = NULL;
	int filerow, filecol, nl, i, suffix_len, head_len = 0;

	nl = splice_newlines(text, insert_len);
	if (nl == 0) {
		return 0;
	}
	if (bcur()->numrows == 0) {
		editor_insert_row(bcur(), 0, "", 0);
		if (bcur()->numrows == 0) {
			return 0;
		}
	}
	filerow = editor_current_filerow();
	filecol = editor_current_filecol_in_row(&bcur()->row[filerow]);
	/* What follows point on the split row, which the last inserted row
	 * takes over -- and which is what makes that row longer than the
	 * text's last segment, so point lands before it. */
	suffix_len = bcur()->row[filerow].size - filecol;
	if (!splice_alloc(
		text, insert_len, nl, filecol, &head, &head_len, &newrows)) {
		editor_nomem();
		return 0;
	}
	if (!editor_rows_reserve(
		&bcur()->row, &bcur()->row_capacity, bcur()->numrows + nl)) {
		for (i = 0; i < nl; i++) {
			free(newrows[i].chars);
		}
		free(head);
		free(newrows);
		editor_nomem();
		return 0;
	}

	/* Nothing below here can fail. */
	row = &bcur()->row[filerow];
	memmove(bcur()->row + filerow + 1 + nl, bcur()->row + filerow + 1,
	    sizeof(*bcur()->row) * (size_t)(bcur()->numrows - filerow - 1));
	memcpy(
	    bcur()->row + filerow + 1, newrows, sizeof(*newrows) * (size_t)nl);
	free(newrows);
	bcur()->numrows += nl;
	free(row->chars);
	row->chars = head;
	row->size = head_len;

	for (i = filerow; i < bcur()->numrows; i++) {
		bcur()->row[i].idx = i;
	}
	for (i = filerow; i <= filerow + nl; i++) {
		editor_update_row(bcur(), &bcur()->row[i]);
	}
	editor_cursor_goto(
	    filerow + nl, bcur()->row[filerow + nl].size - suffix_len);
	bcur()->dirty++;
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
	bcur()->dirty++;
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
	bcur()->dirty++;
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
	bcur()->dirty++;
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
	bcur()->dirty++;
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
	if (!editor.rect_mode && filecol > row->size) {
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
	if (editor.cx == editor.screencols - 1) {
		editor.coloff++;
	} else {
		editor.cx++;
	}
	bcur()->dirty++;
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
	editor.cx = 0;
	editor.coloff = 0;
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
	if (!editor.rect_mode && memchr(text, '\n', len)) {
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
			if (!editor.rect_mode && filecol > row->size) {
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
	if (editor.cy == editor.screenrows - 1) {
		editor.rowoff++;
	} else {
		editor.cy++;
	}
	editor.cx = indent;
	editor.coloff = 0;
	if (editor.cx >= editor.screencols) {
		editor.coloff = indent - editor.screencols + 1;
		editor.cx = editor.screencols - 1;
	}
}

/* Insert a newline at point without advancing the cursor (C-o).
 * Splits the current line and leaves the cursor on the original line. */
void editor_open_line(void)
{
	int cy = editor.cy;
	int cx = editor.cx;
	int rowoff = editor.rowoff;
	int coloff = editor.coloff;

	editor_insert_newline();

	editor.cy = cy;
	editor.cx = cx;
	editor.rowoff = rowoff;
	editor.coloff = coloff;
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
	erow *row;
	char *newchars;
	int new_size, alloc_sz;

	if (!row_replace_range_valid(
		filerow, at, delete_len, insert_len, &new_size)) {
		return 0;
	}
	row = &bcur()->row[filerow];
	if (delete_len == 0 && insert_len == 0) {
		return 1;
	}
	if (!checked_add_int_size(&alloc_sz, new_size, 1)) {
		return 0;
	}
	if (new_size > row->size) {
		newchars = realloc(row->chars, (size_t)alloc_sz);
		if (!newchars) {
			editor_nomem();
			return 0;
		}
		row->chars = newchars;
	}
	if (!(options & KG_EDIT_NO_UNDO)
	    && !undo_push(bcur(), UNDO_REPLACE_TEXT, filerow, at, insert_len,
		row->chars + at, delete_len)) {
		editor_nomem();
		return 0;
	}
	memmove(row->chars + at + insert_len, row->chars + at + delete_len,
	    (size_t)(row->size - at - delete_len));
	memcpy(row->chars + at, insert, (size_t)insert_len);
	row->size = new_size;
	row->chars[new_size] = '\0';
	editor_update_row(bcur(), row);
	bcur()->dirty++;
	return 1;
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
		bcur()->dirty++;
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
		bcur()->dirty++;
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
	bcur()->dirty++;

	if (editor.cx == editor.screencols - 1) {
		editor.coloff++;
	} else {
		editor.cx++;
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
 * glyph lands, since editor.cx is a byte offset into row->chars.
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
		bcur()->dirty++;
	}
}

/* Transpose characters around point (C-t), following Emacs' newline and
 * end-of-line cases.  kg stores rows as bytes, so this swaps UTF-8 codepoint
 * byte spans rather than single bytes. */
void editor_transpose_chars(void)
{
	char *buf, *newbuf, *orig, *repl;
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

	point = editor_current_flat_offset();
	if (point < 0) {
		point = 0;
	}
	if (point > len) {
		point = len;
	}
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

	orig = malloc(span_len);
	repl = malloc(span_len);
	newbuf = malloc(len + 1);
	if (!orig || !repl || !newbuf) {
		free(orig);
		free(repl);
		free(newbuf);
		editor_nomem();
		goto done;
	}

	memcpy(orig, buf + a_start, span_len);
	memcpy(repl, buf + b_start, b_len);
	memcpy(repl + b_len, buf + a_start, a_len);
	memcpy(newbuf, buf, len + 1);
	memcpy(newbuf + a_start, repl, span_len);

	editor_flat_offset_to_row_col(buf, len, a_start, &row, &col);
	undo_push(
	    bcur(), UNDO_REPLACE_TEXT, row, col, span_len, orig, span_len);
	editor_replace_rows_from_text(newbuf, len);
	editor_flat_offset_to_row_col(newbuf, len, b_end, &row, &col);
	editor_cursor_goto(row, col);
	bcur()->dirty++;

	free(orig);
	free(repl);
	free(newbuf);
done:
	free(buf);
}
