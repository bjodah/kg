/* ======================= Editor rows implementation ======================= */

#include "def.h"

static void editor_nomem(void)
{
	editor_set_status_message("Out of memory");
	running = 0;
}

/* Visual column at byte offset `chars_col` in `row`.  Tabs use kg's
 * own stop convention (advance until (vcol+1) % 8 == 0) — same as the
 * render in editor_update_row and the cursor-placement loop in
 * editor_refresh_screen, so the two metrics agree.  UTF-8 continuation
 * bytes contribute zero width.  `chars_col` past row's end maps to
 * (row's visual width) plus the virtual offset, so cursors that sit
 * in virtual space (rect mode) get a well-defined visual column too. */
int editor_visual_col(erow *row, int chars_col)
{
	int j, vcol = 0;
	int limit = chars_col < row->size ? chars_col : row->size;

	for (j = 0; j < limit; j++) {
		if (row->chars[j] == TAB) {
			vcol++;
			while ((vcol + 1) % 8 != 0) vcol++;
		} else if (!utf8_is_cont((unsigned char)row->chars[j])) {
			vcol++;
		}
	}
	if (chars_col > row->size)
		vcol += chars_col - row->size;
	return vcol;
}

/* Inverse of editor_visual_col(): byte offset into row->chars whose
 * visual position lands at or just before `target_vcol`.  A target
 * that falls inside a tab's expansion snaps to the tab's start byte
 * (closest representable position when cx is a byte offset).  When
 * target is past the row's visual end, returns row->size plus the
 * matching virtual offset (the cursor lives in virtual space). */
int editor_chars_col_at_visual(erow *row, int target_vcol)
{
	int j = 0, vcol = 0;

	while (j < row->size) {
		int next_vcol = vcol;
		if (row->chars[j] == TAB) {
			next_vcol = vcol + 1;
			while ((next_vcol + 1) % 8 != 0) next_vcol++;
		} else if (!utf8_is_cont((unsigned char)row->chars[j])) {
			next_vcol = vcol + 1;
		}
		if (next_vcol > target_vcol) break;
		vcol = next_vcol;
		j++;
	}
	/* Past-EOL only fires when we walked the whole row without hitting
	 * target_vcol.  Breaking early means we hit a glyph (typically a
	 * tab) whose expansion would overshoot — round down to j. */
	if (j >= row->size && vcol < target_vcol)
		return row->size + (target_vcol - vcol);
	return j;
}

/* Bring cx back into a row-valid position.  rect_mode lets cx wander
 * past EOL during virtual-column rectangle navigation; this is the
 * matching snap-back used whenever rect_mode is cleared. */
