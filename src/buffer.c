/* ======================= Editor rows implementation ======================= */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "localvars.h"

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
	if (editor.numrows <= 0) {
		return 0;
	}
	if (editor.rowoff >= editor.numrows
	    || editor.cy >= editor.numrows - editor.rowoff) {
		return editor.numrows;
	}
	return editor.rowoff + editor.cy;
}

int editor_current_filerow(void)
{
	int row = editor_current_filerow_or_eof();

	if (editor.numrows > 0 && row >= editor.numrows) {
		return editor.numrows - 1;
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

	if (editor_current_filerow_or_eof() >= editor.numrows) {
		return;
	}
	filecol = editor_current_filecol();
	rowlen = editor.row[editor_current_filerow()].size;
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
 * in a stable, centered view.  `col` is a rendered column, matching the
 * display/search convention used by coloff and HL_MATCH placement. */
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
		max_rowoff = editor.numrows - editor.screenrows;
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

/* Update the rendered version and the syntax highlight of a row. */
void editor_update_row(erow *row)
{
	unsigned int tabs = 0;
	unsigned long long allocsize;
	int j, idx, render_cap, vcol;

	/* Create a version of the row we can directly print on the screen,
	 * respecting tabs. */
	free(row->render);
	row->render = NULL;
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

	row->render = malloc((size_t)allocsize);
	if (!row->render) {
		editor_nomem();
		return;
	}
	render_cap = (int)allocsize;
	idx = 0;
	vcol = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == TAB) {
			int spaces = tab_stop_advance(vcol);

			if (idx + spaces >= render_cap) {
				free(row->render);
				row->render = NULL;
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
	editor_update_syntax(row);
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editor_insert_row(int at, const char *s, size_t len)
{
	erow *newrows;
	char *newchars;

	if (at > editor.numrows) {
		return;
	}

	newchars = malloc(len + 1);
	if (!newchars) {
		editor_nomem();
		return;
	}
	memcpy(newchars, s, len + 1);

	newrows = realloc(editor.row, sizeof(erow) * (editor.numrows + 1));
	if (!newrows) {
		free(newchars);
		editor_nomem();
		return;
	}
	editor.row = newrows;
	if (at != editor.numrows) {
		memmove(editor.row + at + 1, editor.row + at,
		    sizeof(editor.row[0]) * (editor.numrows - at));
		for (int j = at + 1; j <= editor.numrows; j++) {
			editor.row[j].idx++;
		}
	}

	editor.row[at].size = len;
	editor.row[at].chars = newchars;
	editor.row[at].hl = NULL;
	editor.row[at].hl_oc = 0;
	editor.row[at].render = NULL;
	editor.row[at].rsize = 0;
	editor.row[at].idx = at;
	editor.numrows++;
	editor_update_row(editor.row + at);
	editor.dirty++;
}

/* Free row's heap allocated stuff. */
void editor_free_row(erow *row)
{
	free(row->render);
	free(row->chars);
	free(row->hl);
}

/* Free every row of the current buffer and leave it empty.  Used whenever
 * a buffer's contents are rebuilt from scratch. */
void editor_free_all_rows(void)
{
	int i;

	for (i = 0; i < editor.numrows; i++) {
		editor_free_row(&editor.row[i]);
	}
	free(editor.row);
	editor.row = NULL;
	editor.numrows = 0;
}

/* Remove the row at the specified position, shifting the remaining on the top.
 */
void editor_del_row(int at)
{
	erow *row;

	if (at >= editor.numrows) {
		return;
	}
	row = editor.row + at;
	editor_free_row(row);
	memmove(editor.row + at, editor.row + at + 1,
	    sizeof(editor.row[0]) * (editor.numrows - at - 1));
	for (int j = at; j < editor.numrows - 1; j++) {
		editor.row[j].idx--;
	}
	editor.numrows--;
	editor.dirty++;
	editor_rehighlight_from(at);
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

	for (r = 0; r < filerow && r < editor.numrows; r++) {
		off += editor.row[r].size + 1;
	}
	if (filerow < editor.numrows) {
		if (filecol > editor.row[filerow].size) {
			filecol = editor.row[filerow].size;
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

	if (editor.numrows <= 0 || row < 0) {
		return 0;
	}
	if (row >= editor.numrows) {
		row = editor.numrows - 1;
		col = editor.row[row].size;
	}
	for (i = 0; i < row; i++) {
		off += editor_row_byte_to_char(
			   &editor.row[i], editor.row[i].size)
		    + 1;
	}
	return off + editor_row_byte_to_char(&editor.row[row], col);
}

/* Inverse of editor_char_offset().  `*col` comes back as a byte index so it
 * can be handed straight to editor_cursor_goto(). */
void editor_offset_to_rowcol(long offset, int *row, int *col)
{
	int i;

	*row = 0;
	*col = 0;
	if (editor.numrows <= 0 || offset <= 0) {
		return;
	}
	for (i = 0; i < editor.numrows; i++) {
		long len = editor_row_byte_to_char(
		    &editor.row[i], editor.row[i].size);
		if (offset <= len || i == editor.numrows - 1) {
			if (offset > len) {
				offset = len;
			}
			*row = i;
			*col = editor_row_char_to_byte(
			    &editor.row[i], (int)offset);
			return;
		}
		offset -= len + 1;
	}
}

/* Total codepoints in the buffer, counting the '\n' separators — i.e. the
 * offset of the end of buffer. */
long editor_buffer_char_length(void)
{
	if (editor.numrows <= 0) {
		return 0;
	}
	return editor_char_offset(editor.numrows - 1, INT_MAX);
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
	editor_update_row(row);
	return 1;
}

static void editor_replace_rows_from_text(const char *text, int len)
{
	int start = 0;
	int i;
	int row_count = 1;

	editor_free_all_rows();

	for (i = 0; i < len; i++) {
		if (text[i] == '\n') {
			row_count++;
		}
	}
	editor.row = calloc(row_count, sizeof(erow));
	if (!editor.row) {
		editor_nomem();
		return;
	}

	for (i = 0; i < len; i++) {
		if (text[i] == '\n') {
			erow *row = &editor.row[editor.numrows];
			if (!editor_init_row(
				row, editor.numrows, text + start, i - start)) {
				return;
			}
			editor.numrows++;
			start = i + 1;
		}
	}
	if (!editor_init_row(&editor.row[editor.numrows], editor.numrows,
		text + start, len - start)) {
		return;
	}
	editor.numrows++;
}

static int editor_insert_text_raw_bulk(const char *text, int insert_len)
{
	char *buf, *newbuf;
	int len, new_len, point, row, col;

	buf = editor_rows_to_string(editor.row, editor.numrows, &len);
	if (!buf) {
		return 0;
	}
	if (len > INT_MAX - insert_len - 1) {
		free(buf);
		editor_nomem();
		return 0;
	}

	point = editor_current_flat_offset();
	if (point < 0) {
		point = 0;
	}
	if (point > len) {
		point = len;
	}

	new_len = len + insert_len;
	newbuf = malloc(new_len + 1);
	if (!newbuf) {
		free(buf);
		editor_nomem();
		return 0;
	}
	memcpy(newbuf, buf, point);
	memcpy(newbuf + point, text, insert_len);
	memcpy(newbuf + point + insert_len, buf + point, len - point);
	newbuf[new_len] = '\0';

	editor_replace_rows_from_text(newbuf, new_len);
	editor_flat_offset_to_row_col(
	    newbuf, new_len, point + insert_len, &row, &col);
	editor_cursor_goto(row, col);
	editor.dirty++;

	free(newbuf);
	free(buf);
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
	editor_update_row(row);
	editor.dirty++;
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
	editor_update_row(row);
	editor.dirty++;
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
	editor_update_row(row);
	editor.dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editor_row_del_char(erow *row, int at)
{
	if (row->size <= at) {
		return;
	}
	memmove(row->chars + at, row->chars + at + 1, row->size - at);
	row->size--;
	editor_update_row(row);
	editor.dirty++;
}

/* Insert the specified char at the current prompt position. */
void editor_insert_char(int c)
{
	erow *row;
	int filerow;
	int filecol;

	filerow = editor_current_filerow_or_eof();
	filecol = editor_current_filecol();
	row = (filerow >= editor.numrows) ? NULL : &editor.row[filerow];

	/* If the row where the cursor is currently located does not exist in
	 * our logical representation of the file, add enough empty rows as
	 * needed. */
	if (!row) {
		if (!editor.row && editor.numrows > 0) {
			editor_nomem();
			return;
		}
		while (editor.numrows <= filerow) {
			editor_insert_row(editor.numrows, "", 0);
		}
		if (editor.numrows <= filerow) {
			return;
		}
	}
	if (filerow >= editor.numrows) {
		return;
	}
	row = &editor.row[filerow];
	if (!editor.rect_mode && filecol > row->size) {
		filecol = row->size;
		editor_cursor_goto(filerow, filecol);
	}
	if (editor_virtual_insert_gap_too_large(row, filecol)) {
		editor_nomem();
		return;
	}

	/* Record undo operation */
	undo_push(UNDO_INSERT_CHAR, filerow, filecol, c, NULL, 0);

	editor_row_insert_char(row, filecol, c);
	if (editor.cx == editor.screencols - 1) {
		editor.coloff++;
	} else {
		editor.cx++;
	}
	editor.dirty++;
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

	if (filerow >= editor.numrows) {
		undo_push(UNDO_INSERT_LINE, filerow, 0, 0, NULL, 0);
		editor_insert_row(filerow, "", 0);
		target_row = filerow;
	} else {
		row = &editor.row[filerow];
		if (filecol > row->size) {
			filecol = row->size;
		}
		rest_len = row->size - filecol;
		undo_push(UNDO_SPLIT_LINE, filerow, filecol, 0,
		    row->chars + filecol, rest_len);
		editor_insert_row(filerow + 1, row->chars + filecol, rest_len);
		row = &editor.row[filerow];
		row->chars[filecol] = '\0';
		row->size = filecol;
		editor_update_row(row);
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
			while (editor.numrows <= filerow) {
				editor_insert_row(editor.numrows, "", 0);
			}
			if (filerow >= editor.numrows) {
				break;
			}
			row = &editor.row[filerow];
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
	row = (filerow >= editor.numrows) ? NULL : &editor.row[filerow];

	if (!row) {
		if (filerow == editor.numrows) {
			editor_insert_row(filerow, "", 0);
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
		undo_push(UNDO_INSERT_LINE, filerow, 0, 0, NULL, 0);
		editor_insert_row(filerow, "", 0);
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
		undo_push(UNDO_SPLIT_LINE, filerow, filecol, 0,
		    row->chars + filecol, rest_len);
		editor_insert_row(filerow + 1, new_content, indent + rest_len);
		free(new_content);
		row = &editor.row[filerow];
		row->chars[filecol] = '\0';
		row->size = filecol;
		editor_update_row(row);
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
	if (editor_current_filerow_or_eof() >= editor.numrows) {
		return 0;
	}
	*filerow = editor_current_filerow();
	*row = &editor.row[*filerow];
	*filecol = editor_current_filecol();
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
	erow *row;

	if (filerow < 0 || filerow >= editor.numrows) {
		return 0;
	}
	row = &editor.row[filerow];
	if (col < 0 || len <= 0 || col > row->size || len > row->size - col) {
		return 0;
	}
	if (!undo_push(
		UNDO_REPLACE_TEXT, filerow, col, 0, row->chars + col, len)) {
		editor_nomem();
		return 0;
	}
	return editor_delete_text_range_raw(filerow, col, len);
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
		undo_push(UNDO_JOIN_LINE, filerow - 1,
		    editor.row[filerow - 1].size, 0, row->chars, row->size);
		filecol = editor.row[filerow - 1].size;
		editor_row_append_string(
		    &editor.row[filerow - 1], row->chars, row->size);
		editor_del_row(filerow);
		editor_cursor_goto(filerow - 1, filecol);
		editor.dirty++;
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
		if (filerow + 1 >= editor.numrows) {
			return;
		}
		undo_push(UNDO_JOIN_LINE, filerow, filecol, 0,
		    editor.row[filerow + 1].chars,
		    editor.row[filerow + 1].size);
		editor_row_append_string(row, editor.row[filerow + 1].chars,
		    editor.row[filerow + 1].size);
		editor_del_row(filerow + 1);
		editor.dirty++;
		return;
	}
	/* Forward-delete takes the whole glyph at point and leaves point
	 * where it was — see editor_del_char() for the granularity rule. */
	editor_delete_span_with_undo(filerow, filecol,
	    utf8_glyph_span_at(row->chars, row->size, filecol));
}

void editor_refresh_readonly_state(void)
{
	editor.readonly = (editor.readonly_override >= 0)
	    ? editor.readonly_override
	    : editor.readonly_local;
}

void editor_set_local_readonly(int enabled)
{
	editor.readonly_local = enabled ? 1 : 0;
	editor_refresh_readonly_state();
}

void editor_set_readonly_override(int enabled)
{
	editor.readonly_override = enabled ? 1 : 0;
	editor_refresh_readonly_state();
}

void editor_toggle_read_only_mode(void)
{
	editor.readonly_override = !editor.readonly;
	editor_refresh_readonly_state();
	editor_set_status_message(editor.readonly ? "Read-only" : "Writable");
}

void editor_toggle_overwrite_mode(void)
{
	editor.overwrite_mode = !editor.overwrite_mode;
	editor_set_status_message(editor.overwrite_mode
		? "Overwrite mode enabled"
		: "Overwrite mode disabled");
}

void editor_overwrite_char(int c)
{
	int filerow = editor_current_filerow_or_eof();
	int filecol = editor_current_filecol();
	erow *row = (filerow >= editor.numrows) ? NULL : &editor.row[filerow];
	int old_len;

	if (!row || filecol >= row->size) {
		editor_insert_char(c);
		return;
	}

	old_len = utf8_glyph_span_at(row->chars, row->size, filecol);

	if (!undo_push(UNDO_REPLACE_TEXT, filerow, filecol, 1,
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

	editor_update_row(row);
	editor.dirty++;

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
	erow *row = (filerow >= editor.numrows) ? NULL : &editor.row[filerow];
	int old_len = 0;

	if (editor.overwrite_mode && row && filecol < row->size) {
		old_len = utf8_glyph_span_at(row->chars, row->size, filecol);
	}
	if (old_len > 0) {
		if (!undo_push(UNDO_REPLACE_TEXT, filerow, filecol, len,
			row->chars + filecol, old_len)) {
			editor_nomem();
			return;
		}
		editor_delete_text_range_raw(filerow, filecol, old_len);
	} else if (!undo_push(UNDO_YANK_TEXT, filerow, filecol, 0, NULL, len)) {
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
		if (filerow + 1 < editor.numrows) {
			/* Save newline to kill ring */
			kill_ring_append("\n", 1);
			/* Record undo: the newline is what leaves the
			 * buffer.  The next row's bytes stay -- they are
			 * appended to this one -- so re-inserting a "\n"
			 * here splits them back off, while re-inserting
			 * the row itself would duplicate it. */
			undo_push(UNDO_KILL_TEXT, filerow, filecol, 0, "\n", 1);
			editor_row_append_string(row,
			    editor.row[filerow + 1].chars,
			    editor.row[filerow + 1].size);
			editor_del_row(filerow + 1);
		}
	} else {
		/* Delete from cursor to end of line and save to kill ring. */
		int kill_len = row->size - filecol;
		if (kill_len > 0) {
			kill_ring_append(row->chars + filecol, kill_len);
			/* Record undo operation */
			undo_push(UNDO_KILL_TEXT, filerow, filecol, 0,
			    row->chars + filecol, kill_len);
		}
		row->chars[filecol] = '\0';
		row->size = filecol;
		editor_update_row(row);
		editor.dirty++;
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

	if (editor.readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}

	buf = editor_rows_to_string(editor.row, editor.numrows, &len);
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
	undo_push(UNDO_REPLACE_TEXT, row, col, span_len, orig, span_len);
	editor_replace_rows_from_text(newbuf, len);
	editor_flat_offset_to_row_col(newbuf, len, b_end, &row, &col);
	editor_cursor_goto(row, col);
	editor.dirty++;

	free(orig);
	free(repl);
	free(newbuf);
done:
	free(buf);
}
