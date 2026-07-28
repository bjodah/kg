/* ========================= Editor events handling  ======================== */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>

#include "def.h"
#include "localvars.h"

static void cursor_advance_screen_col(void)
{
	if (editor.cx == editor.screencols - 1) {
		if (editor.coloff < INT_MAX) {
			editor.coloff++;
		}
	} else if (editor.cx < INT_MAX) {
		editor.cx++;
	}
}

/* Handle cursor position change because arrow keys were pressed. */
void editor_move_cursor(int key)
{
	int filerow = editor_current_filerow_or_eof();
	erow *row = filerow >= editor.numrows ? NULL : &editor.row[filerow];
	int filecol = editor_current_filecol();
	int is_vertical = (key == ARROW_UP || key == ARROW_DOWN);
	int rowlen;

	/* Capture the visual goal column on the first vertical move so a
	 * run of C-n/C-p stays at the same visible column across rows of
	 * mixed UTF-8/tab content. */
	if (is_vertical && editor.desired_visual_col < 0 && row) {
		editor.desired_visual_col = editor_visual_col(row, filecol);
	}

	if (editor.visual_line_mode) {
		struct editor_window *w = &winlist[win_current];
		int win_w = w->w > 0 ? w->w : 1;
		switch (key) {
		case HOME_KEY: {
			if (row) {
				int rcol = visual_line_cursor_col(
				    row, filecol, win_w);
				int target_rcol = (rcol / win_w) * win_w;
				int char_idx = render_col_to_chars(
				    row, target_rcol, win_w);
				editor_cursor_goto(filerow, char_idx);
			}
			return;
		}
		case END_KEY: {
			if (row) {
				int rcol = visual_line_cursor_col(
				    row, filecol, win_w);
				int max_rcol = visual_line_width(row, win_w);
				int target_rcol = filecol == row->size
				    ? max_rcol
				    : ((rcol / win_w) + 1) * win_w - 1;
				if (target_rcol > max_rcol) {
					target_rcol = max_rcol;
				}
				int char_idx = render_col_to_chars(
				    row, target_rcol, win_w);
				editor_cursor_goto(filerow, char_idx);
			}
			return;
		}
		case ARROW_UP: {
			int rcol = row
			    ? visual_line_cursor_col(row, filecol, win_w)
			    : 0;
			int cur_vrow = get_visual_row(editor.row,
			    editor.numrows, win_w, filerow, filecol);
			if (cur_vrow > 0) {
				goto_visual_row_col(cur_vrow - 1, rcol % win_w);
			}
			return;
		}
		case ARROW_DOWN: {
			int rcol = row
			    ? visual_line_cursor_col(row, filecol, win_w)
			    : 0;
			int cur_vrow = get_visual_row(editor.row,
			    editor.numrows, win_w, filerow, filecol);
			goto_visual_row_col(cur_vrow + 1, rcol % win_w);
			return;
		}
		}
	}

	switch (key) {
	case HOME_KEY:
		editor.cx = 0;
		editor.coloff = 0;
		break;
	case END_KEY:
		if (row) {
			if (row->size > editor.screencols - 1) {
				editor.coloff
				    = row->size - editor.screencols + 1;
				editor.cx = editor.screencols - 1;
			} else {
				editor.cx = row->size;
				editor.coloff = 0;
			}
		}
		break;
	case ARROW_LEFT:
		if (filecol == 0) {
			if (filerow >= editor.numrows) {
				int prevrow = editor.numrows - 1;
				prevrow = prevrow < 0 ? 0 : prevrow;
				editor_cursor_goto(prevrow,
				    editor.numrows ? editor.row[prevrow].size
						   : 0);
				break;
			}
			if (filerow > 0) {
				if (editor.cy == 0) {
					editor.rowoff--;
				} else {
					editor.cy--;
				}
				editor.cx = editor.row[filerow - 1].size;
				if (editor.cx > editor.screencols - 1) {
					editor.coloff
					    = editor.cx - editor.screencols + 1;
					editor.cx = editor.screencols - 1;
				}
			}
		} else if (row && filecol > row->size) {
			/* Virtual space past EOL (rect mark mode): plain step.
			 */
			if (editor.cx == 0) {
				if (editor.coloff) {
					editor.coloff--;
				}
			} else {
				editor.cx -= 1;
			}
		} else {
			/* Step back a whole glyph (1 byte for ASCII, 2-4 for
			 * UTF-8 multi-byte) by counting continuation bytes
			 * trailing the previous start byte. */
			int n = 1, pos = filecol - 1;
			while (pos > 0 && row
			    && utf8_is_cont((unsigned char)row->chars[pos])) {
				n++;
				pos--;
			}
			while (n--) {
				if (editor.cx == 0) {
					if (editor.coloff) {
						editor.coloff--;
					} else {
						break;
					}
				} else {
					editor.cx -= 1;
				}
			}
		}
		break;
	case ARROW_RIGHT:
		if (row && filecol < row->size) {
			/* Step forward a whole glyph: 1 + however many
			 * continuation bytes follow the current start byte. */
			int n = 1;
			while (filecol + n < row->size
			    && utf8_is_cont(
				(unsigned char)row->chars[filecol + n])) {
				n++;
			}
			while (n--) {
				cursor_advance_screen_col();
			}
		} else if (row && editor.rect_mode) {
			/* In rect mark mode, extend the cursor into virtual
			 * space past EOL so a rectangle can span columns that
			 * some rows don't reach. */
			cursor_advance_screen_col();
		} else if (row && filecol == row->size
		    && filerow < editor.numrows - 1) {
			editor.cx = 0;
			editor.coloff = 0;
			if (editor.cy == editor.screenrows - 1) {
				editor.rowoff++;
			} else {
				editor.cy += 1;
			}
		}
		break;
	case ARROW_UP:
		if (editor.cy > 0) {
			editor.cy -= 1;
		} else if (editor.rowoff) {
			editor.rowoff--;
		} else {
			editor.desired_visual_col = -1;
			editor.cx = 0;
			editor.coloff = 0;
		}
		break;
	case ARROW_DOWN:
		if (filerow < editor.numrows - 1) {
			if (editor.cy == editor.screenrows - 1) {
				editor.rowoff++;
			} else {
				editor.cy += 1;
			}
		} else if (row) {
			if (row->size > editor.screencols - 1) {
				editor.coloff
				    = row->size - editor.screencols + 1;
				editor.cx = editor.screencols - 1;
			} else {
				editor.cx = row->size;
				editor.coloff = 0;
			}
			editor.desired_visual_col = -1;
		}
		break;
	}
	/* After vertical motion, snap cx to the byte position that lands
	 * at the saved goal column on the new row.  In rect mode the
	 * cursor is allowed to stay past EOL; the trailing clamp below
	 * only fires for non-rect-mode. */
	filerow = editor_current_filerow_or_eof();
	row = filerow >= editor.numrows ? NULL : &editor.row[filerow];
	if (is_vertical && row && editor.desired_visual_col >= 0) {
		int target = editor_chars_col_at_visual(
		    row, editor.desired_visual_col);
		editor.cx = target - editor.coloff;
		if (editor.cx < 0) {
			editor.coloff = target;
			editor.cx = 0;
		} else if (editor.cx > editor.screencols - 1) {
			editor.coloff += editor.cx - (editor.screencols - 1);
			editor.cx = editor.screencols - 1;
		}
	}

	/* Fix cx if the current line has not enough chars.  In rect mark
	 * mode the cursor is allowed to stay past EOL so the user can
	 * extend a rectangle whose right edge crosses shorter lines —
	 * editor_snap_cx_to_row() snaps it back when rect mode ends. */
	rowlen = row ? row->size : 0;
	if (!editor.rect_mode && editor.coloff > rowlen) {
		editor.coloff = rowlen;
		editor.cx = 0;
	} else if (!editor.rect_mode && editor.cx > rowlen - editor.coloff) {
		editor.cx = rowlen - editor.coloff;
	}
}