void editor_snap_cx_to_row(void)
{
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;
	int rowlen;

	if (filerow >= editor.numrows) return;
	rowlen = editor.row[filerow].size;
	if (filecol > rowlen) {
		editor.cx -= filecol - rowlen;
		if (editor.cx < 0) {
			editor.coloff += editor.cx;
			editor.cx = 0;
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

	if (row < 0) row = 0;
	if (col < 0) col = 0;

	if (row < editor.rowoff || row >= editor.rowoff + editor.screenrows) {
		editor.rowoff = row - editor.screenrows / 2;
		if (editor.rowoff < 0)
			editor.rowoff = 0;
		max_rowoff = editor.numrows - editor.screenrows;
		if (max_rowoff < 0)
			max_rowoff = 0;
		if (editor.rowoff > max_rowoff)
			editor.rowoff = max_rowoff;
	}
	editor.cy = row - editor.rowoff;
	if (editor.cy < 0)
		editor.cy = 0;
	else if (editor.cy >= editor.screenrows)
		editor.cy = editor.screenrows - 1;

	if (col < editor.coloff || col >= editor.coloff + editor.screencols) {
		editor.coloff = col - editor.screencols / 2;
		if (editor.coloff < 0)
			editor.coloff = 0;
	}
	editor.cx = col - editor.coloff;
	if (editor.cx < 0)
		editor.cx = 0;
	else if (editor.cx >= editor.screencols)
		editor.cx = editor.screencols - 1;
}

/* Update the rendered version and the syntax highlight of a row. */
void editor_update_row(erow *row)
{
	unsigned int tabs = 0, nonprint = 0;
	unsigned long long allocsize;
	int j, idx, render_cap;

	/* Create a version of the row we can directly print on the screen,
	 * respecting tabs, substituting non printable characters with '?'. */
	free(row->render);
	row->render = NULL;
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == TAB) tabs++;

	allocsize = (unsigned long long)row->size + tabs*8 + nonprint*9 + 1;
	if (allocsize > UINT32_MAX) {
		editor_set_status_message("Line too long for editor");
		running = 0;
		return;
	}

	row->render = malloc(row->size + tabs*8 + nonprint*9 + 1);
	if (!row->render) {
		editor_nomem();
		return;
	}
	render_cap = row->size + tabs*8 + nonprint*9 + 1;
	idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == TAB) {
			int spaces = 7 - (idx % 8);

			if (spaces == 0)
				spaces = 8;

			if (idx + spaces >= render_cap) {
				free(row->render);
				row->render = NULL;
				editor_set_status_message("Line render overflow");
				running = 0;
				return;
			}
			memset(row->render + idx, ' ', spaces);
			idx += spaces;
		} else {
			row->render[idx++] = row->chars[j];
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

	if (at > editor.numrows)
		return;

	newchars = malloc(len+1);
	if (!newchars) {
		editor_nomem();
		return;
	}
	memcpy(newchars, s, len+1);

	newrows = realloc(editor.row, sizeof(erow) * (editor.numrows+1));
	if (!newrows) {
		free(newchars);
		editor_nomem();
		return;
	}
	editor.row = newrows;
	if (at != editor.numrows) {
		memmove(editor.row+at+1, editor.row+at, sizeof(editor.row[0]) * (editor.numrows-at));
		for (int j = at+1; j <= editor.numrows; j++)
			editor.row[j].idx++;
	}

	editor.row[at].size = len;
	editor.row[at].chars = newchars;
	editor.row[at].hl = NULL;
	editor.row[at].hl_oc = 0;
	editor.row[at].render = NULL;
	editor.row[at].rsize = 0;
	editor.row[at].idx = at;
	editor_update_row(editor.row+at);
	editor.numrows++;
	editor.dirty++;
}

/* Free row's heap allocated stuff. */
void editor_free_row(erow *row)
{
	free(row->render);
	free(row->chars);
	free(row->hl);
}

/* Remove the row at the specified position, shifting the remaining on the top. */
void editor_del_row(int at)
{
	erow *row;

	if (at >= editor.numrows) return;
	row = editor.row+at;
	editor_free_row(row);
	memmove(editor.row+at, editor.row+at+1, sizeof(editor.row[0]) * (editor.numrows-at-1));
	for (int j = at; j < editor.numrows-1; j++) editor.row[j].idx--;
	editor.numrows--;
	editor.dirty++;
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, excluding
 * the final nulterm. */
char *editor_rows_to_string(erow *rows, int numrows, int *buflen)
{
	char *buf = NULL, *p;
	int totlen = 0;
	int j;

	/* Compute count of bytes */
	for (j = 0; j < numrows; j++)
		totlen += rows[j].size;
	if (numrows > 1)
		totlen += numrows - 1; /* "\n" between rows */
	*buflen = totlen;
	totlen++; /* Also make space for nulterm */

	p = buf = malloc(totlen);
	if (!buf) {
		editor_nomem();
		*buflen = 0;
		return NULL;
	}
	for (j = 0; j < numrows; j++) {
		memcpy(p, rows[j].chars, rows[j].size);
		p += rows[j].size;
		if (j != numrows - 1)
			*p++ = '\n';
	}
	*p = '\0';
	return buf;
}

static int editor_current_flat_offset(void)
{
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;
	int off = 0;
	int r;

	for (r = 0; r < filerow && r < editor.numrows; r++)
		off += editor.row[r].size + 1;
	if (filerow < editor.numrows) {
		if (filecol < 0) filecol = 0;
		if (filecol > editor.row[filerow].size)
			filecol = editor.row[filerow].size;
		off += filecol;
	}
	return off;
}

static void editor_flat_offset_to_row_col(const char *buf, int len, int off,
                                          int *row, int *col)
{
	int r = 0, c = 0, i;

	if (off < 0) off = 0;
	if (off > len) off = len;
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

	while (start + n < len && utf8_is_cont((unsigned char)buf[start + n]))
		n++;
	return n;
}

static int utf8_prev_glyph_start(const char *buf, int pos)
{
	int start;

	if (pos <= 0)
		return -1;
	start = pos - 1;
	while (start > 0 && utf8_is_cont((unsigned char)buf[start]))
		start--;
	return start;
}

static void editor_replace_rows_from_text(const char *text, int len)
{
	int start = 0;
	int i;

	for (i = 0; i < editor.numrows; i++)
		editor_free_row(&editor.row[i]);
	free(editor.row);
	editor.row = NULL;
	editor.numrows = 0;

	for (i = 0; i < len; i++) {
		if (text[i] == '\n') {
			editor_insert_row(editor.numrows, text + start, i - start);
			start = i + 1;
		}
	}
	if (start <= len)
		editor_insert_row(editor.numrows, text + start, len - start);
}

/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editor_row_insert_char(erow *row, int at, int c)
{
	char *newchars;

	if (!row)
		return;
	if (at < 0)
		return;
	if (at > row->size) {
		/* Pad the string with spaces if the insert location is outside the
		 * current length by more than a single character. */
		int padlen = at - row->size;
		/* In the next line +2 means: new char and null term. */
		newchars = realloc(row->chars, row->size+padlen+2);
		if (!newchars) {
			editor_nomem();
			return;
		}
		row->chars = newchars;
		memset(row->chars+row->size, ' ', padlen);
		row->chars[row->size+padlen+1] = '\0';
		row->size += padlen+1;
	} else {
		/* If we are in the middle of the string just make space for 1 new
		 * char plus the (already existing) null term. */
		newchars = realloc(row->chars, row->size+2);
		if (!newchars) {
			editor_nomem();
			return;
		}
		row->chars = newchars;
		memmove(row->chars+at+1, row->chars+at, row->size-at+1);
		row->size++;
	}
	row->chars[at] = c;
	editor_update_row(row);
	editor.dirty++;
}

/* Append the string 's' at the end of a row */
void editor_row_append_string(erow *row, char *s, size_t len)
{
	char *newchars = realloc(row->chars, row->size+len+1);

	if (!newchars) {
		editor_nomem();
		return;
	}
	row->chars = newchars;
	memcpy(row->chars+row->size, s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editor_update_row(row);
	editor.dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editor_row_del_char(erow *row, int at)
{
	if (row->size <= at) return;
	memmove(row->chars+at, row->chars+at+1, row->size-at);
	editor_update_row(row);
	row->size--;
	editor.dirty++;
}

/* Insert the specified char at the current prompt position. */
void editor_insert_char(int c)
{
	erow *row = (editor.rowoff + editor.cy >= editor.numrows) ? NULL : &editor.row[editor.rowoff + editor.cy];
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;

	/* If the row where the cursor is currently located does not exist in our
	 * logical representation of the file, add enough empty rows as needed. */
	if (!row) {
		while (editor.numrows <= filerow)
			editor_insert_row(editor.numrows, "", 0);
		if (editor.numrows <= filerow)
			return;
	}
	row = (filerow >= editor.numrows) ? NULL : &editor.row[filerow];
	if (!row)
		return;

	/* Record undo operation */
	undo_push(UNDO_INSERT_CHAR, filerow, filecol, c, NULL, 0);

	editor_row_insert_char(row, filecol, c);
	if (editor.cx == editor.screencols - 1)
		editor.coloff++;
	else
		editor.cx++;
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
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;
	int rest_len;
	erow *row;

	if (filerow >= editor.numrows) {
		undo_push(UNDO_INSERT_LINE, filerow, 0, 0, NULL, 0);
		editor_insert_row(filerow, "", 0);
	} else {
		row = &editor.row[filerow];
		if (filecol > row->size) filecol = row->size;
		rest_len = row->size - filecol;
		undo_push(UNDO_SPLIT_LINE, filerow, filecol, 0,
		          row->chars + filecol, rest_len);
		editor_insert_row(filerow + 1, row->chars + filecol, rest_len);
		row = &editor.row[filerow];
		row->chars[filecol] = '\0';
		row->size = filecol;
		editor_update_row(row);
	}
	if (editor.cy == editor.screenrows - 1) editor.rowoff++;
	else editor.cy++;
	editor.cx = 0;
	editor.coloff = 0;
}

/* Insert text character by character without recording undo, using raw
 * newlines (no auto-indent).  Used by editor_yank and UNDO_KILL_TEXT. */
void editor_insert_text_raw(const char *text, int len)
{
	int saved = suppress_undo;
	int i;

	suppress_undo = 1;
	for (i = 0; i < len; i++) {
		if (text[i] == '\n')
			editor_insert_newline_raw();
		else {
			erow *row;
			int filerow = editor.rowoff + editor.cy;
			int filecol = editor.coloff + editor.cx;

			while (editor.numrows <= filerow)
				editor_insert_row(editor.numrows, "", 0);
			row = (filerow >= editor.numrows) ? NULL : &editor.row[filerow];
			if (!row)
				break;
			editor_row_insert_char(row, filecol, (unsigned char)text[i]);
			if (editor.cx == editor.screencols - 1)
				editor.coloff++;
			else
				editor.cx++;
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

	row = (editor.rowoff + editor.cy >= editor.numrows)
		? NULL : &editor.row[editor.rowoff + editor.cy];
	filerow = editor.rowoff + editor.cy;
	filecol = editor.coloff + editor.cx;

	if (!row) {
		if (filerow == editor.numrows) {
			editor_insert_row(filerow, "", 0);
			goto fixcursor;
		}
		return;
	}
	/* If the cursor is over the current line size, we want to conceptually
	 * think it's just over the last character. */
	if (filecol < 0) filecol = 0;
	if (filecol >= row->size) filecol = row->size;
	if (filecol <= 0) {
		undo_push(UNDO_INSERT_LINE, filerow, 0, 0, NULL, 0);
		editor_insert_row(filerow, "", 0);
	} else {
		/* Compute leading whitespace of the current line for auto-indent. */
		while (indent < row->size &&
		       (row->chars[indent] == ' ' || row->chars[indent] == TAB))
			indent++;
		/* Don't indent past the split point. */
		if (indent > filecol) indent = filecol;

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

		/* Record undo: save the original rest without the indent prefix. */
		undo_push(UNDO_SPLIT_LINE, filerow, filecol, 0, row->chars + filecol, rest_len);
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
	int cy     = editor.cy;
	int cx     = editor.cx;
	int rowoff = editor.rowoff;
	int coloff = editor.coloff;

	editor_insert_newline();

	editor.cy     = cy;
	editor.cx     = cx;
	editor.rowoff = rowoff;
	editor.coloff = coloff;
}

/* Delete the char at the current prompt position. */
void editor_del_char(void)
{
	erow *row = (editor.rowoff + editor.cy >= editor.numrows) ? NULL : &editor.row[editor.rowoff + editor.cy];
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;

	if (!row || (filecol == 0 && filerow == 0)) return;
	if (filecol == 0) {
		/* Handle the case of column 0, we need to move the current line
		 * on the right of the previous one. */
		/* Record undo: save the line that will be joined */
		undo_push(UNDO_JOIN_LINE, filerow-1, editor.row[filerow-1].size, 0, row->chars, row->size);
		filecol = editor.row[filerow-1].size;
		editor_row_append_string(&editor.row[filerow-1], row->chars, row->size);
		editor_del_row(filerow);
		row = NULL;
		if (editor.cy == 0)
			editor.rowoff--;
		else
			editor.cy--;
		editor.cx = filecol;
		if (editor.cx >= editor.screencols) {
			int shift = (editor.screencols - editor.cx) + 1;
			editor.cx -= shift;
			editor.coloff += shift;
		}
	} else {
		/* Record undo: save the character being deleted */
		undo_push(UNDO_DELETE_CHAR, filerow, filecol-1, row->chars[filecol-1], NULL, 0);
		editor_row_del_char(row, filecol-1);
		if (editor.cx == 0 && editor.coloff)
			editor.coloff--;
		else
			editor.cx--;
	}
	if (row) editor_update_row(row);
	editor.dirty++;
}

/* Forward-delete the char at the current cursor position (DEL key).
 * At end of line, joins with the next line. */
void editor_del_forward_char(void)
{
	erow *row = (editor.rowoff + editor.cy >= editor.numrows) ? NULL : &editor.row[editor.rowoff + editor.cy];
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;

	if (!row) return;

	if (filecol == row->size) {
		if (filerow + 1 >= editor.numrows) return;
		undo_push(UNDO_JOIN_LINE, filerow, filecol, 0,
			 editor.row[filerow+1].chars, editor.row[filerow+1].size);
		editor_row_append_string(row, editor.row[filerow+1].chars, editor.row[filerow+1].size);
		editor_del_row(filerow + 1);
	} else {
		undo_push(UNDO_DELETE_CHAR, filerow, filecol, row->chars[filecol], NULL, 0);
		editor_row_del_char(row, filecol);
	}
	editor.dirty++;
}

/* Kill (delete) from cursor to end of line (C-k).
 *
 * Invariant relied on by the C-u-batched kill in kbd.c: every byte removed
 * from the buffer here is also kill_ring_append()-ed in the same step.
 * Keep them in lock-step — diverging would silently corrupt undo replay
 * for `C-u N C-k`. */
void editor_kill_line(void)
{
	erow *row = (editor.rowoff + editor.cy >= editor.numrows) ? NULL : &editor.row[editor.rowoff + editor.cy];
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;

	if (!row) return;

	if (filecol >= row->size) {
		/* At end of line, join with next line like C-k in Emacs. */
		if (filerow+1 < editor.numrows) {
			/* Save newline to kill ring */
			kill_ring_append("\n", 1);
			/* Record undo: save the line that will be joined */
			undo_push(UNDO_KILL_TEXT, filerow, filecol, 0,
				 editor.row[filerow+1].chars, editor.row[filerow+1].size);
			editor_row_append_string(row, editor.row[filerow+1].chars, editor.row[filerow+1].size);
			editor_del_row(filerow+1);
		}
	} else {
		/* Delete from cursor to end of line and save to kill ring. */
		int kill_len = row->size - filecol;
		if (kill_len > 0) {
			kill_ring_append(row->chars + filecol, kill_len);
			/* Record undo operation */
			undo_push(UNDO_KILL_TEXT, filerow, filecol, 0, row->chars + filecol, kill_len);
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
	if (!buf)
		return;
	if (len < 2)
		goto done;

	point = editor_current_flat_offset();
	if (point < 0)
		point = 0;
	if (point > len)
		point = len;
	while (point > 0 && point < len && utf8_is_cont((unsigned char)buf[point]))
		point--;

	if (point >= len || buf[point] == '\n') {
		b_start = utf8_prev_glyph_start(buf, point);
		if (b_start < 0)
			goto done;
		a_start = utf8_prev_glyph_start(buf, b_start);
		if (a_start < 0)
			goto done;
		b_len = point - b_start;
	} else {
		b_start = point;
		a_start = utf8_prev_glyph_start(buf, b_start);
		if (a_start < 0)
			goto done;
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