/* Move to the beginning of the document */
void editor_move_to_beginning(void)
{
	editor.cx = 0;
	editor.cy = 0;
	editor.rowoff = 0;
	editor.coloff = 0;
}

/* Move the cursor to the first non-whitespace character on the current
 * line (M-m).  Lands at end-of-line if the line is blank or all whitespace.
 * Doesn't toggle back to column 0 — C-a already does that. */
void editor_move_to_indentation(void)
{
	erow *row;
	int col;

	if (editor_current_filerow_or_eof() >= editor.numrows) {
		return;
	}

	editor_move_cursor(HOME_KEY);

	row = &editor.row[editor_current_filerow()];
	col = editor_current_filecol();
	while (col < row->size && isspace((unsigned char)row->chars[col])) {
		editor_move_cursor(ARROW_RIGHT);
		col = editor_current_filecol();
	}
}

/* Park the cursor at the top, middle, or bottom of the visible window
 * (M-r), cycling through those three on consecutive presses.  Doesn't
 * scroll — only moves the cursor within the existing viewport, clamped
 * to actual content. */
void editor_move_to_window_line(void)
{
	int target;
	int last_visible_row;

	switch (editor.window_line_state) {
	case 0:
		target = 0;
		break;
	case 1:
		target = editor.screenrows / 2;
		break;
	default:
		target = editor.screenrows - 1;
		break;
	}

	last_visible_row = editor.numrows - editor.rowoff - 1;
	if (last_visible_row < 0) {
		last_visible_row = 0;
	}
	if (target > last_visible_row) {
		target = last_visible_row;
	}
	if (target < 0) {
		target = 0;
	}

	editor.cy = target;
	editor.cx = 0;
	editor.coloff = 0;
	editor.window_line_state = (editor.window_line_state + 1) % 3;
}

/* Jump to a specific line (1-based) and column (1-based, 0 = start). */
void editor_goto_line_direct(int line, int col)
{
	int filerow, filecol;
	erow *row;

	if (editor.numrows == 0) {
		return;
	}
	if (line < 1) {
		line = 1;
	}
	if (line > editor.numrows) {
		line = editor.numrows;
	}

	filerow = line - 1;
	filecol = (col > 1) ? col - 1 : 0;
	row = &editor.row[filerow];
	if (filecol > row->size) {
		filecol = row->size;
	}

	/* Centre the target line vertically. */
	editor.rowoff = filerow - editor.screenrows / 2;
	if (editor.rowoff < 0) {
		editor.rowoff = 0;
	}
	editor.cy = filerow - editor.rowoff;

	if (filecol > editor.screencols - 1) {
		editor.coloff = filecol - editor.screencols + 1;
		editor.cx = editor.screencols - 1;
	} else {
		editor.coloff = 0;
		editor.cx = filecol;
	}
}

/* Prompt for a line number (optionally "LINE:COL") and jump to it. */
void editor_goto_line(int fd)
{
	char buf[16] = { 0 };
	int line = 0, col = 1, n;

	if (editor_read_line(fd, "Goto line: ", buf, sizeof(buf)) < 0
	    || !buf[0]) {
		return;
	}
	n = sscanf(buf, "%d:%d", &line, &col);
	if (n < 1) {
		return;
	}
	if (n < 2) {
		col = 1;
	}
	editor_goto_line_direct(line, col);
}

/* Move to the end of the document */
void editor_move_to_end(void)
{
	erow *row;
	int filerow;

	if (editor.numrows == 0) {
		return;
	}

	filerow = editor.numrows - 1;
	row = &editor.row[filerow];

	/* Update cursor position */
	if (filerow >= editor.rowoff + editor.screenrows) {
		editor.rowoff = filerow - editor.screenrows + 1;
		editor.cy = editor.screenrows - 1;
	} else {
		editor.cy = filerow - editor.rowoff;
	}

	/* Move to end of last line */
	if (row->size > editor.screencols - 1) {
		editor.coloff = row->size - editor.screencols + 1;
		editor.cx = editor.screencols - 1;
	} else {
		editor.cx = row->size;
		editor.coloff = 0;
	}
}
